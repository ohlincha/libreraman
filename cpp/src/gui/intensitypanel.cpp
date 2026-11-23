#include "intensitypanel.h"

#include "controller.h"
#include "plotwidget.h"
#include "theme.h"

#include "core/calibrate.h"
#include "core/calibration.h"
#include "core/errors.h"
#include "core/fileio.h"

#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace lr::gui {

namespace {

const QString kTag = QStringLiteral("intensity:");

QGroupBox* box(const QString& title) {
    auto* g = new QGroupBox(title);
    g->setStyleSheet(theme::groupBoxStyle());
    return g;
}

QLabel* muted(const QString& text = {}) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet(QStringLiteral("color: %1;").arg(theme::mutedText().name()));
    return l;
}

Vec normalised(const Vec& x, const Vec& y, double lo, double hi) {   // max over [lo, hi] = 1
    double m = 0;
    for (size_t i = 0; i < x.size(); ++i)
        if (x[i] >= lo && x[i] <= hi && std::isfinite(y[i])) m = std::max(m, y[i]);
    Vec out(y.size(), std::nan(""));
    if (m > 0)
        for (size_t i = 0; i < y.size(); ++i) out[i] = y[i] / m;
    return out;
}

}  // namespace

IntensityPanel::IntensityPanel(Host host, QWidget* parent) : QWidget(parent), m_host(std::move(host)) {
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(0, 6, 0, 0);
    h->setSpacing(14);
    auto* left = new QWidget;
    left->setFixedWidth(410);
    auto* v = new QVBoxLayout(left);
    v->setContentsMargins(0, 0, 8, 0);

    QGroupBox* b = box(QStringLiteral("Intensity response"));
    auto* f = new QFormLayout(b);
    f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    f->addRow(muted(QStringLiteral(
        "Measure a reference whose true emission curve is known. The correction removes the instrument's "
        "spectral sensitivity, so that band ratios across the spectrum can be compared. Raw data are not "
        "changed: tick <i>intensity correction</i> in Measure or Data to apply it.")));
    m_kind = new QComboBox;
    m_kind->addItem(QStringLiteral("NIST SRM 2242 (glass, 532 nm)"), int(response::Reference::Kind::Srm2242));
    m_kind->addItem(QStringLiteral("Tungsten-halogen lamp (blackbody)"), int(response::Reference::Kind::Blackbody));
    m_kind->addItem(QStringLiteral("Table from a file (shift, intensity)"), int(response::Reference::Kind::Table));
    m_kind->setToolTip(QStringLiteral(
        "<p><b>SRM 2242</b>: certified curve, the same coefficients as in the instrument's factory file.<br>"
        "<b>Lamp</b>: Planck curve at the lamp's colour temperature (a few % approximation for tungsten).<br>"
        "<b>Table</b>: e.g. a certificate, two columns: Raman shift (cm⁻¹) and relative intensity.</p>"));
    connect(m_kind, &QComboBox::currentIndexChanged, this, [this] {
        updateForm();
        redraw();
    });
    f->addRow(QStringLiteral("Reference"), m_kind);
    m_temp = new QDoubleSpinBox;
    m_temp->setRange(1000, 4000);
    m_temp->setDecimals(0);
    m_temp->setSingleStep(10);
    m_temp->setValue(2856);
    m_temp->setSuffix(QStringLiteral(" K"));
    m_temp->setToolTip(QStringLiteral("Colour temperature of the lamp (2856 K = CIE illuminant A)"));
    connect(m_temp, &QDoubleSpinBox::valueChanged, this, &IntensityPanel::redraw);
    f->addRow(QStringLiteral("Temperature"), m_temp);
    m_btnTable = new QPushButton(QStringLiteral("Choose table…"));
    m_tableLabel = muted(QStringLiteral("no table"));
    connect(m_btnTable, &QPushButton::clicked, this, [this] {
        const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Reference curve"), QString(),
                                                       QStringLiteral("Tables (*.csv *.txt);;All files (*)"));
        if (!p.isEmpty()) setTableFile(p);
    });
    auto* tr = new QHBoxLayout;
    tr->addWidget(m_btnTable);
    tr->addWidget(m_tableLabel, 1);
    f->addRow(QStringLiteral("Table"), tr);
    m_exposure = new QDoubleSpinBox;
    m_exposure->setRange(0.01, 600);
    m_exposure->setValue(1.0);
    m_exposure->setSuffix(QStringLiteral(" s"));
    m_scans = new QSpinBox;
    m_scans->setRange(1, 1000);
    m_scans->setValue(10);
    auto* er = new QHBoxLayout;
    er->addWidget(m_exposure, 1);
    er->addWidget(new QLabel(QStringLiteral("Scans")));
    er->addWidget(m_scans, 1);
    f->addRow(QStringLiteral("Exposure"), er);
    v->addWidget(b);

    m_btnMeasure = new QPushButton(QStringLiteral("▶  MEASURE REFERENCE"));
    m_btnMeasure->setStyleSheet(theme::bigButtonStyle(theme::Action::Measure));
    m_btnMeasure->setMinimumHeight(44);
    connect(m_btnMeasure, &QPushButton::clicked, this, [this] { measure(); });
    m_btnLoad = new QPushButton(QStringLiteral("Use a saved spectrum…"));
    connect(m_btnLoad, &QPushButton::clicked, this, [this] {
        const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Spectrum of the reference"), QString(),
                                                       QStringLiteral("Spectra (*.csv *.txt *.spc *.jdx);;All files (*)"));
        if (!p.isEmpty()) loadFile(p);
    });
    v->addWidget(m_btnMeasure);
    v->addWidget(m_btnLoad);

    b = box(QStringLiteral("Result"));
    auto* rl = new QVBoxLayout(b);
    m_result = new QLabel(QStringLiteral("—"));
    m_result->setWordWrap(true);
    m_result->setTextFormat(Qt::RichText);
    rl->addWidget(m_result);
    auto* ar = new QHBoxLayout;
    m_btnAccept = new QPushButton(QStringLiteral("Accept"));
    m_btnDiscard = new QPushButton(QStringLiteral("Discard"));
    connect(m_btnAccept, &QPushButton::clicked, this, [this] { acceptPending(); });
    connect(m_btnDiscard, &QPushButton::clicked, this, [this] {
        m_pending.reset();
        m_result->setText(QStringLiteral("—"));
        updateEnabled();
        redraw();
    });
    ar->addWidget(m_btnAccept);
    ar->addWidget(m_btnDiscard);
    rl->addLayout(ar);
    v->addWidget(b);

    b = box(QStringLiteral("In use"));
    auto* ul = new QVBoxLayout(b);
    m_inUse = new QLabel;
    m_inUse->setWordWrap(true);
    ul->addWidget(m_inUse);
    m_btnRemove = new QPushButton(QStringLiteral("Remove correction"));
    m_btnRemove->setToolTip(QStringLiteral("Stop correcting intensities (the history copy is kept)"));
    connect(m_btnRemove, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, QStringLiteral("Intensity response"),
                                  QStringLiteral("Remove the intensity correction of this instrument and channel?")) ==
            QMessageBox::Yes)
            removeActive();
    });
    ul->addWidget(m_btnRemove);
    v->addWidget(b);
    v->addStretch();
    h->addWidget(left);

    auto* split = new QSplitter(Qt::Vertical);
    m_curvePlot = new PlotWidget;
    m_curvePlot->setToolTip(PlotWidget::navigationHelp());
    m_curvePlot->setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    m_curvePlot->setLabel(Axis::Left, QStringLiteral("Relative intensity"));
    m_curvePlot->setHeadroom(0.12);
    split->addWidget(m_curvePlot);
    m_factorPlot = new PlotWidget;
    m_factorPlot->setToolTip(PlotWidget::navigationHelp());
    m_factorPlot->setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    m_factorPlot->setLabel(Axis::Left, QStringLiteral("Correction factor"));
    m_factorPlot->setHeadroom(0.12);
    split->addWidget(m_factorPlot);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    h->addWidget(split, 1);

    updateForm();
    refresh();
}

void IntensityPanel::setController(Controller* c) {
    if (m_ctl) disconnect(m_ctl, nullptr, this, nullptr);
    m_ctl = c;
    if (!c) return;
    connect(c, &Controller::acquired, this, &IntensityPanel::onAcquired);
    connect(c, &Controller::busyChanged, this, [this](bool busy) {
        if (!busy) m_busy = false;                     // ended (with or without a spectrum)
        updateEnabled();
    });
    connect(c, &Controller::status, this, [this](const StatusView&) { updateEnabled(); });
    connect(c, &Controller::connected, this, [this](const InstrumentInfo&) { refresh(); });
}

void IntensityPanel::refresh() {
    const auto a = response::active(m_host.serial(), m_host.channel());
    m_inUse->setText(inUseText());
    m_btnRemove->setEnabled(a != nullptr);
    updateEnabled();
    redraw();
}

QString IntensityPanel::inUseText() const {
    const auto a = response::active(m_host.serial(), m_host.channel());
    if (!a) return QStringLiteral("No intensity correction for %1, %2 nm.").arg(m_host.serial()).arg(m_host.channel());
    return QStringLiteral("%1 (%2), %3–%4 cm⁻¹; the factor varies %5× over that range.")
        .arg(a->reference, a->date)
        .arg(a->range[0], 0, 'f', 0)
        .arg(a->range[1], 0, 'f', 0)
        .arg(a->dynamic, 0, 'f', 1);
}

response::Reference IntensityPanel::reference() const {
    const auto kind = response::Reference::Kind(m_kind->currentData().toInt());
    response::Reference r;
    if (kind == response::Reference::Kind::Table) return m_table;
    r.kind = kind;
    r.temperatureK = m_temp->value();
    if (kind == response::Reference::Kind::Srm2242)
        if (const ChannelCal* c = m_host.channelCal ? m_host.channelCal() : nullptr; c && c->nist_coeff.size() >= 2) {
            r.coeff = c->nist_coeff;
            r.range[0] = c->nist_range[0];
            r.range[1] = c->nist_range[1];
        }
    return r;
}

void IntensityPanel::setReferenceKind(response::Reference::Kind k) { m_kind->setCurrentIndex(m_kind->findData(int(k))); }
void IntensityPanel::setTemperature(double kelvin) { m_temp->setValue(kelvin); }

bool IntensityPanel::setTableFile(const QString& path) {
    try {
        m_table = response::readTable(path);
        m_tableLabel->setText(QStringLiteral("%1 (%2 points, %3–%4 cm⁻¹)")
                                  .arg(m_table.tableName)
                                  .arg(m_table.tableShift.size())
                                  .arg(m_table.tableShift.front(), 0, 'f', 0)
                                  .arg(m_table.tableShift.back(), 0, 'f', 0));
        setReferenceKind(response::Reference::Kind::Table);
        updateEnabled();
        redraw();
        return true;
    } catch (const std::exception& e) {
        m_result->setText(QStringLiteral("<span style='color:%1'>%2</span>")
                              .arg(theme::warningText().name(), QString::fromUtf8(e.what()).toHtmlEscaped()));
        return false;
    }
}

void IntensityPanel::updateForm() {
    const auto kind = response::Reference::Kind(m_kind->currentData().toInt());
    m_temp->setEnabled(kind == response::Reference::Kind::Blackbody);
    m_btnTable->setEnabled(kind == response::Reference::Kind::Table);
    updateEnabled();
}

void IntensityPanel::updateEnabled() {
    const bool tableOk = response::Reference::Kind(m_kind->currentData().toInt()) != response::Reference::Kind::Table ||
                         m_table.tableShift.size() >= 2;
    const bool ready = m_host.ready && m_host.ready() && m_host.controller && m_host.controller();
    m_btnMeasure->setEnabled(ready && !m_busy && tableOk);
    m_btnLoad->setEnabled(!m_busy && tableOk);
    m_btnAccept->setEnabled(m_pending.has_value() && !m_busy);
    m_btnDiscard->setEnabled(m_pending.has_value());
}

bool IntensityPanel::measure() {
    Controller* c = m_host.controller ? m_host.controller() : nullptr;
    if (!c || m_busy) return false;
    MeasureSettings s;
    s.sample = QStringLiteral("intensity reference");
    s.exposure_s = m_exposure->value();
    s.averages = m_scans->value();
    s.background = QStringLiteral("each");
    s.channel = 0;
    if (!c->acquire(s, kTag + reference().label())) return false;
    m_busy = true;
    m_result->setText(QStringLiteral("Measuring the reference…"));
    updateEnabled();
    return true;
}

void IntensityPanel::onAcquired(const lr::Spectrum& sp0, const QString& tag) {
    if (!tag.startsWith(kTag)) return;
    m_busy = false;
    lr::Spectrum sp = sp0;
    sp.setMeta(QStringLiteral("sample"), QStringLiteral("intensity reference"));
    sp.setMeta(QStringLiteral("purpose"), QStringLiteral("intensity response (%1)").arg(tag.mid(kTag.size())));
    QString file;
    try {
        const QString dir = QDir(calib::configDir()).filePath(QStringLiteral("calibration-spectra"));
        QDir().mkpath(dir);
        file = Controller::savePath(dir, QStringLiteral("intensity-reference_") + m_host.serial(), sp.t);
        sp.saveCsv(file);
        if (m_host.log) m_host.log(QStringLiteral("Saved ") + QDir::toNativeSeparators(file));
    } catch (const std::exception& e) {
        if (m_host.log) m_host.log(QStringLiteral("Reference spectrum not saved: ") + QString::fromUtf8(e.what()));
    }
    const Vec& raw = sp.signal.empty() ? sp.intensity : sp.signal;
    const double rawMax = raw.empty() ? std::nan("") : *std::max_element(raw.begin(), raw.end());
    evaluate(sp.shift, sp.intensity, rawMax, sp.exposure_s, sp.averages, file);
}

bool IntensityPanel::loadFile(const QString& path) {
    try {
        std::vector<fileio::DataSpectrum> all = fileio::read(path);
        if (all.empty()) throw lr::Error("no spectrum in the file");
        fileio::DataSpectrum& d = all.front();
        d.sorted();
        const double rawMax = d.signal.empty() ? std::nan("") : *std::max_element(d.signal.begin(), d.signal.end());
        evaluate(d.x, d.y, rawMax, d.metaValue(QStringLiteral("exposure_s")).toDouble(),
                 d.metaValue(QStringLiteral("averages")).toInt(), QFileInfo(path).absoluteFilePath());
        return m_pending.has_value();
    } catch (const std::exception& e) {
        m_result->setText(QStringLiteral("<span style='color:%1'>%2: %3</span>")
                              .arg(theme::warningText().name(), QFileInfo(path).fileName().toHtmlEscaped(),
                                   QString::fromUtf8(e.what()).toHtmlEscaped()));
        return false;
    }
}

void IntensityPanel::evaluate(const Vec& x, const Vec& y, double rawMax, double exposure, int averages,
                              const QString& file) {
    m_x = x;
    m_y = y;
    m_pending.reset();
    try {
        response::Response r = response::compute(x, y, reference(), m_host.laserNm(), rawMax);
        r.serial = m_host.serial();
        r.channel = m_host.channel();
        r.exposure_s = exposure;
        r.averages = averages;
        r.sourceFile = file;
        QString text = QStringLiteral("<b>%1</b>: valid %2–%3 cm⁻¹, S/N %4, factor varies %5×.")
                           .arg(r.reference.toHtmlEscaped())
                           .arg(r.range[0], 0, 'f', 0)
                           .arg(r.range[1], 0, 'f', 0)
                           .arg(r.snr, 0, 'f', 0)
                           .arg(r.dynamic, 0, 'f', 1);
        for (const QString& w : r.warnings)
            text += QStringLiteral("<br><span style='color:%1'>%2</span>").arg(theme::warningText().name(), w.toHtmlEscaped());
        text += QStringLiteral("<br>Accept to use it for %1, %2 nm.").arg(r.serial.toHtmlEscaped()).arg(r.channel);
        m_result->setText(text);
        m_pending = std::move(r);
    } catch (const std::exception& e) {
        m_result->setText(QStringLiteral("<span style='color:%1'>Not usable: %2</span>")
                              .arg(theme::warningText().name(), QString::fromUtf8(e.what()).toHtmlEscaped()));
    }
    updateEnabled();
    redraw();
}

bool IntensityPanel::acceptPending() {
    if (!m_pending) return false;
    try {
        const QString hist = response::accept(*m_pending);
        if (m_host.log)
            m_host.log(QStringLiteral("Intensity correction accepted (%1): %2")
                           .arg(m_pending->summary(), QDir::toNativeSeparators(hist)));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, QStringLiteral("Intensity response"), QString::fromUtf8(e.what()));
        return false;
    }
    m_pending.reset();
    m_result->setText(QStringLiteral("Accepted."));
    refresh();
    emit responseChanged();
    return true;
}

bool IntensityPanel::removeActive() {
    if (!response::remove(m_host.serial(), m_host.channel())) return false;
    if (m_host.log) m_host.log(QStringLiteral("Intensity correction removed for %1, %2 nm").arg(m_host.serial()).arg(m_host.channel()));
    refresh();
    emit responseChanged();
    return true;
}

void IntensityPanel::redraw() {
    const PlotPalette pal = theme::plotPalette();
    m_curvePlot->clear();
    m_factorPlot->clear();
    const auto a = response::active(m_host.serial(), m_host.channel());
    double lo = response::kMinShift, hi = 3500;
    if (m_pending) {
        lo = m_pending->range[0];
        hi = m_pending->range[1];
    }
    if (!m_x.empty()) {
        m_curvePlot->addCurve(m_x, normalised(m_x, m_y, lo, hi), QPen(pal.main, 1.2), QStringLiteral("measured"));
        Vec truth = reference().curve(m_x, m_host.laserNm());
        m_curvePlot->addCurve(m_x, normalised(m_x, truth, lo, hi), QPen(pal.reference, 1.4, Qt::DashLine),
                              QStringLiteral("true curve"));
        if (m_pending) m_curvePlot->addRegion(m_pending->range[0], m_pending->range[1]);
    } else {
        Vec x;
        for (double s = 0; s <= 4000; s += 10) x.push_back(s);
        m_curvePlot->addCurve(x, normalised(x, reference().curve(x, m_host.laserNm()), 0, 4000),
                              QPen(pal.reference, 1.4, Qt::DashLine), QStringLiteral("true curve"));
    }
    if (a) {
        Vec x, f;
        for (size_t i = 0; i < a->shift.size(); ++i)
            if (a->shift[i] >= a->range[0] && a->shift[i] <= a->range[1]) {
                x.push_back(a->shift[i]);
                f.push_back(a->factor[i]);
            }
        m_factorPlot->addCurve(x, f, QPen(pal.previous, 1.2, Qt::DashLine), QStringLiteral("in use"));
    }
    if (m_pending) {
        Vec x, f;
        for (size_t i = 0; i < m_pending->shift.size(); ++i)
            if (m_pending->shift[i] >= m_pending->range[0] && m_pending->shift[i] <= m_pending->range[1]) {
                x.push_back(m_pending->shift[i]);
                f.push_back(m_pending->factor[i]);
            }
        m_factorPlot->addCurve(x, f, QPen(pal.main, 1.4), QStringLiteral("new"));
    }
    m_curvePlot->setXRange(0, 4000);
    m_factorPlot->setXRange(0, 4000);
    m_curvePlot->setAutoY(true);
    m_factorPlot->setAutoY(true);
}

}  // namespace lr::gui
