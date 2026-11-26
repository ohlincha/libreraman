#include "datapage.h"
#include "flowlayout.h"
#include "page.h"

#include "bandstrip.h"
#include "plotwidget.h"
#include "theme.h"

#include "core/errors.h"
#include "core/pyformat.h"
#include "core/response.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QInputDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QElapsedTimer>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace lr::gui {

namespace {
constexpr char kBaselineKey[] = "data/baseline";
constexpr char kStrengthKey[] = "data/baseline_strength";
constexpr char kFitMinKey[] = "data/baseline_fit_min";
}  // namespace


namespace {

const char* OPEN_FILTER =
    "Spectra (*.csv *.txt *.spc *.jdx *.dx *.dat);;LibreRaman / vendor CSV (*.csv);;"
    "Vendor text (*.txt);;Galactic SPC (*.spc);;JCAMP-DX (*.jdx *.dx);;All files (*)";
const QStringList FOLDER_SUFFIXES = {".csv", ".txt", ".spc", ".jdx", ".dx"};
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
constexpr int MAX_TICKED_ON_OPEN = 12;          // files opened later are added unticked (Python)

// List that accepts dropped files and folders.
class SpectrumList : public QListWidget {
public:
    std::function<void(const QStringList&)> onDrop;
    SpectrumList() {
        setAcceptDrops(true);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
        setContextMenuPolicy(Qt::CustomContextMenu);
    }

protected:
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* e) override {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override {
        QStringList paths;
        for (const QUrl& u : e->mimeData()->urls())
            if (u.isLocalFile()) paths << u.toLocalFile();
        e->acceptProposedAction();
        if (onDrop && !paths.isEmpty()) onDrop(paths);
    }
};

// numpy.interp: xp ascending; constant extrapolation with the end values.
double interp1(double xq, const Vec& xp, const Vec& fp) {
    const std::size_t n = std::min(xp.size(), fp.size());
    if (n == 0) return NaN;
    if (xq <= xp[0]) return fp[0];
    if (xq >= xp[n - 1]) return fp[n - 1];
    const auto it = std::upper_bound(xp.begin(), xp.begin() + std::ptrdiff_t(n), xq);
    const std::size_t j = std::size_t(it - xp.begin());
    const double x0 = xp[j - 1], x1 = xp[j];
    if (x1 == x0) return fp[j];
    return fp[j - 1] + (fp[j] - fp[j - 1]) * (xq - x0) / (x1 - x0);
}

Vec interp(const Vec& xq, const Vec& xp, const Vec& fp) {
    Vec out(xq.size());
    for (std::size_t i = 0; i < xq.size(); ++i) out[i] = interp1(xq[i], xp, fp);
    return out;
}

// numpy.percentile (linear interpolation) over the finite values.
double percentile(const Vec& y, double q) {
    Vec v;
    v.reserve(y.size());
    for (double a : y)
        if (std::isfinite(a)) v.push_back(a);
    if (v.empty()) return NaN;
    std::sort(v.begin(), v.end());
    const double pos = q / 100.0 * double(v.size() - 1);
    const std::size_t i = std::size_t(std::floor(pos));
    const double f = pos - double(i);
    return i + 1 < v.size() ? v[i] + (v[i + 1] - v[i]) * f : v[i];
}

QIcon swatch(const QColor& c) {
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    if (c.isValid()) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(c.darker(130), 1));
        p.setBrush(c);
        p.drawRoundedRect(QRectF(1.5, 1.5, 11, 11), 3, 3);
    }
    return QIcon(pm);
}

QString safeName(const QString& name) {
    QString s;
    for (QChar c : name) s += (c.isLetterOrNumber() || c == '-' || c == '_' || c == '.') ? c : QChar('_');
    return s;
}

QString signedTerm(double k) {                  // " + 2·" / " − 0.5·" (Python calculator names)
    return QString(" %1 %2").arg(k >= 0 ? QStringLiteral("+") : QStringLiteral("−"), py::g(std::abs(k)));
}

}  // namespace

// ============================================================================================== set-up
DataPage::DataPage(QWidget* parent) : QWidget(parent) {
    m_settingsFn = [] {
        processing::Settings s;            // Measure page defaults: arPLS, strength 6 (slider 60)
        s.baseline = QStringLiteral("arpls");
        s.strength = 6.0;
        return s;
    };
    build();
}

DataPage::~DataPage() = default;

void DataPage::build() {
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(8, 4, 8, 8);
    auto* split = new QSplitter(Qt::Horizontal);
    outer->addWidget(split);

    // --- left: list
    auto* left = new QWidget;
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel(tr("<b>Spectra</b> (tick to show; drop files here)"));
    title->setToolTip(tr("<p>Double-click a name to rename it; right-click for colour, “Add to library…” and "
                         "removal.</p>"));
    lv->addWidget(title);
    auto* list = new SpectrumList;
    list->onDrop = [this](const QStringList& p) { openFiles(p); };
    m_list = list;
    connect(m_list, &QListWidget::itemChanged, this, &DataPage::onItemChanged);
    connect(m_list, &QListWidget::currentRowChanged, this, [this] {
        if (m_batch) return;
        clearFit();
        setNote({});
        updatePeaks();
    });
    connect(m_list, &QWidget::customContextMenuRequested, this, &DataPage::listContextMenu);
    lv->addWidget(m_list, 1);
    auto* row = new QHBoxLayout;
    auto button = [](const QString& text, QHBoxLayout* l) {
        auto* b = new QPushButton(text);
        l->addWidget(b);
        return b;
    };
    connect(button(tr("Open…"), row), &QPushButton::clicked, this, &DataPage::openFilesDialog);
    connect(button(tr("Remove"), row), &QPushButton::clicked, this, &DataPage::removeSelected);
    connect(button(tr("Clear"), row), &QPushButton::clicked, this, &DataPage::clear);
    lv->addLayout(row);
    row = new QHBoxLayout;
    connect(button(tr("All"), row), &QPushButton::clicked, this, [this] { checkAll(true); });
    connect(button(tr("None"), row), &QPushButton::clicked, this, [this] { checkAll(false); });
    lv->addLayout(row);
    split->addWidget(left);

    // --- right: controls, plot, peaks
    auto* right = new QWidget;
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    auto* bar = new QHBoxLayout;
    bar->addWidget(new QLabel(tr("Show")));
    m_mode = new QComboBox;
    m_mode->addItems({tr("Overlay"), tr("Stacked"), tr("Heat map")});
    connect(m_mode, qOverload<int>(&QComboBox::currentIndexChanged), this, &DataPage::redraw);
    bar->addWidget(m_mode);
    bar->addSpacing(6);
    bar->addWidget(new QLabel(tr("Normalise")));
    m_norm = new QComboBox;
    m_norm->addItems({"none", "max", "area"});
    connect(m_norm, qOverload<int>(&QComboBox::currentIndexChanged), this, &DataPage::redraw);
    bar->addWidget(m_norm);
    bar->addSpacing(6);
    bar->addWidget(new QLabel(tr("Baseline")));
    m_baseline = new QComboBox;
    m_baseline->addItem(tr("as Measure page"), QStringLiteral("measure"));
    for (const auto& [key, desc] : processing::baselines()) {
        m_baseline->addItem(key == QLatin1String("none") ? tr("none") : desc, key);
        m_baseline->setItemData(m_baseline->count() - 1, desc, Qt::ToolTipRole);
    }
    m_baseline->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_baseline->setMinimumContentsLength(14);
    m_baseline->setToolTip(tr("<p>Baseline correction. <i>as Measure page</i> uses the Measure page's method and "
                              "strength. arPLS (the default) suits clean and fluorescent samples (CCl₄, ZnS); a polynomial of "
                              "order 1 (straight line) suits clean samples on a flat or sloping background. Baselines are fitted from the shift set under "
                              "<i>fit from</i> (150 cm⁻¹: below it the instrument's edge filter cuts the "
                              "signal).</p>"));
    m_strength = new QDoubleSpinBox;
    m_strength->setRange(0.0, 10.0);
    m_strength->setSingleStep(0.5);
    m_strength->setDecimals(1);
    m_strength->setValue(6.0);
    m_strength->setEnabled(false);
    m_strength->setToolTip(tr("<p>Baseline stiffness 0–10: larger = smoother baseline</p>"));
    connect(m_baseline, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        const QString k = baselineMethod();
        if (k != QLatin1String("none")) m_lastBaseline = k;
        strengthMode(true);
        m_strength->setEnabled(k != QLatin1String("measure") && k != QLatin1String("none"));
        m_fitMin->setEnabled(k != QLatin1String("none"));
        saveBaselineSettings();
        redraw();
    });
    connect(m_strength, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] {
        saveBaselineSettings();
        redraw();
    });
    m_fitMin = new QSpinBox;
    m_fitMin->setRange(0, 1000);
    m_fitMin->setSingleStep(10);
    m_fitMin->setValue(int(processing::kBaselineFitMin));
    m_fitMin->setPrefix(tr("fit from "));
    m_fitMin->setSuffix(tr(" cm⁻¹"));
    m_fitMin->setToolTip(tr("<p>The baseline is fitted from this shift upwards; below it the corrected spectrum is "
                            "zero. 150 cm⁻¹ suits this instrument (its edge filter cuts the signal below). For "
                            "imported spectra with real bands lower down, use 0.</p>"));
    connect(m_fitMin, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
        saveBaselineSettings();
        redraw();
    });
    bar->addWidget(m_baseline);
    bar->addWidget(m_strength);
    bar->addWidget(m_fitMin);
    m_diff = new QCheckBox(tr("difference A − B"));
    m_diff->setToolTip(tr("<p>First ticked spectrum minus the second, both processed as shown</p>"));
    connect(m_diff, &QCheckBox::toggled, this, &DataPage::redraw);
    bar->addWidget(m_diff);
    // stack controls on a second row (the first one is full on narrow windows / macOS)
    auto* bar2 = new QHBoxLayout;
    auto* offLabel = new QLabel(tr("Stacked:  offset"));
    bar2->addWidget(offLabel);
    m_offset = new QSlider(Qt::Horizontal);
    m_offset->setRange(0, 100);
    m_offset->setValue(40);
    m_offset->setMinimumWidth(120);
    m_offset->setMaximumWidth(220);
    m_offset->setToolTip(tr("<p>Vertical offset between stacked spectra</p>"));
    connect(m_offset, &QSlider::valueChanged, this, &DataPage::redraw);
    bar2->addWidget(m_offset);
    bar2->addSpacing(8);
    bar2->addWidget(new QLabel(tr("angle")));
    m_angle = new QSlider(Qt::Horizontal);
    m_angle->setRange(-60, 60);
    m_angle->setValue(0);
    m_angle->setMinimumWidth(120);
    m_angle->setMaximumWidth(220);
    m_angle->setToolTip(tr("<p>Stacked view: shifts each spectrum sideways for a 3D (waterfall) look; the "
                           "shift follows the zoomed range, so narrow regions can be compared. Double-click "
                           "the slider for 0.</p>"));
    connect(m_angle, &QSlider::valueChanged, this, &DataPage::redraw);
    bar2->addWidget(m_angle);
    bar2->addSpacing(8);
    m_mono = new QCheckBox(tr("one colour"));
    m_mono->setToolTip(tr("<p>Stacked view: draw all spectra in one colour</p>"));
    connect(m_mono, &QCheckBox::toggled, this, &DataPage::redraw);
    bar2->addWidget(m_mono);
    bar2->addSpacing(16);
    bar2->addWidget(new QLabel(tr("Band help:")));
    m_bandOrganic = new QCheckBox(tr("organic"));
    m_bandInorganic = new QCheckBox(tr("inorganic"));
    const QString bandTip = tr("<p>Shows typical Raman ranges of functional groups (organic) or of ions, minerals and "
                               "oxides (inorganic) above the spectrum; tick both for mixed samples. Hover over a bar "
                               "for details; the peak table lists candidate assignments. Zoom in to see more bands. "
                               "Ranges are typical values: positions shift with phase, hydrogen bonding and "
                               "neighbouring groups.</p>");
    for (QCheckBox* c : {m_bandOrganic, m_bandInorganic}) {
        c->setToolTip(bandTip);
        connect(c, &QCheckBox::toggled, this, [this] {
            updateBandStrip();
            updateAssignments();
        });
        bar2->addWidget(c);
    }
    bar2->addSpacing(16);
    m_intensity = new QCheckBox(tr("intensity correction"));
    m_intensity->setToolTip(tr("<p>Correct each spectrum with the intensity response of the instrument it was "
                               "measured on (Calibration page, tab <i>Intensity</i>; matched by the serial number in "
                               "the file). Spectra without one are shown unchanged.</p>"));
    connect(m_intensity, &QCheckBox::toggled, this, &DataPage::redraw);
    bar2->addWidget(m_intensity);
    bar2->addStretch();
    bar->addStretch();
    auto* calc = new QPushButton(tr("Calculator…"));
    calc->setToolTip(tr("<p>New spectrum = a·A + b·B + c (scaling, offset, weighted difference)</p>"));
    connect(calc, &QPushButton::clicked, this, &DataPage::calculatorDialog);
    bar->addWidget(calc);
    m_addLibBtn = new QPushButton(tr("Add to library…"));
    m_addLibBtn->setToolTip(tr("<p>Store a spectrum in your spectral library (Library area). With several ticked "
                               "spectra you choose which.</p>"));
    m_addLibBtn->setEnabled(false);
    connect(m_addLibBtn, &QPushButton::clicked, this, &DataPage::addToLibraryDialog);
    bar->addWidget(m_addLibBtn);
    m_matchBtn = new QPushButton(tr("Match"));
    m_matchBtn->setToolTip(tr("<p>Identify a spectrum: rank the library entries against it (Library area, Match "
                              "tab). With several ticked spectra you choose which.</p>"));
    m_matchBtn->setEnabled(false);
    connect(m_matchBtn, &QPushButton::clicked, this, &DataPage::matchDialog);
    bar->addWidget(m_matchBtn);
    auto* exp = new QToolButton;
    exp->setText(tr("Export ▾"));
    exp->setPopupMode(QToolButton::InstantPopup);
    auto* m = new QMenu(exp);
    m->addAction(tr("Ticked spectra as CSV…"), this, [this] { exportDialog("csv"); });
    m->addAction(tr("Ticked spectra as JCAMP-DX…"), this, [this] { exportDialog("jdx"); });
    m->addAction(tr("Ticked spectra as one table (common axis)…"), this, [this] { exportDialog("table"); });
    m->addAction(tr("Peak table (CSV)…"), this, [this] { exportDialog("peaks"); });
    m->addAction(tr("Ticked spectra as one JCAMP-DX file (multi-block)…"), this, [this] { exportDialog("onejdx"); });
    m->addAction(tr("Ticked spectra as one SPC file (multi-file)…"), this, [this] { exportDialog("onespc"); });
    m->addSeparator();
    m->addAction(tr("Plot as PNG…"), this, [this] { exportDialog("png"); });
    m->addAction(tr("Plot as SVG…"), this, [this] { exportDialog("svg"); });
    exp->setMenu(m);
    bar->addWidget(exp);
    // both rows wrap on narrow (laptop) windows; a label stays with its control, the actions stay together last
    auto* flow = new FlowLayout;
    flow->addGroups(bar);
    QLayoutItem* actions = flow->takeAt(flow->count() - 1);
    flow->addGroups(bar2);
    flow->addItem(actions);
    delete bar;
    delete bar2;
    rv->addLayout(flow);
    {   // last session's baseline settings (#6)
        const QSignalBlocker b1(m_baseline), b2(m_strength), b3(m_fitMin);
        const QSettings st;
        setBaselineMethod(st.value(QLatin1String(kBaselineKey), QStringLiteral("measure")).toString());
        strengthMode(false);
        m_strength->setValue(st.value(QLatin1String(kStrengthKey), m_strength->value()).toDouble());
        m_fitMin->setValue(st.value(QLatin1String(kFitMinKey), int(processing::kBaselineFitMin)).toInt());
        const QString k = baselineMethod();
        if (k != QLatin1String("none")) m_lastBaseline = k;
        m_strength->setEnabled(k != QLatin1String("measure") && k != QLatin1String("none"));
        m_fitMin->setEnabled(k != QLatin1String("none"));
    }

    m_stack = new QStackedWidget;
    m_plot = new PlotWidget;
    m_plot->installEventFilter(this);           // band-strip tooltips
    m_plot->setHomeXRange(150, 3500);                   // as the Measure page (zoom out for the full range)
    {   // waterfall: the sideways shift follows the zoomed x range (debounced while the wheel turns)
        auto* t = new QTimer(this);
        t->setSingleShot(true);
        t->setInterval(120);
        connect(t, &QTimer::timeout, this, [this] {
            if (view() == View::Stacked && m_angle && m_angle->value() != 0) redraw();
        });
        connect(m_plot, &PlotWidget::xRangeChanged, t, [t, this] {
            if (view() == View::Stacked && m_angle && m_angle->value() != 0) t->start();
        });
    }
    m_plot->setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    m_plot->setLabel(Axis::Left, tr("Intensity"));
    m_plot->setGrid(true, true);
    m_plot->setLegendVisible(true);
    m_plot->setHeadroom(0.08);              // Python rescale_y(0.08)
    m_plot->setToolTip(PlotWidget::navigationHelp() +
                       tr("<p>Click: add a peak to the current spectrum. Right-click: add/remove a peak, fit.</p>"));
    m_stack->addWidget(m_plot);
    // A click adds a peak once it is clear it was not the first half of a double-click (zoom out).
    m_clickTimer = new QTimer(this);
    m_clickTimer->setSingleShot(true);
    m_clickTimer->setInterval(QApplication::doubleClickInterval());
    connect(m_clickTimer, &QTimer::timeout, this, [this] { addPeakAt(m_clickX); });
    connect(m_plot, &PlotWidget::clicked, this,
            [this](const QPointF& d, Qt::MouseButton b, Qt::KeyboardModifiers) { plotClicked(d, b); });
    connect(m_plot, &PlotWidget::autoRangeRequested, m_clickTimer, &QTimer::stop);
    connect(m_plot, &PlotWidget::rightClicked, this, &DataPage::plotMenu);
    m_heat = new PlotWidget;
    m_heat->setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    m_heat->setLabel(Axis::Left, tr("spectrum #"));
    m_heat->setGrid(false, false);
    m_heat->setWheelMode(WheelMode::ZoomX);
    m_stack->addWidget(m_heat);
    rv->addWidget(m_stack, 3);

    auto* prow = new QHBoxLayout;
    auto* ptitle = new QLabel(tr("<b>Peaks</b>"));
    ptitle->setToolTip(tr("<p>Peaks of the current spectrum (≥ 100 cm⁻¹). <b>Click</b> the plot to add a peak, "
                          "<b>right-click</b> near a peak to remove it (or to add one there). The peaks inside the "
                          "fit window are the start values of the fit; editing them refits.</p>"));
    prow->addWidget(ptitle);
    prow->addSpacing(6);
    prow->addWidget(new QLabel(tr("S/N ≥")));
    m_snr = new QDoubleSpinBox;
    m_snr->setRange(1.0, 10000.0);
    m_snr->setDecimals(1);
    m_snr->setSingleStep(1.0);
    m_snr->setValue(8.0);
    m_snr->setToolTip(tr("<p>Sensitivity of the peak picking: minimum peak height (prominence) over the noise</p>"));
    connect(m_snr, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &DataPage::peaksChanged);
    prow->addWidget(m_snr);
    prow->addWidget(new QLabel(tr("on")));
    m_pickOn = new QComboBox;
    m_pickOn->addItems({tr("processed"), tr("raw")});
    m_pickOn->setToolTip(tr("<p>Pick the peaks on the spectrum as shown (processed) or on the unprocessed data</p>"));
    connect(m_pickOn, qOverload<int>(&QComboBox::currentIndexChanged), this, &DataPage::peaksChanged);
    prow->addWidget(m_pickOn);
    prow->addSpacing(6);
    prow->addWidget(new QLabel(tr("labels")));
    m_labels = new QComboBox;
    m_labels->addItems({tr("off"), tr("current"), tr("all ticked")});
    m_labels->setCurrentIndex(int(Labels::Current));
    m_labels->setToolTip(tr("<p>Peak labels on the plot: none, the current spectrum, or every ticked spectrum "
                            "(also the spectra written by Export ▸ Peak table)</p>"));
    connect(m_labels, qOverload<int>(&QComboBox::currentIndexChanged), this, &DataPage::updatePeakOverlay);
    prow->addWidget(m_labels);
    prow->addStretch();
    prow->addWidget(new QLabel(tr("fit")));
    m_profile = new QComboBox;
    m_profile->addItems({"pvoigt", "voigt", "lorentz", "gauss"});   // Python order (default pvoigt)
    connect(m_profile, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (hasFit()) refit();
    });
    prow->addWidget(m_profile);
    prow->addWidget(new QLabel(tr("max.")));
    m_maxComp = new QSpinBox;
    m_maxComp->setRange(1, 40);
    m_maxComp->setValue(12);
    m_maxComp->setToolTip(tr("<p>Maximum number of fit components: if the fit window holds more peaks, the "
                             "strongest are fitted</p>"));
    connect(m_maxComp, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
        if (hasFit()) refit();
    });
    prow->addWidget(m_maxComp);
    auto* fit = new QPushButton(tr("Fit peaks in view"));
    fit->setToolTip(tr("<p>Fits the peaks of the current spectrum inside the visible x range (at most “max.” "
                       "components, the strongest). Click the plot to add a component, right-click near one to "
                       "remove it. After a fit, the fit window is shaded (it spans the view you fitted, so zoom out to see its "
                       "edges); drag it or its edges to fit another range.</p>"));
    connect(fit, &QPushButton::clicked, this, [this] { fitView(); });
    prow->addWidget(fit);
    auto* clr = new QPushButton(tr("Clear fit"));
    connect(clr, &QPushButton::clicked, this, [this] {
        clearFit();
        setNote({});
        updatePeaks();
    });
    prow->addWidget(clr);
    rv->addLayout(prow);
    m_table = new QTableWidget(0, 7);
    m_table->setHorizontalHeaderLabels({QStringLiteral("position (cm⁻¹)"), QStringLiteral("±"), tr("height"),
                                        QStringLiteral("FWHM (cm⁻¹)"), tr("area"), tr("S/N or η"),
                                        tr("possible assignment")});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Interactive);
    m_table->setColumnWidth(6, 260);
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 6);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setMaximumHeight(170);
    rv->addWidget(m_table, 1);
    auto* foot = new QHBoxLayout;
    m_fitLabel = new QLabel;
    m_fitLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    foot->addWidget(m_fitLabel, 1);
    m_note = new QLabel;
    m_note->setStyleSheet(QStringLiteral("color: %1;").arg(theme::warningText().name()));
    foot->addWidget(m_note);
    rv->addLayout(foot);
    split->addWidget(right);
    split->setStretchFactor(1, 1);
    split->setSizes({260, 900});
    redraw();
}

void DataPage::setProcessingSettings(SettingsFn fn) {
    if (fn) m_settingsFn = std::move(fn);
    redraw();
}

// ======================================================================================= list management
void DataPage::addSpectrum(const Spectrum& sp) {
    QString name = sp.metaValue(QStringLiteral("sample"), QStringLiteral("spectrum")) + " " +
                   sp.t.toLocalTime().toString(QStringLiteral("HH:mm:ss"));
    const QString idx = sp.metaValue(QStringLiteral("series_index"));
    if (!idx.isEmpty()) name += " #" + idx;
    addSpectrum(fileio::fromSpectrum(sp, name), true);
}

void DataPage::addSpectra(const std::vector<Spectrum>& spectra) {
    ++m_batch;
    for (const Spectrum& sp : spectra) addSpectrum(sp);
    --m_batch;
    if (m_dirty) redraw();
}

void DataPage::addSpectrum(const fileio::DataSpectrum& sp, bool checked) {
    addEntry(sp, checked);
    redraw();
}

void DataPage::addEntry(const fileio::DataSpectrum& sp, bool checked) {
    m_entries.push_back({sp, QColor(), {}, {}, {}, {}, {}});
    auto* it = new QListWidgetItem(sp.name);
    it->setFlags(it->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
    it->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    it->setToolTip(sp.source.isEmpty() ? tr("measured in this session") : sp.source);
    {
        const QSignalBlocker block(m_list);
        m_list->addItem(it);
    }
    m_list->setCurrentRow(m_list->count() - 1, QItemSelectionModel::ClearAndSelect);
    m_dirty = true;
    emit spectraChanged();
}

QStringList DataPage::openFiles(const QStringList& paths) {
    QStringList errors;
    ++m_batch;
    openPaths(paths, errors);
    --m_batch;
    if (m_dirty) redraw();
    if (!errors.isEmpty())
        warn(tr("Open spectra"), tr("Some files could not be read:") + "\n\n" + errors.mid(0, 15).join('\n'));
    return errors;
}

void DataPage::openPaths(const QStringList& paths, QStringList& errors) {
    for (const QString& p : paths) {
        const QFileInfo fi(p);
        if (fi.isDir()) {                        // Python os.walk: files of a folder sorted, then subfolders
            QDir d(p);
            QStringList files;
            for (const QString& f : d.entryList(QDir::Files | QDir::Hidden, QDir::Name))
                for (const QString& s : FOLDER_SUFFIXES)
                    if (f.toLower().endsWith(s)) {
                        files << d.filePath(f);
                        break;
                    }
            openPaths(files, errors);
            QStringList subs;
            for (const QString& s : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name))
                subs << d.filePath(s);
            openPaths(subs, errors);
            continue;
        }
        try {
            for (const fileio::DataSpectrum& sp : fileio::read(p)) addEntry(sp, count() < MAX_TICKED_ON_OPEN);
        } catch (const std::exception& e) {
            errors << fi.fileName() + ": " + QString::fromUtf8(e.what());
        }
    }
}

void DataPage::onItemChanged(QListWidgetItem* it) {
    const int r = m_list->row(it);
    if (r < 0 || r >= count()) return;
    fileio::DataSpectrum& sp = m_entries[size_t(r)].sp;
    const QString name = it->text().trimmed();
    if (name.isEmpty()) {                        // renamed to nothing: keep the old name
        const QSignalBlocker block(m_list);
        it->setText(sp.name);
    } else {
        sp.name = name;
    }
    emit spectraChanged();
    redraw();
}

void DataPage::listContextMenu(const QPoint& pos) {
    QListWidgetItem* it = m_list->itemAt(pos);
    if (!it) return;
    QMenu menu(this);
    fillListMenu(menu, m_list->row(it));
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

void DataPage::fillListMenu(QMenu& menu, int r) {
    QListWidgetItem* it = m_list->item(r);
    if (!it || r >= count()) return;
    menu.addAction(tr("Rename"), this, [this, it] { m_list->editItem(it); });
    menu.addAction(tr("Colour…"), this, [this, r] {
        const QColor c = QColorDialog::getColor(colorOf(r), this, tr("Spectrum colour"));
        if (c.isValid()) setColor(r, c);
    });
    QAction* autoColor = menu.addAction(tr("Automatic colour"), this, [this, r] { setColor(r, QColor()); });
    autoColor->setEnabled(m_entries[size_t(r)].color.isValid());
    menu.addSeparator();
    menu.addAction(tr("Add to library…"), this, [this, r] {
        if (r >= 0 && r < count()) emit addToLibraryRequested(m_entries[size_t(r)].sp);
    });
    menu.addSeparator();
    menu.addAction(tr("Remove"), this, [this, r] {
        std::vector<int> rows;
        for (const QModelIndex& i : m_list->selectionModel()->selectedRows()) rows.push_back(i.row());
        if (std::find(rows.begin(), rows.end(), r) == rows.end()) rows = {r};
        removeRows(rows);
    });
}

bool DataPage::isTicked(int row) const {
    const QListWidgetItem* it = m_list->item(row);
    return it && it->checkState() == Qt::Checked;
}

void DataPage::setTicked(int row, bool on) {
    if (QListWidgetItem* it = m_list->item(row)) it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
}

void DataPage::checkAll(bool on) {
    {
        const QSignalBlocker block(m_list);
        for (int i = 0; i < m_list->count(); ++i) m_list->item(i)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
    emit spectraChanged();
    redraw();
}

void DataPage::rename(int row, const QString& name) {
    if (QListWidgetItem* it = m_list->item(row)) it->setText(name);
}

void DataPage::setColor(int row, const QColor& c) {
    if (row < 0 || row >= count()) return;
    m_entries[size_t(row)].color = c;
    redraw();
}

QColor DataPage::colorOf(int row) const {
    if (!isTicked(row)) return {};
    if (m_entries[size_t(row)].color.isValid()) return m_entries[size_t(row)].color;
    const std::vector<int> t = tickedRows();
    const int k = int(std::find(t.begin(), t.end(), row) - t.begin());
    return m_plot->plotPalette().seriesColor(k);
}

void DataPage::removeRows(std::vector<int> rows) {
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    if (rows.empty()) return;
    {
        const QSignalBlocker block(m_list);
        for (auto r = rows.rbegin(); r != rows.rend(); ++r) {
            if (*r < 0 || *r >= count()) continue;
            delete m_list->takeItem(*r);
            m_entries.erase(m_entries.begin() + *r);
        }
    }
    emit spectraChanged();
    redraw();
}

void DataPage::removeSelected() {
    std::vector<int> rows;
    for (const QModelIndex& i : m_list->selectionModel()->selectedRows()) rows.push_back(i.row());
    removeRows(rows);
}

void DataPage::clear() {
    {
        const QSignalBlocker block(m_list);
        m_list->clear();
    }
    m_entries.clear();
    emit spectraChanged();
    redraw();
}

int DataPage::currentRow() const { return m_list->currentRow(); }

// Add to library…: the one ticked spectrum, or (several ticked) the one chosen from a list with the current
// one preselected; none ticked: the current one. Without dialogs (tests): the current ticked one, else the first.
int DataPage::libraryCandidate(bool ask, const QString& title) {
    std::vector<int> rows = tickedRows();
    const int cur = currentRow();
    if (rows.empty()) return cur >= 0 && cur < count() ? cur : -1;
    if (rows.size() == 1) return rows[0];
    int pre = 0;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i] == cur) pre = int(i);
    if (!ask) return rows[size_t(pre)];
    QStringList names;
    for (int r : rows) names << m_entries[size_t(r)].sp.name;
    bool ok = false;
    const QString pick = QInputDialog::getItem(this, title.isEmpty() ? tr("Add to library") : title,
                                               title.isEmpty() ? tr("Spectrum to add:") : tr("Spectrum:"), names,
                                               pre, false, &ok);
    if (!ok) return -1;
    return rows[size_t(names.indexOf(pick))];
}

void DataPage::addToLibraryDialog() {
    const int r = libraryCandidate(dialogsEnabled());
    if (r >= 0) emit addToLibraryRequested(m_entries[size_t(r)].sp);
}

void DataPage::matchDialog() {
    const int r = libraryCandidate(dialogsEnabled(), tr("Match"));
    if (r >= 0) emit matchRequested(m_entries[size_t(r)].sp);
}

void DataPage::setCurrentRow(int row) { m_list->setCurrentRow(row, QItemSelectionModel::ClearAndSelect); }

std::vector<int> DataPage::tickedRows() const {
    std::vector<int> r;
    for (int i = 0; i < count(); ++i)
        if (isTicked(i)) r.push_back(i);
    return r;
}

// =================================================================================== view and processing
void DataPage::setView(View v) { m_mode->setCurrentIndex(int(v)); }
DataPage::View DataPage::view() const { return View(m_mode->currentIndex()); }
void DataPage::setNormalisation(const QString& mode) { m_norm->setCurrentText(mode); }
QString DataPage::normalisation() const { return m_norm->currentText(); }
void DataPage::setBaselineMethod(const QString& key) {
    const int i = m_baseline->findData(key);
    if (i >= 0) m_baseline->setCurrentIndex(i);
}
QString DataPage::baselineMethod() const { return m_baseline->currentData().toString(); }
void DataPage::setBaselineStrength(double s) { m_strength->setValue(s); }

// The polynomial's control is its order (1–10, whole numbers; processing strength = order − 1); the other
// methods' is the strength 0–10. reset: put the new mode's default in (method changed by the user).
void DataPage::strengthMode(bool reset) {
    const bool poly = baselineMethod() == QLatin1String("polynomial");
    if (poly == (m_strength->prefix() == tr("order "))) return;
    const QSignalBlocker b(m_strength);
    if (poly) {
        m_strength->setDecimals(0);
        m_strength->setRange(1, 10);
        m_strength->setSingleStep(1);
        m_strength->setPrefix(tr("order "));
        m_strength->setToolTip(tr("<p>Polynomial order: 1 = straight line (offset and slope). Higher orders bend "
                                  "more and can cut into broad bands.</p>"));
        if (reset) m_strength->setValue(1);
    } else {
        m_strength->setPrefix(QString());
        m_strength->setDecimals(1);
        m_strength->setRange(0.0, 10.0);
        m_strength->setSingleStep(0.5);
        m_strength->setToolTip(tr("<p>Baseline stiffness 0–10: larger = smoother baseline</p>"));
        if (reset) m_strength->setValue(6.0);
    }
}
double DataPage::baselineStrength() const { return m_strength->value(); }
void DataPage::setBaselineFitMin(double cm) { m_fitMin->setValue(int(std::lround(cm))); }
double DataPage::baselineFitMin() const { return m_fitMin->value(); }

void DataPage::saveBaselineSettings() const {
    QSettings st;
    st.setValue(QLatin1String(kBaselineKey), baselineMethod());
    st.setValue(QLatin1String(kStrengthKey), m_strength->value());
    st.setValue(QLatin1String(kFitMinKey), m_fitMin->value());
}
void DataPage::setBaselineCorrection(bool on) { setBaselineMethod(on ? m_lastBaseline : QStringLiteral("none")); }
void DataPage::setIntensityCorrection(bool on) { m_intensity->setChecked(on); }
bool DataPage::intensityCorrection() const { return m_intensity->isChecked(); }
bool DataPage::baselineCorrection() const { return baselineMethod() != QLatin1String("none"); }
void DataPage::setDifference(bool on) { m_diff->setChecked(on); }
bool DataPage::difference() const { return m_diff->isChecked(); }
void DataPage::setOffset(int percent) { m_offset->setValue(percent); }
void DataPage::setAngle(int degrees) { m_angle->setValue(degrees); }
int DataPage::angle() const { return m_angle->value(); }
int DataPage::offset() const { return m_offset->value(); }
void DataPage::setFitProfile(const QString& shape) { m_profile->setCurrentText(shape); }
QString DataPage::fitText() const { return m_fitLabel->text(); }

processing::Settings DataPage::settings() const {
    processing::Settings s = m_settingsFn();
    const QString k = baselineMethod();
    if (k != QLatin1String("measure")) {
        s.baseline = k;
        s.strength = k == QLatin1String("polynomial") ? m_strength->value() - 1 : m_strength->value();   // order
    }
    s.fitMin = m_fitMin->value();
    s.intensity = m_intensity->isChecked();
    s.normalize = m_norm->currentText();
    return s;
}

std::pair<Vec, Vec> DataPage::processed(const fileio::DataSpectrum& sp, QString* error) const {
    try {
        const processing::Settings s = settings();
        if (s.intensity) {
            const Vec y = response::corrected(sp.x, sp.y, sp.metaValue(QLatin1String(response::kSerialKey)),
                                              response::channelOf(sp.metaValue(QStringLiteral("channel"))));
            return {sp.x, processing::process(sp.x, y, s).first};
        }
        return {sp.x, processing::process(sp.x, sp.y, s).first};
    } catch (const std::exception& e) {
        if (error) *error = sp.name + ": " + QString::fromUtf8(e.what());
        return {sp.x, sp.y};
    }
}

QString DataPage::settingsKey() const {
    const processing::Settings s = settings();
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
        .arg(s.baseline, QString::number(s.strength, 'g', 17), QString::number(int(s.despike)),
             QString::number(int(s.smooth)), QString::number(s.smoothWindow), QString::number(s.derivative),
             QString::number(s.derivWindow), s.normalize, s.band ? QString::number(*s.band, 'g', 17) : QString())
           + QStringLiteral("|intensity=%1|fit_min=%2").arg(int(s.intensity)).arg(s.fitMin, 0, 'g', 17);
}

QString DataPage::describeProcessing() const {
    const processing::Settings ps = settings();
    return QStringLiteral("baseline=%1 strength=%2 despike=%3 smooth=%4 normalize=%5")
        .arg(ps.baseline, py::fixed(ps.strength, 1),
             py::str(ps.despike), py::str(ps.smooth), m_norm->currentText())
           + (ps.fitMin != processing::kBaselineFitMin ? QStringLiteral(" fit_min=%1").arg(ps.fitMin) : QString())
           + (ps.intensity ? QStringLiteral(" intensity=True") : QString());
}

void DataPage::updateSwatches() {
    const QSignalBlocker block(m_list);
    for (int i = 0; i < m_list->count(); ++i) m_list->item(i)->setIcon(swatch(colorOf(i)));
}

void DataPage::redraw() {
    m_addLibBtn->setEnabled(count() > 0);
    m_matchBtn->setEnabled(count() > 0);
    if (m_batch) {
        m_dirty = true;
        return;
    }
    m_dirty = false;
    clearFit();
    m_labelItems.clear();                       // deleted by clear()
    m_labelRows.clear();
    m_peakMarks = nullptr;
    m_drawn.clear();
    m_plot->clear();
    m_bandStrip = nullptr;                      // deleted by clear()
    updateBandStrip();
    m_heat->clear();
    m_heatItem = nullptr;
    m_baseNote.clear();
    m_note->clear();
    m_note->setToolTip({});
    std::vector<int> rows = tickedRows();
    const View mode = view();
    const bool diff = m_diff->isChecked() && rows.size() >= 2;
    m_stack->setCurrentIndex(mode == View::HeatMap && !diff ? 1 : 0);
    m_offset->setEnabled(mode == View::Stacked && !diff);
    m_angle->setEnabled(mode == View::Stacked && !diff);
    m_mono->setEnabled(mode == View::Stacked && !diff);
    updateSwatches();
    if (rows.empty()) {
        updatePeaks();
        return;
    }
    // Overlay / stacked: at most kMaxCurves curves (evenly thinned, the newest always drawn); hundreds of
    // overlapping curves are unreadable and slow to paint. The heat map shows every ticked spectrum.
    QString thinned;
    if (!diff && mode != View::HeatMap && rows.size() > size_t(kMaxCurves)) {
        std::vector<int> sub;
        const size_t n = rows.size();
        for (int k = 0; k < kMaxCurves; ++k)
            sub.push_back(rows[size_t(std::llround(double(k) * double(n - 1) / double(kMaxCurves - 1)))]);
        thinned = tr("showing %1 of %2 ticked spectra (heat map: all)").arg(kMaxCurves).arg(n);
        rows = std::move(sub);
    }
    QStringList problems;
    std::vector<std::pair<Vec, Vec>> data;
    const QString key = settingsKey();
    // Processing is cached per entry. Spectra not processed yet are processed for at most kProcessBudgetMs per
    // redraw (a long series added at once); the rest are drawn by follow-up redraws in later event-loop passes.
    QElapsedTimer budget;
    budget.start();
    std::vector<int> shown;
    bool incomplete = false;
    for (int r : rows) {
        Entry& e = m_entries[size_t(r)];
        if (e.key != key) {
            if (shown.size() >= 2 && budget.elapsed() >= kProcessBudgetMs) {
                incomplete = true;
                continue;
            }
            e.procError.clear();
            e.procY = processed(e.sp, &e.procError).second;
            e.key = key;
        }
        shown.push_back(r);
        data.emplace_back(e.sp.x, e.procY);
        if (!e.procError.isEmpty()) problems << e.procError;
    }
    rows = std::move(shown);
    if (incomplete && !m_followUp) {
        m_followUp = true;
        QTimer::singleShot(0, this, [this] {
            m_followUp = false;
            redraw();
        });
    }
    if (!problems.isEmpty()) {
        m_baseNote = tr("processing failed, shown unprocessed: %1").arg(problems.join("; "));
        m_note->setToolTip(problems.join('\n'));
    } else if (!thinned.isEmpty()) {
        m_baseNote = thinned;
    }
    const PlotPalette& pal = m_plot->plotPalette();
    auto nameOf = [&](int k) { return m_entries[size_t(rows[size_t(k)])].sp.name; };

    if (diff) {
        const auto& [xa, ya] = data[0];
        const auto& [xb, yb] = data[1];
        const Vec ybi = interp(xa, xb, yb);
        Vec yd(xa.size());
        for (std::size_t i = 0; i < xa.size(); ++i) yd[i] = ya[i] - ybi[i];
        m_plot->addCurve(xa, yd, QPen(colorOf(rows[0]), 1.4), nameOf(0) + " − " + nameOf(1));
        QPen zero(pal.reference, 1, Qt::DashLine);
        m_plot->addHLine(0, zero)->setAutoScale(false);
    } else if (mode == View::HeatMap) {
        double lo = -std::numeric_limits<double>::infinity(), hi = std::numeric_limits<double>::infinity();
        for (const auto& [x, y] : data) {
            if (x.empty()) continue;
            lo = std::max(lo, x.front());
            hi = std::min(hi, x.back());
        }
        if (std::isfinite(lo) && std::isfinite(hi) && hi > lo) {
            constexpr int cols = 1200;
            Vec grid(cols);
            for (int i = 0; i < cols; ++i) grid[size_t(i)] = lo + (hi - lo) * i / (cols - 1);
            Vec img;
            img.reserve(data.size() * cols);
            for (const auto& [x, y] : data) {
                const Vec v = interp(grid, x, y);
                img.insert(img.end(), v.begin(), v.end());
            }
            m_heatItem = m_heat->addHeatMap();
            m_heatItem->setData(int(data.size()), cols, std::move(img));
            m_heatItem->setExtent(lo, hi, 0, double(data.size()));
            m_heatItem->setColorMap(ColorMap::Viridis);
            m_heatItem->setColorBarVisible(true);
            m_heatItem->setColorBarLabel(tr("intensity"));
        } else {
            m_baseNote = tr("heat map: the ticked spectra have no common x range");
        }
    } else {
        double span = 0;
        for (const auto& d : data) {
            const double s = percentile(d.second, 99.5) - percentile(d.second, 1);
            if (std::isfinite(s)) span = std::max(span, s);
        }
        if (span == 0) span = 1.0;
        const bool stacked = mode == View::Stacked;
        const double step = stacked ? span * m_offset->value() / 100.0 : 0.0;
        // Waterfall (stacked + angle): spectrum k is shifted sideways by k·dx. dx follows the visible x range
        // when zoomed (so a narrow region keeps its 3D look), else the data range; the last spectrum moves by
        // tan(angle)·half the range. Each spectrum is filled with the plot background down to its own base, so
        // it hides the ones behind it (hidden lines).
        const double ang = stacked ? m_angle->value() : 0;
        double dx = 0;
        if (ang != 0 && data.size() > 1) {
            double range;
            if (!m_plot->autoX()) {
                range = m_plot->xRange().span();
            } else {
                const Vec& x0 = data[0].first;
                range = x0.empty() ? 1.0 : x0.back() - x0.front();
            }
            const double total = std::tan(ang * M_PI / 180.0) / std::tan(60.0 * M_PI / 180.0) * 0.25 * range;
            dx = total / double(data.size() - 1);       // back spectrum moves by at most ¼ of the range
        }
        double base = std::numeric_limits<double>::infinity();
        for (const auto& d : data) {
            const double p = percentile(d.second, 1);
            if (std::isfinite(p)) base = std::min(base, p);
        }
        if (!std::isfinite(base)) base = 0;
        const bool hide = stacked && (step != 0 || dx != 0);
        const QColor bg = pal.plotArea;
        const bool mono = stacked && m_mono->isChecked();
        for (std::size_t k = 0; k < data.size(); ++k) {
            Vec x = data[k].first;
            Vec y = data[k].second;
            if (step != 0)
                for (double& v : y) v += double(k) * step;
            if (dx != 0)
                for (double& v : x) v += double(k) * dx;
            // many curves: 1 px lines (Qt strokes wider antialiased lines through a much slower path)
            const QColor col = mono ? pal.main : colorOf(rows[k]);
            Curve* c = m_plot->addCurve(std::move(x), std::move(y), QPen(col, data.size() > 6 ? 1.0 : 1.3),
                                        mono && k > 0 ? QString() : nameOf(int(k)));
            if (hide) {
                c->setFill(QBrush(bg), base + double(k) * step);
                c->setZValue(-double(k));                // front (first) spectrum on top
            }
            m_drawn.push_back({rows[k], double(k) * step, double(k) * dx});
        }
    }
    setNote({});
    updatePeaks();
}

// ==================================================================================== peaks and fitting
void DataPage::fillPeakRow(const std::vector<std::pair<double, int>>& values) {
    const int r = m_table->rowCount();
    m_table->insertRow(r);
    for (int c = 0; c < int(values.size()); ++c) {
        const auto& [v, dec] = values[size_t(c)];
        auto* it = new QTableWidgetItem(std::isnan(v) ? QString() : py::fixed(v, dec));
        it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_table->setItem(r, c, it);
    }
    if (!values.empty() && std::isfinite(values[0].first)) {
        auto* it = new QTableWidgetItem(assignmentText(values[0].first));
        it->setToolTip(assignmentText(values[0].first, 12).replace(QStringLiteral("; "), QStringLiteral("\n")));
        m_table->setItem(r, 6, it);
    }
}

// ------------------------------------------------------------------------------------------- band help
QString DataPage::assignmentText(double position, int max) const {
    const bool org = m_bandOrganic && m_bandOrganic->isChecked();
    const bool inorg = m_bandInorganic && m_bandInorganic->isChecked();
    if (!org && !inorg)
        return {};
    QStringList out;
    for (const bands::Band* b : bands::candidates(position, org, inorg)) {
        if (int(out.size()) >= max)
            break;
        out << QStringLiteral("%1 %2").arg(b->group, b->mode);
    }
    return out.join(QStringLiteral("; "));
}

void DataPage::updateAssignments() {
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const QTableWidgetItem* pos = m_table->item(r, 0);
        bool ok = false;
        const double v = pos ? pos->text().toDouble(&ok) : 0;
        auto* it = new QTableWidgetItem(ok ? assignmentText(v) : QString());
        if (ok)
            it->setToolTip(assignmentText(v, 12).replace(QStringLiteral("; "), QStringLiteral("\n")));
        m_table->setItem(r, 6, it);
    }
}

void DataPage::setBandHelp(bool organic, bool inorganic) {
    m_bandOrganic->setChecked(organic);
    m_bandInorganic->setChecked(inorganic);
}

void DataPage::updateBandStrip() {
    const bool org = m_bandOrganic && m_bandOrganic->isChecked();
    const bool inorg = m_bandInorganic && m_bandInorganic->isChecked();
    m_plot->setHeadroom(org || inorg ? 0.32 : 0.08);  // room above the curves for the band strip
    if (!org && !inorg) {
        if (m_bandStrip)
            m_plot->removeItem(m_bandStrip);
        m_bandStrip = nullptr;
        return;
    }
    if (!m_bandStrip)
        m_bandStrip = m_plot->addItem<BandStripItem>();
    m_bandStrip->setCategories(org, inorg);
}

bool DataPage::eventFilter(QObject* obj, QEvent* e) {
    if (obj == m_plot && e->type() == QEvent::ToolTip && m_bandStrip) {
        auto* he = static_cast<QHelpEvent*>(e);
        const QString tip = m_bandStrip->toolTipAt(QPointF(he->pos()));
        if (!tip.isEmpty()) {
            QToolTip::showText(he->globalPos(), tip, m_plot);
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}

int DataPage::heightDecimals() const {
    return m_norm->currentText() == QLatin1String("none") ? 0 : 3;   // normalised: 0..1 (Python: always 0)
}

void DataPage::changeEvent(QEvent* e) {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_plot) redraw();      // theme switch: new series colours
}

// ------------------------------------------------------------------------------------------- peak list
void DataPage::setPeakSnr(double snr) { m_snr->setValue(snr); }
double DataPage::peakSnr() const { return m_snr->value(); }
void DataPage::setPickOnRaw(bool raw) { m_pickOn->setCurrentIndex(raw ? 1 : 0); }
bool DataPage::pickOnRaw() const { return m_pickOn->currentIndex() == 1; }
void DataPage::setPeakLabels(Labels l) { m_labels->setCurrentIndex(int(l)); }
DataPage::Labels DataPage::peakLabels() const { return Labels(m_labels->currentIndex()); }
void DataPage::setMaxComponents(int n) { m_maxComp->setValue(n); }
int DataPage::maxComponents() const { return m_maxComp->value(); }
QString DataPage::note() const { return m_note->text(); }

void DataPage::setNote(const QString& text) { m_note->setText(text.isEmpty() ? m_baseNote : text); }

Vec DataPage::shownY(int row) const {
    const Entry& e = m_entries[size_t(row)];
    if (e.key == settingsKey() && e.procY.size() == e.sp.x.size()) return e.procY;
    return processed(e.sp).second;
}

std::pair<Vec, Vec> DataPage::pickData(int row) const {
    const fileio::DataSpectrum& sp = m_entries[size_t(row)].sp;
    if (pickOnRaw()) return {sp.x, sp.y};
    return {sp.x, shownY(row)};
}

namespace {

constexpr double kSameTol = 3.0;                // cm⁻¹: a removed/added peak matches a picked one

// A peak added by hand at x (already snapped to a data point): height there, width/area/S/N from a local
// pick at low threshold if one lies close (else FWHM 10 cm⁻¹, the fit's default start width).
peaks::Peak manualPeak(const Vec& x, const Vec& y, double pos) {
    peaks::Peak p;
    p.position = pos;
    p.height = interp1(pos, x, y);
    p.fwhm = 10.0;
    p.area = NaN;
    try {
        const double noise = peaks::noiseLevel(y);
        if (noise > 0) p.snr = p.height / noise;
        for (const peaks::Peak& q : peaks::pick(x, y, 1.0, 2.0, false, pos - 30.0, pos + 30.0))
            if (std::abs(q.position - pos) <= kSameTol) {
                p.fwhm = q.fwhm;
                p.area = q.area;
                break;
            }
    } catch (const std::exception&) {
    }
    return p;
}

}  // namespace

std::vector<peaks::Peak> DataPage::peaksOf(int row) const {
    std::vector<peaks::Peak> out;
    if (row < 0 || row >= count()) return out;
    const Entry& e = m_entries[size_t(row)];
    const auto [x, y] = pickData(row);
    try {
        for (const peaks::Peak& p : peaks::pick(x, y, peakSnr(), 2.0, false, kMinPeakShift)) {
            const bool removed = std::any_of(e.removedPeaks.begin(), e.removedPeaks.end(),
                                             [&](double r) { return std::abs(r - p.position) <= kSameTol; });
            if (!removed) out.push_back(p);
        }
    } catch (const std::exception&) {
        // too few points etc.: only the added peaks
    }
    for (double a : e.addedPeaks) out.push_back(manualPeak(x, y, a));
    std::sort(out.begin(), out.end(), [](const peaks::Peak& a, const peaks::Peak& b) { return a.position < b.position; });
    return out;
}

double DataPage::pixelTolerance(double px) const {
    const double w = m_plot->plotArea().width();
    const double span = m_plot->xRange().span();
    return w > 50 ? px * span / w : 0.01 * span;
}

double DataPage::drawnShift(int row) const {
    for (const Drawn& d : m_drawn)
        if (d.row == row) return d.dx;
    return 0.0;
}

bool DataPage::findPeakNear(double x, double tol, double* pos) const {
    const int row = currentRow();
    if (row < 0 || row >= count()) return false;
    double best = tol, found = NaN;
    for (const peaks::Peak& p : peaksOf(row))
        if (std::abs(p.position - x) <= best) {
            best = std::abs(p.position - x);
            found = p.position;
        }
    for (std::size_t k = 0; k < m_fitResultPositions.size() && k < m_fitPositions.size(); ++k)
        if (std::abs(m_fitResultPositions[k] - x) <= best) {   // a component: remove its start peak
            best = std::abs(m_fitResultPositions[k] - x);
            found = m_fitPositions[k];
        }
    if (std::isnan(found)) return false;
    if (pos) *pos = found;
    return true;
}

bool DataPage::addPeakAt(double x) {
    const int row = currentRow();
    if (row < 0 || row >= count()) return false;
    const auto [xs, ys] = pickData(row);
    if (xs.size() < 2 || x < xs.front() || x > xs.back()) return false;
    if (x < kMinPeakShift) {
        setNote(tr("no peaks below %1 cm⁻¹").arg(int(kMinPeakShift)));
        return false;
    }
    // snap to the highest point within ±3 cm⁻¹ (a click is rarely exactly on the top)
    std::size_t best = xs.size();
    for (std::size_t i = 0; i < xs.size(); ++i)
        if (std::abs(xs[i] - x) <= kSameTol && xs[i] >= kMinPeakShift && (best == xs.size() || ys[i] > ys[best]))
            best = i;
    const double pos = best < xs.size() ? xs[best] : x;
    Entry& e = m_entries[size_t(row)];
    for (const peaks::Peak& p : peaksOf(row))
        if (std::abs(p.position - pos) <= 1.5) {
            setNote(tr("there is a peak at %1 cm⁻¹ already").arg(py::fixed(p.position, 1)));
            return false;
        }
    const auto rem = std::find_if(e.removedPeaks.begin(), e.removedPeaks.end(),
                                  [&](double r) { return std::abs(r - pos) <= kSameTol; });
    if (rem != e.removedPeaks.end()) e.removedPeaks.erase(rem);     // a removed pick comes back
    else e.addedPeaks.push_back(pos);
    setNote({});
    peaksChanged();
    return true;
}

bool DataPage::removePeakNear(double x, double tolerance) {
    const int row = currentRow();
    if (row < 0 || row >= count()) return false;
    double pos = 0;
    if (!findPeakNear(x, tolerance < 0 ? pixelTolerance(12) : tolerance, &pos)) return false;
    Entry& e = m_entries[size_t(row)];
    const auto add = std::find_if(e.addedPeaks.begin(), e.addedPeaks.end(),
                                  [&](double a) { return std::abs(a - pos) < 1e-9; });
    if (add != e.addedPeaks.end()) e.addedPeaks.erase(add);
    else e.removedPeaks.push_back(pos);
    setNote({});
    peaksChanged();
    return true;
}

void DataPage::resetPeaks(int row) {
    if (row < 0 || row >= count()) return;
    m_entries[size_t(row)].addedPeaks.clear();
    m_entries[size_t(row)].removedPeaks.clear();
    if (row == currentRow()) peaksChanged();
}

void DataPage::peaksChanged() {
    if (hasFit() && m_fitRegion) {
        refit();
        updatePeakOverlay();
    } else {
        updatePeaks();
    }
}

PeakLabels* DataPage::peakLabelItem(int row) const {
    for (std::size_t k = 0; k < m_labelRows.size(); ++k)
        if (m_labelRows[k] == row) return m_labelItems[k];
    return nullptr;
}

void DataPage::updatePeaks() {
    if (!hasFit()) {
        m_table->setRowCount(0);
        const int r = currentRow();
        if (r >= 0 && r < count()) {
            const int dh = heightDecimals();
            for (const peaks::Peak& p : peaksOf(r))
                fillPeakRow({{p.position, 1}, {NaN, 2}, {p.height, dh}, {p.fwhm, 1}, {p.area, dh}, {p.snr, 2}});
        }
    }
    updatePeakOverlay();
}

void DataPage::updatePeakOverlay() {
    for (PeakLabels* l : m_labelItems) m_plot->removeItem(l);
    m_labelItems.clear();
    m_labelRows.clear();
    if (m_peakMarks) m_plot->removeItem(m_peakMarks);
    m_peakMarks = nullptr;
    const Labels mode = peakLabels();
    if (mode == Labels::Off || m_drawn.empty()) return;
    const int cur = currentRow();
    const PlotPalette& pal = m_plot->plotPalette();
    for (const Drawn& d : m_drawn) {
        if (mode == Labels::Current && d.row != cur) continue;
        const fileio::DataSpectrum& sp = m_entries[size_t(d.row)].sp;
        Vec x = sp.x, y = shownY(d.row);            // labels sit on the curve as drawn
        for (double& v : x) v += d.dx;
        for (double& v : y) v += d.dy;
        Vec pos;
        for (const peaks::Peak& p : peaksOf(d.row)) pos.push_back(p.position + d.dx);
        PeakLabels* l = m_plot->addPeakLabels();
        l->setAutoScale(false);
        l->setPeaks(pos, x, y, 0);
        if (mode == Labels::AllTicked && m_drawn.size() > 1) l->setColor(colorOf(d.row));
        m_labelItems.push_back(l);
        m_labelRows.push_back(d.row);
        if (d.row == cur && !pos.empty()) {         // markers on the tops of the current spectrum's peaks
            Vec my;
            for (double p : pos) my.push_back(interp1(p, x, y));
            m_peakMarks = m_plot->addScatter(pos, std::move(my), pal.accent, 5, Marker::Diamond);
            m_peakMarks->setAutoScale(false);
            m_peakMarks->setZValue(6);
        }
    }
}

void DataPage::plotClicked(const QPointF& data, Qt::MouseButton button) {
    if (button != Qt::LeftButton) return;
    const int row = currentRow();
    if (row < 0 || !m_plot->plotArea().contains(m_plot->mapFromData(data))) return;
    if (std::none_of(m_drawn.begin(), m_drawn.end(), [row](const Drawn& d) { return d.row == row; })) return;
    m_clickX = data.x() - drawnShift(row);
    m_clickTimer->start();
}

void DataPage::fillPlotMenu(QMenu& menu, double x) {
    const int row = currentRow();
    const bool shown = std::any_of(m_drawn.begin(), m_drawn.end(), [row](const Drawn& d) { return d.row == row; });
    if (row >= 0 && shown) {
        if (x >= kMinPeakShift)
            menu.addAction(tr("Add peak here (%1 cm⁻¹)").arg(py::fixed(x, 1)), this, [this, x] { addPeakAt(x); });
        double pos = 0;
        if (findPeakNear(x, pixelTolerance(12), &pos))
            menu.addAction(tr("Remove peak at %1 cm⁻¹").arg(py::fixed(pos, 1)), this,
                           [this, pos] { removePeakNear(pos, 1e-6); });
        const Entry& e = m_entries[size_t(row)];
        if (!e.addedPeaks.empty() || !e.removedPeaks.empty())
            menu.addAction(tr("Reset the peaks of this spectrum"), this, [this, row] { resetPeaks(row); });
        menu.addSeparator();
        menu.addAction(tr("Fit peaks in view"), this, [this] { fitView(); });
        menu.addAction(tr("Carbon D/G…"), this, [this] { carbonDialog(); });
        if (hasFit()) {
            menu.addAction(tr("Refit"), this, [this] { refit(); });
            menu.addAction(tr("Clear fit"), this, [this] {
                clearFit();
                setNote({});
                updatePeaks();
            });
        }
    }
}

void DataPage::plotMenu(const QPointF& data, const QPoint& global) {
    m_clickTimer->stop();
    if (!m_plot->plotArea().contains(m_plot->mapFromData(data))) return;
    QMenu menu(this);
    fillPlotMenu(menu, data.x() - drawnShift(currentRow()));
    if (!menu.actions().isEmpty()) menu.exec(global);
}

// ---------------------------------------------------------------------------------------------- fitting
void DataPage::clearFit() {
    for (PlotItem* it : m_fitItems) m_plot->removeItem(it);
    m_fitItems.clear();
    if (m_fitRegion) m_plot->removeItem(m_fitRegion);
    m_fitRegion = nullptr;
    m_fitPositions.clear();
    m_fitResultPositions.clear();
    m_fitPeaks.clear();
    m_fitShape.clear();
    m_carbon.reset();
    m_fitLabel->clear();
}

bool DataPage::fitView(QString* error) {
    if (currentRow() < 0) return false;
    const Range v = m_plot->xRange();
    return fitRange(v.lo, v.hi, error);
}

bool DataPage::refit(QString* error) {
    if (!m_fitRegion) return fitView(error);
    const Range r = m_fitRegion->region();
    return fitRange(r.lo, r.hi, error);
}

void DataPage::drawFit(const peaks::FitResult& res, int row) {
    // stacked waterfall: draw the fit where the spectrum is drawn
    double dx = 0, dy = 0;
    for (const Drawn& d : m_drawn)
        if (d.row == row) dx = d.dx, dy = d.dy;
    Vec fx = res.x;
    for (double& v : fx) v += dx;
    Vec model = res.model;
    for (double& v : model) v += dy;
    const PlotPalette& pal = m_plot->plotPalette();
    Curve* mc = m_plot->addCurve(fx, std::move(model), QPen(pal.label, 1.2, Qt::DashLine));
    mc->setAutoScale(false);
    mc->setZValue(5);
    m_fitItems.push_back(mc);
    double xm = 0;
    for (double v : res.x) xm += v;
    xm /= double(std::max<std::size_t>(1, res.x.size()));
    for (std::size_t k = 0; k < res.components.size(); ++k) {
        Vec yc = res.components[k];
        for (std::size_t i = 0; i < yc.size() && i < res.x.size(); ++i)
            yc[i] += res.baseline.first + res.baseline.second * (res.x[i] - xm) + dy;
        Curve* c = m_plot->addCurve(fx, std::move(yc), QPen(pal.seriesColor(int(k) + 3), 1));
        c->setAutoScale(false);
        c->setZValue(4);
        m_fitItems.push_back(c);
    }
}

bool DataPage::fitRange(double lo, double hi, QString* error) {
    const int row = currentRow();
    if (row < 0 || row >= count()) return false;
    if (lo > hi) std::swap(lo, hi);
    const fileio::DataSpectrum& sp = m_entries[size_t(row)].sp;
    const Vec y = shownY(row);
    // start values: the peaks of the spectrum in the window (picked + added − removed), at most
    // maxComponents() of them (the strongest)
    std::vector<peaks::Peak> guesses;
    for (const peaks::Peak& p : peaksOf(row))
        if (p.position >= lo && p.position <= hi) guesses.push_back(p);
    const std::size_t inWindow = guesses.size();
    const std::size_t cap = std::size_t(maxComponents());
    if (guesses.size() > cap) {
        std::sort(guesses.begin(), guesses.end(),
                  [](const peaks::Peak& a, const peaks::Peak& b) { return a.height > b.height; });
        guesses.resize(cap);
        std::sort(guesses.begin(), guesses.end(),
                  [](const peaks::Peak& a, const peaks::Peak& b) { return a.position < b.position; });
    }
    auto fail = [&](const QString& why) {
        const QString msg = tr("no fit in %1–%2 cm⁻¹: %3").arg(py::fixed(lo, 0), py::fixed(hi, 0), why);
        if (error) *error = msg;
        if (m_fitRegion) {                      // a dragged fit window: no fit there now
            for (PlotItem* it : m_fitItems) m_plot->removeItem(it);
            m_fitItems.clear();
            m_fitPositions.clear();
            m_fitResultPositions.clear();
            m_fitPeaks.clear();
            m_fitLabel->clear();
            updatePeaks();                      // the table shows the picked peaks again
        }
        setNote(msg);
        return false;
    };
    if (guesses.empty())
        return fail(tr("no peaks (lower S/N, or click the plot to add one)"));
    peaks::FitResult res;
    try {
        res = peaks::fit(sp.x, y, {lo, hi}, guesses, m_profile->currentText());
    } catch (const std::exception& e) {
        return fail(QString::fromUtf8(e.what()));
    }
    for (PlotItem* it : m_fitItems) m_plot->removeItem(it);
    m_fitItems.clear();
    m_carbon.reset();
    m_fitPositions.clear();
    for (const peaks::Peak& g : guesses) m_fitPositions.push_back(g.position);
    m_fitResultPositions.clear();
    for (const peaks::FitPeak& p : res.peaks) m_fitResultPositions.push_back(p.position);
    m_fitPeaks = res.peaks;
    m_fitShape = m_profile->currentText();
    drawFit(res, row);
    const PlotPalette& pal = m_plot->plotPalette();
    double dx = 0;
    for (const Drawn& d : m_drawn)
        if (d.row == row) dx = d.dx;
    if (!m_fitRegion) {
        m_fitRegion = m_plot->addRegion(lo + dx, hi + dx);
        m_fitRegion->setAutoScale(false);
        m_fitRegion->setBrush(pal.region);
        connect(m_fitRegion, &RegionItem::regionChangeFinished, this,
                [this](double a, double b) {
                    const double s = drawnShift(currentRow());
                    fitRange(a - s, b - s);
                },
                Qt::QueuedConnection);
    } else {
        const QSignalBlocker block(m_fitRegion);
        m_fitRegion->setRegion(lo + dx, hi + dx);
    }
    m_table->setRowCount(0);
    const int dh = heightDecimals();
    for (const peaks::FitPeak& p : res.peaks)
        fillPeakRow({{p.position, 1}, {p.positionErr, 2}, {p.height, dh}, {p.fwhm, 1}, {p.area, dh},
                     {p.eta ? *p.eta : NaN, 2}});
    m_fitLabel->setText(QStringLiteral("%1 fit of %2 band(s) in %3–%4 cm⁻¹: R² = %5, RMS residual %6")
                            .arg(res.shape)
                            .arg(res.peaks.size())
                            .arg(py::fixed(lo, 0), py::fixed(hi, 0), py::fixed(res.r2, 5), py::fixed(res.rms, 1)));
    if (inWindow > cap)
        setNote(tr("fitted the %1 strongest of %2 peaks in the window (raise “max.” or zoom in for more)")
                    .arg(cap)
                    .arg(inWindow));
    else
        setNote({});
    return true;
}

// ------------------------------------------------------------------------------------------ carbon D/G
bool DataPage::carbonAnalysis(carbon::Model model, QString* error) {
    const int row = currentRow();
    if (row < 0 || row >= count()) {
        if (error) *error = tr("no spectrum");
        return false;
    }
    const fileio::DataSpectrum& sp = m_entries[size_t(row)].sp;
    carbon::Result r;
    try {
        carbon::Options o;
        o.model = model;
        r = carbon::analyse(sp.x, shownY(row), o);
    } catch (const std::exception& e) {
        const QString msg = tr("no carbon D/G analysis: %1").arg(QString::fromUtf8(e.what()));
        if (error) *error = msg;
        setNote(msg);
        return false;
    }
    clearFit();
    drawFit(r.fit, row);
    std::vector<carbon::Band> bands = r.bands;
    m_fitPeaks = r.fit.peaks;
    if (r.band2d && r.fit2d) {
        drawFit(*r.fit2d, row);
        bands.push_back(*r.band2d);
        m_fitPeaks.push_back(r.fit2d->peaks[0]);
    }
    for (std::size_t k = 0; k < bands.size() && k < m_fitPeaks.size(); ++k)   // bwf: the maximum, as in the table
        m_fitPeaks[k].position = bands[k].position;
    m_fitShape = QStringLiteral("carbon ") + carbon::modelKey(model);
    m_table->setRowCount(0);
    const int dh = heightDecimals();
    for (const carbon::Band& b : bands) {
        fillPeakRow({{b.position, 1}, {b.positionErr, 2}, {b.height, dh}, {b.fwhm, 1}, {b.area, dh},
                     {b.q ? *b.q : NaN, 2}});
        auto* it = new QTableWidgetItem(b.name + QStringLiteral(" (carbon)"));
        m_table->setItem(m_table->rowCount() - 1, 6, it);
    }
    m_fitLabel->setText(QStringLiteral("Carbon D/G (%1): I_D/I_G = %2 (area), %3 (height); L_a = %4 nm; R² = %5")
                            .arg(carbon::modelName(model), py::fixed(r.idigArea, 3), py::fixed(r.idigHeight, 3),
                                 py::fixed(r.la, 1), py::fixed(r.fit.r2, 5)));
    m_carbon = std::move(r);
    m_carbonSpectrum = sp.name;
    setNote({});
    updatePeakOverlay();
    if (m_carbonTable) fillCarbonTable();
    return true;
}

void DataPage::fillCarbonTable() {
    if (!m_carbonTable) return;
    m_carbonTable->setRowCount(0);
    if (!m_carbon) return;
    for (const QStringList& r : m_carbon->rows()) {
        const int n = m_carbonTable->rowCount();
        m_carbonTable->insertRow(n);
        for (int c = 0; c < 4 && c < int(r.size()); ++c) {
            auto* it = new QTableWidgetItem(r[c]);
            if (c == 1 || c == 2) it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_carbonTable->setItem(n, c, it);
        }
    }
}

bool DataPage::exportCarbon(const QString& path) {
    if (!m_carbon) return false;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        warn(tr("Export"), tr("Could not write %1").arg(path));
        return false;
    }
    f.write(m_carbon->csv(m_carbonSpectrum).toUtf8());
    if (!f.commit()) {
        warn(tr("Export"), tr("Could not write %1").arg(path));
        return false;
    }
    return true;
}

void DataPage::carbonDialog() {
    if (!m_carbonDlg) {
        m_carbonDlg = new QDialog(this);
        m_carbonDlg->setWindowTitle(tr("Carbon D/G analysis"));
        auto* v = new QVBoxLayout(m_carbonDlg);
        auto* top = new QHBoxLayout;
        top->addWidget(new QLabel(tr("Model")));
        m_carbonModel = new QComboBox;
        m_carbonModel->addItem(carbon::modelName(carbon::Model::LorentzBwf));
        m_carbonModel->addItem(carbon::modelName(carbon::Model::Sadezky));
        top->addWidget(m_carbonModel, 1);
        auto* again = new QPushButton(tr("Analyse current spectrum"));
        top->addWidget(again);
        v->addLayout(top);
        m_carbonNote = new QLabel;
        m_carbonNote->setWordWrap(true);
        m_carbonNote->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(m_carbonNote);
        m_carbonTable = new QTableWidget(0, 4);
        m_carbonTable->setHorizontalHeaderLabels({tr("quantity"), tr("value"), QStringLiteral("±"), tr("unit")});
        m_carbonTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_carbonTable->verticalHeader()->setVisible(false);
        m_carbonTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_carbonTable->setAlternatingRowColors(true);
        m_carbonTable->setColumnWidth(0, 330);
        v->addWidget(m_carbonTable, 1);
        auto* help = new QLabel(tr("<small>Spectrum as shown (baseline-corrected); fit region 950–1800 cm⁻¹ with a "
                                   "linear baseline. G (BWF): position = maximum ω0 + Γ/(2q), q &lt; 0 = tail to low "
                                   "shift. L_a = 2.4·10⁻¹⁰ λ⁴ / (I_D/I_G) nm from the <b>area</b> ratio (Cançado 2006, "
                                   "λ = 532 nm), valid for nanocrystalline graphite (L_a ≳ 2 nm).</small>"));
        help->setWordWrap(true);
        v->addWidget(help);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
        auto* copy = bb->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
        auto* save = bb->addButton(tr("Save CSV…"), QDialogButtonBox::ActionRole);
        v->addWidget(bb);
        connect(bb, &QDialogButtonBox::rejected, m_carbonDlg, &QDialog::close);
        connect(copy, &QPushButton::clicked, this, [this] {
            if (m_carbon) QApplication::clipboard()->setText(m_carbonSpectrum + '\n' + m_carbon->table() + '\n');
        });
        connect(save, &QPushButton::clicked, this, [this] {
            if (!m_carbon) return;
            const QString p = QFileDialog::getSaveFileName(m_carbonDlg, tr("Save carbon D/G results"),
                                                           QStringLiteral("carbon.csv"), QStringLiteral("CSV (*.csv)"));
            if (!p.isEmpty()) exportCarbon(p);
        });
        connect(again, &QPushButton::clicked, this, &DataPage::runCarbon);
        connect(m_carbonModel, qOverload<int>(&QComboBox::currentIndexChanged), this, &DataPage::runCarbon);
        m_carbonDlg->resize(560, 620);
    }
    m_carbonDlg->show();
    m_carbonDlg->raise();
    runCarbon();
}

void DataPage::runCarbon() {
    QString err;
    const auto model = m_carbonModel->currentIndex() == 1 ? carbon::Model::Sadezky : carbon::Model::LorentzBwf;
    if (carbonAnalysis(model, &err)) {
        m_carbonNote->setText(tr("Spectrum: %1").arg(m_carbonSpectrum.toHtmlEscaped()));
    } else {
        m_carbonNote->setText(err.toHtmlEscaped());
        m_carbonTable->setRowCount(0);
    }
}

// =========================================================================================== calculator
int DataPage::calculate(int aRow, double a, int bRow, double b, double c) {
    if (aRow < 0 || aRow >= count() || bRow >= count()) return -1;
    const fileio::DataSpectrum& A = m_entries[size_t(aRow)].sp;
    const auto [xa, ya] = processed(A);
    Vec y(ya.size());
    for (std::size_t i = 0; i < ya.size(); ++i) y[i] = a * ya[i] + c;
    QString name = py::g(a) + "·[" + A.name + "]";
    if (bRow >= 0) {
        const fileio::DataSpectrum& B = m_entries[size_t(bRow)].sp;
        const auto [xb, yb] = processed(B);
        const Vec ybi = interp(xa, xb, yb);
        for (std::size_t i = 0; i < y.size(); ++i) y[i] += b * ybi[i];
        name += signedTerm(b) + "·[" + B.name + "]";
    }
    if (c != 0) name += signedTerm(c);
    fileio::DataSpectrum res;
    res.name = name;
    res.x = xa;
    res.y = std::move(y);
    res.meta = {{QStringLiteral("calculated"), name}, {QStringLiteral("processing"), describeProcessing()}};
    addSpectrum(res, true);
    return count() - 1;
}

void DataPage::calculatorDialog() {
    if (m_entries.empty()) return;
    QDialog d(this);
    d.setWindowTitle(tr("Spectrum calculator"));
    auto* f = new QFormLayout(&d);
    QStringList names;
    for (const Entry& e : m_entries) names << e.sp.name;
    auto* aSel = new QComboBox;
    auto* bSel = new QComboBox;
    aSel->addItems(names);
    bSel->addItems(QStringList{tr("(none)")} + names);
    aSel->setCurrentIndex(std::max(0, currentRow()));
    auto spin = [](double v) {
        auto* w = new QDoubleSpinBox;
        w->setDecimals(4);
        w->setRange(-1e9, 1e9);
        w->setValue(v);
        return w;
    };
    QDoubleSpinBox *ka = spin(1.0), *kb = spin(-1.0), *kc = spin(0.0);
    f->addRow(tr("A"), aSel);
    f->addRow(tr("a (multiplier for A)"), ka);
    f->addRow(tr("B"), bSel);
    f->addRow(tr("b (multiplier for B)"), kb);
    f->addRow(tr("c (offset)"), kc);
    f->addRow(new QLabel(tr("Result = a·A + b·B + c, using the spectra as shown (processing applied).")));
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    f->addRow(bb);
    if (d.exec() != QDialog::Accepted) return;
    calculate(aSel->currentIndex(), ka->value(), bSel->currentIndex() - 1, kb->value(), kc->value());
}

// =============================================================================================== export
QStringList DataPage::exportSpectra(const QString& dir, const QString& kind) {
    QStringList written, errors;
    for (int r : tickedRows()) {
        const fileio::DataSpectrum& s = m_entries[size_t(r)].sp;
        fileio::DataSpectrum out;
        out.name = s.name;
        std::tie(out.x, out.y) = processed(s);
        out.meta = s.meta;
        out.setMeta(QStringLiteral("processing"), describeProcessing());
        const QString path = QDir(dir).filePath(safeName(s.name) + (kind == "csv" ? ".csv" : ".jdx"));
        try {
            if (kind == "csv") fileio::writeCsv(path, out);
            else fileio::writeJcamp(path, out);
            written << path;
        } catch (const std::exception& e) {
            errors << QFileInfo(path).fileName() + ": " + QString::fromUtf8(e.what());
        }
    }
    if (!errors.isEmpty()) warn(tr("Export"), errors.join('\n'));
    return written;
}

bool DataPage::exportTable(const QString& path) {
    std::vector<fileio::DataSpectrum> list;
    for (int r : tickedRows()) {
        fileio::DataSpectrum d;
        d.name = m_entries[size_t(r)].sp.name;
        std::tie(d.x, d.y) = processed(m_entries[size_t(r)].sp);
        list.push_back(std::move(d));
    }
    if (list.empty()) return false;
    try {
        fileio::writeTable(path, list);
    } catch (const std::exception& e) {
        warn(tr("Export"), QString::fromUtf8(e.what()));
        return false;
    }
    return true;
}

bool DataPage::exportPeaks(const QString& path) {
    auto num = [](double v) { return std::isfinite(v) ? QString::number(v, 'g', 10) : QString(); };
    auto quoted = [](QString t) {
        if (t.contains(',') || t.contains('"')) t = '"' + t.replace('"', QStringLiteral("\"\"")) + '"';
        return t;
    };
    QStringList lines;
    if (hasFit()) {
        lines << QStringLiteral("spectrum,position_cm-1,position_err,height,fwhm_cm-1,area,eta,fit,possible_assignment");
        const QString name = quoted(m_entries[size_t(currentRow())].sp.name);
        for (const peaks::FitPeak& p : m_fitPeaks)
            lines << QStringList{name, num(p.position), num(p.positionErr), num(p.height), num(p.fwhm), num(p.area),
                                 p.eta ? num(*p.eta) : QString(), m_fitShape,
                                 quoted(assignmentText(p.position))}
                         .join(',');
    } else {
        lines << QStringLiteral("spectrum,position_cm-1,height,fwhm_cm-1,area,snr,added_by_hand,possible_assignment");
        std::vector<int> rows;
        if (peakLabels() == Labels::AllTicked) rows = tickedRows();
        else if (currentRow() >= 0) rows = {currentRow()};
        for (int r : rows) {
            const Entry& e = m_entries[size_t(r)];
            for (const peaks::Peak& p : peaksOf(r)) {
                const bool manual = std::any_of(e.addedPeaks.begin(), e.addedPeaks.end(),
                                                [&](double a) { return std::abs(a - p.position) < 1e-9; });
                lines << QStringList{quoted(e.sp.name), num(p.position), num(p.height), num(p.fwhm), num(p.area),
                                     num(p.snr), manual ? QStringLiteral("yes") : QStringLiteral("no"),
                                     quoted(assignmentText(p.position))}
                             .join(',');
            }
        }
        if (rows.empty()) return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        warn(tr("Export"), tr("Could not write %1").arg(path));
        return false;
    }
    f.write((lines.join('\n') + '\n').toUtf8());
    if (!f.commit()) {
        warn(tr("Export"), tr("Could not write %1").arg(path));
        return false;
    }
    return true;
}

bool DataPage::exportOneFile(const QString& path) {
    std::vector<fileio::DataSpectrum> list;
    for (int r : tickedRows()) {
        const fileio::DataSpectrum& s = m_entries[size_t(r)].sp;
        fileio::DataSpectrum d;
        d.name = s.name;
        std::tie(d.x, d.y) = processed(s);
        d.meta = s.meta;
        d.setMeta(QStringLiteral("processing"), describeProcessing());
        list.push_back(std::move(d));
    }
    if (list.empty()) return false;
    const QString title = tr("%n spectra", nullptr, int(list.size()));
    try {
        if (path.endsWith(QStringLiteral(".spc"), Qt::CaseInsensitive)) fileio::writeSpcMulti(path, list, title);
        else fileio::writeJcampMulti(path, list, title);
    } catch (const std::exception& e) {
        warn(tr("Export"), QString::fromUtf8(e.what()));
        return false;
    }
    return true;
}

bool DataPage::exportPlot(const QString& path) {
    PlotWidget* w = m_stack->currentIndex() == 1 ? m_heat : m_plot;
    QSize size = w->isVisible() && w->width() > 200 ? w->size() : QSize(1000, 620);
    bool ok;
    if (path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) ok = w->exportSvg(path, size);
    else ok = w->exportPng(path, size, 2000.0 / size.width());     // Python: 2000 px wide
    if (!ok) warn(tr("Export"), tr("Could not write %1").arg(path));
    return ok;
}

void DataPage::openFilesDialog() {
    const QStringList paths =
        QFileDialog::getOpenFileNames(this, tr("Open spectra"), QDir::homePath(), QString::fromLatin1(OPEN_FILTER));
    if (!paths.isEmpty()) openFiles(paths);
}

void DataPage::exportDialog(const QString& kind) {
    if (kind == "png" || kind == "svg") {
        const QString p = QFileDialog::getSaveFileName(this, tr("Save plot"), "spectra." + kind,
                                                       kind.toUpper() + " (*." + kind + ")");
        if (!p.isEmpty()) exportPlot(p);
        return;
    }
    if (kind == "peaks") {
        const QString p = QFileDialog::getSaveFileName(this, tr("Save peak table"), "peaks.csv", "CSV (*.csv)");
        if (!p.isEmpty()) exportPeaks(p);
        return;
    }
    if (tickedRows().empty()) return;
    if (kind == "table") {
        const QString p = QFileDialog::getSaveFileName(this, tr("Save table"), "spectra.csv", "CSV (*.csv)");
        if (!p.isEmpty()) exportTable(p);
        return;
    }
    if (kind == "onejdx" || kind == "onespc") {
        const bool spc = kind == "onespc";
        const QString p = QFileDialog::getSaveFileName(this, tr("Save spectra in one file"),
                                                       spc ? "spectra.spc" : "spectra.jdx",
                                                       spc ? "Galactic SPC (*.spc)" : "JCAMP-DX (*.jdx *.dx)");
        if (!p.isEmpty()) exportOneFile(p);
        return;
    }
    const QString d = QFileDialog::getExistingDirectory(this, tr("Folder for the exported spectra"));
    if (!d.isEmpty()) exportSpectra(d, kind);
}

// ============================================================================================= messages
void DataPage::warn(const QString& title, const QString& text) {
    emit message(title, text);
    if (m_messages) QMessageBox::warning(this, title, text);
}

void DataPage::info(const QString& title, const QString& text) {
    emit message(title, text);
    if (m_messages) QMessageBox::information(this, title, text);
}

}  // namespace lr::gui
