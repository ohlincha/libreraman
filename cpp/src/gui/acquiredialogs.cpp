#include "acquiredialogs.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <cmath>

#include "measurepage.h"
#include "plotwidget.h"
#include "theme.h"

namespace lr::gui {

namespace {

QString clock(double s) {
    const int t = int(std::lround(std::max(0.0, s)));
    return QStringLiteral("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QLatin1Char('0'));
}

QDoubleSpinBox* doubleBox(int decimals, double lo, double hi, double value, const QString& suffix = {}) {
    auto* b = new QDoubleSpinBox;
    b->setDecimals(decimals);
    b->setRange(lo, hi);
    b->setValue(value);
    b->setSuffix(suffix);
    return b;
}

QString muted(const QString& html) {
    return QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::mutedText().name(), html);
}

}  // namespace

// ============================================================================================ SnrDialog
SnrDialog::SnrDialog(Controller* ctl, MeasurePage* page, QWidget* parent)
    : QWidget(parent, Qt::Window), m_ctl(ctl), m_page(page) {
    setWindowTitle(QStringLiteral("Acquire until S/N – LibreRaman"));
    resize(520, 360);
    auto* v = new QVBoxLayout(this);
    auto* intro = new QLabel(QStringLiteral(
        "Short frames are added up until the chosen band reaches the target signal-to-noise ratio, the time limit "
        "runs out, or the sample starts changing under the laser. Useful for weak or heat-sensitive samples. "
        "Sample name, folder, background and laser come from the Measure page."));
    intro->setWordWrap(true);
    v->addWidget(intro);
    auto* f = new QFormLayout;
    m_target = doubleBox(0, 2, 100000, 100);
    m_target->setToolTip(QStringLiteral("<p>Band height above the background divided by the noise. ~10: band clearly "
                                        "visible; 50–100: positions and ratios reliable; S/N grows with the square "
                                        "root of the number of frames.</p>"));
    m_band = doubleBox(0, 0, 4000, 0, QStringLiteral(" cm⁻¹"));
    m_band->setSpecialValueText(QStringLiteral("strongest"));
    m_band->setToolTip(QStringLiteral("The band whose S/N counts: the strongest (0), or the one at this shift (±15 cm⁻¹)"));
    m_exposure = doubleBox(2, 0.01, 600, page ? page->exposureBox()->value() : 1.0, QStringLiteral(" s"));
    m_exposure->setToolTip(QStringLiteral("<p>Exposure of each frame. Short frames on dark samples: less heating per "
                                          "frame, and the damage check sees a change early.</p>"));
    m_minutes = doubleBox(1, 0.1, 600, 10, QStringLiteral(" min"));
    m_minutes->setToolTip(QStringLiteral("Stop after this time even if the target is not reached (laser-on time)"));
    m_stopOnDamage = new QCheckBox(QStringLiteral("stop if the sample changes under the laser"));
    m_stopOnDamage->setChecked(true);
    m_stopOnDamage->setToolTip(QStringLiteral("<p>The first and the last third of the frames are compared (band "
                                              "positions, band heights, background). The spectrum so far is saved "
                                              "either way, marked in its file.</p>"));
    f->addRow(QStringLiteral("Target S/N"), m_target);
    f->addRow(QStringLiteral("Band"), m_band);
    f->addRow(QStringLiteral("Frame exposure"), m_exposure);
    f->addRow(QStringLiteral("Time limit"), m_minutes);
    f->addRow(m_stopOnDamage);
    v->addLayout(f);
    m_bar = new QProgressBar;
    m_bar->setRange(0, 100);
    m_bar->setValue(0);
    m_bar->setTextVisible(false);
    v->addWidget(m_bar);
    m_status = new QLabel(QStringLiteral("—"));
    m_status->setObjectName(QStringLiteral("snrStatus"));
    v->addWidget(m_status);
    m_result = new QLabel;
    m_result->setWordWrap(true);
    v->addWidget(m_result);
    v->addStretch();
    auto* row = new QHBoxLayout;
    m_start = new QPushButton(QStringLiteral("Start"));
    m_start->setDefault(true);
    m_stop = new QPushButton(QStringLiteral("■  Stop"));
    m_stop->setToolTip(QStringLiteral("Laser off at once; the frames so far are saved as the spectrum"));
    auto* close = new QPushButton(QStringLiteral("Close"));
    row->addWidget(m_start);
    row->addWidget(m_stop);
    row->addStretch();
    row->addWidget(close);
    v->addLayout(row);
    connect(m_start, &QPushButton::clicked, this, &SnrDialog::start);
    connect(m_stop, &QPushButton::clicked, this, [this] {
        if (m_ctl) m_ctl->stop();
    });
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    if (m_ctl) {
        connect(m_ctl, &Controller::snrProgress, this, &SnrDialog::onProgress);
        connect(m_ctl, &Controller::busyChanged, this, &SnrDialog::onBusy);
    }
    onBusy(m_ctl && m_ctl->isBusy());
}

void SnrDialog::start() {
    if (!m_ctl || (m_page && !m_page->prepareMeasurement()))
        return;
    MeasureSettings s = m_page ? m_page->measureSettings() : MeasureSettings{};
    s.exposure_s = m_exposure->value();
    lr::acq::SnrTarget t;
    t.target = m_target->value();
    if (m_band->value() > 0) t.band = m_band->value();
    t.maxSeconds = m_minutes->value() * 60;
    t.stopOnDamage = m_stopOnDamage->isChecked();
    m_result->clear();
    m_bar->setValue(0);
    m_status->setText(QStringLiteral("Background (laser off)…"));
    m_running = m_ctl->measureUntilSnr(s, t, QStringLiteral("GUI (until S/N)"));
    onBusy(m_ctl->isBusy());
}

void SnrDialog::onProgress(const lr::acq::SnrProgress& p) {
    if (!m_running)
        return;
    const bool ok = std::isfinite(p.snr);
    m_bar->setValue(ok ? std::clamp(int(100 * p.snr / p.target), 0, 100) : 0);
    QString t = QStringLiteral("S/N %1 of %2").arg(ok ? QString::number(p.snr, 'f', 0) : QStringLiteral("—"))
                    .arg(p.target, 0, 'f', 0);
    if (std::isfinite(p.band)) t += QStringLiteral(" (band at %1 cm⁻¹)").arg(p.band, 0, 'f', 0);
    t += QStringLiteral(" · %1 frames · %2 of %3").arg(p.frames).arg(clock(p.elapsed), clock(p.maxSeconds));
    if (!p.finished && p.framesNeeded > p.frames)
        t += QStringLiteral(" · about %1 more frames").arg(p.framesNeeded - p.frames);
    m_status->setText(t);
    if (p.finished) {
        QString r = QStringLiteral("Finished: %1.").arg(p.stopText);
        if (p.damage)
            r += QStringLiteral(" <b style='color:%1'>%2</b>").arg(theme::warningText().name(), lr::acq::damageAdvice());
        if (p.frames > 0) r += QStringLiteral(" ") + muted(QStringLiteral("The spectrum is saved and shown on the Measure page."));
        m_result->setText(r);
    }
}

void SnrDialog::onBusy(bool busy) {
    if (!busy)
        m_running = false;
    m_start->setEnabled(!busy && m_ctl);
    m_stop->setEnabled(busy && m_running);
}

// ============================================================================================ RepeatDialog
RepeatDialog::RepeatDialog(Controller* ctl, MeasurePage* page, QWidget* parent)
    : QWidget(parent, Qt::Window), m_ctl(ctl), m_page(page) {
    setWindowTitle(QStringLiteral("Repeated measurements – LibreRaman"));
    resize(900, 680);
    auto* v = new QVBoxLayout(this);
    auto* intro = new QLabel(QStringLiteral(
        "A powder is not the same everywhere, and the laser sees one spot. Measure the vial several times, shaking "
        "or rotating it in between; you get each spectrum, their mean and spread, and the band results as mean ± sd. "
        "Each measurement uses the Measure page's settings (exposure, scans, sample name, folder)."));
    intro->setWordWrap(true);
    v->addWidget(intro);
    auto* row = new QHBoxLayout;
    m_count = new QSpinBox;
    m_count->setRange(2, 50);
    m_count->setValue(5);
    m_count->setSuffix(QStringLiteral(" measurements"));
    m_start = new QPushButton(QStringLiteral("Start"));
    m_start->setDefault(true);
    m_stop = new QPushButton(QStringLiteral("■  Stop"));
    m_stop->setToolTip(QStringLiteral("Laser off at once; the measurements so far are evaluated"));
    row->addWidget(new QLabel(QStringLiteral("Repeat")));
    row->addWidget(m_count);
    row->addWidget(m_start);
    row->addWidget(m_stop);
    row->addStretch();
    v->addLayout(row);

    m_prompt = new QFrame;
    m_prompt->setObjectName(QStringLiteral("vialPrompt"));
    m_prompt->setStyleSheet(QStringLiteral("QFrame#vialPrompt { background: #fff3cd; border-left: 4px solid #e0a800; }"
                                           " QFrame#vialPrompt QLabel { color: #3a3000; }"));
    auto* pl = new QHBoxLayout(m_prompt);
    m_promptText = new QLabel;
    m_promptText->setWordWrap(true);
    m_continue = new QPushButton(QStringLiteral("Continue"));
    m_finish = new QPushButton(QStringLiteral("Finish now"));
    m_finish->setToolTip(QStringLiteral("Evaluate the measurements made so far"));
    pl->addWidget(m_promptText, 1);
    pl->addWidget(m_continue);
    pl->addWidget(m_finish);
    m_prompt->setVisible(false);
    v->addWidget(m_prompt);
    m_status = new QLabel(QStringLiteral("—"));
    m_status->setWordWrap(true);
    v->addWidget(m_status);

    auto* split = new QSplitter(Qt::Vertical);
    m_table = new QTableWidget(0, 5);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Use"), QStringLiteral("#"), QStringLiteral("Time"),
                                        QStringLiteral("Shape distance"), QStringLiteral("Note")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setToolTip(QStringLiteral("<p>Untick a spectrum to leave it out of the mean and the band results (e.g. a "
                                       "burnt spot). <i>Shape distance</i>: difference from the median spectrum after "
                                       "scaling each to the same total, 0.1 = 10 %; flagged spectra differ much more "
                                       "than the others.</p>"));
    m_bands = new QTableWidget(0, 4);
    m_bands->setHorizontalHeaderLabels({QStringLiteral("Position (cm⁻¹)"), QStringLiteral("Height (counts)"),
                                        QStringLiteral("Ratio"), QStringLiteral("n")});
    m_bands->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_bands->verticalHeader()->setVisible(false);
    m_bands->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bands->setToolTip(QStringLiteral("<p>Strongest bands of the mean spectrum, measured in every included spectrum: "
                                       "mean ± standard deviation. Ratio: height relative to the strongest band in the "
                                       "same spectrum (independent of how well the powder is packed).</p>"));
    m_plot = new PlotWidget;
    m_plot->setMinimumHeight(180);
    split->addWidget(m_plot);
    split->addWidget(m_table);
    split->addWidget(m_bands);
    v->addWidget(split, 1);
    m_files = new QLabel;
    m_files->setWordWrap(true);
    m_files->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_files);

    connect(m_start, &QPushButton::clicked, this, &RepeatDialog::start);
    connect(m_continue, &QPushButton::clicked, this, &RepeatDialog::measureNext);
    connect(m_finish, &QPushButton::clicked, this, &RepeatDialog::finish);
    connect(m_stop, &QPushButton::clicked, this, [this] {
        if (m_ctl) m_ctl->stop();
    });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (!m_filling && it->column() == kUse)
            setExcluded(it->row(), it->checkState() != Qt::Checked);
    });
    if (m_ctl) {
        connect(m_ctl, &Controller::spectrum, this, &RepeatDialog::onSpectrum);
        connect(m_ctl, &Controller::busyChanged, this, &RepeatDialog::onBusy);
    }
    m_start->setEnabled(m_ctl);
    m_stop->setEnabled(false);
}

void RepeatDialog::start() {
    if (!m_ctl || m_running || (m_page && !m_page->prepareMeasurement()))
        return;
    m_settings = m_page ? m_page->measureSettings() : MeasureSettings{};
    m_total = m_count->value();
    m_group = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    m_spectra.clear();
    m_exclude.clear();
    m_result = {};
    m_meanPath.clear();
    m_summaryPath.clear();
    m_files->clear();
    m_running = true;
    fillTables();
    drawPlot();
    measureNext();
}

void RepeatDialog::measureNext() {
    if (!m_running)
        return;
    m_waiting = false;
    m_prompt->setVisible(false);
    MeasureSettings s = m_settings;
    const int k = int(m_spectra.size()) + 1;
    s.meta = {{QStringLiteral("repeat"), QStringLiteral("%1/%2").arg(k).arg(m_total)},
              {QStringLiteral("repeat_group"), m_group}};
    m_gotOne = false;
    m_status->setText(QStringLiteral("Measurement %1 of %2…").arg(k).arg(m_total));
    m_start->setEnabled(false);
    if (!m_ctl->measureOnce(s, QStringLiteral("GUI (repeat %1/%2)").arg(k).arg(m_total)))
        finish();
    else
        m_stop->setEnabled(true);
}

void RepeatDialog::onSpectrum(const lr::Spectrum& sp) {
    if (!m_running || sp.metaValue(QStringLiteral("repeat_group")) != m_group)
        return;
    m_spectra.push_back(sp);
    m_exclude.push_back(false);
    m_gotOne = true;
    fillTables();
    drawPlot();
}

void RepeatDialog::onBusy(bool busy) {
    m_stop->setEnabled(busy && m_running && !m_waiting);
    if (busy || !m_running || m_waiting)
        return;
    if (!m_gotOne) {                                   // failed or stopped: evaluate what there is
        finish();
        return;
    }
    if (int(m_spectra.size()) >= m_total) {
        finish();
        return;
    }
    m_waiting = true;
    m_promptText->setText(QStringLiteral("<b>Measurement %1 of %2 done.</b> Shake or rotate the vial, put it back and "
                                         "close the lid (press ARM again after opening), then press Continue.")
                              .arg(m_spectra.size())
                              .arg(m_total));
    m_prompt->setVisible(true);
    m_status->setText(QStringLiteral("Waiting for the vial…"));
    m_continue->setFocus();
}

void RepeatDialog::finish() {
    if (!m_running)
        return;
    m_running = false;
    m_waiting = false;
    m_prompt->setVisible(false);
    m_stop->setEnabled(false);
    m_start->setEnabled(m_ctl && !m_ctl->isBusy());
    if (m_ctl && m_ctl->isBusy())                      // "Finish now" while measuring: let it end
        m_ctl->stop();
    if (m_spectra.empty()) {
        m_status->setText(QStringLiteral("No measurements."));
        return;
    }
    analyseAndSave();
}

void RepeatDialog::setExcluded(int index, bool excluded) {
    if (index < 0 || index >= int(m_exclude.size()) || m_exclude[size_t(index)] == excluded)
        return;
    m_exclude[size_t(index)] = excluded;
    if (!m_running)
        analyseAndSave();
}

void RepeatDialog::analyseAndSave() {
    m_result = lr::repeats::analyse(m_spectra, m_exclude);
    fillTables();
    drawPlot();
    int flagged = 0;
    for (bool o : m_result.outlier) flagged += o;
    QString st = QStringLiteral("%1 measurements, %2 used.").arg(m_spectra.size()).arg(m_result.nIncluded);
    if (flagged)
        st += QStringLiteral(" <b style='color:%1'>%2 flagged as different from the others</b> (e.g. a burnt spot): "
                             "untick to leave out.")
                  .arg(theme::warningText().name())
                  .arg(flagged);
    if (m_result.totalIntensity.n > 1 && m_result.totalIntensity.mean > 0)
        st += QStringLiteral(" Overall intensity varies by %1 % (packing, focus).")
                  .arg(100 * m_result.totalIntensity.sd / m_result.totalIntensity.mean, 0, 'f', 0);
    m_status->setText(st);
    if (m_spectra.size() < 2 || m_result.nIncluded == 0) {
        m_files->setText(muted(QStringLiteral("Nothing to average: the mean and the summary are not saved.")));
        return;
    }
    try {
        const QString sample = m_settings.sample;
        const QDateTime t = m_spectra.back().t;
        if (m_meanPath.isEmpty()) {
            QDir().mkpath(m_settings.folder);
            m_meanPath = Controller::savePath(m_settings.folder, sample + QStringLiteral("_mean"), t);
            m_summaryPath = Controller::savePath(m_settings.folder, sample + QStringLiteral("_repeats"), t);
        }
        lr::Spectrum mean = lr::repeats::meanSpectrum(m_spectra, m_result);
        mean.saveCsv(m_meanPath);
        lr::repeats::writeSummary(m_summaryPath, m_spectra, m_result, sample);
        m_files->setText(muted(QStringLiteral("Saved: %1 (mean) and %2 (mean, sd, every spectrum, band results)")
                                   .arg(QFileInfo(m_meanPath).fileName(), QFileInfo(m_summaryPath).fileName())));
    } catch (const std::exception& e) {
        m_files->setText(QStringLiteral("<b style='color:%1'>Could not save: %2</b>")
                             .arg(theme::warningText().name(), QString::fromUtf8(e.what()).toHtmlEscaped()));
    }
}

void RepeatDialog::fillTables() {
    m_filling = true;
    const bool done = !m_running && !m_result.included.empty();
    m_table->setRowCount(int(m_spectra.size()));
    for (int k = 0; k < int(m_spectra.size()); ++k) {
        auto* use = new QTableWidgetItem;
        use->setFlags(done ? Qt::ItemIsUserCheckable | Qt::ItemIsEnabled : Qt::ItemIsUserCheckable);
        use->setCheckState(m_exclude[size_t(k)] ? Qt::Unchecked : Qt::Checked);
        m_table->setItem(k, kUse, use);
        m_table->setItem(k, 1, new QTableWidgetItem(QString::number(k + 1)));
        m_table->setItem(k, 2, new QTableWidgetItem(m_spectra[size_t(k)].t.toString(QStringLiteral("HH:mm:ss"))));
        const bool have = done && size_t(k) < m_result.distance.size() && std::isfinite(m_result.distance[size_t(k)]);
        m_table->setItem(k, 3, new QTableWidgetItem(have ? QString::number(m_result.distance[size_t(k)], 'f', 3) : QStringLiteral("—")));
        QString note;
        if (done && m_result.outlier[size_t(k)]) note = QStringLiteral("differs from the others");
        if (m_spectra[size_t(k)].metaValue(QStringLiteral("laser_damage")) == QLatin1String("suspected"))
            note += (note.isEmpty() ? QString() : QStringLiteral("; ")) + QStringLiteral("changed under the laser");
        auto* n = new QTableWidgetItem(note);
        if (!note.isEmpty()) n->setForeground(theme::warningText());
        m_table->setItem(k, 4, n);
    }
    m_table->resizeColumnsToContents();
    const auto& b = m_result.bands;
    m_bands->setRowCount(done ? int(b.size()) : 0);
    if (done && m_result.reference >= 0)
        m_bands->setHorizontalHeaderItem(2, new QTableWidgetItem(
            QStringLiteral("Ratio to %1 cm⁻¹").arg(b[size_t(m_result.reference)].nominal, 0, 'f', 0)));
    for (int i = 0; done && i < int(b.size()); ++i) {
        const auto& s = b[size_t(i)];
        m_bands->setItem(i, 0, new QTableWidgetItem(s.position.text(1)));
        QString h = s.height.text(0);
        if (s.height.n > 1 && s.height.mean > 0) h += QStringLiteral(" (%1 %)").arg(100 * s.height.sd / s.height.mean, 0, 'f', 0);
        m_bands->setItem(i, 1, new QTableWidgetItem(h));
        m_bands->setItem(i, 2, new QTableWidgetItem(s.ratio.text(3)));
        m_bands->setItem(i, 3, new QTableWidgetItem(QString::number(s.position.n)));
    }
    m_filling = false;
}

void RepeatDialog::drawPlot() {
    m_plot->clear();
    const PlotPalette& pal = PlotPalette::defaultPalette();
    const bool done = !m_running && !m_result.included.empty();
    for (size_t k = 0; k < m_spectra.size(); ++k) {
        QPen pen(done && m_result.outlier[k] ? theme::warningText() : pal.seriesColor(int(k)));
        pen.setWidthF(1.0);
        if (m_exclude[k]) pen.setStyle(Qt::DotLine);
        Curve* c = m_plot->addCurve(m_spectra[k].shift, m_spectra[k].intensity, pen, QStringLiteral("%1").arg(k + 1));
        c->setOpacity(m_exclude[k] ? 0.35 : 0.7);
    }
    if (done && !m_result.mean.empty()) {
        QPen pen(pal.isDark() ? Qt::white : Qt::black);
        pen.setWidthF(2.0);
        m_plot->addCurve(m_result.x, m_result.mean, pen, QStringLiteral("mean"));
    }
    m_plot->setXRange(150, 3500);
}

}  // namespace lr::gui
