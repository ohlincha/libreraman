#include "measurepage.h"

#include "plotwidget.h"
#include "statusstrip.h"
#include "theme.h"

#include "core/peaks.h"
#include "core/processing.h"
#include "core/pyformat.h"
#include "core/response.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTimeEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lr::gui {

namespace {
constexpr char kSampleKey[] = "measure/sample";   // last sample name (#7)

QFormLayout* form(QWidget* parent) {                  // Python form()
    auto* f = new QFormLayout(parent);
    f->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    f->setRowWrapPolicy(QFormLayout::DontWrapRows);
    f->setHorizontalSpacing(8);
    return f;
}

QGroupBox* box(const QString& title) {                // Python box()
    auto* g = new QGroupBox(title);
    g->setStyleSheet(theme::groupBoxStyle());
    return g;
}

QDoubleSpinBox* doubleBox(int decimals, double lo, double hi, double value, const QString& suffix = {}) {
    auto* b = new QDoubleSpinBox;
    b->setDecimals(decimals);
    b->setRange(lo, hi);
    b->setValue(value);
    b->setSuffix(suffix);
    return b;
}

QSpinBox* intBox(int lo, int hi, int value) {
    auto* b = new QSpinBox;
    b->setRange(lo, hi);
    b->setValue(value);
    return b;
}

double unitSeconds(const QString& unit) {
    if (unit == QLatin1String("min"))
        return 60;
    if (unit == QLatin1String("h"))
        return 3600;
    return 1;
}

std::size_t nearestIndex(const Vec& x, double v) {    // np.argmin(np.abs(x - v))
    std::size_t best = 0;
    double d = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < x.size(); ++i)
        if (std::abs(x[i] - v) < d) {
            d = std::abs(x[i] - v);
            best = i;
        }
    return best;
}

// The processing chain on a measured spectrum; with s.intensity first the instrument's intensity correction.
std::pair<Vec, Vec> processSafe(const lr::Spectrum& sp, const lr::processing::Settings& s) {
    const Vec& x = sp.shift;
    const Vec& y = sp.intensity;
    try {
        if (s.intensity)
            return lr::processing::process(
                x, lr::response::corrected(x, y, sp.metaValue(QLatin1String(lr::response::kSerialKey)), sp.channel), s);
        return lr::processing::process(x, y, s);
    } catch (const std::exception&) {              // e.g. too few points for the smoothing window
        return {y, Vec(y.size(), 0.0)};
    }
}

QString cm1(double v) { return QStringLiteral("%1 cm⁻¹").arg(v, 0, 'f', 0); }

}  // namespace

// [p.position for p in pick(x, y, snr=8, xmin=150)][:12], on the useful range only (the noise level is then
// also estimated there, not on the junk beyond the detector range).
Vec MeasurePage::pickPeaks(const Vec& x, const Vec& y) {
    Vec xs, ys;
    const std::size_t n = std::min(x.size(), y.size());
    for (std::size_t i = 0; i < n; ++i)
        if (x[i] >= kUsefulMin && x[i] <= kUsefulMax && std::isfinite(y[i])) {
            xs.push_back(x[i]);
            ys.push_back(y[i]);
        }
    Vec out;
    if (xs.size() < 16)
        return out;
    try {
        for (const auto& p : lr::peaks::pick(xs, ys, 8.0, 2.0, false, kPeakMin, kUsefulMax)) {
            if (out.size() >= 12)
                break;
            out.push_back(p.position);
        }
    } catch (const std::exception&) {
    }
    return out;
}

// ============================================================================================ construction
MeasurePage::MeasurePage(Controller* ctl, QWidget* parent) : Page(parent), m_ctl(ctl) {
    setController(ctl);
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(6, 2, 6, 4);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(4);

    // live spectrum
    m_plot = new PlotWidget;
    m_plot->setToolTip(PlotWidget::navigationHelp());
    m_plot->setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    m_plot->setLabel(Axis::Left, QStringLiteral("Intensity (counts)"));
    m_plot->setGrid(true, true);
    m_plot->setHomeXRange(150, 3500);
    m_plot->setXRange(150, 3500);
    m_plot->setHeadroom(0.18);
    const PlotPalette& pal = m_plot->plotPalette();
    for (int i = 0; i < kFaded; ++i) {                // faded previous spectra, oldest most transparent
        QColor c = pal.previous;
        c.setAlpha(35 + 60 * i / std::max(1, kFaded - 1));
        Curve* old = m_plot->addCurve(QPen(c, 1));
        old->setAutoScale(false);
        old->setZValue(-1);
        m_oldCurves.push_back(old);
    }
    m_baseCurve = m_plot->addCurve(QPen(pal.baseline, 1.2, Qt::DashLine));
    m_baseCurve->setAutoScale(false);
    m_curve = m_plot->addCurve(QPen(pal.main, 1.6));
    m_plot->setLegendVisible(true);                     // names: sample + time (which trace is which)
    m_peakLabels = m_plot->addPeakLabels();
    connect(m_plot, &PlotWidget::rightClicked, this, &MeasurePage::spectrumMenu);
    connect(m_plot, &PlotWidget::autoRangeRequested, this, &MeasurePage::redraw);
    grid->addWidget(m_plot, 0, 0, 1, 3);

    // series box
    auto* sb = box(QStringLiteral("Series"));
    QFormLayout* f = form(sb);
    m_lblN = new QLabel(QStringLiteral("—"));
    m_lblNext = new QLabel(QStringLiteral("—"));
    m_lblElapsed = new QLabel(QStringLiteral("—"));
    m_lblEnd = new QLabel(QStringLiteral("—"));
    m_bar = new QProgressBar;
    m_bar->setValue(0);
    f->addRow(QStringLiteral("Spectrum"), m_lblN);
    f->addRow(QStringLiteral("Next in"), m_lblNext);
    f->addRow(QStringLiteral("Elapsed"), m_lblElapsed);
    f->addRow(QStringLiteral("Ends"), m_lblEnd);
    f->addRow(m_bar);
    grid->addWidget(sb, 1, 0);

    // trend
    auto* tb = box(QStringLiteral("Trend"));
    auto* tl = new QVBoxLayout(tb);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("band at")));
    m_band = intBox(100, 4000, 1000);
    m_band->setSuffix(QStringLiteral(" cm⁻¹"));
    m_band->setToolTip(QStringLiteral("<p>Band followed in the trend. Set automatically to the tallest peak of the "
                                      "first spectrum; change it here, pick a peak, or right-click in the "
                                      "spectrum.</p>"));
    connect(m_band, &QSpinBox::valueChanged, this, &MeasurePage::recomputeTrend);
    connect(m_band, &QSpinBox::editingFinished, this, [this] { m_bandAuto = false; });
    row->addWidget(m_band);
    m_peakBtn = new QToolButton;
    m_peakBtn->setText(QStringLiteral("peaks ▾"));
    m_peakBtn->setPopupMode(QToolButton::InstantPopup);
    m_peakMenu = new QMenu(m_peakBtn);
    m_peakBtn->setMenu(m_peakMenu);
    connect(m_peakMenu, &QMenu::aboutToShow, this, &MeasurePage::fillPeakMenu);
    row->addWidget(m_peakBtn);
    m_trendMode = new QComboBox;
    m_trendMode->addItems({QStringLiteral("trend"), QStringLiteral("waterfall")});
    connect(m_trendMode, &QComboBox::currentIndexChanged, this, [this] {
        m_tplot->setAutoX(true);
        m_tplot->setAutoY(true);
        redrawTrend();
    });
    row->addWidget(m_trendMode);
    row->addStretch();
    tl->addLayout(row);
    m_tplot = new PlotWidget;
    m_tplot->setToolTip(PlotWidget::navigationHelp());
    m_tplot->setMinimumHeight(90);
    m_tplot->setGrid(true, true);
    m_tplot->setHeadroom(0.08);
    tl->addWidget(m_tplot);
    grid->addWidget(tb, 1, 1, 1, 2);

    // detector + last spectrum
    auto* db = box(QStringLiteral("Detector"));
    f = form(db);
    m_fill = new QProgressBar;
    m_fill->setTextVisible(false);                    // (not drawn by every style, e.g. macOS: own label)
    m_fill->setValue(0);
    m_fill->setMaximumHeight(16);
    m_fillPct = new QLabel(QStringLiteral("—"));
    m_fillPct->setMinimumWidth(QFontMetrics(m_fillPct->font()).horizontalAdvance(QStringLiteral("100 %  ")));
    m_fillPct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_fillPct->setToolTip(QStringLiteral("Highest raw signal in % of the detector maximum (65535 counts)"));
    auto* fillRow = new QHBoxLayout;
    fillRow->addWidget(m_fill, 1);
    fillRow->addWidget(m_fillPct);
    m_lblDark = new QLabel(QStringLiteral("—"));
    m_lblSat = new QLabel(QStringLiteral("—"));
    m_lblFluor = new QLabel(QStringLiteral("—"));
    m_lblFluor->setWordWrap(true);
    f->addRow(QStringLiteral("Signal"), fillRow);
    f->addRow(QStringLiteral("Dark level"), m_lblDark);
    f->addRow(QStringLiteral("Saturation"), m_lblSat);
    f->addRow(QStringLiteral("Fluorescence"), m_lblFluor);
    grid->addWidget(db, 2, 0);
    auto* lb = box(QStringLiteral("Last spectrum"));
    f = form(lb);
    m_lblTime = new QLabel(QStringLiteral("—"));
    m_lblSet = new QLabel(QStringLiteral("—"));
    m_lblPeaks = new QLabel(QStringLiteral("—"));
    m_lblPeaks->setWordWrap(true);
    m_moreBtn = new QToolButton;
    m_moreBtn->setText(QStringLiteral("⋯"));
    m_moreBtn->setToolTip(QStringLiteral("Add to library…, Clear (also in the plot's right-click menu)"));
    m_moreBtn->setPopupMode(QToolButton::InstantPopup);
    m_moreBtn->setAutoRaise(true);
    m_moreBtn->setStyleSheet(QStringLiteral("QToolButton { font-size: 13pt; font-weight: bold; padding: 0 6px; }"
                                            " QToolButton::menu-indicator { image: none; width: 0px; }"));
    m_moreMenu = new QMenu(m_moreBtn);
    m_moreBtn->setMenu(m_moreMenu);
    connect(m_moreMenu, &QMenu::aboutToShow, this, [this] {
        m_moreMenu->clear();
        fillMoreMenu(m_moreMenu);
    });
    fillMoreMenu(m_moreMenu);
    auto* timeRow = new QHBoxLayout;
    timeRow->setContentsMargins(0, 0, 0, 0);
    timeRow->addWidget(m_lblTime, 1);
    timeRow->addWidget(m_moreBtn);
    f->addRow(QStringLiteral("Time"), timeRow);
    f->addRow(QStringLiteral("Settings"), m_lblSet);
    f->addRow(QStringLiteral("Peaks (cm⁻¹)"), m_lblPeaks);
    grid->addWidget(lb, 2, 1, 1, 2);

    grid->addWidget(buildSettingsPanel(), 0, 3, 3, 1);
    grid->setColumnStretch(0, 2);
    grid->setColumnStretch(1, 2);
    grid->setColumnStretch(2, 1);
    grid->setColumnStretch(3, 0);
    grid->setRowStretch(0, 5);
    grid->setRowStretch(1, 2);
    grid->setRowStretch(2, 1);

    if (m_ctl) {
        connect(m_ctl, &Controller::seriesProgress, this, &MeasurePage::onSeriesProgress);
        connect(m_ctl, &Controller::busyChanged, this, &MeasurePage::setBusy);
    }
    setFolder(defaultFolder());
    m_folderAuto = true;
    m_redrawTimer = new QTimer(this);
    m_redrawTimer->setSingleShot(true);
    connect(m_redrawTimer, &QTimer::timeout, this, &MeasurePage::flushRedraw);
    m_sinceRedraw.start();
    updateMinInterval();
    redrawTrend();
}

QWidget* MeasurePage::buildSettingsPanel() {
    auto* outer = new QWidget;
    outer->setFixedWidth(380);
    auto* ov = new QVBoxLayout(outer);
    ov->setContentsMargins(0, 0, 0, 0);
    ov->setSpacing(4);
    m_btnOnce = new QPushButton(QStringLiteral("▶  MEASURE"));
    m_btnSeries = new QPushButton(QStringLiteral("⟳  START SERIES"));
    m_btnStop = new QPushButton(QStringLiteral("■  Stop"));
    m_btnPause = new QPushButton(QStringLiteral("❚❚  Pause"));
    m_btnOnce->setStyleSheet(theme::bigButtonStyle(theme::Action::Measure));
    m_btnSeries->setStyleSheet(theme::bigButtonStyle(theme::Action::Series));
    for (QPushButton* b : {m_btnOnce, m_btnSeries})
        b->setMinimumHeight(46);
    for (QPushButton* b : {m_btnStop, m_btnPause})
        b->setMinimumHeight(28);
    m_btnOnce->setToolTip(QStringLiteral("<p>Take one spectrum with the settings below (laser on only while "
                                         "measuring)</p>"));
    m_btnSeries->setToolTip(QStringLiteral("Start the time series set up below"));
    m_btnStop->setToolTip(QStringLiteral("Stop the measurement; the laser goes off at once"));
    m_btnPause->setToolTip(QStringLiteral("<p>Pause the series before its next spectrum (the laser stays off); "
                                          "press again to continue</p>"));
    m_btnPause->setCheckable(true);
    m_btnClear = new QPushButton(QStringLiteral("Clear"));
    m_btnClear->setMinimumHeight(46);
    m_btnClear->setToolTip(QStringLiteral("Clear the plot, the trend and the panels below (saved files are kept)"));
    connect(m_btnClear, &QPushButton::clicked, this, &MeasurePage::clearAll);
    connect(m_btnOnce, &QPushButton::clicked, this, &MeasurePage::measureOnce);
    connect(m_btnSeries, &QPushButton::clicked, this, &MeasurePage::startSeries);
    connect(m_btnStop, &QPushButton::clicked, this, [this] {
        if (m_ctl)
            m_ctl->stop();
    });
    connect(m_btnPause, &QPushButton::toggled, this, [this](bool on) {
        m_btnPause->setText(on ? QStringLiteral("▶  Resume") : QStringLiteral("❚❚  Pause"));
        if (m_ctl)
            m_ctl->setPaused(on);
    });
    ov->addWidget(m_btnOnce);
    auto* seriesRow = new QHBoxLayout;          // START SERIES at half width, Clear beside it
    seriesRow->addWidget(m_btnSeries, 1);
    seriesRow->addWidget(m_btnClear, 1);
    ov->addLayout(seriesRow);
    auto* stopRow = new QHBoxLayout;
    stopRow->addWidget(m_btnStop, 2);
    stopRow->addWidget(m_btnPause, 1);
    ov->addLayout(stopRow);

    auto* scroll = m_settingsScroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* w = new QWidget;
    scroll->setWidget(w);
    ov->addWidget(scroll, 1);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 8, 0);
    v->setSpacing(2);

    // ---- sample
    QGroupBox* s = box(QStringLiteral("Sample"));
    auto* g = new QGridLayout(s);
    g->setHorizontalSpacing(6);
    g->setVerticalSpacing(4);
    m_sample = new QLineEdit(QSettings().value(QLatin1String(kSampleKey)).toString());
    m_sample->setPlaceholderText(QStringLiteral("sample name (used in file names)"));
    connect(m_sample, &QLineEdit::editingFinished, this, [this] {
        const QString n = m_sample->text().trimmed();
        if (!n.isEmpty()) QSettings().setValue(QLatin1String(kSampleKey), n);
    });
    m_folderLabel = new ElidedLabel;
    m_folderLabel->setStyleSheet(QStringLiteral("color: %1;").arg(theme::mutedText().name()));
    auto* change = new QToolButton;
    change->setText(QStringLiteral("Change…"));
    change->setToolTip(QStringLiteral("Folder for the spectra (default: a new folder for each day)"));
    connect(change, &QToolButton::clicked, this, &MeasurePage::browse);
    g->addWidget(new QLabel(QStringLiteral("Name")), 0, 0);
    g->addWidget(m_sample, 0, 1, 1, 2);
    g->addWidget(new QLabel(QStringLiteral("Saved in")), 1, 0);
    g->addWidget(m_folderLabel, 1, 1);
    g->addWidget(change, 1, 2);
    g->setColumnStretch(1, 1);
    v->addWidget(s);

    // ---- spectrum (two compact columns)
    s = box(QStringLiteral("Spectrum"));
    g = new QGridLayout(s);
    g->setHorizontalSpacing(6);
    g->setVerticalSpacing(4);
    m_exposure = doubleBox(2, 0.01, 600, 1.0, QStringLiteral(" s"));
    m_averages = intBox(1, 10000, 3);
    m_averages->setToolTip(QStringLiteral("Number of scans averaged into one spectrum"));
    m_acq = doubleBox(1, 0.01, 36000, 3.0, QStringLiteral(" s"));
    m_acq->setToolTip(QStringLiteral("<p>Total acquisition time = exposure × scans. Change it and the number of "
                                     "scans follows.</p>"));
    m_prebleach = doubleBox(0, 0, 3600, 0, QStringLiteral(" s"));
    m_prebleach->setToolTip(QStringLiteral("<p>Laser on for this long before measuring: photobleaching reduces "
                                           "fluorescence background. Not counted as acquisition.</p>"));
    auto fromScans = [this] {
        if (m_sync)
            return;
        m_sync = true;
        m_acq->setValue(m_exposure->value() * m_averages->value());
        m_sync = false;
    };
    auto fromAcq = [this] {
        if (m_sync)
            return;
        m_sync = true;
        m_averages->setValue(std::max(1, int(std::lround(m_acq->value() / m_exposure->value()))));
        m_sync = false;
    };
    connect(m_exposure, &QDoubleSpinBox::valueChanged, this, fromScans);
    connect(m_averages, &QSpinBox::valueChanged, this, fromScans);
    connect(m_acq, &QDoubleSpinBox::valueChanged, this, fromAcq);
    m_background = new QComboBox;
    m_background->addItems({QStringLiteral("each spectrum"), QStringLiteral("once (series)"), QStringLiteral("none")});
    m_background->setToolTip(QStringLiteral("<p>Background (laser off) frames subtracted from the spectrum: taken "
                                            "with each spectrum, once at the start of a series, or not at all.</p>"));
    m_channel = new QComboBox;
    m_channel->addItems({QStringLiteral("532 nm"), QStringLiteral("1064 nm")});
    g->addWidget(new QLabel(QStringLiteral("Exposure")), 0, 0);
    g->addWidget(m_exposure, 0, 1);
    g->addWidget(new QLabel(QStringLiteral("Scans")), 0, 2);
    g->addWidget(m_averages, 0, 3);
    g->addWidget(new QLabel(QStringLiteral("Acquisition")), 1, 0);
    g->addWidget(m_acq, 1, 1);
    g->addWidget(new QLabel(QStringLiteral("Pre-bleach")), 1, 2);
    g->addWidget(m_prebleach, 1, 3);
    g->addWidget(new QLabel(QStringLiteral("Background")), 2, 0);
    g->addWidget(m_background, 2, 1, 1, 3);
    g->addWidget(new QLabel(QStringLiteral("Laser")), 3, 0);
    g->addWidget(m_channel, 3, 1, 1, 3);
    g->setColumnStretch(1, 1);
    g->setColumnStretch(3, 1);
    v->addWidget(s);

    // ---- time series
    s = box(QStringLiteral("Time series"));
    QFormLayout* fl = form(s);
    m_seriesOn = new QCheckBox(QStringLiteral("measure repeatedly"));
    m_seriesOn->setChecked(true);
    m_seriesOn->setToolTip(QStringLiteral("Enables START SERIES and the settings below"));
    m_interval = doubleBox(1, 0.1, 100000, 5);
    m_intervalUnit = new QComboBox;
    m_intervalUnit->addItems({QStringLiteral("s"), QStringLiteral("min"), QStringLiteral("h")});
    m_intervalUnit->setCurrentText(QStringLiteral("min"));
    auto* irow = new QHBoxLayout;
    irow->addWidget(m_interval, 1);
    irow->addWidget(m_intervalUnit);
    m_stopBy = new QComboBox;
    m_stopBy->addItems({QStringLiteral("number of spectra"), QStringLiteral("duration (h)")});
    m_count = intBox(1, 100000, 60);
    m_duration = doubleBox(1, 0.01, 1000, 5, QStringLiteral(" h"));
    m_start = new QComboBox;
    m_start->addItems({QStringLiteral("now"), QStringLiteral("in 1 min"), QStringLiteral("in 10 min"),
                       QStringLiteral("in 1 h"), QStringLiteral("at")});
    m_startAt = new QTimeEdit(QTime::currentTime().addSecs(3600));
    m_startAt->setDisplayFormat(QStringLiteral("HH:mm"));
    m_startAt->setToolTip(QStringLiteral("Start time (today, or tomorrow if it has passed)"));
    auto* srow = new QHBoxLayout;
    srow->addWidget(m_start, 1);
    srow->addWidget(m_startAt);
    m_fastest = new QCheckBox(QStringLiteral("as fast as possible (laser stays on)"));
    m_fastest->setToolTip(QStringLiteral("<p>Back-to-back spectra with the laser on throughout; the background "
                                         "is taken once at the start.</p>"));
    m_minLabel = new QLabel;
    fl->addRow(m_seriesOn);
    fl->addRow(m_fastest);
    fl->addRow(QStringLiteral("Every"), irow);
    fl->addRow(QString(), m_minLabel);
    fl->addRow(QStringLiteral("Stop after"), m_stopBy);
    fl->addRow(QStringLiteral("Spectra"), m_count);
    fl->addRow(QStringLiteral("Duration"), m_duration);
    fl->addRow(QStringLiteral("Start"), srow);
    connect(m_exposure, &QDoubleSpinBox::valueChanged, this, &MeasurePage::updateMinInterval);
    connect(m_averages, &QSpinBox::valueChanged, this, &MeasurePage::updateMinInterval);
    connect(m_interval, &QDoubleSpinBox::valueChanged, this, &MeasurePage::updateMinInterval);
    connect(m_prebleach, &QDoubleSpinBox::valueChanged, this, &MeasurePage::updateMinInterval);
    connect(m_background, &QComboBox::currentIndexChanged, this, &MeasurePage::updateMinInterval);
    connect(m_intervalUnit, &QComboBox::currentIndexChanged, this, &MeasurePage::updateMinInterval);
    connect(m_fastest, &QCheckBox::toggled, this, &MeasurePage::updateMinInterval);
    connect(m_seriesOn, &QCheckBox::toggled, this, &MeasurePage::updateEnabled);
    connect(m_stopBy, &QComboBox::currentIndexChanged, this, &MeasurePage::updateEnabled);
    connect(m_start, &QComboBox::currentIndexChanged, this, &MeasurePage::updateEnabled);
    v->addWidget(s);

    // ---- processing (collapsed by default: the defaults suit most measurements, and the column must fit
    // low screens without scrolling)
    m_procToggle = new QToolButton;
    m_procToggle->setText(QStringLiteral("Processing"));
    m_procToggle->setCheckable(true);
    m_procToggle->setChecked(false);
    m_procToggle->setArrowType(Qt::RightArrow);
    m_procToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_procToggle->setAutoRaise(true);
    m_procToggle->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; border: none; }"));
    m_procToggle->setToolTip(QStringLiteral("Baseline, cosmic rays, smoothing, normalisation, what is shown"));
    v->addWidget(m_procToggle);
    s = box(QString());
    m_procBody = s;
    s->setVisible(false);
    connect(m_procToggle, &QToolButton::toggled, this, [this](bool on) {
        m_procToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        m_procBody->setVisible(on);
    });
    fl = form(s);
    m_baseline = new QComboBox;
    int snip = 0;
    for (const auto& [key, desc] : lr::processing::baselines()) {
        if (key == QLatin1String("snip4"))
            snip = m_baseline->count();
        m_baseline->addItem(desc, key);
    }
    m_baseline->setCurrentIndex(snip);
    m_baseline->setToolTip(QStringLiteral("<p>SNIP 4th order (default) follows curved fluorescence backgrounds (ZnS, "
                                          "minerals); classic SNIP suits flat backgrounds; polynomial for single "
                                          "spectra; AsLS/arPLS/airPLS when you are willing to adjust the strength. "
                                          "Baselines are fitted above 150 cm⁻¹ only.</p>"));
    m_normalise = new QComboBox;
    m_normalise->addItems({QStringLiteral("none"), QStringLiteral("max"), QStringLiteral("area")});
    m_derivative = new QComboBox;
    m_derivative->addItems({QStringLiteral("none"), QStringLiteral("1st"), QStringLiteral("2nd")});
    m_derivative->setToolTip(QStringLiteral("Savitzky–Golay derivative (after baseline correction)"));
    m_strength = new QSlider(Qt::Horizontal);
    m_strength->setRange(0, 100);
    m_strength->setValue(50);
    m_strength->setToolTip(QStringLiteral("Baseline stiffness: larger = smoother baseline"));
    m_despike = new QCheckBox(QStringLiteral("remove cosmic rays"));
    m_despike->setChecked(true);
    m_smoothing = new QCheckBox(QStringLiteral("smooth (Savitzky–Golay)"));
    m_intensity = new QCheckBox(QStringLiteral("intensity correction"));
    m_intensity->setToolTip(QStringLiteral("<p>Correct the instrument's spectral sensitivity with the intensity "
                                           "response measured on the Calibration page (tab <i>Intensity</i>). "
                                           "Without one for this instrument nothing changes.</p>"));
    m_showRaw = new QRadioButton(QStringLiteral("raw"));
    m_showCor = new QRadioButton(QStringLiteral("corrected"));
    m_showBoth = new QRadioButton(QStringLiteral("both"));
    m_showBoth->setChecked(true);
    auto* grp = new QButtonGroup(this);
    auto* shrow = new QHBoxLayout;
    for (QRadioButton* r : {m_showRaw, m_showCor, m_showBoth}) {
        grp->addButton(r);
        shrow->addWidget(r);
        connect(r, &QRadioButton::toggled, this, [this](bool on) {
            if (on)
                redraw();
        });
    }
    connect(m_baseline, &QComboBox::currentIndexChanged, this, &MeasurePage::processingChanged);
    connect(m_strength, &QSlider::valueChanged, this, &MeasurePage::processingChanged);
    connect(m_despike, &QCheckBox::toggled, this, &MeasurePage::processingChanged);
    connect(m_smoothing, &QCheckBox::toggled, this, &MeasurePage::processingChanged);
    connect(m_intensity, &QCheckBox::toggled, this, &MeasurePage::processingChanged);
    connect(m_normalise, &QComboBox::currentIndexChanged, this, &MeasurePage::processingChanged);
    connect(m_derivative, &QComboBox::currentIndexChanged, this, &MeasurePage::processingChanged);
    fl->addRow(QStringLiteral("Baseline"), m_baseline);
    fl->addRow(QStringLiteral("Strength"), m_strength);
    fl->addRow(m_despike);
    fl->addRow(m_smoothing);
    fl->addRow(m_intensity);
    fl->addRow(QStringLiteral("Normalise"), m_normalise);
    fl->addRow(QStringLiteral("Derivative"), m_derivative);
    fl->addRow(QStringLiteral("Show"), shrow);
    v->addWidget(s);

    for (QGroupBox* gb : w->findChildren<QGroupBox*>()) {   // compact boxes
        if (QLayout* l = gb->layout()) {
            l->setContentsMargins(8, 4, 8, 6);
            l->setSpacing(4);
        }
    }
    v->addStretch();
    for (QComboBox* cb : w->findChildren<QComboBox*>()) {   // long entries must not force the panel wider
        cb->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        cb->setMinimumContentsLength(8);
    }
    for (QLineEdit* le : w->findChildren<QLineEdit*>())
        le->setMinimumWidth(60);
    m_intervalUnit->setMinimumContentsLength(3);
    m_start->setMinimumContentsLength(6);
    updateEnabled();
    return outer;
}

// ============================================================================================ settings
QString MeasurePage::defaultFolder(const QDate& day) {
    return QDir(QDir::homePath()).filePath(QStringLiteral("Documents/LibreRamen/") +
                                           day.toString(QStringLiteral("yyyy-MM-dd")));
}

void MeasurePage::setFolder(const QString& path) {
    m_folder = path;
    m_folderAuto = false;
    showFolder();
}

void MeasurePage::showFolder() {
    const QString home = QDir::homePath();
    QString shown = QDir::toNativeSeparators(m_folder);
    if (m_folder.startsWith(home))
        shown = QStringLiteral("~") + QDir::toNativeSeparators(m_folder.mid(home.size()));
    m_folderLabel->setFullText(shown);
    m_folderLabel->setToolTip(QDir::toNativeSeparators(m_folder));
}

void MeasurePage::resizeEvent(QResizeEvent* e) {
    Page::resizeEvent(e);
    showFolder();
}

void MeasurePage::browse() {
    const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Data folder"), m_folder);
    if (!d.isEmpty())
        setFolder(d);
}

int MeasurePage::channel() const { return m_channel->currentIndex() == 1 ? 1064 : 532; }

MeasureSettings MeasurePage::measureSettings() const {
    MeasureSettings s;
    s.sample = m_sample->text().trimmed();
    if (s.sample.isEmpty())
        s.sample = QStringLiteral("sample");
    s.folder = m_folder;
    s.exposure_s = m_exposure->value();
    s.averages = m_averages->value();
    s.background = m_background->currentIndex() == 2 ? QStringLiteral("none") : QStringLiteral("each");
    s.prebleach_s = m_prebleach->value();
    s.channel = channel();
    return s;
}

double MeasurePage::startDelay() const {
    switch (m_start->currentIndex()) {
    case 1: return 60;
    case 2: return 600;
    case 3: return 3600;
    case 4: {
        const QDateTime now = QDateTime::currentDateTime();
        QDateTime at(now.date(), m_startAt->time());
        if (at <= now)
            at = at.addDays(1);
        return double(now.msecsTo(at)) / 1000.0;
    }
    default: return 0;
    }
}

lr::SeriesSettings MeasurePage::seriesSettings() const {
    const MeasureSettings m = measureSettings();
    lr::SeriesSettings s;
    s.sample = m.sample;
    s.folder = m_folder;
    s.exposure_s = m.exposure_s;
    s.averages = m.averages;
    static const char* const bg[] = {"each", "once", "none"};
    s.background = QString::fromLatin1(bg[std::clamp(m_background->currentIndex(), 0, 2)]);
    s.prebleach_s = m.prebleach_s;
    s.interval_s = m_interval->value() * unitSeconds(m_intervalUnit->currentText());
    s.stopByCount = m_stopBy->currentIndex() == 0;
    s.count = m_count->value();
    s.duration_s = m_duration->value() * 3600;
    s.start_delay_s = startDelay();
    s.laserStaysOn = m_fastest->isChecked();
    return s;
}

lr::processing::Settings MeasurePage::processingSettings() const {
    lr::processing::Settings s;
    s.baseline = m_baseline->currentData().toString();
    s.strength = m_strength->value() / 10.0;
    s.despike = m_despike->isChecked();
    s.smooth = m_smoothing->isChecked();
    s.intensity = m_intensity->isChecked();
    s.normalize = m_normalise->currentText();
    s.derivative = m_derivative->currentIndex();
    return s;
}

double MeasurePage::minInterval() const {
    const bool fast = m_fastest->isChecked();
    const bool bg = m_background->currentIndex() == 0 && !fast;
    const double settle = m_ctl ? m_ctl->laserSettleS() : 3.6;
    const int frames = m_averages->value() * (bg ? 2 : 1);
    const double bleach = fast ? 0.0 : m_prebleach->value();
    return (fast ? 0.0 : settle) + bleach + frames * (m_exposure->value() + 0.02) + 0.5;
}

void MeasurePage::updateMinInterval() {
    const double mi = minInterval();
    const bool fast = m_fastest->isChecked();
    const QString muted = QStringLiteral("color: %1; font-size: 9pt;").arg(theme::mutedText().name());
    updateEnabled();
    if (fast) {
        m_minLabel->setText(QStringLiteral("one spectrum about every %1 s").arg(mi, 0, 'f', 1));
        m_minLabel->setStyleSheet(muted);
        m_interval->setStyleSheet(QString());
        return;
    }
    const bool tooShort = m_interval->value() * unitSeconds(m_intervalUnit->currentText()) < mi;
    m_minLabel->setText(QStringLiteral("minimum ≈ %1 s per spectrum").arg(mi, 0, 'f', 1) +
                        (tooShort ? QStringLiteral("  — interval too short!") : QString()));
    m_minLabel->setStyleSheet(tooShort ? QStringLiteral("color: %1; font-size: 9pt;").arg(theme::warningText().name())
                                       : muted);
    m_interval->setStyleSheet(tooShort ? QStringLiteral("background: %1;").arg(theme::warningBackground().name())
                                       : QString());
}

void MeasurePage::updateEnabled() {
    const bool on = m_seriesOn->isChecked();
    const bool fast = m_fastest->isChecked();
    m_fastest->setEnabled(on);
    m_interval->setEnabled(on && !fast);
    m_intervalUnit->setEnabled(on && !fast);
    m_minLabel->setEnabled(on);
    m_stopBy->setEnabled(on);
    m_count->setEnabled(on && m_stopBy->currentIndex() == 0);
    m_duration->setEnabled(on && m_stopBy->currentIndex() == 1);
    m_start->setEnabled(on);
    m_startAt->setVisible(m_start->currentIndex() == 4);
    m_startAt->setEnabled(on);
    m_btnOnce->setEnabled(m_ready && !m_busy);
    m_btnSeries->setEnabled(m_ready && !m_busy && on);
    const bool series = m_busy && m_ctl && m_ctl->isSeriesRunning();
    m_btnPause->setEnabled(series);
    m_btnClear->setEnabled(!m_busy);
    if (!series && m_btnPause->isChecked()) {
        const QSignalBlocker b(m_btnPause);
        m_btnPause->setChecked(false);
        m_btnPause->setText(QStringLiteral("❚❚  Pause"));
    }
}

// ============================================================================================ actions
bool MeasurePage::confirmSampleName() {
    const QString n = m_sample->text().trimmed();
    if (!n.isEmpty() && n != QLatin1String("sample")) {
        QSettings().setValue(QLatin1String(kSampleKey), n);
        return true;
    }
    if (!dialogsEnabled())
        return true;                                   // tests / self-test: falls back to "sample"
    bool ok = false;
    const QString t = QInputDialog::getText(this, QStringLiteral("Sample name"),
                                            QStringLiteral("Name of the sample (used in the file names, so the "
                                                           "spectra can be found later):"),
                                            QLineEdit::Normal, n, &ok).trimmed();
    if (!ok)
        return false;
    if (!t.isEmpty()) {
        m_sample->setText(t);
        QSettings().setValue(QLatin1String(kSampleKey), t);
    }
    return true;
}

void MeasurePage::measureOnce() {
    if (!confirmSampleName())
        return;
    if (m_folderAuto)                                  // a new folder each day while the program runs
        m_folder = defaultFolder(), showFolder();
    if (m_ctl)
        m_ctl->measureOnce(measureSettings());
}

void MeasurePage::startSeries() {
    if (!m_ctl || !confirmSampleName())
        return;
    if (m_folderAuto)
        m_folder = defaultFolder(), showFolder();
    m_bandAuto = true;                                 // new series: pick the tallest band again
    lr::SeriesSettings s = seriesSettings();
    const double mi = minInterval();
    if (!m_fastest->isChecked() && s.interval_s < mi) {
        if (dialogsEnabled()) {
            const auto r = QMessageBox::question(
                this, QStringLiteral("Interval too short"),
                QStringLiteral("With these settings one spectrum takes at least %1 s, so an interval of %2 s is not "
                               "possible.\n\nUse %1 s instead?")
                    .arg(mi, 0, 'f', 1)
                    .arg(lr::py::g(s.interval_s)));
            if (r != QMessageBox::Yes)
                return;
        }
        m_intervalUnit->setCurrentText(QStringLiteral("s"));
        m_interval->setValue(std::round((mi + 0.05) * 10.0) / 10.0);
        s = seriesSettings();
    }
    m_history.clear();
    m_processed.clear();
    m_trend.clear();
    if (m_ctl->startSeries(s, channel()))
        onSeriesProgress({});
}

// ============================================================================================ results
double MeasurePage::trendTime(const lr::Spectrum& sp) const {
    bool ok = false;
    const double e = sp.metaValue(QStringLiteral("series_elapsed_s")).toDouble(&ok);
    if (ok)
        return e;
    if (m_history.empty())
        return 0.0;
    return double(m_history.front().t.msecsTo(sp.t)) / 1000.0;
}

void MeasurePage::addSpectrum(const lr::Spectrum& sp) {
    const bool first = m_history.empty();
    m_history.push_back(sp);
    const Vec& x = sp.shift;
    {
        auto [y, base] = processSafe(sp, processingSettings());
        m_processed.push_back({std::move(y), std::move(base), m_procStamp});
    }
    while (m_history.size() > 50) {
        m_history.pop_front();
        m_processed.pop_front();
    }
    const Vec& y = m_processed.back().y;
    m_lastPeaks = pickPeaks(x, y);
    if (first && m_bandAuto && !m_lastPeaks.empty()) {   // follow the tallest band by default
        double tall = m_lastPeaks.front(), best = -std::numeric_limits<double>::infinity();
        for (double p : m_lastPeaks) {
            const double v = y[nearestIndex(x, p)];
            if (v > best) {
                best = v;
                tall = p;
            }
        }
        setBand(tall, true);                           // recomputes the trend (includes this spectrum)
    } else {
        m_trend.emplace_back(trendTime(sp), bandValue(x, y));
    }
    const Vec& raw = sp.signal.empty() ? sp.intensity : sp.signal;
    const double rawMax = raw.empty() ? 0.0 : *std::max_element(raw.begin(), raw.end());
    const int pct = std::clamp(int(100 * rawMax / 65535), 0, 100);
    m_fill->setValue(pct);
    m_fillPct->setText(QStringLiteral("%1 %").arg(pct));
    double bgMean = 0;
    bool bgAny = false;
    for (double b : sp.background) {
        bgMean += b;
        bgAny = bgAny || b != 0;
    }
    m_lblDark->setText(bgAny ? QStringLiteral("%1 counts").arg(bgMean / double(sp.background.size()), 0, 'f', 0)
                             : QStringLiteral("—"));
    if (rawMax >= 65000)
        m_lblSat->setText(QStringLiteral("<b style='color:%1'>SATURATED: shorten the exposure</b>")
                              .arg(theme::warningText().name()));
    else
        m_lblSat->setText(rawMax > 55000 ? QStringLiteral("close to maximum") : QStringLiteral("none"));
    {
        double bg = 0;
        const double r = lr::processing::fluorescenceRatio(x, sp.intensity, &bg);
        const bool fluo = std::isfinite(r) && r >= lr::processing::kFluorescenceRatio
                          && bg > lr::processing::kFluorescenceCounts;
        m_lblFluor->setText(!std::isfinite(r) ? QStringLiteral("—")
                            : fluo ? QStringLiteral("background ≈ %1× the strongest band").arg(r, 0, 'f', r < 10 ? 1 : 0)
                                   : QStringLiteral("low"));
        m_lblFluor->setToolTip(fluo ? QStringLiteral("<p>A broad fluorescence background dominates the spectrum. "
                                                     "The <i>SNIP, 4th order</i> baseline handles it best. It often "
                                                     "fades with longer laser exposure (photobleaching).</p>")
                                    : QString());
    }
    m_lblTime->setText(sp.t.toString(QStringLiteral("HH:mm:ss")));
    m_lblSet->setText(QStringLiteral("%1 × %2 s · %3 nm").arg(sp.averages).arg(lr::py::g(sp.exposure_s)).arg(sp.channel));
    scheduleRedraw();
}

// Coalesces redraws: the first spectrum after a quiet period is drawn at once (next event-loop pass), later
// ones at most every kMinRedrawMs; nothing is drawn while the page is hidden (showEvent draws).
void MeasurePage::scheduleRedraw() {
    m_redrawPending = true;
    if (!isVisible() || m_redrawTimer->isActive())
        return;
    m_redrawTimer->start(int(std::max<qint64>(0, kMinRedrawMs - m_sinceRedraw.elapsed())));
}

void MeasurePage::flushRedraw() {
    m_redrawTimer->stop();
    if (!m_redrawPending)
        return;
    redraw();
    redrawTrend();
}

void MeasurePage::showEvent(QShowEvent* e) {
    Page::showEvent(e);
    if (m_redrawPending)
        scheduleRedraw();
}

void MeasurePage::processingChanged() {
    ++m_procStamp;
    redraw();
    if (m_trendMode->currentIndex() == 1)
        redrawTrend();
}

const MeasurePage::Processed& MeasurePage::processedAt(std::size_t i) {
    Processed& p = m_processed[i];
    if (p.stamp != m_procStamp) {
        const lr::Spectrum& sp = m_history[i];
        std::tie(p.y, p.base) = processSafe(sp, processingSettings());
        p.stamp = m_procStamp;
    }
    return p;
}

double MeasurePage::bandValue(const Vec& x, const Vec& y) const {
    if (x.empty() || y.empty())
        return 0.0;
    const std::size_t i = nearestIndex(x, m_band->value());
    const std::size_t lo = i >= 4 ? i - 4 : 0, hi = std::min(y.size(), i + 5);
    return *std::max_element(y.begin() + std::ptrdiff_t(lo), y.begin() + std::ptrdiff_t(hi));
}

void MeasurePage::setBand(double value, bool automatic) {
    m_bandAuto = automatic;
    {
        const QSignalBlocker b(m_band);
        m_band->setValue(int(std::lround(value)));
    }
    recomputeTrend();
}

void MeasurePage::recomputeTrend() {
    if (m_history.empty())
        return;
    m_trend.clear();
    for (std::size_t i = 0; i < m_history.size(); ++i)
        m_trend.emplace_back(trendTime(m_history[i]), bandValue(m_history[i].shift, processedAt(i).y));
    redrawTrend();
}

void MeasurePage::fillPeakMenu() {
    m_peakMenu->clear();
    if (m_lastPeaks.empty())
        m_peakMenu->addAction(QStringLiteral("(no peaks yet)"))->setEnabled(false);
    for (double p : m_lastPeaks)
        m_peakMenu->addAction(cm1(p), this, [this, p] { setBand(p); });
}

void MeasurePage::spectrumMenu(const QPointF& data, const QPoint& globalPos) {
    QMenu m(this);
    const double x = data.x();
    if (!m_lastPeaks.empty()) {
        double near = m_lastPeaks.front();
        for (double p : m_lastPeaks)
            if (std::abs(p - x) < std::abs(near - x))
                near = p;
        if (std::abs(near - x) < 25)
            m.addAction(QStringLiteral("Use peak %1 for the trend").arg(cm1(near)), this, [this, near] { setBand(near); });
    }
    m.addAction(QStringLiteral("Use %1 for the trend").arg(cm1(x)), this, [this, x] { setBand(x); });
    m.addSeparator();
    m.addAction(QStringLiteral("Full view"), m_plot, &PlotWidget::autoRange);
    m.addSeparator();
    fillMoreMenu(&m);
    m.exec(globalPos);
}

void MeasurePage::fillMoreMenu(QMenu* m) {
    QAction* lib = m->addAction(QStringLiteral("Add to library…"), this, &MeasurePage::addLastToLibrary);
    lib->setEnabled(!m_history.empty());
    lib->setToolTip(QStringLiteral("Store the last spectrum in your spectral library (Library area)"));
    QAction* clr = m->addAction(QStringLiteral("Clear"), this, &MeasurePage::clearAll);
    clr->setEnabled(!m_history.empty() || !m_trend.empty());
    clr->setToolTip(QStringLiteral("Clear the plot, the trend and the panels below (saved files are kept)"));
}

void MeasurePage::addLastToLibrary() {
    if (!m_history.empty())
        emit addToLibraryRequested(m_history.back());
}

void MeasurePage::clearAll() {
    m_history.clear();
    m_processed.clear();
    m_trend.clear();
    m_lastPeaks.clear();
    m_bandAuto = true;                                 // the next spectrum picks its tallest band again
    m_viewPending = true;
    m_redrawPending = false;
    m_redrawTimer->stop();
    m_curve->clearData();
    m_curve->setName({});
    m_baseCurve->clearData();
    m_baseCurve->setName({});
    for (Curve* c : m_oldCurves) {
        c->clearData();
        c->setName({});
    }
    m_peakLabels->setLabels({});
    for (QLabel* l : {m_lblN, m_lblNext, m_lblElapsed, m_lblEnd, m_lblDark, m_lblSat, m_lblFluor, m_lblTime, m_lblSet,
                      m_lblPeaks, m_fillPct})
        l->setText(QStringLiteral("—"));
    m_bar->setValue(0);
    m_fill->setValue(0);
    m_plot->setXRange(150, 3500);
    m_plot->setAutoY(true);
    m_tplot->setAutoX(true);
    m_tplot->setAutoY(true);
    m_trendCurve = nullptr;
    m_tplot->clear();
    redrawTrend();
    m_plot->update();
}

void MeasurePage::redraw() {
    m_redrawPending = false;
    m_sinceRedraw.restart();
    ++m_redraws;
    if (m_history.empty()) {
        m_peakLabels->setLabels({});
        return;
    }
    const lr::processing::Settings ps = processingSettings();
    const lr::Spectrum& sp = m_history.back();
    const Vec& x = sp.shift;
    const Vec& raw = sp.intensity;
    const bool fresh = m_processed.back().stamp == m_procStamp;   // peaks of the newest are known
    const Processed& pr = processedAt(m_history.size() - 1);
    const Vec& y = pr.y;
    enum { Raw, Cor, Both } mode = m_showRaw->isChecked() ? Raw : (m_showCor->isChecked() ? Cor : Both);
    Vec shown;
    if (mode == Raw) {
        shown = raw;
        m_baseCurve->clearData();
    } else if (mode == Cor) {
        shown = y;
        m_baseCurve->clearData();
    } else {                                           // Python: smooth(despike(raw)) / despike(raw) / raw
        if (ps.smooth)
            shown = lr::processing::smooth(lr::processing::despike(raw));
        else if (ps.despike)
            shown = lr::processing::despike(raw);
        else
            shown = raw;
        m_baseCurve->setData(x, pr.base);
    }
    m_curve->setData(x, shown);
    const QString sample = sp.metaValue(QStringLiteral("sample"));
    const QString idx = sp.metaValue(QStringLiteral("series_index"));
    m_curve->setName((sample.isEmpty() ? tr("spectrum") : sample) + QStringLiteral("  ") +
                     sp.t.toString(QStringLiteral("HH:mm:ss")) + (idx.isEmpty() ? QString() : QStringLiteral("  #") + idx));
    m_baseCurve->setName(mode == Both && ps.baseline != QLatin1String("none") ? tr("baseline") : QString());
    for (Curve* c : m_oldCurves) {
        c->clearData();
        c->setName({});
    }
    // Earlier spectra are drawn faded behind the newest only if they are of the same sample (repeats, a
    // series); different samples are compared in the Data area, where each spectrum has its name and colour.
    std::size_t same = 0;
    for (std::size_t k = m_history.size() - 1; k-- > 0;) {
        const lr::Spectrum& o = m_history[k];
        if (o.metaValue(QStringLiteral("sample")) != sample || o.channel != sp.channel) break;
        ++same;
    }
    const std::size_t nOld = std::min<std::size_t>(std::size_t(kFaded), same);
    for (std::size_t k = 0; k < nOld; ++k) {            // the newest previous spectrum is the least faded
        const std::size_t i = m_history.size() - 1 - nOld + k;
        const lr::Spectrum& o = m_history[i];
        m_oldCurves[std::size_t(kFaded) - nOld + k]->setData(o.shift, mode == Cor ? processedAt(i).y : o.intensity);
    }
    if (nOld > 0)
        m_oldCurves[std::size_t(kFaded) - 1]->setName(
            tr("earlier %1 (%2)").arg(sample.isEmpty() ? tr("spectra") : sample).arg(same));
    if (!fresh)
        m_lastPeaks = pickPeaks(x, y);
    const Vec& peaks = m_lastPeaks;
    QStringList txt;
    for (double p : peaks)
        txt << QString::number(p, 'f', 1);
    m_lblPeaks->setText(txt.isEmpty() ? QStringLiteral("—") : txt.join(QStringLiteral("  ")));
    m_peakLabels->setPeaks(peaks, x, shown, 1);   // one decimal: calibration changes are often < 1 cm⁻¹
    if (m_viewPending) {                               // first spectrum (after connecting, Clear): default view
        m_viewPending = false;
        m_plot->setXRange(150, 3500);
        m_plot->setAutoY(true);
    }
    m_plot->rescaleY();
}

void MeasurePage::redrawTrend() {
    const PlotPalette& pal = m_tplot->plotPalette();
    if (m_trendMode->currentIndex() == 0) {
        const double span = m_trend.empty() ? 0.0 : m_trend.back().first;
        QString unit = QStringLiteral("s");
        double div = 1;
        if (span >= 3 * 3600)
            unit = QStringLiteral("h"), div = 3600;
        else if (span >= 180)
            unit = QStringLiteral("min"), div = 60;
        m_tplot->setLabel(Axis::Bottom, QStringLiteral("time (%1)").arg(unit));
        m_tplot->setLabel(Axis::Left, QString());   // too short for a title; the band is shown above
        if (m_trend.empty()) {
            m_tplot->clear();
            m_trendCurve = nullptr;
            return;
        }
        Vec t, v;
        t.reserve(m_trend.size());
        v.reserve(m_trend.size());
        for (const auto& [a, b] : m_trend) {
            t.push_back(a / div);
            v.push_back(b);
        }
        if (!m_trendCurve) {                           // first point (or after the waterfall): a new curve
            m_tplot->clear();
            m_trendCurve = m_tplot->addCurve(QPen(pal.main, 1.5));
            m_trendCurve->setMarker(Marker::Circle, 5, QBrush(pal.main));
        }
        const double t0 = t.front();
        const bool single = t.size() == 1;
        m_trendCurve->setData(std::move(t), std::move(v));   // in place: no new plot items per spectrum
        if (single)                                    // one point: time from 0, not -0.5..0.5
            m_tplot->setXRange(0, std::max(1.0, 2 * t0));
        else if (!m_tplot->autoX() && m_trend.size() == 2)
            m_tplot->setAutoX(true);                   // from the second point on: follow the data
        return;
    }
    m_tplot->clear();
    m_trendCurve = nullptr;
    m_tplot->setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    m_tplot->setLabel(Axis::Left, QString());
    const std::size_t n = std::min<std::size_t>(15, m_history.size());
    if (n == 0)
        return;
    std::vector<Vec> ys;
    double off = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t i = m_history.size() - n + k;
        const lr::Spectrum& s = m_history[i];
        ys.push_back(processedAt(i).y);
        Vec sel;                                       // 99.5th percentile above the edge filter
        for (std::size_t j = 0; j < s.shift.size() && j < ys.back().size(); ++j)
            if (s.shift[j] > lr::processing::kBaselineFitMin)
                sel.push_back(ys.back()[j]);
        if (!sel.empty()) {
            std::sort(sel.begin(), sel.end());
            const double pos = 0.995 * double(sel.size() - 1);
            const std::size_t lo = std::size_t(pos);
            const double frac = pos - double(lo);
            const double q = sel[lo] + (lo + 1 < sel.size() ? frac * (sel[lo + 1] - sel[lo]) : 0.0);
            off = std::max(off, q);
        }
    }
    off *= 0.6;
    if (off == 0)
        off = 1;
    for (std::size_t k = 0; k < n; ++k) {
        const lr::Spectrum& s = m_history[m_history.size() - n + k];
        Vec y = std::move(ys[k]);
        for (double& v : y)
            v += double(k) * off;
        const QColor col = QColor::fromHsv(int(360.0 * double(k) / double(std::max<std::size_t>(n, 2))) % 360,
                                           255, 200);
        m_tplot->addCurve(s.shift, std::move(y), QPen(col, 1));
    }
}

// ============================================================================================ progress, state
QString MeasurePage::seriesText() const { return m_lblN->text(); }

void MeasurePage::onSeriesProgress(const lr::SeriesProgress& p) {
    QString n = QStringLiteral("%1 / %2").arg(p.n).arg(p.total);
    if (p.paused)
        n += (m_ctl && m_ctl->isPaused()) ? QStringLiteral("  (paused)") : QStringLiteral("  (paused: press ARM)");
    if (p.done)
        n += QStringLiteral("  done");
    m_lblN->setText(n);
    if (p.next_in_s >= 0 && !p.done) {
        const int s = int(std::max(0.0, p.next_in_s));
        m_lblNext->setText(QStringLiteral("%1:%2").arg(s / 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0')));
    } else {
        m_lblNext->setText(QStringLiteral("—"));
    }
    const int e = int(std::max(0.0, p.elapsed_s));
    m_lblElapsed->setText(QStringLiteral("%1:%2:%3")
                              .arg(e / 3600)
                              .arg(e % 3600 / 60, 2, 10, QChar('0'))
                              .arg(e % 60, 2, 10, QChar('0')));
    if (p.end.isValid())
        m_lblEnd->setText(p.end.toString(QStringLiteral("~HH:mm")));
    m_bar->setValue(p.total ? int(100 * p.n / p.total) : 0);
}

void MeasurePage::setBusy(bool busy) {
    m_busy = busy;
    updateEnabled();
}

void MeasurePage::setReady(bool ready) {
    m_ready = ready;
    updateEnabled();
}

}  // namespace lr::gui
