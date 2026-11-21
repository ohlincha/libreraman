#include "calibrationpage.h"

#include "controller.h"
#include "mainwindow.h"
#include "plotwidget.h"
#include "theme.h"

#include "core/errors.h"
#include "core/fileio.h"
#include "core/series.h"
#include "core/simulator.h"
#include "core/standards.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QStyle>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace lr::gui {

namespace {

// Standards in alphabetical order (case-insensitive) for the lists of this page.
std::vector<standards::Standard> sortedStandards() {
    std::vector<standards::Standard> v(standards::all().begin(), standards::all().end());
    std::stable_sort(v.begin(), v.end(), [](const standards::Standard& a, const standards::Standard& b) {
        return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });
    return v;
}


const char* const kReminderKey = "calibration/reminderDays";
const char* const kToleranceKey = "calibration/checkTolerance";
const QString kCalTag = QStringLiteral("calibration:");
const QString kCheckTag = QStringLiteral("check:");

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

QTableWidget* table(const QStringList& headers) {
    auto* t = new QTableWidget(0, int(headers.size()));
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->hide();
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setAlternatingRowColors(true);
    t->horizontalHeader()->setStretchLastSection(true);
    t->verticalHeader()->setDefaultSectionSize(22);
    return t;
}

QTableWidgetItem* cell(const QString& text, bool right = false) {
    auto* it = new QTableWidgetItem(text);
    if (right)
        it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return it;
}

QString signedNum(double v, int decimals = 2) {
    return (v >= 0 ? QStringLiteral("+") : QStringLiteral("−")) + QString::number(std::abs(v), 'f', decimals);
}

QString strengthLetter(int s) { return s >= 3 ? QStringLiteral("s") : s == 2 ? QStringLiteral("m") : QStringLiteral("w"); }

QString bandList(const QString& name) {
    QStringList parts;
    for (const standards::Band& b : standards::bands(name))
        parts << QStringLiteral("%1 %2").arg(b.position, 0, 'f', 1).arg(strengthLetter(b.strength));
    return parts.join(QStringLiteral(" · "));
}

Vec pixelOrder(const lr::Spectrum& sp) {       // Python _pixel_order (as the CLI)
    if (sp.pixels.size() != sp.intensity.size())
        return sp.intensity;
    std::vector<std::pair<double, double>> pv;
    for (std::size_t i = 0; i < sp.pixels.size(); ++i)
        pv.emplace_back(sp.pixels[i], sp.intensity[i]);
    std::sort(pv.begin(), pv.end());
    Vec y;
    for (const auto& p : pv)
        y.push_back(p.second);
    return y;
}

double mean(const Vec& v) {
    if (v.empty())
        return qQNaN();
    double s = 0;
    for (double x : v)
        s += x;
    return s / double(v.size());
}

QString stdName(const QString& name) {         // canonical standard name (aliases allowed)
    try {
        return standards::find(name).name;
    } catch (const lr::Error&) {
        return name;
    }
}

// Rule (a): bands of a standard within the detector's range (factory calibration), how many must be found
struct BandNeed {
    int total = 0, need = 0;
    std::vector<double> strong;
};

BandNeed bandNeed(const QString& standard, const calib::ShiftFn& factory, int npix) {
    const double lo = std::min(factory(0), factory(npix - 1)), hi = std::max(factory(0), factory(npix - 1));
    BandNeed b;
    for (const standards::Band& band : standards::bands(standard)) {
        if (band.position < lo || band.position > hi)
            continue;
        ++b.total;
        if (band.strength >= standards::S)
            b.strong.push_back(band.position);
    }
    b.need = (2 * b.total + 2) / 3;                    // ceil(2/3 total)
    return b;
}

bool enoughBands(const QString& standard, const std::vector<calib::Match>& m, const calib::ShiftFn& factory, int npix) {
    const BandNeed b = bandNeed(standard, factory, npix);
    return int(m.size()) >= b.need;
}
}  // namespace

// ============================================================================================ construction
CalibrationPage::CalibrationPage(const PageContext& ctx, QWidget* parent) : Page(parent), m_ctx(ctx) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(8, 4, 8, 8);
    v->setSpacing(6);
    v->addWidget(buildHeader());
    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(buildWizard(), QStringLiteral("Calibrate"));
    m_tabs->addTab(buildCheck(), QStringLiteral("Quick check"));
    m_tabs->addTab(buildHistory(), QStringLiteral("History"));
    m_tabs->addTab(buildLog(), QStringLiteral("Instrument log"));
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int k) {
        if (k == 2)
            refreshHistory();
        else if (k == 3)
            refreshLog();
    });
    v->addWidget(m_tabs, 1);
    setSelectedStandards({QStringLiteral("carbon tetrachloride"), QStringLiteral("chloroform")});
    updateHeader();
    refreshHistory();
    refreshLog();
    updateEnabled();
}

QWidget* CalibrationPage::buildHeader() {
    auto* w = new QFrame;
    w->setObjectName(QStringLiteral("calHeader"));
    w->setStyleSheet(QStringLiteral("#calHeader { border-bottom: 1px solid palette(mid); }"));
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(4, 2, 4, 6);
    h->setSpacing(18);
    auto* left = new QVBoxLayout;
    left->setSpacing(2);
    m_hdrInstrument = new QLabel;
    QFont f = m_hdrInstrument->font();
    f.setPointSizeF(f.pointSizeF() * 1.25);
    f.setBold(true);
    m_hdrInstrument->setFont(f);
    m_hdrCal = new QLabel;
    m_hdrCal->setWordWrap(true);
    m_hdrCal->setTextInteractionFlags(Qt::TextSelectableByMouse);
    left->addWidget(m_hdrInstrument);
    auto* calRow = new QHBoxLayout;
    calRow->setSpacing(10);
    calRow->addWidget(m_hdrCal, 1);
    m_btnHdrFactory = new QPushButton(QStringLiteral("↺  Revert to factory calibration"));
    m_btnHdrFactory->setToolTip(QStringLiteral("<p>Stop using the user calibration and use the factory calibration of the "
                                               "instrument again. The history is kept: the calibration can be made "
                                               "active again later (History tab).</p>"));
    connect(m_btnHdrFactory, &QPushButton::clicked, this, &CalibrationPage::confirmRevert);
    calRow->addWidget(m_btnHdrFactory);
    left->addLayout(calRow);
    h->addLayout(left, 1);
    m_hdrReminder = new QLabel;
    m_hdrReminder->setWordWrap(true);
    m_hdrReminder->setMaximumWidth(560);
    h->addWidget(m_hdrReminder);
    auto* rem = new QHBoxLayout;
    rem->setSpacing(6);
    rem->addWidget(new QLabel(QStringLiteral("Remind after")));
    m_reminder = new QSpinBox;
    m_reminder->setRange(0, 3650);
    m_reminder->setSuffix(QStringLiteral(" days"));
    m_reminder->setSpecialValueText(QStringLiteral("never"));
    m_reminder->setValue(reminderDays());
    m_reminder->setToolTip(QStringLiteral("<p>Warn when the last calibration of the instrument is older than this "
                                          "(shown here and in the log when the instrument connects).</p>"));
    connect(m_reminder, &QSpinBox::valueChanged, this, &CalibrationPage::setReminderDays);
    rem->addWidget(m_reminder);
    h->addLayout(rem);
    return w;
}

QWidget* CalibrationPage::buildWizard() {
    auto* page = new QWidget;
    auto* h = new QHBoxLayout(page);
    h->setContentsMargins(0, 6, 0, 0);
    h->setSpacing(14);

    // ---- left: steps
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* inner = new QWidget;
    scroll->setWidget(inner);
    auto* v = new QVBoxLayout(inner);
    v->setContentsMargins(0, 0, 8, 0);

    QGroupBox* b = box(QStringLiteral("1 · Standards"));
    auto* bl = new QVBoxLayout(b);
    m_stdList = new QListWidget;
    m_stdList->setToolTip(QStringLiteral("<p>Tick one or more standards. Several standards are combined into one "
                                         "fit (more bands over a wider range). Silicon alone: one-point offset.</p>"));
    for (const standards::Standard& s : sortedStandards()) {
        auto* it = new QListWidgetItem(QStringLiteral("%1   ·  %2, %3").arg(s.name, s.kind, s.source == QLatin1String("ASTM")
                                                                                         ? QStringLiteral("ASTM E1840")
                                                                                         : QStringLiteral("literature")));
        it->setData(Qt::UserRole, s.name);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(Qt::Unchecked);
        m_stdList->addItem(it);
    }
    m_stdList->setFixedHeight(122);
    connect(m_stdList, &QListWidget::currentRowChanged, this, &CalibrationPage::updateStandardInfo);
    connect(m_stdList, &QListWidget::itemChanged, this, &CalibrationPage::selectedStandardsChanged);
    bl->addWidget(m_stdList, 1);
    m_stdInfo = muted();
    m_stdInfo->setTextFormat(Qt::RichText);
    bl->addWidget(m_stdInfo);
    v->addWidget(b);

    b = box(QStringLiteral("2 · Spectra"));
    auto* g = new QGridLayout(b);
    g->setHorizontalSpacing(6);
    g->setVerticalSpacing(4);
    m_instructions = muted();
    g->addWidget(m_instructions, 0, 0, 1, 4);
    m_exposure = new QDoubleSpinBox;
    m_exposure->setRange(0.01, 600);
    m_exposure->setDecimals(2);
    m_exposure->setValue(1.0);
    m_exposure->setSuffix(QStringLiteral(" s"));
    m_scans = new QSpinBox;
    m_scans->setRange(1, 1000);
    m_scans->setValue(5);
    m_scans->setToolTip(QStringLiteral("Scans averaged (at least 3 recommended: cosmic rays)"));
    g->addWidget(new QLabel(QStringLiteral("Exposure")), 1, 0);
    g->addWidget(m_exposure, 1, 1);
    g->addWidget(new QLabel(QStringLiteral("Scans")), 1, 2);
    g->addWidget(m_scans, 1, 3);
    m_start = new QComboBox;
    m_start->addItems({QStringLiteral("calibration in use"), QStringLiteral("factory calibration")});
    m_start->setToolTip(QStringLiteral(
        "<p>Calibration used to find the reference bands in the spectra (peaks within 12 cm⁻¹ of a band). If the "
        "calibration in use is wrong, few bands are found: match against the factory calibration instead. The new "
        "calibration is always a correction to the factory calibration.</p>"));
    connect(m_start, &QComboBox::currentIndexChanged, this, [this] { rematchAll(); });
    m_autoRetry = new QCheckBox(QStringLiteral("retry with factory"));
    m_autoRetry->setChecked(true);
    m_autoRetry->setToolTip(QStringLiteral("<p>If fewer than ⅔ of a standard's bands are found against the calibration "
                                           "in use, match against the factory calibration automatically.</p>"));
    connect(m_autoRetry, &QCheckBox::toggled, this, [this] { rematchAll(); });
    g->addWidget(new QLabel(QStringLiteral("Match against")), 4, 0);
    g->addWidget(m_start, 4, 1, 1, 3);
    g->addWidget(m_autoRetry, 5, 1, 1, 3);
    m_samplesTable = table({QStringLiteral("Standard"), QStringLiteral("Spectrum"), QStringLiteral("Bands")});
    m_samplesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_samplesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_samplesTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_samplesTable->setFixedHeight(82);
    m_samplesTable->setToolTip(QStringLiteral("<p>MEASURE and Load file… apply to the highlighted row (▶). After each "
                                              "measurement the next standard without a spectrum is selected.</p>"));
    connect(m_samplesTable, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (row >= 0 && row < m_samplesTable->rowCount())
            showSample(m_samplesTable->item(row, 0)->data(Qt::UserRole).toString());
        highlightCurrentRow();
        updateEnabled();
    });
    g->addWidget(m_samplesTable, 2, 0, 1, 4);
    m_btnMeasure = new QPushButton(QStringLiteral("▶  MEASURE"));
    m_btnMeasure->setStyleSheet(theme::bigButtonStyle(theme::Action::Measure));
    m_btnMeasure->setMinimumHeight(38);
    m_btnMeasure->setToolTip(QStringLiteral("<p>Measure the standard selected in the table (laser on only while "
                                            "measuring; the instrument must be armed).</p>"));
    connect(m_btnMeasure, &QPushButton::clicked, this, [this] {
        const int row = m_samplesTable->currentRow();
        if (row >= 0)
            measureStandard(m_samplesTable->item(row, 0)->data(Qt::UserRole).toString());
    });
    m_btnLoad = new QPushButton(QStringLiteral("Load file…"));
    m_btnLoad->setToolTip(QStringLiteral("<p>Use a saved spectrum of the selected standard instead (LibreRamen CSV "
                                         "with one row per detector pixel).</p>"));
    connect(m_btnLoad, &QPushButton::clicked, this, [this] {
        const int row = m_samplesTable->currentRow();
        if (row < 0)
            return;
        const QString name = m_samplesTable->item(row, 0)->data(Qt::UserRole).toString();
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Spectrum of %1").arg(name), QString(),
                                                          QStringLiteral("Spectra (*.csv *.txt);;All files (*)"));
        if (!path.isEmpty())
            loadStandardFile(name, path);
    });
    m_btnStop = new QPushButton(QStringLiteral("■  Stop"));
    connect(m_btnStop, &QPushButton::clicked, this, [this] {
        if (controller())
            controller()->stop();
    });
    auto* row = new QHBoxLayout;
    row->addWidget(m_btnMeasure, 3);
    row->addWidget(m_btnLoad, 2);
    row->addWidget(m_btnStop, 1);
    g->addLayout(row, 3, 0, 1, 4);
    g->setColumnStretch(1, 1);
    g->setColumnStretch(3, 1);
    v->addWidget(b);

    b = box(QStringLiteral("3 · Fit and accept"));
    g = new QGridLayout(b);
    g->setHorizontalSpacing(6);
    m_modePoly = new QRadioButton(QStringLiteral("Polynomial correction"));
    m_modeOffset = new QRadioButton(QStringLiteral("One-point offset / laser wavelength"));
    m_modePoly->setChecked(true);
    m_modePoly->setToolTip(QStringLiteral(
        "<p>Corrects the factory calibration with a smooth curve through all matched bands. Its degree is limited by "
        "how much of the detector the bands cover; outside the covered range the correction is only extrapolated "
        "linearly. Needs bands spread over the spectrum (e.g. carbon tetrachloride + chloroform, or ethanol).</p>"));
    m_modeOffset->setToolTip(QStringLiteral(
        "<p>Shifts the factory calibration by a constant (e.g. silicon 520.5 cm⁻¹). This is the same as correcting "
        "the laser wavelength: a constant Raman-shift offset is exactly a change of the excitation wavelength. "
        "With several bands the mean offset is used.</p>"));
    auto* polyHelp = muted(QStringLiteral("Smooth correction of the factory calibration through all bands; needs bands "
                                          "spread over the spectrum."));
    auto* offsetHelp = muted(QStringLiteral("Corrects a drifted laser wavelength: every band moves by the same amount. "
                                            "One band suffices (silicon 520.5 cm⁻¹ is the classic)."));
    for (QLabel* l : {polyHelp, offsetHelp}) {
        l->setContentsMargins(22, 0, 0, 4);
        QFont f = l->font();
        f.setPointSizeF(f.pointSizeF() * 0.92);
        l->setFont(f);
    }
    auto* grp = new QButtonGroup(b);
    grp->addButton(m_modePoly);
    grp->addButton(m_modeOffset);
    connect(m_modePoly, &QRadioButton::toggled, this, [this] {
        invalidateFit();
        updateEnabled();
    });
    m_degree = new QComboBox;
    m_degree->addItems({QStringLiteral("auto"), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")});
    m_degree->setToolTip(QStringLiteral(
        "<p>Degree of the correction to the factory calibration. Auto: offset if the bands cover less than 25 % of "
        "the detector; linear (2–3 bands) or quadratic (4+ bands) otherwise; cubic only with 6 or more bands spread "
        "over at least 60 %.</p>"));
    connect(m_degree, &QComboBox::currentIndexChanged, this, [this] {
        if (m_fit)
            fit();
    });
    m_btnFit = new QPushButton(QStringLiteral("Fit calibration"));
    m_btnFit->setToolTip(QStringLiteral("<p>Fits the selected kind of calibration (polynomial correction or one-point "
                                        "offset) to the bands found in the measured standards, then shows the "
                                        "deviations, the whole-detector check and the acceptance checklist.</p>"));
    connect(m_btnFit, &QPushButton::clicked, this, &CalibrationPage::fit);
    g->addWidget(m_modePoly, 0, 0, 1, 2);
    g->addWidget(new QLabel(QStringLiteral("degree")), 0, 2);
    g->addWidget(m_degree, 0, 3);
    g->addWidget(polyHelp, 1, 0, 1, 4);
    g->addWidget(m_modeOffset, 2, 0, 1, 4);
    g->addWidget(offsetHelp, 3, 0, 1, 4);
    g->addWidget(m_btnFit, 4, 0, 1, 4);
    g->setColumnStretch(1, 1);
    v->addWidget(b);
    v->addStretch();

    // ---- left, below the steps (always visible): result, acceptance checklist, Accept / Reject
    b = box(QStringLiteral("4 · Check and accept"));
    g = new QGridLayout(b);
    g->setHorizontalSpacing(6);
    g->setVerticalSpacing(4);
    m_fitLabel = new QLabel(QStringLiteral("—"));
    m_fitLabel->setWordWrap(true);
    g->addWidget(m_fitLabel, 4, 0, 1, 4);
    m_checklist = new QLabel;
    m_checklist->setWordWrap(true);
    m_checklist->setTextFormat(Qt::RichText);
    m_checklist->setObjectName(QStringLiteral("calChecklist"));
    m_checklist->setToolTip(QStringLiteral(
        "<p>Accept is possible only when every rule passes: (a) per standard at least ⅔ of its bands and all strong "
        "bands found; (b) range covered by the bands; (c) beyond the covered range the correction changes by at "
        "most 15 cm⁻¹ and the new calibration stays within 50 cm⁻¹ of the factory calibration; (d) residual RMS "
        "at most 3 cm⁻¹.</p>"));
    g->addWidget(m_checklist, 5, 0, 1, 4);
    m_btnFactoryMatch = new QPushButton(QStringLiteral("Match against the factory calibration"));
    m_btnFactoryMatch->setToolTip(QStringLiteral("<p>Find the bands again starting from the factory calibration instead of "
                                                 "the calibration in use, and fit again.</p>"));
    m_btnFactoryMatch->setVisible(false);
    connect(m_btnFactoryMatch, &QPushButton::clicked, this, &CalibrationPage::matchAgainstFactory);
    g->addWidget(m_btnFactoryMatch, 6, 0, 1, 4);
    g->setColumnStretch(1, 1);
    auto* al = new QHBoxLayout;
    m_btnAccept = new QPushButton(QStringLiteral("✓  Accept"));
    m_btnAccept->setStyleSheet(theme::bigButtonStyle(theme::Action::Series));
    m_btnAccept->setMinimumHeight(38);
    m_btnAccept->setToolTip(QStringLiteral("<p>Use the new calibration from now on (spectra measured from now on) "
                                           "and keep it in the history.</p>"));
    connect(m_btnAccept, &QPushButton::clicked, this, &CalibrationPage::acceptCalibration);
    m_btnReject = new QPushButton(QStringLiteral("Reject"));
    m_btnReject->setMinimumHeight(38);
    connect(m_btnReject, &QPushButton::clicked, this, &CalibrationPage::rejectCalibration);
    al->addWidget(m_btnAccept, 2);
    al->addWidget(m_btnReject, 1);
    g->addLayout(al, 7, 0, 1, 4);
    auto* left = new QWidget;
    // wide enough for the steps in this style and font (macOS widgets are wider): no horizontal clipping
    left->setFixedWidth(std::max(410, inner->minimumSizeHint().width() +
                                          scroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 12));
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->setSpacing(6);
    lv->addWidget(scroll, 1);
    auto* footer = new QHBoxLayout;                    // aligned with the steps (their scroll bar margin)
    footer->setContentsMargins(0, 0, 8, 0);
    footer->addWidget(b);
    lv->addLayout(footer);
    h->addWidget(left);

    // ---- right: spectrum, residuals
    auto* split = new QSplitter(Qt::Vertical);
    auto* top = new QWidget;
    auto* tv = new QVBoxLayout(top);
    tv->setContentsMargins(0, 0, 0, 0);
    m_plot = new PlotWidget;
    m_plot->setToolTip(PlotWidget::navigationHelp());
    m_plot->setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    m_plot->setLabel(Axis::Left, QStringLiteral("Intensity (counts)"));
    m_plot->setHeadroom(0.22);
    tv->addWidget(m_plot, 1);
    auto* trow = new QHBoxLayout;
    m_axisNote = muted(QStringLiteral("Dashed lines: reference bands · ◆ matched peaks. x axis: calibration in use."));
    trow->addWidget(m_axisNote, 1);
    m_newAxis = new QCheckBox(QStringLiteral("show with the new calibration"));
    m_newAxis->setEnabled(false);
    connect(m_newAxis, &QCheckBox::toggled, this, [this] { showSample(m_shown); });
    trow->addWidget(m_newAxis);
    tv->addLayout(trow);
    split->addWidget(top);

    auto* bottom = new QWidget;
    auto* bh = new QHBoxLayout(bottom);
    bh->setContentsMargins(0, 0, 0, 0);
    m_devPlot = new PlotWidget;
    m_devPlot->setLabel(Axis::Bottom, QStringLiteral("Detector pixel (shaded: covered by the bands)"));
    m_devPlot->setLabel(Axis::Left, QStringLiteral("New − factory (cm⁻¹)"));
    m_devPlot->setHeadroom(0.1);
    m_devPlot->setLegendVisible(true);
    m_devPlot->setWheelMode(WheelMode::ZoomX);
    m_devPlot->setToolTip(QStringLiteral("<p>Whole-detector check: difference between the new calibration and the factory "
                                         "calibration at every pixel. Inside the shaded range the bands determine it; "
                                         "outside it is extrapolated and should stay flat.</p>"));
    m_resPlot = new PlotWidget;
    m_resPlot->setLabel(Axis::Bottom, QStringLiteral("Reference band (cm⁻¹)"));
    m_resPlot->setLabel(Axis::Left, QStringLiteral("Deviation (cm⁻¹)"));
    m_resPlot->setHeadroom(0.1);
    m_resPlot->setLegendVisible(true);
    m_resPlot->setWheelMode(WheelMode::ZoomX);
    bh->addWidget(m_resPlot, 1);
    bh->addWidget(m_devPlot, 1);
    m_resTable = table({QStringLiteral("Standard"), QStringLiteral("Band"), QStringLiteral("Before"),
                        QStringLiteral("After"), QStringLiteral("Pixel")});
    m_resTable->setToolTip(QStringLiteral("<p>Per band: position from the calibration in use (before) and from the "
                                          "new fit (after), minus the reference value, in cm⁻¹.</p>"));
    for (int c = 0; c < 5; ++c)
        m_resTable->horizontalHeader()->setSectionResizeMode(c, c == 0 ? QHeaderView::Stretch : QHeaderView::ResizeToContents);
    m_resTable->horizontalHeader()->setStretchLastSection(false);
    split->addWidget(bottom);
    split->addWidget(m_resTable);
    m_resTable->setMinimumHeight(90);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setStretchFactor(2, 1);
    h->addWidget(split, 1);
    updateResiduals();
    updateDetectorPlot();
    return page;
}

QWidget* CalibrationPage::buildCheck() {
    auto* page = new QWidget;
    auto* h = new QHBoxLayout(page);
    h->setContentsMargins(0, 6, 0, 0);
    h->setSpacing(14);
    auto* left = new QWidget;
    left->setFixedWidth(410);
    auto* v = new QVBoxLayout(left);
    v->setContentsMargins(0, 0, 8, 0);
    QGroupBox* b = box(QStringLiteral("Quick check"));
    auto* f = new QFormLayout(b);
    f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* intro = muted(QStringLiteral("Measure one standard and compare its bands with the reference values. The "
                                       "calibration is not changed."));
    f->addRow(intro);
    m_checkStd = new QComboBox;
    for (const standards::Standard& s : sortedStandards())
        m_checkStd->addItem(s.name);
    m_checkStd->setCurrentText(QStringLiteral("chloroform"));
    connect(m_checkStd, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        m_checkInfo->setText(QStringLiteral("Bands (cm⁻¹): ") + bandList(name));
    });
    f->addRow(QStringLiteral("Standard"), m_checkStd);
    m_checkInfo = muted(QStringLiteral("Bands (cm⁻¹): ") + bandList(m_checkStd->currentText()));
    f->addRow(m_checkInfo);
    m_checkExposure = new QDoubleSpinBox;
    m_checkExposure->setRange(0.01, 600);
    m_checkExposure->setValue(1.0);
    m_checkExposure->setSuffix(QStringLiteral(" s"));
    m_checkScans = new QSpinBox;
    m_checkScans->setRange(1, 1000);
    m_checkScans->setValue(5);
    auto* er = new QHBoxLayout;
    er->addWidget(m_checkExposure, 1);
    er->addWidget(new QLabel(QStringLiteral("Scans")));
    er->addWidget(m_checkScans, 1);
    f->addRow(QStringLiteral("Exposure"), er);
    m_tolerance = new QDoubleSpinBox;
    m_tolerance->setRange(0.1, 50);
    m_tolerance->setDecimals(1);
    m_tolerance->setSuffix(QStringLiteral(" cm⁻¹"));
    m_tolerance->setValue(QSettings().value(QLatin1String(kToleranceKey), 2.0).toDouble());
    m_tolerance->setToolTip(QStringLiteral("Largest acceptable RMS deviation over the matched bands"));
    connect(m_tolerance, &QDoubleSpinBox::valueChanged, this, [this](double t) {
        QSettings().setValue(QLatin1String(kToleranceKey), t);
        showCheck(false);
    });
    f->addRow(QStringLiteral("Tolerance (RMS)"), m_tolerance);
    v->addWidget(b);
    m_btnCheck = new QPushButton(QStringLiteral("▶  CHECK"));
    m_btnCheck->setStyleSheet(theme::bigButtonStyle(theme::Action::Measure));
    m_btnCheck->setMinimumHeight(44);
    connect(m_btnCheck, &QPushButton::clicked, this, [this] { quickCheck(m_checkStd->currentText()); });
    m_btnCheckFile = new QPushButton(QStringLiteral("Check a saved spectrum…"));
    connect(m_btnCheckFile, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Spectrum of %1").arg(m_checkStd->currentText()),
                                                          QString(), QStringLiteral("Spectra (*.csv *.txt);;All files (*)"));
        if (!path.isEmpty())
            quickCheckFile(m_checkStd->currentText(), path);
    });
    v->addWidget(m_btnCheck);
    v->addWidget(m_btnCheckFile);
    b = box(QStringLiteral("Result"));
    auto* rl = new QVBoxLayout(b);
    m_verdict = new QLabel(QStringLiteral("—"));
    m_verdict->setWordWrap(true);
    m_verdict->setTextFormat(Qt::RichText);
    rl->addWidget(m_verdict);
    v->addWidget(b);
    v->addStretch();
    h->addWidget(left);

    auto* split = new QSplitter(Qt::Vertical);
    m_checkPlot = new PlotWidget;
    m_checkPlot->setToolTip(PlotWidget::navigationHelp());
    m_checkPlot->setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹), calibration in use"));
    m_checkPlot->setLabel(Axis::Left, QStringLiteral("Intensity (counts)"));
    m_checkPlot->setHeadroom(0.22);
    split->addWidget(m_checkPlot);
    m_checkTable = table({QStringLiteral("Reference (cm⁻¹)"), QStringLiteral("Measured"), QStringLiteral("Deviation"),
                          QStringLiteral("Strength"), QStringLiteral("Pixel")});
    for (int c = 0; c < 5; ++c)
        m_checkTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Stretch);
    m_checkTable->setMinimumHeight(170);
    split->addWidget(m_checkTable);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    h->addWidget(split, 1);
    return page;
}

QWidget* CalibrationPage::buildHistory() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 6, 0, 0);
    v->addWidget(muted(QStringLiteral(
        "Calibrations of this instrument and channel, newest first. The active one is used for all spectra; "
        "without one the factory calibration of the instrument is used. Files: ") +
        QDir::toNativeSeparators(calib::configDir())));
    auto* split = new QSplitter(Qt::Horizontal);
    m_histTable = table({QStringLiteral(""), QStringLiteral("Date"), QStringLiteral("Standards"), QStringLiteral("Kind"),
                         QStringLiteral("Bands"), QStringLiteral("Degree"), QStringLiteral("RMS (cm⁻¹)")});
    m_histTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_histTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_histTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    for (int c = 3; c < 7; ++c)
        m_histTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_histTable->horizontalHeader()->setStretchLastSection(false);
    connect(m_histTable, &QTableWidget::itemSelectionChanged, this, &CalibrationPage::showHistoryDetails);
    split->addWidget(m_histTable);
    m_histDetails = new QPlainTextEdit;
    m_histDetails->setReadOnly(true);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(9);
    m_histDetails->setFont(mono);
    m_histDetails->setPlaceholderText(QStringLiteral("Select a calibration to see its bands and residuals"));
    split->addWidget(m_histDetails);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    v->addWidget(split, 1);
    auto* row = new QHBoxLayout;
    m_btnActivate = new QPushButton(QStringLiteral("Make active"));
    m_btnActivate->setToolTip(QStringLiteral("Use the selected calibration from now on"));
    connect(m_btnActivate, &QPushButton::clicked, this, [this] { makeActive(m_histTable->currentRow()); });
    m_btnFactory = new QPushButton(QStringLiteral("Revert to factory calibration"));
    m_btnFactory->setToolTip(QStringLiteral("<p>Stop using a user calibration; the history is kept, so it can be made "
                                            "active again later.</p>"));
    connect(m_btnFactory, &QPushButton::clicked, this, &CalibrationPage::confirmRevert);
    auto* open = new QPushButton(QStringLiteral("Open folder"));
    connect(open, &QPushButton::clicked, this, [] {
        QDir().mkpath(calib::configDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(calib::configDir()));
    });
    row->addWidget(m_btnActivate);
    row->addWidget(m_btnFactory);
    row->addStretch();
    row->addWidget(open);
    v->addLayout(row);
    return page;
}

QWidget* CalibrationPage::buildLog() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 6, 0, 0);
    m_logInfo = muted();
    v->addWidget(m_logInfo);
    auto* plots = new QHBoxLayout;
    m_driftPlot = new PlotWidget;
    m_driftPlot->setLabel(Axis::Left, QStringLiteral("Calibration deviation (cm⁻¹)"));
    m_driftPlot->setHeadroom(0.15);
    m_driftPlot->setLegendVisible(true);
    m_driftPlot->setWheelMode(WheelMode::ZoomX);
    m_darkPlot = new PlotWidget;
    m_darkPlot->setLabel(Axis::Left, QStringLiteral("Dark level (counts/s)"));
    m_darkPlot->setHeadroom(0.15);
    m_darkPlot->setWheelMode(WheelMode::ZoomX);
    plots->addWidget(m_driftPlot, 1);
    plots->addWidget(m_darkPlot, 1);
    v->addLayout(plots, 3);
    m_logTable = table({QStringLiteral("Time"), QStringLiteral("Channel"), QStringLiteral("Event"),
                        QStringLiteral("Standards"), QStringLiteral("Bands"), QStringLiteral("Mean dev."),
                        QStringLiteral("RMS dev."), QStringLiteral("Fit RMS"), QStringLiteral("Dark (counts/s)")});
    for (int c = 0; c < 9; ++c)
        m_logTable->horizontalHeader()->setSectionResizeMode(c, c == 3 ? QHeaderView::Stretch : QHeaderView::ResizeToContents);
    m_logTable->horizontalHeader()->setStretchLastSection(false);
    v->addWidget(m_logTable, 2);
    return page;
}

// ============================================================================================ context
void CalibrationPage::setController(Controller* c) {
    if (controller())
        disconnect(controller(), nullptr, this, nullptr);
    Page::setController(c);
    if (!c)
        return;
    connect(c, &Controller::acquired, this, &CalibrationPage::onAcquired);
    connect(c, &Controller::busyChanged, this, [this](bool busy) {
        m_busy = busy;
        if (!busy && !m_pending.isEmpty()) {           // ended without a spectrum (error, NotArmed, Stop)
            m_pending.clear();
            updateSamplesTable();
        }
        updateEnabled();
    });
    connect(c, &Controller::connected, this, &CalibrationPage::onConnected);
    connect(c, &Controller::status, this, [this](const StatusView&) { updateEnabled(); });
    m_busy = c->isBusy();
    updateHeader();
    updateEnabled();
}

void CalibrationPage::activated() {
    updateHeader();
    if (!m_fit)
        updateDetectorPlot();
    refreshHistory();
    refreshLog();
    updateEnabled();
}

lr::Instrument* CalibrationPage::instrument() const {
    Controller* c = controller();
    return c && c->instrument() && c->connection() == Controller::Connection::Connected ? c->instrument() : nullptr;
}

QString CalibrationPage::serial() const {
    if (lr::Instrument* inst = instrument())
        // the simulator must never overwrite the calibration of the real instrument (same built-in serial)
        return inst->simulated() ? QStringLiteral("SIMULATOR") : inst->calibration().serial;
    return lr::defaultCalibration().serial;
}

int CalibrationPage::channel() const {
    lr::Instrument* inst = instrument();
    return inst && inst->channel() > 0 ? inst->channel() : 532;
}

double CalibrationPage::laserNm() const {
    const lr::Calibration& cal = instrument() ? instrument()->calibration() : lr::defaultCalibration();
    const auto it = cal.channels.find(channel());
    return it != cal.channels.end() ? it->second.laser_nm : 532.1;
}

int CalibrationPage::pixelCount() const {
    if (lr::Instrument* inst = instrument())
        return int(inst->pixelNumbers(channel()).size());
    return channel() == 1064 ? 256 : 2048;
}

std::optional<UserCalibration> CalibrationPage::userCalibration() const {
    if (lr::Instrument* inst = instrument()) {
        const auto it = inst->userCalibration().find(channel());
        if (it != inst->userCalibration().end())
            return it->second;
        return std::nullopt;
    }
    try {
        return calib::load(channel(), serial());
    } catch (const lr::Error&) {
        return std::nullopt;
    }
}

bool CalibrationPage::userCalibrationInUse() const { return userCalibration().has_value(); }

calib::ShiftFn CalibrationPage::factoryShift() const {
    const lr::Calibration& cal = instrument() ? instrument()->calibration() : lr::defaultCalibration();
    const auto it = cal.channels.find(channel());
    const ChannelCal c = it != cal.channels.end() ? it->second : ChannelCal{};
    return [c](double px) { return c.shift(px); };
}

calib::ShiftFn CalibrationPage::currentShift() const {
    if (const std::optional<UserCalibration> user = userCalibration()) {
        const Poly p = user->poly;
        return [p](double px) { return p(px); };
    }
    return factoryShift();
}

QString CalibrationPage::activeCalibrationText() const {
    const std::optional<UserCalibration> user = userCalibration();
    if (!user)
        return QStringLiteral("Factory calibration (%1)").arg(instrument() ? instrument()->calibration().source
                                                                           : lr::defaultCalibration().source);
    QStringList st;
    for (const QString& s : user->standards)
        st << s;
    return QStringLiteral("User calibration of %1 (%2), RMS %3 cm⁻¹")
        .arg(user->date, st.join(QStringLiteral(", ")))
        .arg(user->rms, 0, 'f', 2);
}

int CalibrationPage::reminderDays() { return QSettings().value(QLatin1String(kReminderKey), 90).toInt(); }

void CalibrationPage::setReminderDays(int days) {
    QSettings().setValue(QLatin1String(kReminderKey), days);
    if (m_reminder->value() != days)
        m_reminder->setValue(days);
    updateHeader();
}

QString CalibrationPage::reminderText() const {
    const int limit = reminderDays();
    if (limit <= 0)
        return {};
    const std::optional<UserCalibration> user = userCalibration();
    if (!user)
        return QStringLiteral("The %1 nm channel has not been calibrated with LibreRamen yet.").arg(channel());
    const auto days = calstore::daysSince(user->date);
    if (days && *days > limit)
        return QStringLiteral("Last calibration of the %1 nm channel was %2 days ago (reminder after %3 days).")
            .arg(channel()).arg(*days).arg(limit);
    return {};
}

void CalibrationPage::updateHeader() {
    lr::Instrument* inst = instrument();
    if (inst)
        m_hdrInstrument->setText(QStringLiteral("%1 S/N %2 · %3 nm%4")
                                     .arg(controller()->info().model, serial())
                                     .arg(channel())
                                     .arg(inst->simulated() ? QStringLiteral(" · simulator") : QString()));
    else
        m_hdrInstrument->setText(QStringLiteral("No instrument connected · offline (S/N %1, %2 nm)").arg(serial()).arg(channel()));
    m_hdrCal->setText(QStringLiteral("In use: ") + activeCalibrationText());
    m_btnHdrFactory->setVisible(userCalibrationInUse());
    const QString rem = reminderText();
    const bool warn = !rem.isEmpty() && rem.contains(QLatin1String("days ago"));
    m_hdrReminder->setText(rem.isEmpty() ? QString() : (warn ? QStringLiteral("⚠  ") : QStringLiteral("ⓘ  ")) + rem);
    m_hdrReminder->setStyleSheet(QStringLiteral("color: %1; %2")
                                     .arg(warn ? theme::warningText().name() : theme::mutedText().name(),
                                          warn ? QStringLiteral("font-weight: bold;") : QString()));
    m_hdrReminder->setVisible(!rem.isEmpty());
}

void CalibrationPage::onConnected() {
    // the simulator does not load user calibrations itself (Cora::connect does): use its own files
    if (lr::Instrument* inst = instrument(); inst && inst->simulated()) {
        for (int ch : {532, 1064}) {
            try {
                if (const auto c = calib::load(ch, serial())) {
                    inst->setUserCalibration(ch, *c);
                    emit logMessage(QStringLiteral("Simulator: using calibration for %1 nm from %2").arg(ch).arg(c->date));
                }
            } catch (const lr::Error& e) {
                emit logMessage(QString::fromUtf8(e.what()));
            }
        }
    }
    updateHeader();
    refreshHistory();
    refreshLog();
    updateDetectorPlot();
    updateEnabled();
    const QString rem = reminderText();
    if (!rem.isEmpty())                                // after the window's "Instrument ready" line
        QTimer::singleShot(0, this, [this, rem] { emit logMessage(QStringLiteral("Calibration reminder: ") + rem); });
}

void CalibrationPage::updateEnabled() {
    const bool ready = instrument() != nullptr;
    const bool idle = !m_busy;
    const int row = m_samplesTable->currentRow();
    m_btnMeasure->setEnabled(ready && idle && row >= 0);
    m_btnLoad->setEnabled(idle && row >= 0);
    m_btnStop->setEnabled(m_busy);
    int bands = 0;
    for (const QString& n : selectedStandards())
        if (const Sample* s = sample(n))
            bands += int(s->matches.size());
    m_btnFit->setEnabled(idle && bands >= 1);
    {
        const QStringList ticked = selectedStandards();
        int measuredStd = 0;
        for (const QString& n : ticked)
            measuredStd += sample(n) ? 1 : 0;
        if (bands < 1)
            m_btnFit->setText(QStringLiteral("Fit calibration (measure a standard first)"));
        else if (ticked.size() > 1)
            m_btnFit->setText(QStringLiteral("Fit calibration with %1 of %2 standards").arg(measuredStd).arg(ticked.size()));
        else
            m_btnFit->setText(QStringLiteral("Fit calibration"));
    }
    m_degree->setEnabled(m_modePoly->isChecked());
    m_btnAccept->setEnabled(idle && acceptable());
    m_btnReject->setEnabled(m_fit.has_value());
    m_btnCheck->setEnabled(ready && idle);
    m_btnCheckFile->setEnabled(idle);
    const int h = m_histTable->currentRow();
    m_btnActivate->setEnabled(idle && h >= 0 && h < int(m_history.size()) && !m_history[std::size_t(h)].active);
    bool anyActive = false;
    for (const auto& e : m_history)
        anyActive = anyActive || e.active;
    m_btnFactory->setEnabled(idle && (anyActive || userCalibrationInUse()));
    m_btnHdrFactory->setEnabled(idle);
    m_btnFactoryMatch->setEnabled(idle);
    QString instr;
    if (!ready)
        instr = QStringLiteral("No instrument connected: load saved spectra (one row per pixel), or connect the "
                               "instrument to measure.");
    else if (!m_pending.isEmpty())
        instr = QStringLiteral("Measuring %1…").arg(m_pending.section(QLatin1Char(':'), 1));
    else if (row >= 0) {
        int measured = 0;
        for (int i = 0; i < m_samplesTable->rowCount(); ++i)
            measured += sample(m_samplesTable->item(i, 0)->data(Qt::UserRole).toString()) ? 1 : 0;
        const QString name = m_samplesTable->item(row, 0)->data(Qt::UserRole).toString();
        instr = QStringLiteral("<b>Standard %1 of %2</b> (%3 measured). Put in <b>%4</b>, close the compartment, press "
                               "ARM on the instrument, then MEASURE.")
                    .arg(row + 1)
                    .arg(m_samplesTable->rowCount())
                    .arg(measured)
                    .arg(name.toHtmlEscaped());
        if (sample(name))
            instr += QStringLiteral(" (measuring again replaces its spectrum)");
        if (m_samplesTable->rowCount() > 1)
            instr += QStringLiteral("<br>Measure all ticked standards, then Fit: their bands are fitted together "
                                    "in one calibration.");
    }
    else
        instr = QStringLiteral("Tick at least one standard above.");
    m_instructions->setText(instr);
}

void CalibrationPage::message(const QString& title, const QString& text) {
    emit logMessage(title + QStringLiteral(": ") + text);
    if (dialogsEnabled())
        QMessageBox::warning(m_ctx.window ? m_ctx.window : this, title, text);
}

void CalibrationPage::logEvent(calstore::LogEntry e) {
    e.serial = serial();
    if (!e.channel)
        e.channel = channel();
    try {
        calstore::appendLog(e);
    } catch (const lr::Error& err) {
        emit logMessage(QStringLiteral("Instrument log: ") + QString::fromUtf8(err.what()));
    }
    refreshLog();
}

// ============================================================================================ standards
QStringList CalibrationPage::selectedStandards() const {
    QStringList out;
    for (int i = 0; i < m_stdList->count(); ++i)
        if (m_stdList->item(i)->checkState() == Qt::Checked)
            out << m_stdList->item(i)->data(Qt::UserRole).toString();
    return out;
}

void CalibrationPage::setSelectedStandards(const QStringList& names) {
    QStringList canon;
    for (const QString& n : names)
        canon << stdName(n);
    const QSignalBlocker block(m_stdList);
    int first = -1;
    for (int i = 0; i < m_stdList->count(); ++i) {
        const bool on = canon.contains(m_stdList->item(i)->data(Qt::UserRole).toString());
        m_stdList->item(i)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        if (on && first < 0)
            first = i;
    }
    if (first >= 0) {
        m_stdList->setCurrentRow(first);
        m_stdList->scrollToItem(m_stdList->item(first), QAbstractItemView::PositionAtTop);
    }
    updateStandardInfo();
    selectedStandardsChanged();
}

void CalibrationPage::updateStandardInfo() {
    const int row = m_stdList->currentRow();
    if (row < 0) {
        m_stdInfo->clear();
        return;
    }
    const QString key = m_stdList->item(row)->data(Qt::UserRole).toString();
    const standards::Standard& s = standards::find(key);
    QString t = QStringLiteral("<b>%1</b> bands (cm⁻¹; s/m/w = strong/medium/weak): %2")
                    .arg(s.name.toHtmlEscaped(), bandList(s.name));
    if (!s.note.isEmpty())
        t += QStringLiteral("<br><i>%1</i>").arg(s.note.toHtmlEscaped());
    m_stdInfo->setText(t);
}

void CalibrationPage::selectedStandardsChanged() {
    invalidateFit();
    updateSamplesTable();
    const QStringList sel = selectedStandards();
    if (!sel.contains(m_shown))
        showSample(sel.isEmpty() ? QString() : sel.first());
    // a single one-band standard (silicon): one-point mode
    if (sel.size() == 1 && standards::bands(sel.first()).size() == 1)
        m_modeOffset->setChecked(true);
    updateEnabled();
}

void CalibrationPage::updateSamplesTable() {
    const QStringList sel = selectedStandards();
    const QString current = m_samplesTable->currentRow() >= 0 && m_samplesTable->currentRow() < m_samplesTable->rowCount()
                                ? m_samplesTable->item(m_samplesTable->currentRow(), 0)->data(Qt::UserRole).toString()
                                : QString();
    const QSignalBlocker block(m_samplesTable);
    m_samplesTable->setRowCount(int(sel.size()));
    int currentRow = sel.isEmpty() ? -1 : 0;
    for (int i = 0; i < sel.size(); ++i) {
        const QString& n = sel[i];
        auto* it = cell(n);
        it->setData(Qt::UserRole, n);
        m_samplesTable->setItem(i, 0, it);
        const Sample* s = sample(n);
        QString origin = s ? s->origin : QStringLiteral("—");
        if (m_pending == kCalTag + n)
            origin = QStringLiteral("measuring…");
        m_samplesTable->setItem(i, 1, cell(origin));
        const QString bands = s ? QStringLiteral("%1 / %2").arg(s->matches.size()).arg(standards::bands(n).size())
                                : QStringLiteral("— / %1").arg(standards::bands(n).size());
        auto* bi = cell(bands, true);
        if (s && !enoughBands(n, s->matches, factoryShift(), pixelCount())) {
            bi->setForeground(theme::warningText());
            bi->setToolTip(QStringLiteral("Fewer than ⅔ of the bands found"));
        } else if (s && s->factoryStart && userCalibrationInUse()) {
            bi->setToolTip(QStringLiteral("Matched against the factory calibration"));
        }
        m_samplesTable->setItem(i, 2, bi);
        if (n == current)
            currentRow = i;
    }
    if (currentRow >= 0)
        m_samplesTable->setCurrentCell(currentRow, 0);
    highlightCurrentRow();
}

void CalibrationPage::highlightCurrentRow() {
    // the row MEASURE / Load file apply to: "▶ name" in bold
    const int cur = m_samplesTable->currentRow();
    const QSignalBlocker block(m_samplesTable);
    for (int i = 0; i < m_samplesTable->rowCount(); ++i) {
        QTableWidgetItem* it = m_samplesTable->item(i, 0);
        if (!it)
            continue;
        const QString name = it->data(Qt::UserRole).toString();
        it->setText(i == cur ? QStringLiteral("▶ ") + name : name);
        QFont f = it->font();
        f.setBold(i == cur);
        it->setFont(f);
    }
}

void CalibrationPage::selectNextUnmeasured() {
    const int n = m_samplesTable->rowCount();
    const int cur = std::max(0, m_samplesTable->currentRow());
    for (int k = 1; k <= n; ++k) {
        const int i = (cur + k) % n;
        if (!sample(m_samplesTable->item(i, 0)->data(Qt::UserRole).toString())) {
            const QSignalBlocker block(m_samplesTable);   // keep showing the spectrum just measured
            m_samplesTable->setCurrentCell(i, 0);
            break;
        }
    }
    highlightCurrentRow();
}

const CalibrationPage::Sample* CalibrationPage::sample(const QString& name) const {
    const QString n = stdName(name);
    for (const Sample& s : m_samples)
        if (s.standard == n)
            return &s;
    return nullptr;
}

void CalibrationPage::matchSample(Sample& s) {
    const std::vector<calib::PixelPeak> peaks = calib::findPeaks(s.y);
    const bool user = userCalibrationInUse();
    s.factoryStart = start() == Start::Factory || !user;
    s.retried = false;
    s.matches = calib::match(peaks, s.factoryStart ? factoryShift() : currentShift(), s.standard);
    if (!s.factoryStart && m_autoRetry->isChecked() && !enoughBands(s.standard, s.matches, factoryShift(), pixelCount())) {
        std::vector<calib::Match> f = calib::match(peaks, factoryShift(), s.standard);
        const std::size_t before = s.matches.size();
        if (f.size() > before) {
            s.matches = std::move(f);
            s.factoryStart = true;
            s.retried = true;
            emit logMessage(QStringLiteral("Calibration: only %1 of %2 %3 bands found with the calibration in use – "
                                           "matched against the factory calibration instead: %4 found")
                                .arg(before).arg(standards::bands(s.standard).size()).arg(s.standard)
                                .arg(s.matches.size()));
        }
    }
}

void CalibrationPage::rematchAll() {
    for (Sample& s : m_samples)
        matchSample(s);
    const bool had = m_fit.has_value();
    invalidateFit();
    updateSamplesTable();
    showSample(m_shown);
    if (had)
        fit();
    updateEnabled();
}

void CalibrationPage::setStart(Start st) { m_start->setCurrentIndex(st == Start::Factory ? 1 : 0); }

CalibrationPage::Start CalibrationPage::start() const {
    return m_start->currentIndex() == 1 ? Start::Factory : Start::InUse;
}

void CalibrationPage::setAutoFactoryRetry(bool on) { m_autoRetry->setChecked(on); }

bool CalibrationPage::matchAgainstFactory() {
    if (m_busy)
        return false;
    {
        const QSignalBlocker block(m_start);
        m_start->setCurrentIndex(1);
    }
    emit logMessage(QStringLiteral("Calibration: matching the bands against the factory calibration"));
    const bool hadFit = m_fit.has_value();
    rematchAll();                                      // refits if there was a fit
    if (hadFit && !m_fit)
        fit();
    return m_fit.has_value();
}

void CalibrationPage::addSample(Sample s) {
    matchSample(s);
    const auto it = std::find_if(m_samples.begin(), m_samples.end(),
                                 [&](const Sample& old) { return old.standard == s.standard; });
    if (it != m_samples.end())
        *it = s;
    else
        m_samples.push_back(s);
    invalidateFit();
    updateSamplesTable();
    showSample(s.standard);
    selectNextUnmeasured();
    updateEnabled();
    emit logMessage(QStringLiteral("Calibration: %1 – %2 of %3 reference bands found%4")
                        .arg(s.standard).arg(s.matches.size()).arg(standards::bands(s.standard).size())
                        .arg(s.factoryStart && userCalibrationInUse() ? QStringLiteral(" (against the factory calibration)")
                                                                      : QString()));
    emit sampleReady(s.standard);
}

// ============================================================================================ measuring
MeasureSettings CalibrationPage::measureSettings(const QString& standard, double exposure, int scans) const {
    MeasureSettings s;
    s.sample = standard;
    s.exposure_s = exposure;
    s.averages = scans;
    s.background = QStringLiteral("each");
    s.channel = 0;                                     // the channel in use
    return s;
}

void CalibrationPage::prepareSimulator(const QString& standard) {
    // demo: the simulator holds the standard if it has a recording of it (as the CLI does)
    lr::SimControls* sim = controller() ? controller()->simControls() : nullptr;
    if (!sim || !sim->sampleNames().contains(standard) || sim->sample() == standard)
        return;
    if (auto* win = qobject_cast<MainWindow*>(m_ctx.window); win && win->simulatorSample())
        win->simulatorSample()->setCurrentText(standard);   // keeps the simulator bar in step
    else
        controller()->simSetSample(standard);
    emit logMessage(QStringLiteral("Simulator: sample set to %1").arg(standard));
}

bool CalibrationPage::measureStandard(const QString& name0) {
    const QString name = stdName(name0);
    if (!instrument()) {
        message(QStringLiteral("Calibration"), QStringLiteral("No instrument connected."));
        return false;
    }
    if (m_busy || !m_pending.isEmpty())
        return false;
    prepareSimulator(name);
    m_pending = kCalTag + name;
    if (!controller()->acquire(measureSettings(name, m_exposure->value(), m_scans->value()), m_pending)) {
        m_pending.clear();
        return false;
    }
    m_busy = true;
    updateSamplesTable();
    updateEnabled();
    return true;
}

QString CalibrationPage::saveSpectrum(const lr::Spectrum& sp, const QString& standard) const {
    const QString dir = QDir(calib::configDir()).filePath(QStringLiteral("calibration-spectra"));
    QDir().mkpath(dir);
    const QString path = Controller::savePath(dir, standard + QLatin1Char('_') + serial(), sp.t);
    sp.saveCsv(path);
    return path;
}

void CalibrationPage::onAcquired(const lr::Spectrum& sp0, const QString& tag) {
    if (tag != m_pending)
        return;
    m_pending.clear();
    lr::Spectrum sp = sp0;
    const bool check = tag.startsWith(kCheckTag);
    const QString name = tag.section(QLatin1Char(':'), 1);
    sp.setMeta(QStringLiteral("sample"), name);
    sp.setMeta(QStringLiteral("purpose"), check ? QStringLiteral("calibration check") : QStringLiteral("calibration"));
    Sample s;
    s.standard = name;
    s.y = pixelOrder(sp);
    s.dark = mean(sp.background);
    s.exposure = sp.exposure_s;
    s.origin = QStringLiteral("measured ") + sp.t.toString(QStringLiteral("HH:mm"));
    try {
        s.file = saveSpectrum(sp, name);
        emit logMessage(QStringLiteral("Saved ") + QDir::toNativeSeparators(s.file));
    } catch (const lr::Error& e) {
        emit logMessage(QStringLiteral("Calibration spectrum not saved: ") + QString::fromUtf8(e.what()));
    }
    if (check) {
        m_checkSample = std::move(s);
        m_checkSample->matches = calib::match(calib::findPeaks(m_checkSample->y), currentShift(), name);
        showCheck(true);
        return;
    }
    addSample(std::move(s));
}

std::optional<CalibrationPage::Sample> CalibrationPage::readSampleFile(const QString& name, const QString& path) {
    try {
        std::vector<fileio::DataSpectrum> all = fileio::read(path);
        if (all.empty())
            throw lr::Error("no spectrum in the file");
        fileio::DataSpectrum& d = all.front();
        d.sorted();
        const int n = pixelCount();
        if (int(d.y.size()) != n)
            throw lr::Error(QStringLiteral("%1 points, but the %2 nm detector has %3 pixels: calibration needs the "
                                           "spectrum as measured (one row per pixel, e.g. a LibreRamen CSV).")
                                .arg(d.y.size()).arg(channel()).arg(n).toStdString());
        Sample s;
        s.standard = name;
        s.y = d.y;
        s.dark = mean(d.background);
        s.exposure = d.metaValue(QStringLiteral("exposure_s")).toDouble();
        s.origin = QFileInfo(path).fileName();
        s.file = QFileInfo(path).absoluteFilePath();
        return s;
    } catch (const std::exception& e) {
        message(QStringLiteral("Calibration"), QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), QString::fromUtf8(e.what())));
        return std::nullopt;
    }
}

bool CalibrationPage::loadStandardFile(const QString& name0, const QString& path) {
    const QString name = stdName(name0);
    if (!selectedStandards().contains(name)) {
        QStringList sel = selectedStandards();
        sel << name;
        setSelectedStandards(sel);
    }
    auto s = readSampleFile(name, path);
    if (!s)
        return false;
    addSample(std::move(*s));
    return true;
}

// ============================================================================================ plots
void CalibrationPage::drawSpectrum(PlotWidget* plot, const Sample& s, const calib::ShiftFn& shift, bool showBands) {
    plot->clear();
    const PlotPalette& pal = plot->plotPalette();
    Vec x(s.y.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = shift(double(i));
    plot->addCurve(x, s.y, QPen(pal.main, 1.4), s.standard);
    Bounds xb;
    QPen ref(pal.reference, 1, Qt::DashLine);
    QColor refc = pal.reference;
    refc.setAlpha(150);
    ref.setColor(refc);
    if (showBands) {
        for (const standards::Band& b : standards::bands(s.standard)) {
            InfiniteLine* l = plot->addVLine(b.position, ref);
            l->setLabel(QString::number(b.position, 'f', 0));
            l->setAutoScale(false);
            xb.add(b.position);
        }
    }
    Vec mx, my;
    for (const calib::Match& m : s.matches) {
        const auto i = std::size_t(std::clamp(std::lround(m.pixel), 0L, long(s.y.size()) - 1));
        mx.push_back(shift(m.pixel));
        my.push_back(s.y[i]);
    }
    if (!mx.empty()) {
        Curve* c = plot->addScatter(mx, my, pal.seriesColor(1), 9, Marker::Diamond);
        c->setName(QStringLiteral("matched peaks"));
        c->setZValue(5);
    }
    // x range: the reference bands with some margin (where calibration happens), else everything
    if (xb.valid()) {
        const double pad = std::max(60.0, 0.08 * (xb.hi - xb.lo));
        plot->setHomeXRange(std::max(x.front(), xb.lo - pad), std::min(x.back(), xb.hi + pad));
        plot->autoRange();
    } else if (!x.empty()) {
        plot->clearHomeXRange();
        plot->autoRange();
    }
}

void CalibrationPage::showSample(const QString& name) {
    m_shown = name;
    const Sample* s = sample(name);
    if (!s) {
        m_plot->clear();
        if (!name.isEmpty()) {                         // show where the bands will be
            for (const standards::Band& b : standards::bands(name)) {
                QPen ref(m_plot->plotPalette().reference, 1, Qt::DashLine);
                m_plot->addVLine(b.position, ref)->setLabel(QString::number(b.position, 'f', 0));
            }
        }
        return;
    }
    // x axis: the calibration the bands were matched against (there the matched peaks sit next to their dashed
    // reference lines), or the new fit
    calib::ShiftFn shift = s->factoryStart ? factoryShift() : currentShift();
    QString axis = s->factoryStart && userCalibrationInUse() ? QStringLiteral("factory calibration (matched against it)")
                                                             : QStringLiteral("calibration in use");
    if (m_fit && m_newAxis->isChecked()) {
        const Poly p = m_fit->poly;
        shift = [p](double px) { return p(px); };
        axis = QStringLiteral("new calibration");
    }
    m_axisNote->setText(QStringLiteral("Dashed lines: reference bands · ◆ matched peaks. x axis: %1.").arg(axis));
    drawSpectrum(m_plot, *s, shift, true);
}

void CalibrationPage::updateResiduals() {
    m_resPlot->clear();
    m_resTable->setRowCount(0);
    const PlotPalette& pal = m_resPlot->plotPalette();
    m_resPlot->addHLine(0, QPen(pal.reference, 1));
    if (m_before.empty())
        return;
    Vec bx, by, ax, ay;
    m_resTable->setRowCount(int(m_before.size()));
    std::vector<calstore::Deviation> rows = m_before;
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.reference < b.reference; });
    for (int i = 0; i < int(rows.size()); ++i) {
        const calstore::Deviation& d = rows[std::size_t(i)];
        bx.push_back(d.reference);
        by.push_back(d.deviation());
        m_resTable->setItem(i, 0, cell(d.standard));
        m_resTable->setItem(i, 1, cell(QString::number(d.reference, 'f', 1), true));
        m_resTable->setItem(i, 2, cell(signedNum(d.deviation()), true));
        if (m_fit) {
            const double after = m_fit->poly(d.pixel) - d.reference;
            ax.push_back(d.reference);
            ay.push_back(after);
            auto* it = cell(signedNum(after), true);
            QFont f = it->font();
            f.setBold(true);
            it->setFont(f);
            m_resTable->setItem(i, 3, it);
        }
        m_resTable->setItem(i, 4, cell(QString::number(d.pixel, 'f', 1), true));
    }
    Curve* before = m_resPlot->addScatter(bx, by, pal.previous, 7, Marker::Circle);
    before->setName(QStringLiteral("calibration in use"));
    if (!ax.empty()) {
        Curve* after = m_resPlot->addScatter(ax, ay, pal.seriesColor(1), 8, Marker::Diamond);
        after->setName(QStringLiteral("new fit"));
    }
    // y range: the new fit's residuals, and the deviations of the calibration in use up to 20 cm-1 (a wrong one
    // can be off by thousands; those points are clipped)
    double maxBefore = 0, maxAfter = 0;
    for (double v : by)
        maxBefore = std::max(maxBefore, std::abs(v));
    for (double v : ay)
        maxAfter = std::max(maxAfter, std::abs(v));
    const double span = std::max({1.0, maxAfter, ay.empty() ? maxBefore : std::min(maxBefore, 20.0)});
    m_resPlot->setAutoY(false);
    m_resPlot->setYRange(-span * 1.3, span * 1.3);
    const auto [lo, hi] = std::minmax_element(bx.begin(), bx.end());
    const double pad = std::max(50.0, (*hi - *lo) * 0.06);
    m_resPlot->setHomeXRange(*lo - pad, *hi + pad);
    m_resPlot->setXRange(*lo - pad, *hi + pad);
}

// ============================================================================================ fit
void CalibrationPage::setMode(Mode m) { (m == Mode::Offset ? m_modeOffset : m_modePoly)->setChecked(true); }

CalibrationPage::Mode CalibrationPage::mode() const { return m_modeOffset->isChecked() ? Mode::Offset : Mode::Polynomial; }

void CalibrationPage::setDegree(int degree) { m_degree->setCurrentIndex(std::clamp(degree, 0, 3)); }

void CalibrationPage::invalidateFit() {
    const bool had = m_fit.has_value();
    m_fit.reset();
    m_before.clear();
    m_offset = 0;
    m_newAxis->setChecked(false);
    m_newAxis->setEnabled(false);
    m_fitLabel->setText(QStringLiteral("—"));
    m_rules.clear();
    m_offerFactory = false;
    showRules();
    if (had)
        showSample(m_shown);
    updateResiduals();
    updateDetectorPlot();
}

bool CalibrationPage::fit() {
    // The new calibration is always a correction to the FACTORY calibration (calib::fit with the factory shift as
    // base): a calibration in use that is wrong can only affect which peaks are matched, never the shape of the
    // new curve, and the stored polynomial is factory + correction over the whole detector.
    const calib::ShiftFn factory = factoryShift();
    const calib::ShiftFn inUse = currentShift();
    const int npix = pixelCount();
    calib::Result r;
    std::vector<calib::Match> all;
    QStringList missing;
    for (const QString& n : selectedStandards()) {
        const Sample* s = sample(n);
        if (!s) {
            missing << n;
            continue;
        }
        r.matches.emplace_back(n, s->matches);
        r.standards.push_back(n);
        all.insert(all.end(), s->matches.begin(), s->matches.end());
    }
    if (!missing.isEmpty() && !r.standards.empty()) {
        const QString text = QStringLiteral("No spectrum yet for %1 (ticked): the fit uses only %2.")
                                 .arg(missing.join(QStringLiteral(", ")),
                                      QStringList(r.standards.begin(), r.standards.end()).join(QStringLiteral(", ")));
        emit logMessage(QStringLiteral("Calibration: ") + text);
        if (dialogsEnabled() &&
            QMessageBox::question(m_ctx.window ? m_ctx.window : this, QStringLiteral("Calibration"),
                                  text + QStringLiteral("\n\nFit anyway?")) != QMessageBox::Yes)
            return false;
    }
    invalidateFit();
    if (all.empty()) {
        message(QStringLiteral("Calibration"), QStringLiteral("No reference band was found in the spectra. Check the "
                                                              "sample and the signal (exposure, scans)."));
        return false;
    }
    try {
        if (mode() == Mode::Offset) {
            // one-point calibration: the factory calibration shifted by a constant = the strength-weighted mean
            // deviation of the bands (a degree-0 correction; equivalent to a corrected laser wavelength)
            double sw = 0, so = 0;
            for (const calib::Match& m : all) {
                sw += m.strength;
                so += m.strength * (m.reference - factory(m.pixel));
            }
            m_offset = so / sw;
            const calib::FitOutput f = calib::fit(all, 0, factory, npix);
            r.poly = f.poly;
            r.residuals = f.residuals;
            r.rms = f.rms;
            r.degree = 0;
            m_fitKind = QStringLiteral("offset");
        } else {
            const int deg = m_degree->currentIndex();
            if (deg > 0 && int(all.size()) <= deg)
                throw lr::Error(QStringLiteral("a degree-%1 correction needs at least %2 bands (%3 matched)")
                                    .arg(deg).arg(deg + 1).arg(all.size()).toStdString());
            const calib::FitOutput f = calib::fit(all, deg > 0 ? std::optional<int>(deg) : std::nullopt, factory, npix);
            r.poly = f.poly;
            r.residuals = f.residuals;
            r.rms = f.rms;
            r.degree = f.degree;
            m_fitKind = QStringLiteral("polynomial");
        }
    } catch (const lr::Error& e) {
        message(QStringLiteral("Calibration"), QStringLiteral("Fit failed: ") + QString::fromUtf8(e.what()));
        updateEnabled();
        return false;
    }
    r.date = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    const calstore::CheckResult before = calstore::check(r.matches, inUse);
    m_before = before.bands;
    m_fit = r;
    if (m_fitKind == QLatin1String("offset")) {
        m_fitLabel->setText(QStringLiteral("<b>Offset %1 cm⁻¹</b> to the factory calibration (%2 band(s), RMS %3 cm⁻¹)"
                                           "<br>= laser wavelength %4 nm instead of %5 nm")
                                .arg(signedNum(m_offset), QString::number(all.size()))
                                .arg(r.rms, 0, 'f', 2)
                                .arg(calstore::equivalentLaserNm(laserNm(), m_offset), 0, 'f', 3)
                                .arg(laserNm(), 0, 'f', 3));
    } else {
        QString kind = calib::degreeName(r.degree);
        kind[0] = kind[0].toUpper();
        const QString t = QStringLiteral("<b>%1 correction to the factory calibration</b> · %2 bands · RMS %3 cm⁻¹ "
                                         "(calibration in use: RMS %4 cm⁻¹)")
                        .arg(kind)
                        .arg(all.size())
                        .arg(r.rms, 0, 'f', 2)
                        .arg(before.rms, 0, 'f', 2);
        m_fitLabel->setText(t);
    }
    m_newAxis->setEnabled(true);
    evaluateRules();
    showRules();
    updateResiduals();
    updateDetectorPlot();
    updateEnabled();
    if (m_fitKind == QLatin1String("offset"))
        emit logMessage(QStringLiteral("Calibration fit (one-point offset): %1 cm-1 from %2 band(s), RMS %3 cm-1")
                            .arg(signedNum(m_offset)).arg(all.size()).arg(r.rms, 0, 'f', 2));
    else
        emit logMessage(QStringLiteral("Calibration fit: ") + calib::report(r, inUse).section(QLatin1Char('\n'), 0, 0));
    for (const Rule& rule : m_rules)
        if (rule.state == Rule::Fail)
            emit logMessage(QStringLiteral("Calibration check failed: ") + rule.text);
    return true;
}

void CalibrationPage::evaluateRules() {
    m_rules.clear();
    m_offerFactory = false;
    if (!m_fit || m_fit->residuals.empty())
        return;
    const calib::ShiftFn factory = factoryShift();
    const int npix = pixelCount();
    const bool user = userCalibrationInUse();
    // (a) per standard: >= 2/3 of the bands within the detector range, and every strong band
    for (const auto& [name, matches] : m_fit->matches) {
        const BandNeed need = bandNeed(name, factory, npix);
        QStringList lost;
        for (double pos : need.strong)
            if (std::none_of(matches.begin(), matches.end(), [pos](const calib::Match& m) { return m.reference == pos; }))
                lost << QString::number(pos, 'f', pos == std::floor(pos) ? 0 : 1);
        const Sample* s = sample(name);
        const bool fromInUse = s && !s->factoryStart && user;
        if (int(matches.size()) < need.need) {
            m_rules.push_back({Rule::Fail, QStringLiteral("Only %1 of %2 %3 bands found — %4")
                                               .arg(matches.size()).arg(need.total).arg(name,
                                                    fromInUse ? QStringLiteral("the calibration in use may be wrong")
                                                              : QStringLiteral("check the sample and the signal "
                                                                               "(exposure, scans)"))});
            m_offerFactory = m_offerFactory || fromInUse;
        } else if (!lost.isEmpty()) {
            m_rules.push_back({Rule::Fail, QStringLiteral("%1: strong band %2 cm⁻¹ not found").arg(name, lost.join(QStringLiteral(", ")))});
            m_offerFactory = m_offerFactory || fromInUse;
        } else {
            m_rules.push_back({Rule::Pass, QStringLiteral("%1: %2 of %3 bands found, all strong ones%4")
                                               .arg(name).arg(matches.size()).arg(need.total)
                                               .arg(s && s->retried ? QStringLiteral(" (matched against the factory calibration)")
                                                                    : QString())});
        }
    }
    // (b) coverage
    double rlo = 1e300, rhi = -1e300, plo = 1e300, phi = -1e300;
    for (const calib::Residual& r : m_fit->residuals) {
        rlo = std::min(rlo, r.reference);
        rhi = std::max(rhi, r.reference);
        plo = std::min(plo, r.pixel);
        phi = std::max(phi, r.pixel);
    }
    const double cover = 100.0 * (phi - plo) / double(std::max(1, npix - 1));
    QString cov = QStringLiteral("Bands cover %1–%2 cm⁻¹ (pixels %3–%4, %5 % of the detector); %6")
                      .arg(rlo, 0, 'f', 0).arg(rhi, 0, 'f', 0).arg(plo, 0, 'f', 0).arg(phi, 0, 'f', 0)
                      .arg(cover, 0, 'f', 0)
                      .arg(m_fit->degree == 0 ? QStringLiteral("the offset applies to the whole detector")
                                              : QStringLiteral("outside this range the correction is extrapolated linearly"));
    if (cover < 25.0 && m_fitKind == QLatin1String("polynomial"))
        cov += m_fit->degree > 0 ? QStringLiteral(" — narrow coverage: the one-point offset is recommended")
                                 : QStringLiteral(" — too narrow for more than an offset (add a standard with bands "
                                                  "elsewhere for a polynomial correction)");
    m_rules.push_back({Rule::Info, cov});
    // (c) whole detector: no runaway extrapolation, no wild departure from the factory calibration
    double maxAbs = 0, extrap = 0, dmin = 1e300, dmax = -1e300;
    const double dlo = m_fit->poly(plo) - factory(plo), dhi = m_fit->poly(phi) - factory(phi);
    for (int p = 0; p < npix; ++p) {
        const double d = m_fit->poly(p) - factory(p);
        maxAbs = std::max(maxAbs, std::abs(d));
        dmin = std::min(dmin, d);
        dmax = std::max(dmax, d);
        if (p < plo)
            extrap = std::max(extrap, std::abs(d - dlo));
        else if (p > phi)
            extrap = std::max(extrap, std::abs(d - dhi));
    }
    if (extrap > kMaxExtrapolation)
        m_rules.push_back({Rule::Fail, QStringLiteral("Beyond the bands the correction changes by %1 cm⁻¹ (limit %2): "
                                                      "extrapolated too far — use the offset or add a standard with "
                                                      "bands elsewhere")
                                           .arg(extrap, 0, 'f', 1).arg(kMaxExtrapolation, 0, 'f', 0)});
    else if (maxAbs > kMaxFromFactory)
        m_rules.push_back({Rule::Fail, QStringLiteral("New − factory reaches %1 cm⁻¹ (limit %2) — check the matched bands")
                                           .arg(maxAbs, 0, 'f', 1).arg(kMaxFromFactory, 0, 'f', 0)});
    else
        m_rules.push_back({Rule::Pass, QStringLiteral("Whole detector: new − factory %1 … %2 cm⁻¹; beyond the bands it "
                                                      "changes by ≤ %3 cm⁻¹ (limit %4)")
                                           .arg(signedNum(dmin, 1), signedNum(dmax, 1))
                                           .arg(extrap, 0, 'f', 1).arg(kMaxExtrapolation, 0, 'f', 0)});
    // (d) residuals
    const int n = int(m_fit->residuals.size());
    if (n == 1)
        m_rules.push_back({Rule::Info, QStringLiteral("One band: no residual check (offset only)")});
    else if (n <= m_fit->degree + 1)
        m_rules.push_back({Rule::Info, QStringLiteral("%1 bands for a %2 correction: exact fit, residuals not checked")
                                           .arg(n).arg(calib::degreeName(m_fit->degree))});
    else if (m_fit->rms > kMaxRms)
        m_rules.push_back({Rule::Fail, QStringLiteral("Residual RMS %1 cm⁻¹ (limit %2) — a band may be mismatched")
                                           .arg(m_fit->rms, 0, 'f', 2).arg(kMaxRms, 0, 'f', 1)});
    else
        m_rules.push_back({Rule::Pass, QStringLiteral("Residual RMS %1 cm⁻¹ (limit %2)")
                                           .arg(m_fit->rms, 0, 'f', 2).arg(kMaxRms, 0, 'f', 1)});
}

void CalibrationPage::showRules() {
    m_btnFactoryMatch->setVisible(m_fit && m_offerFactory);
    if (m_rules.empty()) {
        m_checklist->clear();
        m_checklist->setVisible(false);
        return;
    }
    const QString green = theme::toneColor(theme::Tone::Green).name(), warn = theme::warningText().name(),
                  mutedc = theme::mutedText().name();
    QString html = QStringLiteral("<table cellspacing='0' cellpadding='1'>");
    for (const Rule& r : m_rules) {
        const QString icon = r.state == Rule::Pass ? QStringLiteral("✓") : r.state == Rule::Fail ? QStringLiteral("✗")
                                                                                                   : QStringLiteral("ⓘ");
        const QString col = r.state == Rule::Pass ? green : r.state == Rule::Fail ? warn : mutedc;
        html += QStringLiteral("<tr><td style='color:%1; font-weight:bold; padding-right:4px'>%2</td><td%3>%4</td></tr>")
                    .arg(col, icon, r.state == Rule::Fail ? QStringLiteral(" style='color:%1'").arg(warn) : QString(),
                         r.text.toHtmlEscaped());
    }
    html += QStringLiteral("</table>");
    if (!acceptable())
        html += QStringLiteral("<span style='color:%1; font-weight:bold'>Accept is disabled until every check passes."
                               "</span>").arg(warn);
    m_checklist->setText(html);
    m_checklist->setVisible(true);
}

bool CalibrationPage::acceptable() const {
    return m_fit.has_value() &&
           std::none_of(m_rules.begin(), m_rules.end(), [](const Rule& r) { return r.state == Rule::Fail; });
}

void CalibrationPage::updateDetectorPlot() {
    m_devPlot->clear();
    const PlotPalette& pal = m_devPlot->plotPalette();
    m_devPlot->addHLine(0, QPen(pal.reference, 1));
    const int npix = pixelCount();
    const calib::ShiftFn factory = factoryShift();
    Vec x(std::size_t(npix), 0.0), inUse, fresh;
    for (int p = 0; p < npix; ++p)
        x[std::size_t(p)] = p;
    double span = 5.0;
    if (userCalibrationInUse()) {
        const calib::ShiftFn cur = currentShift();
        for (int p = 0; p < npix; ++p)
            inUse.push_back(cur(p) - factory(p));
        QColor c = pal.previous;
        c.setAlpha(220);
        m_devPlot->addCurve(x, inUse, QPen(c, 1.3, Qt::DashLine), QStringLiteral("calibration in use"));
        if (!m_fit)
            for (double v : inUse)
                span = std::max(span, 1.3 * std::abs(v));
    }
    if (m_fit) {
        double plo = 1e300, phi = -1e300;
        for (const calib::Residual& r : m_fit->residuals) {
            plo = std::min(plo, r.pixel);
            phi = std::max(phi, r.pixel);
        }
        RegionItem* reg = m_devPlot->addRegion(plo, std::max(phi, plo + 2));
        reg->setMovable(false);
        for (int p = 0; p < npix; ++p) {
            fresh.push_back(m_fit->poly(p) - factory(p));
            span = std::max(span, 1.6 * std::abs(fresh.back()));
        }
        Curve* c = m_devPlot->addCurve(x, fresh, QPen(pal.seriesColor(1), 2.0), QStringLiteral("new fit"));
        c->setZValue(5);
    }
    m_devPlot->setAutoY(false);
    m_devPlot->setYRange(-span, span);
    m_devPlot->setHomeXRange(0, npix - 1);
    m_devPlot->setXRange(0, npix - 1);
}

bool CalibrationPage::acceptCalibration() {
    if (!m_fit)
        return false;
    if (!acceptable()) {
        QStringList failed;
        for (const Rule& r : m_rules)
            if (r.state == Rule::Fail)
                failed << r.text;
        message(QStringLiteral("Calibration"), QStringLiteral("Not accepted: ") + failed.join(QStringLiteral("; ")));
        return false;
    }
    if (m_busy) {
        message(QStringLiteral("Calibration"), QStringLiteral("Stop the running measurement first."));
        return false;
    }
    QStringList files;
    double dark = 0, exposure = qQNaN();
    int nd = 0;
    for (const QString& n : m_fit->standards)
        if (const Sample* s = sample(n)) {
            if (!s->file.isEmpty())
                files << s->file;
            if (std::isfinite(s->dark) && s->exposure > 0) {   // per second: comparable between exposures
                dark += s->dark / s->exposure;
                ++nd;
            }
            exposure = s->exposure;
        }
    try {
        m_lastAccepted = calstore::accept(*m_fit, channel(), serial(), m_fitKind, files,
                                          m_fitKind == QLatin1String("offset") ? calstore::equivalentLaserNm(laserNm(), m_offset) : 0.0);
    } catch (const lr::Error& e) {
        message(QStringLiteral("Calibration"), QString::fromUtf8(e.what()));
        return false;
    }
    if (lr::Instrument* inst = instrument())
        inst->setUserCalibration(channel(), m_fit->userCalibration());
    calstore::LogEntry e;
    e.event = QStringLiteral("calibration");
    for (const QString& s : m_fit->standards)
        e.standards << s;
    e.bands = int(m_before.size());
    double sm = 0, ss = 0;
    for (const auto& d : m_before) {
        sm += d.deviation();
        ss += d.deviation() * d.deviation();
    }
    if (!m_before.empty()) {
        e.devMean = sm / double(m_before.size());
        e.devRms = std::sqrt(ss / double(m_before.size()));
    }
    e.fitRms = m_fit->rms;
    e.degree = m_fit->degree;
    e.dark = nd ? dark / nd : qQNaN();
    e.exposure = exposure;
    e.note = m_fitKind;
    logEvent(e);
    emit logMessage(QStringLiteral("Calibration accepted for %1 nm (%2, RMS %3 cm⁻¹); saved %4")
                        .arg(channel())
                        .arg(m_fitKind)
                        .arg(m_fit->rms, 0, 'f', 2)
                        .arg(QDir::toNativeSeparators(m_lastAccepted)));
    m_samples.clear();                                 // matched against the old calibration
    invalidateFit();
    updateSamplesTable();
    showSample(m_shown);
    updateHeader();
    refreshHistory();
    updateEnabled();
    emit calibrationChanged();
    return true;
}

void CalibrationPage::rejectCalibration() {
    if (m_fit)
        emit logMessage(QStringLiteral("Calibration rejected; the calibration in use is unchanged"));
    invalidateFit();
    updateEnabled();
}

// ============================================================================================ quick check
bool CalibrationPage::quickCheck(const QString& standard0) {
    const QString name = stdName(standard0);
    if (!instrument()) {
        message(QStringLiteral("Quick check"), QStringLiteral("No instrument connected."));
        return false;
    }
    if (m_busy || !m_pending.isEmpty())
        return false;
    m_checkStd->setCurrentText(name);
    prepareSimulator(name);
    m_pending = kCheckTag + name;
    if (!controller()->acquire(measureSettings(name, m_checkExposure->value(), m_checkScans->value()), m_pending)) {
        m_pending.clear();
        return false;
    }
    m_busy = true;
    m_verdict->setText(QStringLiteral("Measuring %1…").arg(name.toHtmlEscaped()));
    updateEnabled();
    return true;
}

bool CalibrationPage::quickCheckFile(const QString& standard0, const QString& path) {
    const QString name = stdName(standard0);
    m_checkStd->setCurrentText(name);
    auto s = readSampleFile(name, path);
    if (!s)
        return false;
    s->matches = calib::match(calib::findPeaks(s->y), currentShift(), name);
    m_checkSample = std::move(s);
    showCheck(true);
    return true;
}

QString CalibrationPage::checkVerdict() const {
    if (!m_check)
        return {};
    if (m_check->bands.empty())
        return QStringLiteral("no bands");
    return m_check->rms <= m_tolerance->value() ? QStringLiteral("ok") : QStringLiteral("recalibrate");
}

void CalibrationPage::showCheck(bool record) {
    if (!m_checkSample)
        return;
    const Sample& s = *m_checkSample;
    m_check = calstore::check({{s.standard, s.matches}}, currentShift());
    drawSpectrum(m_checkPlot, s, currentShift(), true);
    m_checkTable->setRowCount(int(m_check->bands.size()));
    for (int i = 0; i < int(m_check->bands.size()); ++i) {
        const calstore::Deviation& d = m_check->bands[std::size_t(i)];
        m_checkTable->setItem(i, 0, cell(QString::number(d.reference, 'f', 1), true));
        m_checkTable->setItem(i, 1, cell(QString::number(d.measured, 'f', 1), true));
        auto* dev = cell(signedNum(d.deviation()), true);
        if (std::abs(d.deviation()) > m_tolerance->value())
            dev->setForeground(theme::warningText());
        m_checkTable->setItem(i, 2, dev);
        m_checkTable->setItem(i, 3, cell(strengthLetter(d.strength), true));
        m_checkTable->setItem(i, 4, cell(QString::number(d.pixel, 'f', 1), true));
    }
    const int total = int(standards::bands(s.standard).size());
    QString text;
    const QString v = checkVerdict();
    if (v == QLatin1String("no bands")) {
        text = QStringLiteral("<span style='color:%1; font-size:14pt; font-weight:bold'>No reference band found</span>"
                              "<br>Wrong sample, too little signal, or the calibration is far off.")
                   .arg(theme::warningText().name());
    } else {
        const bool ok = v == QLatin1String("ok");
        text = QStringLiteral("<span style='color:%1; font-size:14pt; font-weight:bold'>%2</span><br>"
                              "%3: %4 of %5 bands · mean %6 · RMS %7 · max %8 cm⁻¹<br>"
                              "<span style='color:%9'>%10</span>")
                   .arg(ok ? theme::toneColor(theme::Tone::Green).name() : theme::warningText().name(),
                        ok ? QStringLiteral("✓  Calibration OK") : QStringLiteral("⚠  Recalibration recommended"),
                        s.standard.toHtmlEscaped())
                   .arg(m_check->bands.size())
                   .arg(total)
                   .arg(signedNum(m_check->mean), QString::number(m_check->rms, 'f', 2),
                        QString::number(m_check->maxAbs, 'f', 2), theme::mutedText().name(),
                        QStringLiteral("Tolerance %1 cm⁻¹ (RMS). In use: %2")
                            .arg(m_tolerance->value(), 0, 'f', 1)
                            .arg(activeCalibrationText().toHtmlEscaped()));
    }
    m_verdict->setText(text);
    if (record) {                                      // once per spectrum (not on tolerance changes)
        calstore::LogEntry e;
        e.event = QStringLiteral("quick check");
        e.standards = {s.standard};
        e.bands = int(m_check->bands.size());
        if (!m_check->bands.empty()) {
            e.devMean = m_check->mean;
            e.devRms = m_check->rms;
        }
        e.dark = std::isfinite(s.dark) && s.exposure > 0 ? s.dark / s.exposure : qQNaN();
        e.exposure = s.exposure;
        e.note = checkVerdict();
        logEvent(e);
        emit logMessage(QStringLiteral("Calibration check with %1: %2 bands, mean %3, RMS %4 cm⁻¹ – %5")
                            .arg(s.standard)
                            .arg(m_check->bands.size())
                            .arg(signedNum(m_check->mean), QString::number(m_check->rms, 'f', 2), checkVerdict()));
        emit checkFinished();
    }
    updateEnabled();
}

// ============================================================================================ history
void CalibrationPage::refreshHistory() {
    const QString selected = m_histTable->currentRow() >= 0 && m_histTable->currentRow() < int(m_history.size())
                                 ? m_history[std::size_t(m_histTable->currentRow())].path
                                 : QString();
    m_history = calstore::history(serial(), channel());
    const QSignalBlocker block(m_histTable);
    m_histTable->setRowCount(int(m_history.size()));
    int sel = -1;
    for (int i = 0; i < int(m_history.size()); ++i) {
        const calstore::HistoryEntry& e = m_history[std::size_t(i)];
        auto* a = cell(e.active ? QStringLiteral("● active") : QString());
        if (e.active) {
            a->setForeground(theme::toneColor(theme::Tone::Green));
            QFont f = a->font();
            f.setBold(true);
            a->setFont(f);
        }
        m_histTable->setItem(i, 0, a);
        m_histTable->setItem(i, 1, cell(e.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        m_histTable->setItem(i, 2, cell(e.standards.join(QStringLiteral(", "))));
        m_histTable->setItem(i, 3, cell(e.kind));
        m_histTable->setItem(i, 4, cell(QString::number(e.bands), true));
        m_histTable->setItem(i, 5, cell(QString::number(e.degree), true));
        m_histTable->setItem(i, 6, cell(QString::number(e.rms, 'f', 2), true));
        m_histTable->item(i, 1)->setToolTip(QDir::toNativeSeparators(e.path));
        if (e.path == selected)
            sel = i;
    }
    if (sel >= 0)
        m_histTable->selectRow(sel);
    showHistoryDetails();
    updateEnabled();
}

void CalibrationPage::showHistoryDetails() {
    const int row = m_histTable->currentRow();
    if (row < 0 || row >= int(m_history.size())) {
        m_histDetails->clear();
        updateEnabled();
        return;
    }
    const calstore::HistoryEntry& e = m_history[std::size_t(row)];
    QFile f(e.path);
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly))
        o = QJsonDocument::fromJson(f.readAll()).object();
    QStringList lines;
    lines << QStringLiteral("%1 nm, %2 (%3)").arg(e.channel).arg(e.date, e.kind);
    lines << QStringLiteral("Standards: %1").arg(e.standards.join(QStringLiteral(", ")));
    lines << QStringLiteral("%1 bands, degree %2, RMS %3 cm-1").arg(e.bands).arg(e.degree).arg(e.rms, 0, 'f', 2);
    if (o.contains(QLatin1String("laser_nm_equiv")))
        lines << QStringLiteral("Equivalent laser wavelength: %1 nm").arg(o.value(QLatin1String("laser_nm_equiv")).toDouble(), 0, 'f', 3);
    lines << QString() << QStringLiteral("  reference   residual     pixel");
    std::vector<std::array<double, 3>> res;
    for (const QJsonValue& v : o.value(QLatin1String("residuals")).toArray()) {
        const QJsonArray a = v.toArray();
        res.push_back({a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble()});
    }
    std::sort(res.begin(), res.end());
    for (const auto& r : res)
        lines << QString::asprintf("  %9.1f   %+8.2f   %7.1f", r[0], r[1], r[2]);
    const QJsonArray spectra = o.value(QLatin1String("spectra")).toArray();
    if (!spectra.isEmpty()) {
        lines << QString() << QStringLiteral("Spectra:");
        for (const QJsonValue& v : spectra)
            lines << QStringLiteral("  ") + QDir::toNativeSeparators(v.toString());
    }
    lines << QString() << QStringLiteral("File: ") + QDir::toNativeSeparators(e.path);
    m_histDetails->setPlainText(lines.join(QLatin1Char('\n')));
    updateEnabled();
}

bool CalibrationPage::makeActive(int row) {
    if (row < 0 || row >= int(m_history.size()))
        return false;
    if (m_busy) {
        message(QStringLiteral("Calibration"), QStringLiteral("Stop the running measurement first."));
        return false;
    }
    const calstore::HistoryEntry e = m_history[std::size_t(row)];
    try {
        calstore::makeActive(e.path, channel(), serial());
        const auto c = calib::load(channel(), serial());
        if (lr::Instrument* inst = instrument(); inst && c)
            inst->setUserCalibration(channel(), *c);
    } catch (const lr::Error& err) {
        message(QStringLiteral("Calibration"), QString::fromUtf8(err.what()));
        return false;
    }
    calstore::LogEntry le;
    le.event = QStringLiteral("activated");
    le.standards = e.standards;
    le.bands = e.bands;
    le.fitRms = e.rms;
    le.degree = e.degree;
    le.note = QFileInfo(e.path).fileName();
    logEvent(le);
    emit logMessage(QStringLiteral("Calibration of %1 is now active for %2 nm").arg(e.date).arg(channel()));
    m_samples.clear();
    invalidateFit();
    updateSamplesTable();
    showSample(m_shown);
    updateHeader();
    refreshHistory();
    emit calibrationChanged();
    return true;
}

void CalibrationPage::confirmRevert() {
    if (dialogsEnabled() &&
        QMessageBox::question(this, QStringLiteral("Calibration"),
                              QStringLiteral("Use the factory calibration of the instrument from now on?\n\nThe "
                                             "current calibration stays in the history and can be made active again.")) !=
            QMessageBox::Yes)
        return;
    revertToFactory();
}

bool CalibrationPage::revertToFactory() {
    if (m_busy) {
        message(QStringLiteral("Calibration"), QStringLiteral("Stop the running measurement first."));
        return false;
    }
    bool removed = false;
    try {
        removed = calstore::revertToFactory(channel(), serial());
    } catch (const lr::Error& err) {
        message(QStringLiteral("Calibration"), QString::fromUtf8(err.what()));
        return false;
    }
    if (lr::Instrument* inst = instrument())
        inst->clearUserCalibration(channel());
    if (removed) {
        calstore::LogEntry le;
        le.event = QStringLiteral("factory");
        logEvent(le);
    }
    emit logMessage(QStringLiteral("Factory calibration in use for %1 nm (history kept)").arg(channel()));
    m_samples.clear();
    invalidateFit();
    updateSamplesTable();
    showSample(m_shown);
    updateHeader();
    refreshHistory();
    emit calibrationChanged();
    return true;
}

// ============================================================================================ log
void CalibrationPage::refreshLog() {
    m_log = calstore::readLog(serial());
    m_logInfo->setText(QStringLiteral("Instrument log of S/N %1: one row per calibration and quick check (dark level "
                                      "of the background frames, deviation of the calibration in use from the "
                                      "reference bands = drift). File: %2")
                           .arg(serial(), QDir::toNativeSeparators(calstore::logPath(serial()))));
    m_logTable->setRowCount(int(m_log.size()));
    for (int i = 0; i < int(m_log.size()); ++i) {
        const calstore::LogEntry& e = m_log[m_log.size() - 1 - std::size_t(i)];   // newest first
        auto num = [](double v, int d) { return std::isfinite(v) ? QString::number(v, 'f', d) : QStringLiteral("—"); };
        m_logTable->setItem(i, 0, cell(e.time.toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
        m_logTable->setItem(i, 1, cell(QStringLiteral("%1 nm").arg(e.channel)));
        m_logTable->setItem(i, 2, cell(e.event + (e.note.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(e.note))));
        m_logTable->setItem(i, 3, cell(e.standards.join(QStringLiteral(", "))));
        m_logTable->setItem(i, 4, cell(e.bands ? QString::number(e.bands) : QStringLiteral("—"), true));
        m_logTable->setItem(i, 5, cell(std::isfinite(e.devMean) ? signedNum(e.devMean) : QStringLiteral("—"), true));
        m_logTable->setItem(i, 6, cell(num(e.devRms, 2), true));
        m_logTable->setItem(i, 7, cell(num(e.fitRms, 2), true));
        m_logTable->setItem(i, 8, cell(num(e.dark, 1), true));
    }
    // plots: x = days since the first entry of this channel
    m_driftPlot->clear();
    m_darkPlot->clear();
    const PlotPalette& pal = m_driftPlot->plotPalette();
    m_driftPlot->addHLine(0, QPen(pal.reference, 1));
    QDateTime t0;
    for (const auto& e : m_log)
        if (e.channel == channel() && (!t0.isValid() || e.time < t0))
            t0 = e.time;
    const QString xl = t0.isValid() ? QStringLiteral("Days since %1").arg(t0.toString(QStringLiteral("yyyy-MM-dd")))
                                    : QStringLiteral("Days");
    m_driftPlot->setLabel(Axis::Bottom, xl);
    m_darkPlot->setLabel(Axis::Bottom, xl);
    Vec cx, cy, qx, qy, dx, dy;
    for (const auto& e : m_log) {
        if (e.channel != channel())
            continue;
        const double d = double(t0.msecsTo(e.time)) / 86400e3;
        if (std::isfinite(e.devMean)) {
            if (e.event == QLatin1String("calibration")) {
                cx.push_back(d);
                cy.push_back(e.devMean);
            } else if (e.event == QLatin1String("quick check")) {
                qx.push_back(d);
                qy.push_back(e.devMean);
            }
        }
        if (std::isfinite(e.dark)) {
            dx.push_back(d);
            dy.push_back(e.dark);
        }
    }
    if (!qx.empty()) {
        Curve* c = m_driftPlot->addScatter(qx, qy, pal.seriesColor(0), 7, Marker::Circle);
        c->setName(QStringLiteral("quick check (mean deviation)"));
    }
    if (!cx.empty()) {
        Curve* c = m_driftPlot->addScatter(cx, cy, pal.seriesColor(1), 9, Marker::Diamond);
        c->setName(QStringLiteral("before calibration"));
    }
    if (!dx.empty()) {
        Curve* c = m_darkPlot->addCurve(dx, dy, QPen(pal.main, 1.2));
        c->setMarker(Marker::Circle, 6, QBrush(pal.main));
    }
    double span = 1.0;
    for (const Vec* v : {&cy, &qy})
        for (double y : *v)
            span = std::max(span, std::abs(y));
    m_driftPlot->setAutoY(false);
    m_driftPlot->setYRange(-span * 1.3, span * 1.3);
    for (PlotWidget* p : {m_driftPlot, m_darkPlot}) {
        Bounds b;
        for (double v : dx)
            b.add(v);
        for (double v : cx)
            b.add(v);
        for (double v : qx)
            b.add(v);
        const double hi = b.valid() ? std::max(1.0, b.hi) : 1.0;
        p->setHomeXRange(-0.05 * hi, hi * 1.05);
        p->setXRange(-0.05 * hi, hi * 1.05);
    }
}

}  // namespace lr::gui
