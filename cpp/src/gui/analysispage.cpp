// Analysis area: data set, preprocessing with live preview, PCA / EFA / pure variables / MCR-ALS / NNLS /
// peak trends.
#include "analysispage.h"

#include <QAbstractTableModel>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMetaObject>
#include <QPushButton>
#include <QSaveFile>
#include <QSlider>
#include <QSpinBox>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTextStream>
#include <QElapsedTimer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/errors.h"
#include "core/processing.h"
#include "plotwidget.h"
#include "theme.h"

namespace lr::gui {

using namespace lr::chemo;

namespace {

const QString kShift = QStringLiteral("Raman shift (cm⁻¹)");

int methodIndex(AnalysisPage::Method m) { return int(m); }

QString pct(double v, int decimals = 1) { return QString::number(100.0 * v, 'f', decimals) + QStringLiteral(" %"); }

Vec col(const Matrix& M, Eigen::Index j) { return toVec(M.col(j)); }
Vec rowv(const Matrix& M, Eigen::Index i) { return toVec(M.row(i).transpose()); }

Vec scaled(Vec v, double s) {
    for (double& x : v) x *= s;
    return v;
}

double absMax(const Vec& v) {
    double m = 0;
    for (double x : v)
        if (std::isfinite(x)) m = std::max(m, std::abs(x));
    return m;
}

// Largest |value| with its sign (to scale a curve to a maximum of +1).
double signedMax(const Vec& v) {
    double m = 0;
    for (double x : v)
        if (std::isfinite(x) && std::abs(x) > std::abs(m)) m = x;
    return m;
}

Vec seq(int n, int first = 1) {
    Vec v;
    for (int i = 0; i < n; ++i) v.push_back(first + i);
    return v;
}

QString num(double v) { return QString::number(v, 'g', 10); }

// Columns to CSV: optional text column first.
bool writeCsv(const QString& path, const QStringList& comments, const QStringList& header, const QStringList& text,
              const std::vector<Vec>& cols, QString& error) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    QTextStream o(&f);
    for (const QString& c : comments) o << "# " << c << "\n";
    QStringList h;
    for (const QString& s : header) h << (s.contains(',') ? '"' + s + '"' : s);
    o << h.join(',') << "\n";
    size_t rows = text.isEmpty() ? 0 : size_t(text.size());
    for (const Vec& c : cols) rows = std::max(rows, c.size());
    for (size_t r = 0; r < rows; ++r) {
        QStringList cells;
        if (!text.isEmpty()) {
            QString t = r < size_t(text.size()) ? text[int(r)] : QString();
            if (t.contains(',') || t.contains('"')) t = '"' + t.replace("\"", "\"\"") + '"';
            cells << t;
        }
        for (const Vec& c : cols) cells << (r < c.size() && std::isfinite(c[r]) ? num(c[r]) : QString());
        o << cells.join(',') << "\n";
    }
    if (!f.commit()) {
        error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    return true;
}

// An invisible two-point curve that widens the automatic ranges by `frac` of the data span on every side
// (scatter plots: markers and labels at the extremes stay inside the plot).
void padRange(PlotWidget* p, const Vec& xs, const Vec& ys, double frac = 0.06) {
    Bounds bx, by;
    for (double v : xs)
        if (std::isfinite(v)) bx.add(v);
    for (double v : ys)
        if (std::isfinite(v)) by.add(v);
    if (!bx.valid() || !by.valid()) return;
    const double dx = std::max(bx.hi - bx.lo, 1e-12) * frac, dy = std::max(by.hi - by.lo, 1e-12) * frac;
    auto* c = p->addCurve({bx.lo - dx, bx.hi + dx}, {by.lo - dy, by.hi + dy});
    c->setLineVisible(false);
    c->setMarker(Marker::None);
}

QColor faded(QColor c, int alpha) {
    c.setAlpha(alpha);
    return c;
}

}  // namespace

// ============================================================================================ construction
AnalysisPage::AnalysisPage(QWidget* parent) : QWidget(parent) {
    m_updating = true;                            // no reactions to the initial widget values
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(200);
    connect(m_timer, &QTimer::timeout, this, &AnalysisPage::runVisible);

    auto* split = new QSplitter(Qt::Horizontal, this);
    {   // the data panel scrolls instead of squashing its widgets on small screens
        auto* scroll = new QScrollArea;
        scroll->setWidget(buildDataPanel());
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setMinimumWidth(scroll->widget()->minimumSizeHint().width() + 16);
        split->addWidget(scroll);
    }
    m_tabs = new QTabWidget;
    m_tabs->addTab(buildPcaTab(), QStringLiteral("PCA"));
    m_tabs->addTab(buildEfaTab(), QStringLiteral("EFA"));
    m_tabs->addTab(buildPureTab(), QStringLiteral("Pure variables"));
    m_tabs->addTab(buildMcrTab(), QStringLiteral("MCR-ALS"));
    m_tabs->addTab(buildNnlsTab(), QStringLiteral("NNLS fit"));
    m_tabs->addTab(buildTrendsTab(), QStringLiteral("Peak trends"));
    m_tabs->setTabToolTip(0, QStringLiteral("Principal component analysis: scores, loadings, explained variance"));
    m_tabs->setTabToolTip(1, QStringLiteral("Evolving factor analysis: where components appear and disappear"));
    m_tabs->setTabToolTip(2, QStringLiteral("OPA and SIMPLISMA: purest spectra or variables (initial estimates)"));
    m_tabs->setTabToolTip(3, QStringLiteral("Multivariate curve resolution: pure spectra and concentration profiles"));
    m_tabs->setTabToolTip(4, QStringLiteral("Non-negative least-squares fit with known spectra"));
    m_tabs->setTabToolTip(5, QStringLiteral("Band heights, areas, fitted areas, positions and widths vs time (kinetics)"));
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] { schedule(); });
    split->addWidget(m_tabs);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({340, 1000});
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(split);
    m_updating = false;
    preprocessChanged();
}

AnalysisPage::~AnalysisPage() {
    m_stop = true;
    m_trendStop = true;
    joinWorker();
    if (m_autoThread.joinable()) m_autoThread.join();
    if (m_trendThread.joinable()) m_trendThread.join();
}

PlotWidget* AnalysisPage::newPlot(const QString& xLabel, const QString& yLabel) {
    auto* p = new PlotWidget;
    p->setLabel(Axis::Bottom, xLabel);
    p->setLabel(Axis::Left, yLabel);
    p->setHeadroom(0.08);
    p->setWheelMode(WheelMode::ZoomX);
    p->setToolTip(PlotWidget::navigationHelp(WheelMode::ZoomX));
    return p;
}

QWidget* AnalysisPage::titled(const QString& name, const QString& title, PlotWidget* plot) {
    m_plots.push_back({name, title, plot});
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* t = new QLabel(title);
    QFont f = t->font();
    f.setBold(true);
    t->setFont(f);
    v->addWidget(t);
    v->addWidget(plot, 1);
    return w;
}

QToolButton* AnalysisPage::exportButton(Method m) {
    auto* b = new QToolButton;
    b->setText(QStringLiteral("Export ▾"));
    b->setPopupMode(QToolButton::InstantPopup);
    b->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0px; }"));
    auto* menu = new QMenu(b);
    struct Item {
        QString label, kind;
    };
    std::vector<Item> csv;
    QStringList plots;
    switch (m) {
    case Method::Pca:
        csv = {{"Scores, Q and T² (CSV)…", "scores"}, {"Loadings (CSV)…", "loadings"}, {"Explained variance (CSV)…", "variance"}};
        plots = {"pca-scree", "pca-scores", "pca-loadings"};
        break;
    case Method::Efa:
        csv = {{"Singular values (CSV)…", "efa"}};
        plots = {"efa"};
        break;
    case Method::PureVariables:
        csv = {{"Pure spectra (CSV)…", "pure"}};
        plots = {"pure-purity", "pure-spectra"};
        break;
    case Method::Mcr:
        csv = {{"Resolved spectra (CSV)…", "spectra"}, {"Concentration profiles (CSV)…", "concentrations"}};
        plots = {"mcr-spectra", "mcr-concentrations", "mcr-lof"};
        break;
    case Method::Nnls:
        csv = {{"Fit results (CSV)…", "nnls"}};
        plots = {"nnls-fit", "nnls-residual"};
        break;
    case Method::Trends:
        csv = {{"Peak trends (CSV)…", "trends"}};
        plots = {"trends-spectra", "trends"};
        break;
    }
    for (const Item& it : csv) {
        const QString kind = it.kind;
        menu->addAction(it.label, this, [this, kind] {
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export results"), kind + ".csv",
                                                              QStringLiteral("CSV files (*.csv)"));
            if (path.isEmpty()) return;
            if (!exportCsv(kind, path)) emit statusMessage(m_error);
            else emit statusMessage(QStringLiteral("Saved %1").arg(path));
        });
    }
    menu->addSeparator();
    for (const QString& name : plots) {
        QString title = name;
        for (const PlotEntry& e : m_plots)
            if (e.name == name) title = e.title;
        menu->addAction(QStringLiteral("Plot: %1 (PNG/SVG)…").arg(title), this, [this, name] {
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export plot"), name + ".png",
                                                              QStringLiteral("PNG image (*.png);;SVG image (*.svg)"));
            if (path.isEmpty()) return;
            if (!exportPlot(name, path)) emit statusMessage(m_error);
            else emit statusMessage(QStringLiteral("Saved %1").arg(path));
        });
    }
    b->setMenu(menu);
    return b;
}

// ---------------------------------------------------------------------------------------------- data panel
QWidget* AnalysisPage::buildDataPanel() {
    auto* panel = new QWidget;
    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(6, 6, 6, 6);

    auto* dataBox = new QGroupBox(QStringLiteral("Data set"));
    dataBox->setStyleSheet(theme::groupBoxStyle());
    auto* dv = new QVBoxLayout(dataBox);
    m_list = new QListWidget;
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setToolTip(QStringLiteral("Checked spectra are analysed (resampled onto their common range if needed)"));
    m_list->setMinimumHeight(80);
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
        if (m_updating) return;
        const int i = m_list->row(it);
        if (i >= 0 && size_t(i) < m_checked.size()) {
            m_checked[size_t(i)] = it->checkState() == Qt::Checked;
            dataSetChanged();
        }
    });
    dv->addWidget(m_list, 1);
    auto* row = new QHBoxLayout;
    auto* open = new QPushButton(QStringLiteral("Add files…"));
    open->setToolTip(QStringLiteral("Spectra or a time series (series.csv, series.jdx, series.spc); any format the Data area reads"));
    connect(open, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Add spectra"), QString(),
            QStringLiteral("Spectra (*.csv *.txt *.spc *.jdx *.dx *.jcamp);;All files (*)"));
        for (const QString& f : files)
            if (!addFile(f)) emit statusMessage(m_error);
    });
    auto* remove = new QPushButton(QStringLiteral("Remove"));
    connect(remove, &QPushButton::clicked, this, [this] {
        std::vector<int> rows;
        for (QListWidgetItem* it : m_list->selectedItems()) rows.push_back(m_list->row(it));
        std::sort(rows.rbegin(), rows.rend());
        for (int r : rows) {
            m_spectra.erase(m_spectra.begin() + r);
            m_checked.erase(m_checked.begin() + r);
        }
        refreshList();
        dataSetChanged();
    });
    auto* clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, &AnalysisPage::clearSpectra);
    auto* all = new QToolButton;
    all->setText(QStringLiteral("All/none"));
    connect(all, &QToolButton::clicked, this, [this] {
        const bool any = std::find(m_checked.begin(), m_checked.end(), false) != m_checked.end();
        std::fill(m_checked.begin(), m_checked.end(), any);
        refreshList();
        dataSetChanged();
    });
    row->addWidget(open);
    row->addWidget(remove);
    dv->addLayout(row);
    auto* row2 = new QHBoxLayout;
    row2->addWidget(clear);
    row2->addWidget(all);
    dv->addLayout(row2);
    m_dataInfo = new QLabel;
    m_dataInfo->setWordWrap(true);
    dv->addWidget(m_dataInfo);
    v->addWidget(dataBox, 2);

    auto* ppBox = new QGroupBox(QStringLiteral("Preprocessing"));
    ppBox->setStyleSheet(theme::groupBoxStyle());
    auto* form = new QFormLayout(ppBox);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto changed = [this] { preprocessChanged(); };
    // region
    m_regionOn = new QCheckBox;
    m_xmin = new QDoubleSpinBox;
    m_xmax = new QDoubleSpinBox;
    for (QDoubleSpinBox* s : {m_xmin, m_xmax}) {
        s->setRange(-2000, 10000);
        s->setDecimals(0);
        s->setSingleStep(10);
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] {
            if (!m_updating) {
                m_regionOn->setChecked(true);
                preprocessChanged();
            }
        });
    }
    m_xmin->setValue(200);
    m_xmax->setValue(3200);
    connect(m_regionOn, &QCheckBox::toggled, this, changed);
    auto* reg = new QHBoxLayout;
    reg->addWidget(m_regionOn);
    reg->addWidget(m_xmin, 1);
    reg->addWidget(new QLabel(QStringLiteral("–")));
    reg->addWidget(m_xmax, 1);
    m_regionOn->setToolTip(QStringLiteral("Analyse only this Raman-shift range (cm⁻¹); off: the whole range"));
    form->addRow(QStringLiteral("Region"), reg);
    // smoothing
    m_smooth = new QCheckBox;
    m_smoothWindow = new QSpinBox;
    m_smoothWindow->setRange(5, 51);
    m_smoothWindow->setSingleStep(2);
    m_smoothWindow->setValue(9);
    m_smoothWindow->setSuffix(QStringLiteral(" points"));
    connect(m_smooth, &QCheckBox::toggled, this, changed);
    connect(m_smoothWindow, &QSpinBox::valueChanged, this, changed);
    auto* sm = new QHBoxLayout;
    sm->addWidget(m_smooth);
    sm->addWidget(m_smoothWindow, 1);
    form->addRow(QStringLiteral("Smoothing"), sm);
    // baseline
    m_baseline = new QComboBox;
    for (const auto& b : processing::baselines()) m_baseline->addItem(b.second, b.first);
    m_baseline->setCurrentIndex(std::max(0, m_baseline->findData(QStringLiteral("none"))));
    m_baseline->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);   // long names: popup only
    m_baseline->setMinimumContentsLength(16);
    m_strength = new QSlider(Qt::Horizontal);
    m_strength->setRange(0, 20);
    m_strength->setValue(10);
    m_strength->setToolTip(QStringLiteral("Baseline strength 0–10 (larger = stiffer baseline)"));
    connect(m_baseline, &QComboBox::currentIndexChanged, this, changed);
    connect(m_strength, &QSlider::valueChanged, this, changed);
    form->addRow(QStringLiteral("Baseline"), m_baseline);
    form->addRow(QStringLiteral("Strength"), m_strength);
    // normalisation
    m_normalize = new QComboBox;
    m_normalize->addItem(QStringLiteral("None"), QStringLiteral("none"));
    m_normalize->addItem(QStringLiteral("Maximum"), QStringLiteral("max"));
    m_normalize->addItem(QStringLiteral("Area"), QStringLiteral("area"));
    m_normalize->addItem(QStringLiteral("Band"), QStringLiteral("band"));
    m_band = new QDoubleSpinBox;
    m_band->setRange(-2000, 10000);
    m_band->setDecimals(0);
    m_band->setValue(1000);
    m_band->setSuffix(QStringLiteral(" cm⁻¹"));
    m_band->setEnabled(false);
    connect(m_normalize, &QComboBox::currentIndexChanged, this, [this] {
        m_band->setEnabled(m_normalize->currentData().toString() == QLatin1String("band"));
        preprocessChanged();
    });
    connect(m_band, &QDoubleSpinBox::valueChanged, this, changed);
    auto* nl = new QHBoxLayout;
    nl->addWidget(m_normalize, 1);
    nl->addWidget(m_band, 1);
    form->addRow(QStringLiteral("Normalise"), nl);
    // derivative
    m_derivative = new QComboBox;
    m_derivative->addItems({QStringLiteral("None"), QStringLiteral("1st"), QStringLiteral("2nd")});
    m_derivWindow = new QSpinBox;
    m_derivWindow->setRange(5, 51);
    m_derivWindow->setSingleStep(2);
    m_derivWindow->setValue(11);
    m_derivWindow->setSuffix(QStringLiteral(" points"));
    connect(m_derivative, &QComboBox::currentIndexChanged, this, changed);
    connect(m_derivWindow, &QSpinBox::valueChanged, this, changed);
    auto* dl = new QHBoxLayout;
    dl->addWidget(m_derivative, 1);
    dl->addWidget(m_derivWindow, 1);
    form->addRow(QStringLiteral("Derivative"), dl);
    v->addWidget(ppBox);

    // preview
    auto* pv = new QHBoxLayout;
    auto* pl = new QLabel(QStringLiteral("Preview"));
    QFont f = pl->font();
    f.setBold(true);
    pl->setFont(f);
    m_previewMode = new QComboBox;
    m_previewMode->addItem(QStringLiteral("Preprocessed"));
    m_previewMode->addItem(QStringLiteral("Raw, with region"));
    connect(m_previewMode, &QComboBox::currentIndexChanged, this, &AnalysisPage::updatePreview);
    pv->addWidget(pl);
    pv->addStretch();
    pv->addWidget(m_previewMode);
    v->addLayout(pv);
    m_preview = newPlot(kShift, QStringLiteral("Intensity"));
    m_preview->setMinimumHeight(180);
    m_plots.push_back({QStringLiteral("preview"), QStringLiteral("Preview"), m_preview});
    v->addWidget(m_preview, 3);
    panel->setMinimumWidth(300);
    return panel;
}

// ---------------------------------------------------------------------------------------------- PCA tab
QWidget* AnalysisPage::buildPcaTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    auto* bar = new QHBoxLayout;
    m_pcaK = new QSpinBox;
    m_pcaK->setRange(1, 20);
    m_pcaK->setValue(4);
    m_pcaScaling = new QComboBox;
    m_pcaScaling->addItems({QStringLiteral("None"), QStringLiteral("Mean-centred"), QStringLiteral("Autoscaled")});
    m_pcaScaling->setCurrentIndex(1);
    m_pcaView = new QComboBox;
    m_pcaView->addItems({QStringLiteral("Scores"), QStringLiteral("Scores vs time"), QStringLiteral("Q vs T²")});
    m_pcaX = new QSpinBox;
    m_pcaY = new QSpinBox;
    m_pcaX->setRange(1, 20);
    m_pcaY->setRange(1, 20);
    m_pcaX->setValue(1);
    m_pcaY->setValue(2);
    m_pcaX->setPrefix(QStringLiteral("PC"));
    m_pcaY->setPrefix(QStringLiteral("PC"));
    m_pcaLabels = new QComboBox;
    m_pcaLabels->addItems({QStringLiteral("No labels"), QStringLiteral("Number"), QStringLiteral("Name"), QStringLiteral("Time")});
    m_pcaLabels->setCurrentIndex(1);
    bar->addWidget(new QLabel(QStringLiteral("Components")));
    bar->addWidget(m_pcaK);
    bar->addWidget(new QLabel(QStringLiteral("Scaling")));
    bar->addWidget(m_pcaScaling);
    bar->addSpacing(12);
    bar->addWidget(m_pcaView);
    bar->addWidget(m_pcaX);
    bar->addWidget(new QLabel(QStringLiteral("vs")));
    bar->addWidget(m_pcaY);
    bar->addWidget(m_pcaLabels);
    bar->addStretch();
    bar->addWidget(exportButton(Method::Pca));
    v->addLayout(bar);
    connect(m_pcaK, &QSpinBox::valueChanged, this, [this] { m_dirty[0] = true; schedule(); });
    connect(m_pcaScaling, &QComboBox::currentIndexChanged, this, [this] { m_dirty[0] = true; schedule(); });
    for (QComboBox* c : {m_pcaView, m_pcaLabels}) connect(c, &QComboBox::currentIndexChanged, this, &AnalysisPage::drawPca);
    for (QSpinBox* s : {m_pcaX, m_pcaY}) connect(s, &QSpinBox::valueChanged, this, &AnalysisPage::drawPca);

    m_scree = newPlot(QStringLiteral("Component"), QStringLiteral("Explained variance (%)"));
    m_scores = newPlot(QStringLiteral("PC1"), QStringLiteral("PC2"));
    m_scores->setPanY(true);
    m_loadings = newPlot(kShift, QStringLiteral("Loading"));
    m_loadings->setLegendVisible(true);
    m_loadings->setHeadroom(0.25);
    auto* top = new QSplitter(Qt::Horizontal);
    top->addWidget(titled(QStringLiteral("pca-scree"), QStringLiteral("Explained variance"), m_scree));
    top->addWidget(titled(QStringLiteral("pca-scores"), QStringLiteral("Scores"), m_scores));
    top->setSizes({300, 500});
    auto* vs = new QSplitter(Qt::Vertical);
    vs->addWidget(top);
    vs->addWidget(titled(QStringLiteral("pca-loadings"), QStringLiteral("Loadings"), m_loadings));
    v->addWidget(vs, 1);
    m_pcaInfo = new QLabel;
    m_pcaInfo->setWordWrap(true);
    m_pcaInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_pcaInfo);
    return w;
}

// ---------------------------------------------------------------------------------------------- EFA tab
QWidget* AnalysisPage::buildEfaTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    auto* bar = new QHBoxLayout;
    m_efaK = new QSpinBox;
    m_efaK->setRange(1, 15);
    m_efaK->setValue(4);
    bar->addWidget(new QLabel(QStringLiteral("Singular values")));
    bar->addWidget(m_efaK);
    auto* note = new QLabel(QStringLiteral("solid: forward (spectra 1…i), dashed: backward (spectra i…n)"));
    note->setStyleSheet(QStringLiteral("color: %1").arg(theme::mutedText().name()));
    bar->addWidget(note);
    bar->addStretch();
    bar->addWidget(exportButton(Method::Efa));
    v->addLayout(bar);
    connect(m_efaK, &QSpinBox::valueChanged, this, [this] { m_dirty[1] = true; schedule(); });
    m_efaPlot = newPlot(QStringLiteral("Time (s)"), QStringLiteral("log₁₀ singular value"));
    m_efaPlot->setLegendVisible(true);
    m_efaPlot->setHeadroom(0.12);
    v->addWidget(titled(QStringLiteral("efa"), QStringLiteral("Evolving factor analysis"), m_efaPlot), 1);
    m_efaInfo = new QLabel;
    m_efaInfo->setWordWrap(true);
    m_efaInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_efaInfo);
    return w;
}

// ---------------------------------------------------------------------------------------------- pure variables
QWidget* AnalysisPage::buildPureTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    auto* bar = new QHBoxLayout;
    m_pureMethod = new QComboBox;
    m_pureMethod->addItem(QStringLiteral("SIMPLISMA (pure variables)"));
    m_pureMethod->addItem(QStringLiteral("OPA (purest spectra)"));
    m_pureMethod->addItem(QStringLiteral("OPA (purest variables)"));
    m_pureK = new QSpinBox;
    m_pureK->setRange(1, 15);
    m_pureK->setValue(3);
    m_offset = new QDoubleSpinBox;
    m_offset->setRange(0, 100);
    m_offset->setDecimals(1);
    m_offset->setValue(5);
    m_offset->setSuffix(QStringLiteral(" %"));
    m_offset->setToolTip(QStringLiteral("SIMPLISMA offset: % of the largest mean intensity added to the means, so that "
                                        "weak, noisy variables do not look pure"));
    bar->addWidget(m_pureMethod);
    bar->addWidget(new QLabel(QStringLiteral("Components")));
    bar->addWidget(m_pureK);
    bar->addWidget(new QLabel(QStringLiteral("Offset")));
    bar->addWidget(m_offset);
    bar->addStretch();
    bar->addWidget(exportButton(Method::PureVariables));
    v->addLayout(bar);
    connect(m_pureMethod, &QComboBox::currentIndexChanged, this, [this] {
        m_offset->setEnabled(m_pureMethod->currentIndex() == 0);
        m_dirty[2] = true;
        schedule();
    });
    connect(m_pureK, &QSpinBox::valueChanged, this, [this] { m_dirty[2] = true; schedule(); });
    connect(m_offset, &QDoubleSpinBox::valueChanged, this, [this] { m_dirty[2] = true; schedule(); });
    m_purity = newPlot(kShift, QStringLiteral("Purity (scaled)"));
    m_purity->setLegendVisible(true);
    m_purity->setHeadroom(0.3);
    m_pureSpectra = newPlot(kShift, QStringLiteral("Intensity (scaled)"));
    m_pureSpectra->setLegendVisible(true);
    m_pureSpectra->setHeadroom(0.3);
    auto* vs = new QSplitter(Qt::Vertical);
    vs->addWidget(titled(QStringLiteral("pure-purity"), QStringLiteral("Purity / dissimilarity"), m_purity));
    vs->addWidget(titled(QStringLiteral("pure-spectra"), QStringLiteral("Pure-component estimates"), m_pureSpectra));
    v->addWidget(vs, 1);
    m_pureInfo = new QLabel;
    m_pureInfo->setWordWrap(true);
    m_pureInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_pureInfo);
    return w;
}

// ---------------------------------------------------------------------------------------------- MCR-ALS
QWidget* AnalysisPage::buildMcrTab() {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    auto* side = new QWidget;
    side->setMaximumWidth(290);
    auto* sv = new QVBoxLayout(side);
    sv->setContentsMargins(0, 0, 0, 0);

    auto* setup = new QGroupBox(QStringLiteral("Model"));
    setup->setStyleSheet(theme::groupBoxStyle());
    auto* form = new QFormLayout(setup);
    m_mcrK = new QSpinBox;
    m_mcrK->setRange(1, 12);
    m_mcrK->setValue(3);
    m_mcrInit = new QComboBox;
    m_mcrInit->addItem(QStringLiteral("SIMPLISMA spectra"));
    m_mcrInit->addItem(QStringLiteral("OPA spectra"));
    m_mcrInit->addItem(QStringLiteral("EFA concentrations"));
    m_mcrInit->setToolTip(QStringLiteral("Initial estimates; SIMPLISMA uses the offset of the Pure variables tab"));
    form->addRow(QStringLiteral("Components"), m_mcrK);
    form->addRow(QStringLiteral("Start from"), m_mcrInit);
    sv->addWidget(setup);

    auto* cons = new QGroupBox(QStringLiteral("Constraints"));
    cons->setStyleSheet(theme::groupBoxStyle());
    auto* cv = new QVBoxLayout(cons);
    m_nnC = new QCheckBox(QStringLiteral("Non-negative concentrations"));
    m_nnS = new QCheckBox(QStringLiteral("Non-negative spectra"));
    m_nnC->setChecked(true);
    m_nnS->setChecked(true);
    m_unimodal = new QCheckBox(QStringLiteral("Unimodal concentrations"));
    m_unimodal->setToolTip(QStringLiteral("Each concentration profile has a single maximum (e.g. intermediates)"));
    m_closure = new QCheckBox(QStringLiteral("Closure, total"));
    m_closure->setToolTip(QStringLiteral("The concentrations of every spectrum add up to the total (closed system)"));
    m_closureTotal = new QDoubleSpinBox;
    m_closureTotal->setRange(1e-6, 1e9);
    m_closureTotal->setDecimals(3);
    m_closureTotal->setValue(1.0);
    auto* cl = new QHBoxLayout;
    cl->addWidget(m_closure);
    cl->addWidget(m_closureTotal, 1);
    cv->addWidget(m_nnC);
    cv->addWidget(m_nnS);
    cv->addWidget(m_unimodal);
    cv->addLayout(cl);
    auto* kn = new QHBoxLayout;
    auto* knownBtn = new QPushButton(QStringLiteral("Known spectra…"));
    knownBtn->setToolTip(QStringLiteral("Spectra of known components; they are kept fixed (the first components)"));
    connect(knownBtn, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Known spectra"), QString(),
            QStringLiteral("Spectra (*.csv *.txt *.spc *.jdx *.dx *.jcamp);;All files (*)"));
        if (files.isEmpty()) return;
        std::vector<fileio::DataSpectrum> sp;
        try {
            for (const QString& f : files)
                for (auto& d : fileio::read(f)) sp.push_back(d);
        } catch (const Error& e) {
            emit statusMessage(QString::fromStdString(e.what()));
            return;
        }
        setKnownSpectra(sp);
    });
    auto* knownClear = new QToolButton;
    knownClear->setText(QStringLiteral("Clear"));
    connect(knownClear, &QToolButton::clicked, this, [this] { setKnownSpectra({}); });
    kn->addWidget(knownBtn, 1);
    kn->addWidget(knownClear);
    cv->addLayout(kn);
    m_knownLabel = new QLabel(QStringLiteral("No fixed spectra"));
    m_knownLabel->setWordWrap(true);
    m_knownLabel->setStyleSheet(QStringLiteral("color: %1").arg(theme::mutedText().name()));
    cv->addWidget(m_knownLabel);
    sv->addWidget(cons);

    auto* conv = new QGroupBox(QStringLiteral("Iterations"));
    conv->setStyleSheet(theme::groupBoxStyle());
    auto* cf = new QFormLayout(conv);
    m_maxIter = new QSpinBox;
    m_maxIter->setRange(1, 10000);
    m_maxIter->setValue(200);
    m_tol = new QDoubleSpinBox;
    m_tol->setRange(0, 10);
    m_tol->setDecimals(3);
    m_tol->setSingleStep(0.01);
    m_tol->setValue(0.1);
    m_tol->setSuffix(QStringLiteral(" %"));
    m_tol->setToolTip(QStringLiteral("Stop when the residual standard deviation changes by less than this"));
    cf->addRow(QStringLiteral("Maximum"), m_maxIter);
    cf->addRow(QStringLiteral("Convergence"), m_tol);
    sv->addWidget(conv);

    auto* runRow = new QHBoxLayout;
    m_runMcr = new QPushButton(QStringLiteral("▶ Run"));
    m_stopMcr = new QPushButton(QStringLiteral("■ Stop"));
    m_stopMcr->setEnabled(false);
    connect(m_runMcr, &QPushButton::clicked, this, [this] { runMcr(false); });
    connect(m_stopMcr, &QPushButton::clicked, this, &AnalysisPage::stopMcr);
    runRow->addWidget(m_runMcr, 1);
    runRow->addWidget(m_stopMcr, 1);
    sv->addLayout(runRow);
    m_bandsButton = new QPushButton(QStringLiteral("Rotational ambiguity"));
    m_bandsButton->setToolTip(QStringLiteral(
        "MCR-BANDS: the smallest and largest contribution of each component over all rotations of the solution "
        "that fit equally well and respect the non-negativity constraints. A wide band means that the shapes "
        "are not unique."));
    m_bandsButton->setEnabled(false);
    connect(m_bandsButton, &QPushButton::clicked, this, [this] { computeAmbiguity(false); });
    sv->addWidget(m_bandsButton);
    m_showBands = new QCheckBox(QStringLiteral("Show band extremes"));
    m_showBands->setChecked(true);
    connect(m_showBands, &QCheckBox::toggled, this, &AnalysisPage::drawMcr);
    sv->addWidget(m_showBands);
    m_mcrInfo = new QLabel;
    m_mcrInfo->setWordWrap(true);
    m_mcrInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_mcrInfo->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    sv->addWidget(m_mcrInfo, 1);
    auto* ex = new QHBoxLayout;
    ex->addStretch();
    ex->addWidget(exportButton(Method::Mcr));
    sv->addLayout(ex);
    h->addWidget(side);

    m_mcrSpectra = newPlot(kShift, QStringLiteral("Intensity (scaled)"));
    m_mcrSpectra->setLegendVisible(true);
    m_mcrSpectra->setHeadroom(0.3);
    m_mcrConc = newPlot(QStringLiteral("Time (s)"), QStringLiteral("Concentration"));
    m_mcrConc->setLegendVisible(true);
    m_mcrConc->setHeadroom(0.25);
    m_mcrLof = newPlot(QStringLiteral("Iteration"), QStringLiteral("Lack of fit (%)"));
    auto* bottom = new QSplitter(Qt::Horizontal);
    bottom->addWidget(titled(QStringLiteral("mcr-concentrations"), QStringLiteral("Concentration profiles"), m_mcrConc));
    bottom->addWidget(titled(QStringLiteral("mcr-lof"), QStringLiteral("Lack of fit"), m_mcrLof));
    bottom->setSizes({600, 240});
    auto* vs = new QSplitter(Qt::Vertical);
    vs->addWidget(titled(QStringLiteral("mcr-spectra"), QStringLiteral("Resolved spectra"), m_mcrSpectra));
    vs->addWidget(bottom);
    h->addWidget(vs, 1);
    return w;
}

// ---------------------------------------------------------------------------------------------- NNLS
QWidget* AnalysisPage::buildNnlsTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    auto* bar = new QHBoxLayout;
    m_nnlsTarget = new QComboBox;
    m_nnlsTarget->setMinimumContentsLength(18);
    m_nnlsTarget->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_nnlsSource = new QComboBox;
    m_nnlsSource->addItem(QStringLiteral("MCR-ALS spectra"));
    m_nnlsSource->addItem(QStringLiteral("Reference spectra"));
    m_nnlsSource->addItem(QStringLiteral("Pure-variable estimates"));
    m_nnlsOrder = new QSpinBox;
    m_nnlsOrder->setRange(-1, 6);
    m_nnlsOrder->setValue(-1);
    m_nnlsOrder->setSpecialValueText(QStringLiteral("none"));
    m_nnlsOrder->setToolTip(QStringLiteral("Polynomial background fitted together with the spectra (order)"));
    auto* refs = new QPushButton(QStringLiteral("Reference spectra…"));
    connect(refs, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Reference spectra"), QString(),
            QStringLiteral("Spectra (*.csv *.txt *.spc *.jdx *.dx *.jcamp);;All files (*)"));
        if (files.isEmpty()) return;
        std::vector<fileio::DataSpectrum> sp;
        try {
            for (const QString& f : files)
                for (auto& d : fileio::read(f)) sp.push_back(d);
        } catch (const Error& e) {
            emit statusMessage(QString::fromStdString(e.what()));
            return;
        }
        setReferenceSpectra(sp);
        m_nnlsSource->setCurrentIndex(1);
    });
    bar->addWidget(new QLabel(QStringLiteral("Fit")));
    bar->addWidget(m_nnlsTarget);
    bar->addWidget(new QLabel(QStringLiteral("with")));
    bar->addWidget(m_nnlsSource);
    bar->addWidget(refs);
    bar->addWidget(new QLabel(QStringLiteral("Background")));
    bar->addWidget(m_nnlsOrder);
    bar->addStretch();
    bar->addWidget(exportButton(Method::Nnls));
    v->addLayout(bar);
    m_refLabel = new QLabel(QStringLiteral("No reference spectra loaded"));
    m_refLabel->setStyleSheet(QStringLiteral("color: %1").arg(theme::mutedText().name()));
    v->addWidget(m_refLabel);
    for (QComboBox* c : {m_nnlsTarget, m_nnlsSource})
        connect(c, &QComboBox::currentIndexChanged, this, [this] {
            if (!m_updating) {
                m_dirty[4] = true;
                schedule();
            }
        });
    connect(m_nnlsOrder, &QSpinBox::valueChanged, this, [this] { m_dirty[4] = true; schedule(); });
    m_nnlsPlot = newPlot(kShift, QStringLiteral("Intensity"));
    m_nnlsPlot->setLegendVisible(true);
    m_nnlsPlot->setHeadroom(0.25);
    m_nnlsResidual = newPlot(kShift, QStringLiteral("Residual"));
    auto* vs = new QSplitter(Qt::Vertical);
    vs->addWidget(titled(QStringLiteral("nnls-fit"), QStringLiteral("Fit"), m_nnlsPlot));
    vs->addWidget(titled(QStringLiteral("nnls-residual"), QStringLiteral("Residual"), m_nnlsResidual));
    vs->setSizes({500, 200});
    v->addWidget(vs, 1);
    m_nnlsInfo = new QLabel;
    m_nnlsInfo->setWordWrap(true);
    m_nnlsInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_nnlsInfo);
    return w;
}

// ============================================================================================ data set
void AnalysisPage::setSpectra(const std::vector<fileio::DataSpectrum>& spectra) {
    m_spectra = spectra;
    m_checked.assign(m_spectra.size(), true);
    refreshList();
    dataSetChanged();
}

namespace {
fileio::DataSpectrum fromAcquisition(const Spectrum& sp, int index) {
    fileio::DataSpectrum d = fileio::fromSpectrum(sp);
    if (d.name == QLatin1String("spectrum")) d.name = QStringLiteral("spectrum %1").arg(index + 1);
    if (sp.t.isValid() && d.metaValue(QStringLiteral("time")).isEmpty()) d.setMeta(QStringLiteral("time"), iso(sp.t));
    return d;
}
}  // namespace

void AnalysisPage::setSpectra(const std::vector<Spectrum>& spectra) {
    std::vector<fileio::DataSpectrum> ds;
    for (size_t i = 0; i < spectra.size(); ++i) ds.push_back(fromAcquisition(spectra[i], int(i)));
    setSpectra(ds);
}

void AnalysisPage::addSpectrum(const Spectrum& sp) { addSpectra({fromAcquisition(sp, int(m_spectra.size()))}); }

void AnalysisPage::addSpectra(const std::vector<Spectrum>& spectra) {
    std::vector<fileio::DataSpectrum> ds;
    ds.reserve(spectra.size());
    for (const Spectrum& sp : spectra) ds.push_back(fromAcquisition(sp, int(m_spectra.size() + ds.size())));
    addSpectra(ds);
}

void AnalysisPage::addSpectra(const std::vector<fileio::DataSpectrum>& spectra) {
    const size_t from = m_spectra.size();
    for (const auto& d : spectra) {
        m_spectra.push_back(d);
        m_checked.push_back(true);
    }
    appendListItems(from);                        // not refreshList(): a long series would rebuild the list
    dataSetChanged();
}

bool AnalysisPage::loadSeriesFile(const QString& path) {
    try {
        setSpectra(fileio::read(path));
    } catch (const Error& e) {
        m_error = QString::fromStdString(e.what());
        return false;
    }
    emit statusMessage(QStringLiteral("Loaded %1 spectra from %2").arg(m_spectra.size()).arg(QFileInfo(path).fileName()));
    return true;
}

bool AnalysisPage::addFile(const QString& path) {
    try {
        addSpectra(fileio::read(path));
    } catch (const Error& e) {
        m_error = QString::fromStdString(e.what());
        return false;
    }
    return true;
}

void AnalysisPage::clearSpectra() {
    m_spectra.clear();
    m_checked.clear();
    refreshList();
    dataSetChanged();
}

void AnalysisPage::refreshList() {
    m_updating = true;
    m_list->clear();
    m_updating = false;
    appendListItems(0);
}

void AnalysisPage::appendListItems(size_t from) {
    m_updating = true;
    for (size_t i = from; i < m_spectra.size(); ++i) {
        auto* it = new QListWidgetItem(m_spectra[i].name.isEmpty() ? QStringLiteral("spectrum %1").arg(i + 1)
                                                                   : m_spectra[i].name);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(m_checked[i] ? Qt::Checked : Qt::Unchecked);
        it->setToolTip(m_spectra[i].source);
        m_list->addItem(it);
    }
    m_updating = false;
}

void AnalysisPage::setInfo(QLabel* label, const QString& text, bool warning) {
    label->setText(text);
    label->setStyleSheet(warning ? QStringLiteral("color: %1").arg(theme::warningText().name()) : QString());
}

void AnalysisPage::dataSetChanged() {
    std::vector<fileio::DataSpectrum> used;
    for (size_t i = 0; i < m_spectra.size(); ++i)
        if (m_checked[i]) used.push_back(m_spectra[i]);
    m_raw = DataMatrix();
    try {
        m_raw = buildMatrix(used);
    } catch (const Error& e) {
        setInfo(m_dataInfo, QString::fromStdString(e.what()), true);
    }
    // the region follows the data unless the user set one
    if (!m_raw.empty() && !m_regionOn->isChecked()) {
        m_updating = true;
        m_xmin->setValue(std::floor(m_raw.x.front()));
        m_xmax->setValue(std::ceil(m_raw.x.back()));
        m_updating = false;
    }
    // target list of the NNLS tab
    m_updating = true;
    const QString target = m_nnlsTarget->currentText();
    m_nnlsTarget->clear();
    m_nnlsTarget->addItem(QStringLiteral("All spectra (amounts vs time)"));
    for (const QString& n : m_raw.names) m_nnlsTarget->addItem(n);
    const int ti = m_nnlsTarget->findText(target);
    m_nnlsTarget->setCurrentIndex(ti >= 0 ? ti : (m_raw.rows() > 0 ? 1 : 0));
    m_updating = false;
    preprocessChanged();
}

void AnalysisPage::preprocessChanged() {
    if (m_updating) return;
    m_data = DataMatrix();
    ++m_generation;
    QString err;
    if (!m_raw.empty()) {
        try {
            m_data = preprocess(m_raw, preprocessSettings());
        } catch (const Error& e) {
            err = QString::fromStdString(e.what());
        }
    }
    if (m_raw.empty() && err.isEmpty() && !m_spectra.empty() && m_dataInfo->styleSheet().isEmpty())
        err = QStringLiteral("No spectra checked");
    if (!err.isEmpty()) {
        setInfo(m_dataInfo, err, true);
    } else if (m_spectra.empty()) {
        setInfo(m_dataInfo, QStringLiteral("No spectra. Add files or a time series."));
    } else if (!m_data.empty()) {
        setInfo(m_dataInfo, QStringLiteral("%1 spectra × %2 points, %3–%4 cm⁻¹%5")
                                .arg(m_data.rows())
                                .arg(m_data.cols())
                                .arg(m_data.x.front(), 0, 'f', 0)
                                .arg(m_data.x.back(), 0, 'f', 0)
                                .arg(m_data.hasTimes ? QStringLiteral(", %1 s").arg(m_data.times.back(), 0, 'f', 0)
                                                     : QString()));
    }
    for (PlotWidget* p : {m_efaPlot, m_mcrConc}) p->setLabel(Axis::Bottom, timeLabel());
    updatePreview();
    invalidateResults();
    emit dataChanged();
    schedule();
}

Preprocess AnalysisPage::preprocessSettings() const {
    Preprocess p;
    if (m_regionOn->isChecked()) {
        p.xmin = std::min(m_xmin->value(), m_xmax->value());
        p.xmax = std::max(m_xmin->value(), m_xmax->value());
    }
    p.smooth = m_smooth->isChecked();
    p.smoothWindow = m_smoothWindow->value() | 1;
    p.baseline = m_baseline->currentData().toString();
    p.strength = m_strength->value() / 2.0;
    p.normalize = m_normalize->currentData().toString();
    if (p.normalize == QLatin1String("band")) p.band = m_band->value();
    p.derivative = m_derivative->currentIndex();
    p.derivWindow = m_derivWindow->value() | 1;
    return p;
}

void AnalysisPage::setPreprocess(const Preprocess& p) {
    m_updating = true;
    m_regionOn->setChecked(p.xmin.has_value() || p.xmax.has_value());
    if (p.xmin) m_xmin->setValue(*p.xmin);
    if (p.xmax) m_xmax->setValue(*p.xmax);
    m_smooth->setChecked(p.smooth);
    m_smoothWindow->setValue(p.smoothWindow);
    m_baseline->setCurrentIndex(std::max(0, m_baseline->findData(p.baseline.isEmpty() ? QStringLiteral("none") : p.baseline)));
    m_strength->setValue(int(std::lround(p.strength * 2)));
    m_normalize->setCurrentIndex(std::max(0, m_normalize->findData(p.normalize)));
    m_band->setEnabled(p.normalize == QLatin1String("band"));
    if (p.band) m_band->setValue(*p.band);
    m_derivative->setCurrentIndex(std::clamp(p.derivative, 0, 2));
    m_derivWindow->setValue(p.derivWindow);
    m_updating = false;
    preprocessChanged();
}

QString AnalysisPage::timeLabel() const { return m_data.hasTimes ? QStringLiteral("Time (s)") : QStringLiteral("Spectrum"); }

QStringList AnalysisPage::componentNames(int k) const {
    QStringList n;
    const int known = int(m_known.size());
    for (int j = 0; j < k; ++j)
        n << (j < known ? m_known[size_t(j)].name : QStringLiteral("Component %1").arg(j + 1));
    return n;
}

void AnalysisPage::updatePreview() {
    m_preview->clear();
    m_regionItem = nullptr;
    const bool raw = m_previewMode->currentIndex() == 1;
    const DataMatrix& d = raw ? m_raw : m_data;
    if (d.empty()) return;
    const PlotPalette& pal = m_preview->plotPalette();
    const int n = d.rows();
    // at most ~60 curves: thin out long series evenly
    const int stride = std::max(1, (n + 59) / 60);
    for (int i = 0; i < n; i += stride) {
        const double t = n > 1 ? double(i) / (n - 1) : 0.0;
        QColor c = n > 1 ? colorMapColor(ColorMap::Viridis, pal.isDark() ? 0.3 + 0.7 * t : 0.05 + 0.8 * t) : pal.main;
        auto* cv = m_preview->addCurve(d.x, d.row(i), QPen(c, 1.0));
        cv->setOpacity(n > 1 ? 0.8 : 1.0);
    }
    m_preview->setLabel(Axis::Left, raw ? QStringLiteral("Intensity") : QStringLiteral("Preprocessed"));
    if (raw) {
        const Preprocess p = preprocessSettings();
        const double lo = p.xmin.value_or(d.x.front()), hi = p.xmax.value_or(d.x.back());
        m_regionItem = m_preview->addRegion(lo, hi);
        connect(m_regionItem, &RegionItem::regionChangeFinished, this, [this](double a, double b) {
            m_updating = true;
            m_xmin->setValue(std::round(a));
            m_xmax->setValue(std::round(b));
            m_regionOn->setChecked(true);
            m_updating = false;
            preprocessChanged();
        });
    }
    m_preview->autoRange();
}

void AnalysisPage::invalidateResults() {
    for (bool& d : m_dirty) d = true;
    m_pca.reset();
    m_efa.reset();
    m_opa.reset();
    m_simplisma.reset();
    m_nnls.reset();
    m_nnlsAll.clear();
    if (m_busy) m_stop = true;                    // a running MCR works on old data
    if (m_mcr && m_mcrGeneration != m_generation)
        setInfo(m_mcrInfo, QStringLiteral("The data or the preprocessing changed since this result: run again."), true);
    m_bandsButton->setEnabled(m_mcr && m_mcrGeneration == m_generation && !m_busy);
}

void AnalysisPage::schedule() { m_timer->start(); }

void AnalysisPage::runVisible() {
    const int i = m_tabs->currentIndex();
    if (i < 0 || !m_dirty[i]) return;
    // large data sets (a long series): PCA and EFA in a background thread, so the GUI stays responsive
    if ((Method(i) == Method::Pca || Method(i) == Method::Efa) && m_data.rows() >= kAsyncRows) {
        runAsync(Method(i));
        return;
    }
    QElapsedTimer t;
    t.start();
    struct Done {                                 // duration of this automatic run (host: update rate)
        QElapsedTimer& t;
        qint64& ms;
        ~Done() { ms = t.elapsed(); }
    } done{t, m_lastRunMs};
    switch (Method(i)) {
    case Method::Pca: runPca(); break;
    case Method::Efa: runEfa(); break;
    case Method::PureVariables: runPureVariables(); break;
    case Method::Mcr: break;                      // explicit Run
    case Method::Nnls: runNnls(); break;
    case Method::Trends: runTrends(); break;
    }
}

void AnalysisPage::runAsync(Method m) {
    if (m_autoBusy) {                             // one at a time; runs again for the newest data afterwards
        m_autoAgain = true;
        return;
    }
    if (m_autoThread.joinable()) m_autoThread.join();
    m_autoBusy = true;
    m_autoAgain = false;
    const unsigned gen = m_generation;
    const int k = m == Method::Pca ? m_pcaK->value() : m_efaK->value();
    const int scaling = m_pcaScaling->currentIndex();
    m_autoThread = std::thread([this, m, gen, k, scaling, D = Matrix(m_data.D)] {
        QElapsedTimer t;
        t.start();
        std::shared_ptr<PcaResult> p;
        std::shared_ptr<EfaResult> e;
        QString err;
        try {
            if (m == Method::Pca) p = std::make_shared<PcaResult>(pca(D, k, Scaling(scaling)));
            else e = std::make_shared<EfaResult>(efa(D, k));
        } catch (const std::exception& ex) {
            err = QString::fromUtf8(ex.what());
        }
        const qint64 ms = t.elapsed();
        QMetaObject::invokeMethod(
            this, [this, m, gen, k, scaling, p, e, err, ms] { asyncDone(m, gen, k, scaling, p, e, err, ms); },
            Qt::QueuedConnection);
    });
}

void AnalysisPage::asyncDone(Method m, unsigned gen, int k, int scaling, std::shared_ptr<PcaResult> p,
                             std::shared_ptr<EfaResult> e, const QString& err, qint64 ms) {
    if (m_autoThread.joinable()) m_autoThread.join();
    m_autoBusy = false;
    QElapsedTimer t;                              // GUI-thread cost: drawing only (`ms` was spent elsewhere)
    t.start();
    const int i = methodIndex(m);
    const bool current = gen == m_generation && m_dirty[i] &&
                         (m == Method::Pca ? k == m_pcaK->value() && scaling == m_pcaScaling->currentIndex()
                                           : k == m_efaK->value());
    if (current) {
        m_dirty[i] = false;
        if (m == Method::Pca) {
            m_pca = p ? std::make_unique<PcaResult>(std::move(*p)) : nullptr;
            if (!err.isEmpty()) setInfo(m_pcaInfo, err, true);
            drawPca();
        } else {
            m_efa = e ? std::make_unique<EfaResult>(std::move(*e)) : nullptr;
            if (!err.isEmpty()) setInfo(m_efaInfo, err, true);
            drawEfa();
        }
    }
    m_lastRunMs = t.elapsed();
    m_lastAsyncMs = ms;
    if (!current || m_autoAgain) schedule();
}

void AnalysisPage::showMethod(Method m) {
    m_tabs->setCurrentIndex(methodIndex(m));
    runVisible();
}

AnalysisPage::Method AnalysisPage::currentMethod() const { return Method(m_tabs->currentIndex()); }

void AnalysisPage::setComponents(int k) {
    for (QSpinBox* s : {m_pcaK, m_efaK, m_pureK, m_mcrK}) s->setValue(k);
}

void AnalysisPage::setMcrInitialEstimate(InitialEstimate e) { m_mcrInit->setCurrentIndex(int(e)); }

void AnalysisPage::setMcrOptions(const McrOptions& o) {
    m_nnC->setChecked(o.nonNegC);
    m_nnS->setChecked(o.nonNegS);
    m_closure->setChecked(o.closure);
    m_closureTotal->setValue(o.closureTotal);
    m_unimodal->setChecked(o.unimodalC);
    m_maxIter->setValue(o.maxIterations);
    m_tol->setValue(o.tolerancePercent);
}

McrOptions AnalysisPage::mcrOptions() const {
    McrOptions o;
    o.nonNegC = m_nnC->isChecked();
    o.nonNegS = m_nnS->isChecked();
    o.closure = m_closure->isChecked();
    o.closureTotal = m_closureTotal->value();
    o.unimodalC = m_unimodal->isChecked();
    o.maxIterations = m_maxIter->value();
    o.tolerancePercent = m_tol->value();
    return o;
}

// ============================================================================================ PCA
void AnalysisPage::runPca() {
    m_dirty[0] = false;
    m_pca.reset();
    if (m_data.rows() < 2) {
        setInfo(m_pcaInfo, QStringLiteral("PCA needs at least two spectra."), !m_spectra.empty());
        drawPca();
        return;
    }
    try {
        m_pca = std::make_unique<PcaResult>(pca(m_data.D, m_pcaK->value(), Scaling(m_pcaScaling->currentIndex())));
    } catch (const Error& e) {
        setInfo(m_pcaInfo, QString::fromStdString(e.what()), true);
    }
    drawPca();
}

void AnalysisPage::drawPca() {
    for (PlotWidget* p : {m_scree, m_scores, m_loadings}) p->clear();
    if (!m_pca) return;
    const PcaResult& r = *m_pca;
    const PlotPalette& pal = m_scree->plotPalette();
    const int k = r.components;
    // scree
    const int shown = std::min(int(r.explained.size()), std::max(10, k + 2));
    Vec ex, cum;
    for (int i = 0; i < shown; ++i) {
        ex.push_back(100 * r.explained[size_t(i)]);
        cum.push_back(100 * r.cumulative[size_t(i)]);
    }
    auto* c1 = m_scree->addCurve(seq(shown), cum, QPen(pal.reference, 1.2, Qt::DashLine), QStringLiteral("cumulative"));
    c1->setMarker(Marker::Square, 5, pal.reference);
    auto* c0 = m_scree->addCurve(seq(shown), ex, QPen(pal.main, 1.6), QStringLiteral("per component"));
    c0->setMarker(Marker::Circle, 7, pal.main);
    for (int i = 0; i < std::min(k, shown); ++i) {
        auto* t = m_scree->addText(QString::number(ex[size_t(i)], 'f', ex[size_t(i)] < 1 ? 2 : 1), i + 1, ex[size_t(i)],
                                   QPointF(-0.15, 1.2));
        t->setColor(pal.label);
    }
    m_scree->setLegendVisible(true);
    m_scree->setAutoY(false);
    m_scree->setYRange(0, 128);
    m_scree->setXRange(0.5, shown + 0.5);
    m_scree->addVLine(k + 0.5, QPen(faded(pal.accent, 160), 1, Qt::DotLine));

    // scores
    const int n = int(r.scores.rows());
    const QStringList names = m_data.names;
    auto label = [&](int i) -> QString {
        switch (m_pcaLabels->currentIndex()) {
        case 1: return QString::number(i + 1);
        case 2: return i < names.size() ? names[i] : QString();
        case 3: return m_data.hasTimes ? QString::number(m_data.times[size_t(i)], 'f', 0) + " s" : QString::number(i + 1);
        default: return {};
        }
    };
    const int view = m_pcaView->currentIndex();
    m_pcaX->setEnabled(view == 0);
    m_pcaY->setEnabled(view == 0);
    m_scores->setLegendVisible(view == 1);
    if (view == 1) {
        m_scores->setLabel(Axis::Bottom, timeLabel());
        m_scores->setLabel(Axis::Left, QStringLiteral("Score"));
        m_scores->addHLine(0, QPen(pal.reference, 1, Qt::DashLine));
        for (int j = 0; j < k; ++j) {
            auto* c = m_scores->addCurve(m_data.times, col(r.scores, j), QPen(pal.seriesColor(j), 1.5),
                                         QStringLiteral("PC%1").arg(j + 1));
            c->setMarker(Marker::Circle, 4, pal.seriesColor(j));
        }
    } else {
        Vec xs, ys;
        double xl = 0, yl = 0;
        if (view == 0) {
            const int a = std::min(m_pcaX->value(), k) - 1, b = std::min(m_pcaY->value(), k) - 1;
            xs = col(r.scores, a);
            ys = col(r.scores, b);
            m_scores->setLabel(Axis::Bottom, QStringLiteral("PC%1 (%2)").arg(a + 1).arg(pct(r.explained[size_t(a)])));
            m_scores->setLabel(Axis::Left, QStringLiteral("PC%1 (%2)").arg(b + 1).arg(pct(r.explained[size_t(b)])));
            m_scores->addHLine(0, QPen(pal.reference, 1, Qt::DashLine));
            m_scores->addVLine(0, QPen(pal.reference, 1, Qt::DashLine));
        } else {
            xs = r.t2;
            ys = r.q;
            xl = r.t2Limit;
            yl = r.qLimit;
            m_scores->setLabel(Axis::Bottom, QStringLiteral("Hotelling T²"));
            m_scores->setLabel(Axis::Left, QStringLiteral("Q residual"));
            if (std::isfinite(xl)) m_scores->addVLine(xl, QPen(pal.baseline, 1, Qt::DashLine))->setLabel(QStringLiteral("95 %"));
            if (std::isfinite(yl) && yl > 0) m_scores->addHLine(yl, QPen(pal.baseline, 1, Qt::DashLine))->setLabel(QStringLiteral("95 %"));
        }
        auto* path = m_scores->addCurve(xs, ys, QPen(faded(pal.reference, 110), 1.0));
        path->setDecimation(false);
        path->setVisible(view == 0 && m_data.hasTimes);
        // labels: skip those that would sit on top of an already labelled point (2.5 % of the spans)
        double sx = 0, sy = 0;
        {
            Bounds bx, by;
            for (double v : xs) bx.add(v);
            for (double v : ys) by.add(v);
            sx = std::max(bx.hi - bx.lo, 1e-300);
            sy = std::max(by.hi - by.lo, 1e-300);
        }
        std::vector<QPointF> labelled;
        for (int i = 0; i < n; ++i) {
            const double t = n > 1 ? double(i) / (n - 1) : 0.0;
            const QColor c = colorMapColor(ColorMap::Viridis, pal.isDark() ? 0.3 + 0.7 * t : 0.05 + 0.8 * t);
            m_scores->addScatter({xs[size_t(i)]}, {ys[size_t(i)]}, c, 8, Marker::Circle);
            const QString l = label(i);
            if (l.isEmpty()) continue;
            const QPointF q(xs[size_t(i)] / sx, ys[size_t(i)] / sy);
            bool crowded = false;
            for (const QPointF& o : labelled)
                crowded = crowded || (std::abs(o.x() - q.x()) < 0.04 && std::abs(o.y() - q.y()) < 0.03);
            if (crowded) continue;
            labelled.push_back(q);
            auto* t2 = m_scores->addText(l, xs[size_t(i)], ys[size_t(i)], QPointF(-0.25, 1.05));
            t2->setColor(pal.label);
        }
        padRange(m_scores, xs, ys);
    }
    m_scores->autoRange();

    // loadings
    for (int j = 0; j < k; ++j)
        m_loadings->addCurve(m_data.x, col(r.loadings, j), QPen(pal.seriesColor(j), 1.3),
                             QStringLiteral("PC%1 (%2)").arg(j + 1).arg(pct(r.explained[size_t(j)])));
    m_loadings->addHLine(0, QPen(pal.reference, 1, Qt::DashLine));
    m_loadings->autoRange();

    QString info = QStringLiteral("%1 spectra, %2 variables. ").arg(m_data.rows()).arg(m_data.cols());
    QStringList parts;
    for (int j = 0; j < std::min(k, 6); ++j) parts << QStringLiteral("PC%1 %2").arg(j + 1).arg(pct(r.explained[size_t(j)], 2));
    info += parts.join(QStringLiteral(", "));
    info += QStringLiteral("; cumulative %1. ").arg(pct(r.cumulative[size_t(k - 1)], 2));
    info += QStringLiteral("99 % of the variance needs %1, 99.9 % needs %2 components.")
                .arg(rankForVariance(r, 0.99))
                .arg(rankForVariance(r, 0.999));
    setInfo(m_pcaInfo, info);
}

// ============================================================================================ EFA
void AnalysisPage::runEfa() {
    m_dirty[1] = false;
    m_efa.reset();
    if (m_data.rows() < 2) {
        setInfo(m_efaInfo, QStringLiteral("EFA needs at least two spectra (an evolving series)."), !m_spectra.empty());
        drawEfa();
        return;
    }
    try {
        m_efa = std::make_unique<EfaResult>(efa(m_data.D, m_efaK->value()));
    } catch (const Error& e) {
        setInfo(m_efaInfo, QString::fromStdString(e.what()), true);
    }
    drawEfa();
}

void AnalysisPage::drawEfa() {
    m_efaPlot->clear();
    if (!m_efa) return;
    const EfaResult& e = *m_efa;
    const PlotPalette& pal = m_efaPlot->plotPalette();
    const int k = int(e.forward.cols()), n = int(e.forward.rows());
    auto lg = [](double v) { return v > 0 && std::isfinite(v) ? std::log10(v) : std::numeric_limits<double>::quiet_NaN(); };
    for (int j = 0; j < k; ++j) {
        Vec f(static_cast<size_t>(n)), b(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            f[size_t(i)] = lg(e.forward(i, j));
            b[size_t(i)] = lg(e.backward(i, j));
        }
        m_efaPlot->addCurve(m_data.times, f, QPen(pal.seriesColor(j), 1.8), QStringLiteral("%1").arg(j + 1));
        m_efaPlot->addCurve(m_data.times, b, QPen(pal.seriesColor(j), 1.4, Qt::DashLine));
    }
    if (e.threshold > 0) {
        auto* l = m_efaPlot->addHLine(std::log10(e.threshold), QPen(pal.reference, 1, Qt::DotLine));
        l->setLabel(QStringLiteral("threshold"));
    }
    if (e.noise > 0) {
        auto* l = m_efaPlot->addHLine(std::log10(e.noise), QPen(pal.baseline, 1, Qt::DotLine));
        l->setLabel(QStringLiteral("noise"));
    }
    m_efaPlot->autoRange();
    QStringList w;
    auto at = [&](int i) {
        return m_data.hasTimes ? QString::number(m_data.times[size_t(i)], 'f', 0) + " s" : QString::number(i + 1);
    };
    for (size_t j = 0; j < e.windows.size(); ++j)
        w << QStringLiteral("%1: %2 – %3").arg(j + 1).arg(at(e.windows[j].first)).arg(at(e.windows[j].second));
    setInfo(m_efaInfo, QStringLiteral("Windows above the threshold (2 × the first noise singular value, first-in-first-out): ") +
                           w.join(QStringLiteral("; ")));
}

// ============================================================================================ pure variables
void AnalysisPage::runPureVariables() {
    m_dirty[2] = false;
    m_opa.reset();
    m_simplisma.reset();
    if (m_data.rows() < 2) {
        setInfo(m_pureInfo, QStringLiteral("Needs at least two spectra."), !m_spectra.empty());
        drawPure();
        return;
    }
    try {
        const int k = m_pureK->value();
        switch (m_pureMethod->currentIndex()) {
        case 0: m_simplisma = std::make_unique<SimplismaResult>(simplisma(m_data.D, k, m_offset->value())); break;
        case 1: m_opa = std::make_unique<OpaResult>(opa(m_data.D, k, false)); break;
        default: m_opa = std::make_unique<OpaResult>(opa(m_data.D, k, true)); break;
        }
    } catch (const Error& e) {
        setInfo(m_pureInfo, QString::fromStdString(e.what()), true);
    }
    drawPure();
}

void AnalysisPage::drawPure() {
    m_purity->clear();
    m_pureSpectra->clear();
    if (!m_opa && !m_simplisma) return;
    const PlotPalette& pal = m_purity->plotPalette();
    const int method = m_pureMethod->currentIndex();
    const bool overSpectra = method == 1;
    m_purity->setLabel(Axis::Bottom, overSpectra ? timeLabel() : kShift);
    m_purity->setLabel(Axis::Left, method == 0 ? QStringLiteral("Purity (scaled)") : QStringLiteral("Dissimilarity (scaled)"));
    const std::vector<Vec>& curves = m_simplisma ? m_simplisma->purity : m_opa->dissimilarity;
    const std::vector<int>& sel = m_simplisma ? m_simplisma->pureVariables : m_opa->selected;
    const Vec& xs = overSpectra ? m_data.times : m_data.x;
    QStringList picked;
    for (size_t s = 0; s < curves.size(); ++s) {
        const double m = absMax(curves[s]);
        auto* c = m_purity->addCurve(xs, scaled(curves[s], m > 0 ? 1.0 / m : 1.0), QPen(pal.seriesColor(int(s)), 1.2),
                                     QStringLiteral("step %1").arg(s + 1));
        if (overSpectra) c->setMarker(Marker::Circle, 3, pal.seriesColor(int(s)));
        const double xv = xs[size_t(sel[s])];
        auto* l = m_purity->addVLine(xv, QPen(pal.seriesColor(int(s)), 1, Qt::DashLine));
        l->setLabel(QString::number(s + 1));
        picked << (overSpectra ? (m_data.hasTimes ? QString::number(xv, 'f', 0) + " s" : QString::number(sel[s] + 1)) + " (" +
                                     m_data.names.value(sel[s]) + ")"
                               : QString::number(xv, 'f', 0) + " cm⁻¹");
    }
    m_purity->autoRange();
    // pure-component estimates (scaled to a maximum of 1)
    Matrix S;
    if (m_simplisma) {
        S = m_simplisma->S;
    } else if (overSpectra) {
        S = m_opa->estimate;
    } else {
        const Matrix& C = m_opa->estimate;
        S = Eigen::CompleteOrthogonalDecomposition<Matrix>(C.transpose() * C).solve(C.transpose() * m_data.D);
    }
    for (Eigen::Index j = 0; j < S.rows(); ++j) {
        const Vec s = rowv(S, j);
        const double m = signedMax(s);
        m_pureSpectra->addCurve(m_data.x, scaled(s, m != 0 ? 1.0 / m : 1.0), QPen(pal.seriesColor(int(j)), 1.4),
                                QStringLiteral("%1: %2").arg(j + 1).arg(picked.value(int(j))));
    }
    m_pureSpectra->autoRange();
    const QString what = method == 0 ? QStringLiteral("Pure variables") : overSpectra ? QStringLiteral("Purest spectra")
                                                                                      : QStringLiteral("Purest variables");
    setInfo(m_pureInfo, what + QStringLiteral(" in selection order: ") + picked.join(QStringLiteral(", ")) +
                            QStringLiteral(". These are initial estimates; MCR-ALS refines them."));
}

// ============================================================================================ MCR-ALS
Matrix AnalysisPage::onGrid(const std::vector<fileio::DataSpectrum>& spectra, QString& error) const {
    if (m_raw.empty() || m_data.empty()) {
        error = QStringLiteral("no data");
        return {};
    }
    DataMatrix m;
    m.x = m_raw.x;
    m.D.resize(Eigen::Index(spectra.size()), Eigen::Index(m.x.size()));
    for (size_t i = 0; i < spectra.size(); ++i) {
        fileio::DataSpectrum d = spectra[i];
        d.sorted();
        if (d.x.size() < 2 || d.x.front() > m.x.back() || d.x.back() < m.x.front()) {
            error = QStringLiteral("%1 does not cover the range of the data").arg(d.name);
            return {};
        }
        const Vec y = resample(d.x, d.y, m.x);
        for (size_t j = 0; j < y.size(); ++j) m.D(Eigen::Index(i), Eigen::Index(j)) = y[j];
        m.names << d.name;
        m.times.push_back(double(i));
    }
    try {
        return preprocess(m, preprocessSettings()).D;
    } catch (const Error& e) {
        error = QString::fromStdString(e.what());
        return {};
    }
}

bool AnalysisPage::mcrStart(McrStart& start, QString& error) const {
    const Matrix& D = m_data.D;
    const int k = m_mcrK->value();
    const int nKnown = int(m_known.size());
    if (nKnown > k) {
        error = QStringLiteral("more known spectra than components");
        return false;
    }
    Matrix Sk;
    if (nKnown > 0) {
        Sk = onGrid(m_known, error);
        if (Sk.size() == 0) return false;
    }
    // residual after the known components (amounts by NNLS) for the estimates of the others
    Matrix R = D;
    if (nKnown > 0) {
        const Matrix G = Sk * Sk.transpose();
        const Matrix B = D * Sk.transpose();
        Matrix Ck(D.rows(), nKnown);
        for (Eigen::Index i = 0; i < D.rows(); ++i) Ck.row(i) = nnlsNormal(G, B.row(i).transpose()).transpose();
        R = D - Ck * Sk;
    }
    const int free = k - nKnown;
    const int init = m_mcrInit->currentIndex();
    if (nKnown == 0 && init == int(InitialEstimate::Efa)) {
        start.C = efaConcentrations(efa(D, k), int(D.rows()));
        return true;
    }
    Matrix Sf(free, D.cols());
    if (free > 0) {
        if (init == int(InitialEstimate::OpaSpectra)) {
            Sf = opa(R, free).estimate;
        } else if (init == int(InitialEstimate::Simplisma)) {
            Sf = simplisma(R, free, m_offset->value()).S;
        } else {                                  // EFA with known spectra: least-squares spectra from the windows
            const Matrix C = efaConcentrations(efa(R, free), int(D.rows()));
            Sf = Eigen::CompleteOrthogonalDecomposition<Matrix>(C.transpose() * C).solve(C.transpose() * R);
        }
        Sf = Sf.cwiseMax(0.0);                    // estimates of non-negative spectra
        for (Eigen::Index j = 0; j < Sf.rows(); ++j)
            if (Sf.row(j).maxCoeff() <= 0) Sf.row(j).setOnes();
    }
    start.S.resize(k, D.cols());
    if (nKnown > 0) start.S.topRows(nKnown) = Sk;
    if (free > 0) start.S.bottomRows(free) = Sf;
    return true;
}

void AnalysisPage::joinWorker() {
    if (m_worker.joinable()) m_worker.join();
}

void AnalysisPage::runMcr(bool wait) {
    if (m_busy) return;
    joinWorker();
    m_dirty[3] = false;
    if (m_data.rows() < 2) {
        setInfo(m_mcrInfo, QStringLiteral("MCR-ALS needs at least two spectra."), true);
        return;
    }
    McrStart start;
    QString err;
    McrOptions opt = mcrOptions();
    try {
        if (!mcrStart(start, err)) {
            setInfo(m_mcrInfo, err, true);
            return;
        }
    } catch (const Error& e) {
        setInfo(m_mcrInfo, QString::fromStdString(e.what()), true);
        return;
    }
    for (int j = 0; j < int(m_known.size()); ++j) opt.fixedSpectra.push_back(j);
    m_mcrUsed = opt;
    m_bands.clear();
    m_liveLof.clear();
    m_stop = false;
    const unsigned gen = m_generation;
    const Matrix D = m_data.D;
    auto work = [this, D, start, opt, gen, wait]() {
        auto r = std::make_shared<McrResult>();
        QString error;
        try {
            *r = mcrAls(D, start, opt, &m_stop, [this, wait](int, double lof) {
                if (wait) return;
                QMetaObject::invokeMethod(this, [this, lof] {
                    m_liveLof.push_back(lof);
                    drawMcrLof();
                    m_mcrInfo->setText(QStringLiteral("Iteration %1, lack of fit %2 %")
                                           .arg(m_liveLof.size())
                                           .arg(lof, 0, 'f', 3));
                }, Qt::QueuedConnection);
            });
        } catch (const Error& e) {
            r.reset();
            error = QString::fromStdString(e.what());
        }
        if (wait) {
            if (!r) m_error = error;
            mcrDone(r, gen);
            return;
        }
        QMetaObject::invokeMethod(this, [this, r, gen, error] {
            joinWorker();
            m_busy = false;
            if (!r) m_error = error;
            mcrDone(r, gen);
        }, Qt::QueuedConnection);
    };
    m_runMcr->setEnabled(false);
    m_stopMcr->setEnabled(true);
    m_bandsButton->setEnabled(false);
    setInfo(m_mcrInfo, QStringLiteral("Running…"));
    if (wait) {
        work();
    } else {
        m_busy = true;
        m_worker = std::thread(work);
    }
}

void AnalysisPage::stopMcr() { m_stop = true; }

void AnalysisPage::mcrDone(std::shared_ptr<McrResult> r, unsigned generation) {
    m_runMcr->setEnabled(true);
    m_stopMcr->setEnabled(false);
    if (!r) {
        setInfo(m_mcrInfo, m_error, true);
        emit mcrFinished();
        return;
    }
    if (generation != m_generation) {
        setInfo(m_mcrInfo, QStringLiteral("The data changed while MCR-ALS was running: run again."), true);
        emit mcrFinished();
        return;
    }
    m_mcr = std::move(r);
    m_mcrGeneration = generation;
    m_bandsButton->setEnabled(true);
    drawMcr();
    m_dirty[4] = true;                            // NNLS may use the MCR spectra
    emit mcrFinished();
}

void AnalysisPage::computeAmbiguity(bool wait) {
    if (m_busy || !m_mcr || m_mcrGeneration != m_generation) return;
    joinWorker();
    m_stop = false;
    const auto r = m_mcr;
    const McrOptions opt = m_mcrUsed;
    const unsigned gen = m_generation;
    auto work = [this, r, opt, gen, wait] {
        std::vector<AmbiguityBand> b = ambiguityBands(*r, opt, 0.01, &m_stop);
        if (wait) {
            bandsDone(std::move(b), gen);
            return;
        }
        QMetaObject::invokeMethod(this, [this, b, gen] {
            joinWorker();
            m_busy = false;
            bandsDone(b, gen);
        }, Qt::QueuedConnection);
    };
    m_runMcr->setEnabled(false);
    m_stopMcr->setEnabled(true);
    m_bandsButton->setEnabled(false);
    setInfo(m_mcrInfo, m_mcrInfo->text() + QStringLiteral("\n\nComputing the rotational ambiguity…"));
    if (wait) {
        work();
    } else {
        m_busy = true;
        m_worker = std::thread(work);
    }
}

void AnalysisPage::bandsDone(std::vector<AmbiguityBand> b, unsigned generation) {
    m_runMcr->setEnabled(true);
    m_stopMcr->setEnabled(false);
    m_bandsButton->setEnabled(m_mcr != nullptr);
    if (generation == m_generation && !m_stop) m_bands = std::move(b);
    drawMcr();
    emit ambiguityFinished();
}

void AnalysisPage::drawMcrLof() {
    m_mcrLof->clear();
    const Vec& h = m_busy || !m_mcr ? m_liveLof : m_mcr->lofHistory;
    if (h.empty()) return;
    const PlotPalette& pal = m_mcrLof->plotPalette();
    auto* c = m_mcrLof->addCurve(seq(int(h.size())), h, QPen(pal.main, 1.4));
    c->setMarker(Marker::Circle, 4, pal.main);
    if (!m_busy && m_mcr && m_mcr->bestIteration > 0) {
        auto* best = m_mcrLof->addScatter({double(m_mcr->bestIteration)}, {h[size_t(m_mcr->bestIteration - 1)]},
                                          pal.baseline, 9, Marker::Diamond);
        best->setName(QString());
    }
    m_mcrLof->autoRange();
}

void AnalysisPage::drawMcr() {
    m_mcrSpectra->clear();
    m_mcrConc->clear();
    drawMcrLof();
    if (!m_mcr) return;
    const McrResult& r = *m_mcr;
    const PlotPalette& pal = m_mcrSpectra->plotPalette();
    const int k = int(r.S.rows());
    const QStringList names = componentNames(k);
    const bool closure = m_mcrUsed.closure;
    const bool bands = m_showBands->isChecked() && int(m_bands.size()) == k;
    m_mcrConc->setLabel(Axis::Left, closure ? QStringLiteral("Fraction") : QStringLiteral("Concentration (data units)"));
    for (int j = 0; j < k; ++j) {
        Vec s = rowv(r.S, j), c = col(r.C, j);
        double sm = signedMax(s);
        if (sm == 0) sm = 1;
        if (!closure) {                           // spectra to a maximum of 1, C carries the intensity
            s = scaled(s, 1.0 / sm);
            c = scaled(c, sm);
        } else {
            s = scaled(s, 1.0 / sm);
        }
        const QColor colr = pal.seriesColor(j);
        if (bands && !m_bands[size_t(j)].sMin.empty()) {
            const AmbiguityBand& b = m_bands[size_t(j)];
            for (int e = 0; e < 2; ++e) {
                const Vec& sb = e == 0 ? b.sMin : b.sMax;
                const Vec& cb = e == 0 ? b.cMin : b.cMax;
                const double m = signedMax(sb);
                if (m == 0) continue;
                // same convention as the optimal solution: spectrum to max 1, profile scaled so c·s is kept
                const double cs = closure ? m / sm : m;
                auto* a = m_mcrSpectra->addCurve(m_data.x, scaled(sb, 1.0 / m), QPen(colr, 1.0, Qt::DashLine));
                a->setOpacity(0.7);
                auto* bc = m_mcrConc->addCurve(m_data.times, scaled(cb, cs), QPen(colr, 1.0, Qt::DashLine));
                bc->setOpacity(0.7);
            }
        }
        m_mcrSpectra->addCurve(m_data.x, s, QPen(colr, 1.6), names[j]);
        auto* cc = m_mcrConc->addCurve(m_data.times, c, QPen(colr, 1.6), names[j]);
        cc->setMarker(Marker::Circle, 4, colr);
    }
    m_mcrSpectra->addHLine(0, QPen(pal.reference, 1, Qt::DotLine));
    m_mcrConc->addHLine(0, QPen(pal.reference, 1, Qt::DotLine));
    m_mcrSpectra->autoRange();
    m_mcrConc->autoRange();

    QString info = QStringLiteral("<b>%1</b><br>Lack of fit %2 %, explained variance %3 %")
                       .arg(r.message.toHtmlEscaped())
                       .arg(r.lof, 0, 'f', 3)
                       .arg(r.r2, 0, 'f', 3);
    if (r.bestIteration != r.iterations) info += QStringLiteral(" (best: iteration %1)").arg(r.bestIteration);
    if (!m_bands.empty()) {
        info += QStringLiteral("<br><br><b>Rotational ambiguity</b> (contribution f = ‖c s‖ / ‖C S‖):");
        for (size_t j = 0; j < m_bands.size(); ++j) {
            const AmbiguityBand& b = m_bands[j];
            info += QStringLiteral("<br>%1: %2, feasible %3 – %4")
                        .arg(names.value(int(j)).toHtmlEscaped())
                        .arg(b.fOptimal, 0, 'f', 3)
                        .arg(b.fMin, 0, 'f', 3)
                        .arg(std::isfinite(b.fMax) ? QString::number(b.fMax, 'f', 3) : QStringLiteral("∞"));
            const double w = b.fMax - b.fMin;
            info += w < 0.02 * std::max(b.fOptimal, 1e-9) + 0.005 ? QStringLiteral(" (unique)") : QString();
        }
        if (std::isinf(m_bands[0].fMax))
            info += QStringLiteral("<br>Without non-negativity the rotations are unbounded.");
        else
            info += QStringLiteral("<br>Dashed: the extreme solutions.");
    }
    m_mcrInfo->setStyleSheet(QString());
    m_mcrInfo->setText(info);
}

// ============================================================================================ NNLS
void AnalysisPage::setKnownSpectra(const std::vector<fileio::DataSpectrum>& spectra) {
    m_known = spectra;
    QStringList n;
    for (const auto& d : m_known) n << d.name;
    m_knownLabel->setText(n.isEmpty() ? QStringLiteral("No fixed spectra")
                                      : QStringLiteral("Fixed: ") + n.join(QStringLiteral(", ")));
    if (int(m_known.size()) > m_mcrK->value()) m_mcrK->setValue(int(m_known.size()));
}

void AnalysisPage::setReferenceSpectra(const std::vector<fileio::DataSpectrum>& spectra) {
    m_references = spectra;
    QStringList n;
    for (const auto& d : m_references) n << d.name;
    m_refLabel->setText(n.isEmpty() ? QStringLiteral("No reference spectra loaded")
                                    : QStringLiteral("References: ") + n.join(QStringLiteral(", ")));
    m_dirty[4] = true;
    schedule();
}

void AnalysisPage::runNnls() {
    m_dirty[4] = false;
    m_nnls.reset();
    m_nnlsAll.clear();
    m_nnlsNames.clear();
    if (m_data.empty()) {
        setInfo(m_nnlsInfo, QStringLiteral("No data."), !m_spectra.empty());
        drawNnls();
        return;
    }
    Matrix K;
    QString err;
    switch (m_nnlsSource->currentIndex()) {
    case 0:
        if (!m_mcr || m_mcrGeneration != m_generation) err = QStringLiteral("Run MCR-ALS first (on the current data).");
        else {
            K = m_mcr->S;
            m_nnlsNames = componentNames(int(K.rows()));
        }
        break;
    case 1:
        if (m_references.empty()) err = QStringLiteral("Load reference spectra first.");
        else {
            K = onGrid(m_references, err);
            for (const auto& d : m_references) m_nnlsNames << d.name;
        }
        break;
    default:
        if (!m_simplisma && !m_opa) runPureVariables();
        if (m_simplisma) K = m_simplisma->S;
        else if (m_opa && m_pureMethod->currentIndex() == 1) K = m_opa->estimate;
        else if (m_opa) {
            const Matrix& C = m_opa->estimate;
            K = Eigen::CompleteOrthogonalDecomposition<Matrix>(C.transpose() * C).solve(C.transpose() * m_data.D);
        }
        if (K.size() == 0) err = QStringLiteral("No pure-variable estimates.");
        for (Eigen::Index j = 0; j < K.rows(); ++j) m_nnlsNames << QStringLiteral("Estimate %1").arg(j + 1);
        break;
    }
    if (K.size() == 0 || K.cols() != m_data.cols()) {
        setInfo(m_nnlsInfo, err.isEmpty() ? QStringLiteral("The component spectra do not match the data.") : err, true);
        drawNnls();
        return;
    }
    try {
        const int target = m_nnlsTarget->currentIndex() - 1;
        if (target < 0) {
            for (int i = 0; i < m_data.rows(); ++i)
                m_nnlsAll.push_back(nnlsFit(K, m_data.x, m_data.row(i), m_nnlsOrder->value()));
        } else if (target < m_data.rows()) {
            m_nnls = std::make_unique<NnlsFit>(nnlsFit(K, m_data.x, m_data.row(target), m_nnlsOrder->value()));
        }
    } catch (const Error& e) {
        setInfo(m_nnlsInfo, QString::fromStdString(e.what()), true);
    }
    m_nnlsComponents = K;
    drawNnls();
}

void AnalysisPage::drawNnls() {
    m_nnlsPlot->clear();
    m_nnlsResidual->clear();
    const PlotPalette& pal = m_nnlsPlot->plotPalette();
    if (!m_nnlsAll.empty()) {
        m_nnlsPlot->setLabel(Axis::Bottom, timeLabel());
        m_nnlsPlot->setLabel(Axis::Left, QStringLiteral("Amount"));
        m_nnlsResidual->setLabel(Axis::Bottom, timeLabel());
        m_nnlsResidual->setLabel(Axis::Left, QStringLiteral("R²"));
        const int k = int(m_nnlsAll[0].coefficients.size());
        for (int j = 0; j < k; ++j) {
            Vec a;
            for (const auto& f : m_nnlsAll) a.push_back(f.coefficients[size_t(j)]);
            auto* c = m_nnlsPlot->addCurve(m_data.times, a, QPen(pal.seriesColor(j), 1.6), m_nnlsNames.value(j));
            c->setMarker(Marker::Circle, 4, pal.seriesColor(j));
        }
        Vec r2;
        for (const auto& f : m_nnlsAll) r2.push_back(f.r2);
        auto* c = m_nnlsResidual->addCurve(m_data.times, r2, QPen(pal.main, 1.4));
        c->setMarker(Marker::Circle, 4, pal.main);
        m_nnlsPlot->autoRange();
        m_nnlsResidual->autoRange();
        double mean = 0;
        for (double v : r2) mean += v;
        setInfo(m_nnlsInfo, QStringLiteral("%1 spectra fitted; mean R² %2.").arg(r2.size()).arg(mean / double(r2.size()), 0, 'f', 5));
        return;
    }
    m_nnlsPlot->setLabel(Axis::Bottom, kShift);
    m_nnlsPlot->setLabel(Axis::Left, QStringLiteral("Intensity"));
    m_nnlsResidual->setLabel(Axis::Bottom, kShift);
    m_nnlsResidual->setLabel(Axis::Left, QStringLiteral("Residual"));
    if (!m_nnls) return;
    const NnlsFit& f = *m_nnls;
    const int target = m_nnlsTarget->currentIndex() - 1;
    m_nnlsPlot->addCurve(m_data.x, m_data.row(target), QPen(pal.label, 1.4), m_data.names.value(target));
    for (Eigen::Index j = 0; j < m_nnlsComponents.rows(); ++j) {
        const double a = f.coefficients[size_t(j)];
        if (a <= 0) continue;
        Vec part = scaled(rowv(m_nnlsComponents, j), a);
        for (size_t i = 0; i < part.size(); ++i) part[i] += f.background[i];
        auto* c = m_nnlsPlot->addCurve(m_data.x, part, QPen(pal.seriesColor(int(j)), 1.0), m_nnlsNames.value(int(j)));
        c->setOpacity(0.85);
    }
    if (m_nnlsOrder->value() >= 0)
        m_nnlsPlot->addCurve(m_data.x, f.background, QPen(pal.reference, 1.0, Qt::DashLine), QStringLiteral("background"));
    m_nnlsPlot->addCurve(m_data.x, f.fitted, QPen(pal.baseline, 1.4, Qt::DashLine), QStringLiteral("fit"));
    m_nnlsResidual->addCurve(m_data.x, f.residual, QPen(pal.main, 1.0));
    m_nnlsResidual->addHLine(0, QPen(pal.reference, 1, Qt::DotLine));
    m_nnlsPlot->autoRange();
    m_nnlsResidual->autoRange();
    QStringList parts;
    for (size_t j = 0; j < f.coefficients.size(); ++j)
        parts << QStringLiteral("%1: %2").arg(m_nnlsNames.value(int(j))).arg(f.coefficients[j], 0, 'g', 5);
    setInfo(m_nnlsInfo, QStringLiteral("Amounts: ") + parts.join(QStringLiteral(", ")) +
                            QStringLiteral(". R² %1, residual norm %2.").arg(f.r2, 0, 'f', 5).arg(f.residualNorm, 0, 'g', 4));
}

// ============================================================================================ peak trends
// Table model of the trend values (a long series: no item per cell).
class TrendModel : public QAbstractTableModel {
public:
    using QAbstractTableModel::QAbstractTableModel;
    // decimals per column of cols (-1 or missing: 6 significant digits)
    void set(QStringList header, QStringList names, std::vector<Vec> cols, std::vector<int> decimals = {}) {
        beginResetModel();
        m_header = std::move(header);
        m_names = std::move(names);
        m_cols = std::move(cols);
        m_decimals = std::move(decimals);
        m_rows = m_names.size();
        for (const Vec& c : m_cols) m_rows = std::max(m_rows, qsizetype(c.size()));
        endResetModel();
    }
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(m_rows); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(m_header.size()); }
    QVariant data(const QModelIndex& index, int role) const override {
        const int r = index.row(), c = index.column();
        if (role == Qt::TextAlignmentRole && c > 0) return int(Qt::AlignRight | Qt::AlignVCenter);
        if (role != Qt::DisplayRole) return {};
        if (c == 0) return r < m_names.size() ? m_names[r] : QString();
        const Vec& v = m_cols[size_t(c - 1)];
        if (size_t(r) >= v.size() || !std::isfinite(v[size_t(r)])) return QString();
        const int dec = size_t(c - 1) < m_decimals.size() ? m_decimals[size_t(c - 1)] : -1;
        return dec >= 0 ? QString::number(v[size_t(r)], 'f', dec) : QString::number(v[size_t(r)], 'g', 6);
    }
    QVariant headerData(int section, Qt::Orientation o, int role) const override {
        if (role != Qt::DisplayRole) return {};
        if (o == Qt::Vertical) return section + 1;
        return section < m_header.size() ? m_header[section] : QString();
    }

private:
    QStringList m_header, m_names;
    std::vector<Vec> m_cols;
    std::vector<int> m_decimals;
    qsizetype m_rows = 0;
};

namespace {

enum BandColumn { ColColor, ColName, ColFrom, ColTo, ColQuantity, ColBaseline, ColProfile, BandColumns };

quint64 fnv(quint64 h, double v) {                // FNV-1a over the bits of one double
    quint64 bits = 0;
    static_assert(sizeof bits == sizeof v, "double is 64 bits");
    std::memcpy(&bits, &v, sizeof v);
    h ^= bits;
    return h * 1099511628211ULL;
}

bool intensityLike(bandtrend::Quantity q) {      // ratios and relative values make sense
    using Q = bandtrend::Quantity;
    return q == Q::Height || q == Q::Area || q == Q::FitArea || q == Q::FitHeight;
}

QLabel* boldLabel(const QString& text) {
    auto* l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

}  // namespace

QWidget* AnalysisPage::buildTrendsTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    auto* bar = new QHBoxLayout;
    m_addBand = new QToolButton;
    m_addBand->setText(QStringLiteral("Add band"));
    m_addBand->setPopupMode(QToolButton::InstantPopup);
    m_addBand->setToolTip(QStringLiteral("Add a band around a peak of the mean spectrum (±2 FWHM); right-click in the "
                                         "spectra to add one anywhere"));
    auto* addMenu = new QMenu(m_addBand);
    m_addBand->setMenu(addMenu);
    connect(addMenu, &QMenu::aboutToShow, this, &AnalysisPage::fillAddBandMenu);
    auto* remove = new QPushButton(QStringLiteral("Remove band"));
    connect(remove, &QPushButton::clicked, this, [this] {
        const int r = m_bandTable->currentRow();
        if (r >= 0) removeTrendBand(r);
        else if (!m_trendBands.empty()) removeTrendBand(int(m_trendBands.size()) - 1);
    });
    m_trendNorm = new QComboBox;
    m_trendNorm->addItem(QStringLiteral("None"));
    m_trendNorm->addItem(QStringLiteral("Ratio to band"));
    m_trendNorm->addItem(QStringLiteral("First spectrum = 1"));
    m_trendNorm->setToolTip(QStringLiteral("Heights and areas divided by those of a reference band, or by their first "
                                           "value (positions and widths are not normalised)"));
    m_trendRef = new QComboBox;
    m_trendRef->setMinimumContentsLength(9);
    m_trendRef->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_trendRef->setToolTip(QStringLiteral("Reference band of the ratio"));
    m_trendRef->setEnabled(false);
    m_trendKinetic = new QComboBox;
    m_trendKinetic->addItem(QStringLiteral("None"));
    m_trendKinetic->addItem(QStringLiteral("Exponential  A + B·e^(−t/τ)"));
    m_trendKinetic->addItem(QStringLiteral("Linear  a + b·t"));
    m_trendKinetic->setToolTip(QStringLiteral("Fitted to every trend (fitted quantities weighted by their errors)"));
    connect(m_trendNorm, &QComboBox::currentIndexChanged, this, [this] {
        m_trendRef->setEnabled(m_trendNorm->currentIndex() == int(TrendNormalisation::Ratio));
        if (!m_updating) drawTrends();
    });
    for (QComboBox* c : {m_trendRef, m_trendKinetic})
        connect(c, &QComboBox::currentIndexChanged, this, [this] {
            if (!m_updating) drawTrends();
        });
    bar->addWidget(m_addBand);
    bar->addWidget(remove);
    bar->addSpacing(12);
    bar->addWidget(new QLabel(QStringLiteral("Normalise")));
    bar->addWidget(m_trendNorm);
    bar->addWidget(m_trendRef);
    bar->addSpacing(12);
    bar->addWidget(new QLabel(QStringLiteral("Kinetic fit")));
    bar->addWidget(m_trendKinetic);
    bar->addStretch();
    bar->addWidget(exportButton(Method::Trends));
    v->addLayout(bar);

    m_trendSpectra = newPlot(kShift, QStringLiteral("Preprocessed"));
    m_trendSpectra->setHeadroom(0.16);
    m_trendSpectra->setToolTip(PlotWidget::navigationHelp(WheelMode::ZoomX) +
                               QStringLiteral("\nDrag a band's edges to resize it, its inside to move it; "
                                              "right-click: add or remove a band"));
    connect(m_trendSpectra, &PlotWidget::rightClicked, this, &AnalysisPage::trendSpectraMenu);
    m_trendPlot = newPlot(QStringLiteral("Time (s)"), QStringLiteral("Height"));
    m_trendPlot->setLegendVisible(true);
    m_trendPlot->setHeadroom(0.32);                // room for the legend (fit results) above the data
    m_trendPlot->setPanY(true);

    m_bandTable = new QTableWidget(0, BandColumns);
    m_bandTable->setHorizontalHeaderLabels({QString(), QStringLiteral("Name"), QStringLiteral("From"), QStringLiteral("To"),
                                            QStringLiteral("Quantity"), QStringLiteral("Baseline"), QStringLiteral("Profile")});
    m_bandTable->horizontalHeaderItem(ColBaseline)->setToolTip(QStringLiteral("Local linear baseline between the band edges"));
    m_bandTable->horizontalHeaderItem(ColProfile)->setToolTip(QStringLiteral("Peak profile of the fitted quantities"));
    m_bandTable->verticalHeader()->setVisible(false);
    m_bandTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_bandTable->setSelectionMode(QAbstractItemView::SingleSelection);
    QHeaderView* hh = m_bandTable->horizontalHeader();
    hh->setSectionResizeMode(QHeaderView::ResizeToContents);
    hh->setStretchLastSection(true);
    m_bandTable->setColumnWidth(ColColor, 22);
    hh->setSectionResizeMode(ColColor, QHeaderView::Fixed);
    connect(m_bandTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (m_updating) return;
        const int r = it->row();
        if (r < 0 || r >= int(m_trendBands.size())) return;
        TrendBand& tb = m_trendBands[size_t(r)];
        if (it->column() == ColName) {
            tb.band.name = it->text().trimmed();          // not part of the cache key: no re-measuring
            trendBandsChanged();
        } else if (it->column() == ColBaseline) {
            tb.band.localBaseline = it->checkState() == Qt::Checked;
            trendBandsChanged();
        }
    });
    connect(m_bandTable, &QTableWidget::cellClicked, this, [this](int r, int c) {
        if (c != ColColor || r < 0 || r >= int(m_trendBands.size())) return;
        const QColor col = QColorDialog::getColor(m_trendBands[size_t(r)].color, this, QStringLiteral("Band colour"));
        if (!col.isValid()) return;
        m_trendBands[size_t(r)].color = col;
        refreshBandTable();
        drawTrendSpectra();
        drawTrends();
    });
    auto* bandPanel = new QWidget;
    auto* bl = new QVBoxLayout(bandPanel);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(2);
    bl->addWidget(boldLabel(QStringLiteral("Bands")));
    bl->addWidget(m_bandTable, 1);

    m_trendModel = new TrendModel(this);
    m_trendTable = new QTableView;
    m_trendTable->setModel(m_trendModel);
    m_trendTable->verticalHeader()->setVisible(false);
    m_trendTable->verticalHeader()->setDefaultSectionSize(m_trendTable->fontMetrics().height() + 6);
    m_trendTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_trendTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_trendTable->setAlternatingRowColors(true);
    auto* valuePanel = new QWidget;
    auto* vl = new QVBoxLayout(valuePanel);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(2);
    vl->addWidget(boldLabel(QStringLiteral("Values")));
    vl->addWidget(m_trendTable, 1);

    auto* top = new QSplitter(Qt::Horizontal);
    top->addWidget(titled(QStringLiteral("trends-spectra"), QStringLiteral("Spectra and bands"), m_trendSpectra));
    top->addWidget(bandPanel);
    top->setStretchFactor(0, 1);
    top->setSizes({620, 480});
    auto* bottom = new QSplitter(Qt::Horizontal);
    bottom->addWidget(titled(QStringLiteral("trends"), QStringLiteral("Trends"), m_trendPlot));
    bottom->addWidget(valuePanel);
    bottom->setStretchFactor(0, 1);
    bottom->setSizes({680, 420});
    auto* vs = new QSplitter(Qt::Vertical);
    vs->addWidget(top);
    vs->addWidget(bottom);
    vs->setSizes({400, 460});
    v->addWidget(vs, 1);
    m_trendInfo = new QLabel(QStringLiteral("Add a band: from the peaks of the mean spectrum (Add band) or by "
                                            "right-clicking in the spectra."));
    m_trendInfo->setWordWrap(true);
    m_trendInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_trendInfo);
    return w;
}

QString AnalysisPage::trendKey(const bandtrend::Band& b) {
    // everything the measurements depend on (not the name, not the quantity unless it needs a fit)
    QString k = QStringLiteral("%1|%2|%3").arg(b.lo, 0, 'g', 17).arg(b.hi, 0, 'g', 17).arg(b.localBaseline);
    if (bandtrend::needsFit(b.quantity)) {
        k += QStringLiteral("|fit|%1|%2").arg(b.profile).arg(b.main);
        for (const peaks::Peak& p : b.guesses) k += QStringLiteral("|%1/%2").arg(p.position, 0, 'g', 17).arg(p.fwhm, 0, 'g', 17);
    }
    return k;
}

void AnalysisPage::updateTrendMean() {
    if (m_trendMeanGen == m_generation) return;
    m_trendMeanGen = m_generation;
    m_trendMean.clear();
    m_trendPeaks.clear();
    if (m_data.empty()) return;
    m_trendMean = toVec(m_data.D.colwise().mean().transpose());
    try {
        m_trendPeaks = peaks::pick(m_data.x, m_trendMean, 5.0);
    } catch (const std::exception&) {
    }
}

const std::vector<peaks::Peak>& AnalysisPage::trendPeaks() {
    updateTrendMean();
    return m_trendPeaks;
}

int AnalysisPage::addBand(bandtrend::Band b) {
    TrendBand tb;
    if (b.lo > b.hi) std::swap(b.lo, b.hi);
    tb.band = std::move(b);
    tb.id = m_nextBandId++;
    // the first palette colour not used yet
    const PlotPalette& pal = m_trendPlot->plotPalette();
    for (int k = 0; k < 64; ++k) {
        const QColor c = pal.seriesColor(k);
        bool used = false;
        for (const TrendBand& o : m_trendBands) used = used || o.color == c;
        if (!used) {
            tb.color = c;
            break;
        }
    }
    if (!tb.color.isValid()) tb.color = pal.seriesColor(tb.id);
    m_trendBands.push_back(tb);
    m_trendCache.emplace_back();
    refreshBandTable();
    trendBandsChanged();
    return int(m_trendBands.size()) - 1;
}

int AnalysisPage::addTrendBand(double lo, double hi, const QString& name) {
    bandtrend::Band b;
    b.lo = std::min(lo, hi);
    b.hi = std::max(lo, hi);
    b.name = name.isEmpty() ? bandtrend::defaultName((lo + hi) / 2) : name;
    return addBand(b);
}

int AnalysisPage::addTrendBandAtPeak(double position) {
    const std::vector<peaks::Peak>& pk = trendPeaks();
    const peaks::Peak* best = nullptr;
    for (const peaks::Peak& p : pk)
        if (!best || std::abs(p.position - position) < std::abs(best->position - position)) best = &p;
    if (!best || std::abs(best->position - position) > 25) return -1;
    return addBand(bandtrend::bandAround(*best, m_data.x));
}

void AnalysisPage::removeTrendBand(int i) {
    if (i < 0 || i >= int(m_trendBands.size())) return;
    m_trendBands.erase(m_trendBands.begin() + i);
    m_trendCache.erase(m_trendCache.begin() + i);
    refreshBandTable();
    trendBandsChanged();
}

void AnalysisPage::setTrendBand(int i, const bandtrend::Band& b) {
    if (i < 0 || i >= int(m_trendBands.size())) return;
    bandtrend::Band& o = m_trendBands[size_t(i)].band;
    const bool refit = o.lo != b.lo || o.hi != b.hi || o.profile != b.profile;
    o.name = b.name;
    o.lo = std::min(b.lo, b.hi);
    o.hi = std::max(b.lo, b.hi);
    o.quantity = b.quantity;
    o.localBaseline = b.localBaseline;
    o.profile = b.profile;
    if (!b.guesses.empty()) {
        o.guesses = b.guesses;
        o.main = b.main;
    } else if (refit) {
        o.guesses.clear();                        // from the mean spectrum at the next run
    }
    syncBandRow(i);
    trendBandsChanged();
}

void AnalysisPage::setTrendQuantity(int i, bandtrend::Quantity q) {
    if (i < 0 || i >= int(m_trendBands.size())) return;
    bandtrend::Band b = m_trendBands[size_t(i)].band;
    b.quantity = q;
    setTrendBand(i, b);
}

void AnalysisPage::setTrendNormalisation(TrendNormalisation n, int reference) {
    m_updating = true;
    m_trendRef->setCurrentIndex(std::clamp(reference, 0, std::max(0, m_trendRef->count() - 1)));
    m_trendNorm->setCurrentIndex(int(n));
    m_trendRef->setEnabled(n == TrendNormalisation::Ratio);
    m_updating = false;
    drawTrends();
}

void AnalysisPage::setTrendKinetics(bandtrend::Kinetic k) {
    m_trendKinetic->setCurrentIndex(int(k));      // redraws
}

void AnalysisPage::refreshBandTable() {
    m_updating = true;
    m_bandTable->setRowCount(int(m_trendBands.size()));
    for (int r = 0; r < int(m_trendBands.size()); ++r) {
        const TrendBand& tb = m_trendBands[size_t(r)];
        auto* c = new QTableWidgetItem;
        c->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        c->setBackground(tb.color);
        c->setToolTip(QStringLiteral("Click to change the colour"));
        m_bandTable->setItem(r, ColColor, c);
        auto* n = new QTableWidgetItem(tb.band.name);
        n->setToolTip(QStringLiteral("Name (double-click to edit)"));
        m_bandTable->setItem(r, ColName, n);
        for (int col : {ColFrom, ColTo}) {
            auto* sb = new QDoubleSpinBox;
            sb->setRange(-2000, 10000);
            sb->setDecimals(1);
            sb->setFrame(false);
            sb->setKeyboardTracking(false);
            sb->setButtonSymbols(QAbstractSpinBox::NoButtons);
            sb->setValue(col == ColFrom ? tb.band.lo : tb.band.hi);
            const int id = tb.id;
            connect(sb, &QDoubleSpinBox::valueChanged, this, [this, id, col](double val) {
                if (m_updating) return;
                for (size_t k = 0; k < m_trendBands.size(); ++k) {
                    if (m_trendBands[k].id != id) continue;
                    bandtrend::Band b = m_trendBands[k].band;
                    (col == ColFrom ? b.lo : b.hi) = val;
                    if (b.lo >= b.hi) return;
                    setTrendBand(int(k), b);
                }
            });
            m_bandTable->setCellWidget(r, col, sb);
        }
        auto* q = new QComboBox;
        q->addItems(bandtrend::quantityNames());
        q->setCurrentIndex(int(tb.band.quantity));
        q->setToolTip(QStringLiteral("<p>Height: maximum in the band. Area: integrated (trapezoid). Centroid: centre of "
                                     "the part above half maximum. FWHM: interpolated width at half maximum. Fitted: "
                                     "profile fit of the band (number of peaks from the mean spectrum), main peak; "
                                     "with standard errors.</p>"));
        const int id = tb.id;
        connect(q, &QComboBox::currentIndexChanged, this, [this, id](int qi) {
            if (m_updating) return;
            for (size_t k = 0; k < m_trendBands.size(); ++k)
                if (m_trendBands[k].id == id) setTrendQuantity(int(k), bandtrend::Quantity(qi));
        });
        m_bandTable->setCellWidget(r, ColQuantity, q);
        auto* b = new QTableWidgetItem;
        b->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        b->setCheckState(tb.band.localBaseline ? Qt::Checked : Qt::Unchecked);
        m_bandTable->setItem(r, ColBaseline, b);
        auto* pr = new QComboBox;
        pr->addItems(peaks::profiles());
        pr->setCurrentIndex(std::max(0, pr->findText(tb.band.profile)));
        pr->setEnabled(bandtrend::needsFit(tb.band.quantity));
        connect(pr, &QComboBox::currentTextChanged, this, [this, id](const QString& shape) {
            if (m_updating) return;
            for (size_t k = 0; k < m_trendBands.size(); ++k)
                if (m_trendBands[k].id == id) {
                    bandtrend::Band bb = m_trendBands[k].band;
                    bb.profile = shape;
                    setTrendBand(int(k), bb);
                }
        });
        m_bandTable->setCellWidget(r, ColProfile, pr);
    }
    // reference band choices
    const int ref = m_trendRef->currentIndex();
    m_trendRef->clear();
    for (const TrendBand& tb : m_trendBands) m_trendRef->addItem(tb.band.name);
    m_trendRef->setCurrentIndex(std::clamp(ref, 0, std::max(0, int(m_trendBands.size()) - 1)));
    m_updating = false;
}

// Values of row i's cells from the band, in place (the cell widgets may be the senders of this change).
void AnalysisPage::syncBandRow(int i) {
    if (i < 0 || i >= m_bandTable->rowCount() || i >= int(m_trendBands.size())) return;
    const bandtrend::Band& b = m_trendBands[size_t(i)].band;
    m_updating = true;
    if (QTableWidgetItem* n = m_bandTable->item(i, ColName)) n->setText(b.name);
    if (QTableWidgetItem* c = m_bandTable->item(i, ColBaseline)) c->setCheckState(b.localBaseline ? Qt::Checked : Qt::Unchecked);
    if (auto* sb = qobject_cast<QDoubleSpinBox*>(m_bandTable->cellWidget(i, ColFrom))) sb->setValue(b.lo);
    if (auto* sb = qobject_cast<QDoubleSpinBox*>(m_bandTable->cellWidget(i, ColTo))) sb->setValue(b.hi);
    if (auto* q = qobject_cast<QComboBox*>(m_bandTable->cellWidget(i, ColQuantity))) q->setCurrentIndex(int(b.quantity));
    if (auto* pr = qobject_cast<QComboBox*>(m_bandTable->cellWidget(i, ColProfile))) pr->setCurrentIndex(std::max(0, pr->findText(b.profile)));
    m_updating = false;
}

void AnalysisPage::trendBandsChanged(bool redrawSpectra) {
    // a band's name may have changed: reference combo texts
    m_updating = true;
    for (int i = 0; i < m_trendRef->count() && i < int(m_trendBands.size()); ++i)
        m_trendRef->setItemText(i, m_trendBands[size_t(i)].band.name);
    for (int r = 0; r < m_bandTable->rowCount() && r < int(m_trendBands.size()); ++r)
        if (auto* pr = qobject_cast<QComboBox*>(m_bandTable->cellWidget(r, ColProfile)))
            pr->setEnabled(bandtrend::needsFit(m_trendBands[size_t(r)].band.quantity));
    m_updating = false;
    m_dirty[methodIndex(Method::Trends)] = true;
    if (redrawSpectra) drawTrendSpectra();
    drawTrends();
    schedule();
}

void AnalysisPage::runTrends(bool wait) {
    m_dirty[methodIndex(Method::Trends)] = false;
    updateTrendMean();
    const int n = m_data.rows();
    // hashes of the preprocessed spectra (including the axis): the cached rows that are still valid
    quint64 gx = 1469598103934665603ULL;
    for (double v : m_data.x) gx = fnv(gx, v);
    std::vector<quint64> hashes(size_t(std::max(n, 0)));
    for (int i = 0; i < n; ++i) {
        quint64 h = gx;
        for (Eigen::Index j = 0; j < m_data.D.cols(); ++j) h = fnv(h, m_data.D(i, j));
        hashes[size_t(i)] = h;
    }
    const size_t before = m_trendHashes.size();
    size_t valid = 0;
    while (valid < hashes.size() && valid < m_trendHashes.size() && hashes[valid] == m_trendHashes[valid]) ++valid;
    m_trendHashes = std::move(hashes);
    if (m_trendBusy && valid < before) m_trendStop = true;   // the worker measures spectra that changed
    size_t fits = 0;
    for (size_t b = 0; b < m_trendBands.size(); ++b) {
        bandtrend::Band& band = m_trendBands[b].band;
        if (bandtrend::needsFit(band.quantity) && band.guesses.empty() && !m_trendMean.empty())
            bandtrend::setGuesses(band, m_data.x, m_trendMean);
        TrendCache& c = m_trendCache[b];
        const QString key = trendKey(band);
        if (c.key != key) {
            c.key = key;
            c.rows.clear();
        }
        if (c.rows.size() > valid) c.rows.resize(valid);
        if (bandtrend::needsFit(band.quantity)) fits += size_t(n) - c.rows.size();
    }
    if (m_trendBusy && wait) {                    // finish synchronously instead
        m_trendStop = true;
        if (m_trendThread.joinable()) m_trendThread.join();
        m_trendBusy = false;
        m_trendStop = false;
        ++m_trendRun;                             // its completion message is stale now
    }
    const bool async = !wait && (m_trendBusy || fits > size_t(kAsyncFits));
    // measure the missing spectra (fitted bands: in the background if there are many)
    for (size_t b = 0; b < m_trendBands.size(); ++b) {
        const bandtrend::Band& band = m_trendBands[b].band;
        if (async && bandtrend::needsFit(band.quantity)) continue;
        TrendCache& c = m_trendCache[b];
        for (int i = int(c.rows.size()); i < n; ++i)
            c.rows.push_back(bandtrend::measure(m_data.x, m_data.row(i), band, bandtrend::needsFit(band.quantity)));
    }
    if (async) {
        if (m_trendBusy) m_trendAgain = true;     // one at a time: runs again for the newest data afterwards
        else if (fits > 0) runTrendsAsync();
    }
    drawTrendSpectra();
    drawTrends();
}

void AnalysisPage::runTrendsAsync() {
    if (m_trendThread.joinable()) m_trendThread.join();
    std::vector<int> ids;
    std::vector<QString> keys;
    std::vector<size_t> starts;
    std::vector<bandtrend::Band> bands;
    size_t from = size_t(m_data.rows());
    for (size_t b = 0; b < m_trendBands.size(); ++b) {
        if (!bandtrend::needsFit(m_trendBands[b].band.quantity)) continue;
        if (m_trendCache[b].rows.size() >= size_t(m_data.rows())) continue;
        ids.push_back(m_trendBands[b].id);
        keys.push_back(m_trendCache[b].key);
        starts.push_back(m_trendCache[b].rows.size());
        bands.push_back(m_trendBands[b].band);
        from = std::min(from, starts.back());
    }
    if (ids.empty()) return;
    std::vector<Vec> rows;
    for (int i = int(from); i < m_data.rows(); ++i) rows.push_back(m_data.row(i));
    std::vector<quint64> hashes(m_trendHashes.begin() + std::ptrdiff_t(from), m_trendHashes.end());
    m_trendBusy = true;
    m_trendAgain = false;
    m_trendStop = false;
    const unsigned run = ++m_trendRun;
    m_trendThread = std::thread([this, x = m_data.x, rows = std::move(rows), hashes = std::move(hashes), ids, keys,
                                 starts, bands, from, run] {
        constexpr size_t kChunk = 16;
        const size_t end = from + rows.size();
        for (size_t c0 = from; c0 < end && !m_trendStop; c0 += kChunk) {
            const size_t c1 = std::min(end, c0 + kChunk);
            auto chunk = std::make_shared<TrendChunk>();
            chunk->ids = ids;
            chunk->keys = keys;
            chunk->from = c0;
            chunk->hashes.assign(hashes.begin() + std::ptrdiff_t(c0 - from), hashes.begin() + std::ptrdiff_t(c1 - from));
            for (size_t b = 0; b < bands.size(); ++b) {
                const size_t s = std::max(starts[b], c0);
                chunk->starts.push_back(s);
                std::vector<bandtrend::Measure> m;
                for (size_t i = s; i < c1 && !m_trendStop; ++i)
                    m.push_back(bandtrend::measure(x, rows[i - from], bands[b], true));
                chunk->rows.push_back(std::move(m));
            }
            if (m_trendStop) break;
            QMetaObject::invokeMethod(this, [this, chunk] { mergeTrendChunk(*chunk); }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(
            this,
            [this, run] {
                if (run != m_trendRun) return;     // superseded (runTrends(wait) joined it)
                if (m_trendThread.joinable()) m_trendThread.join();
                m_trendBusy = false;
                m_trendStop = false;
                drawTrends();
                emit trendsFinished();
                bool missing = m_trendAgain;
                for (const TrendCache& c : m_trendCache) missing = missing || c.rows.size() < size_t(m_data.rows());
                if (missing) {
                    m_dirty[methodIndex(Method::Trends)] = true;
                    schedule();
                }
            },
            Qt::QueuedConnection);
    });
}

void AnalysisPage::mergeTrendChunk(const TrendChunk& c) {
    bool any = false;
    for (size_t k = 0; k < c.ids.size(); ++k) {
        size_t b = 0;
        while (b < m_trendBands.size() && m_trendBands[b].id != c.ids[k]) ++b;
        if (b == m_trendBands.size()) continue;   // removed meanwhile
        TrendCache& cache = m_trendCache[b];
        if (cache.key != c.keys[k] || cache.rows.size() != c.starts[k]) continue;
        bool same = true;                          // measured on the spectra we still have?
        for (size_t j = 0; j < c.rows[k].size() && same; ++j) {
            const size_t i = c.starts[k] + j;
            same = i < m_trendHashes.size() && m_trendHashes[i] == c.hashes[i - c.from];
        }
        if (!same) continue;
        cache.rows.insert(cache.rows.end(), c.rows[k].begin(), c.rows[k].end());
        any = true;
    }
    if (any) drawTrends();
}

void AnalysisPage::drawTrendSpectra() {
    const bool first = m_trendSpectra->items().empty();
    m_trendSpectra->clear();
    const DataMatrix& d = m_data;
    if (d.empty()) return;
    const PlotPalette& pal = m_trendSpectra->plotPalette();
    const int n = d.rows();
    const int stride = std::max(1, (n + 39) / 40);         // at most ~40 curves (as the preview, thinner)
    std::vector<std::pair<int, Vec>> shown;
    for (int i = 0; i < n; i += stride) shown.emplace_back(i, d.row(i));
    if (n > 1 && shown.back().first != n - 1) shown.emplace_back(n - 1, d.row(n - 1));   // always the newest
    for (const auto& [i, y] : shown) {
        const double t = n > 1 ? double(i) / (n - 1) : 0.0;
        // grey, oldest faint, newest strong: the coloured bands stand out
        const QColor c = n > 1 ? colorMapColor(ColorMap::Grey, pal.isDark() ? 0.35 + 0.5 * t : 0.72 - 0.5 * t) : pal.main;
        auto* cv = m_trendSpectra->addCurve(d.x, y, QPen(c, 1.0));
        cv->setOpacity(n > 1 ? 0.75 : 1.0);
    }
    for (size_t b = 0; b < m_trendBands.size(); ++b) {
        const TrendBand& tb = m_trendBands[b];
        auto* r = m_trendSpectra->addRegion(tb.band.lo, tb.band.hi);
        r->setBrush(faded(tb.color, pal.isDark() ? 70 : 45));
        r->setEdgePen(QPen(tb.color, 1.4));
        const int id = tb.id;
        connect(r, &RegionItem::regionChangeFinished, this, [this, id](double a, double bb) {
            // later: redrawing deletes this region, which must not happen inside its own signal
            QTimer::singleShot(0, this, [this, id, a, bb] {
                for (size_t k = 0; k < m_trendBands.size(); ++k) {
                    if (m_trendBands[k].id != id) continue;
                    bandtrend::Band band = m_trendBands[k].band;
                    const QString oldDefault = bandtrend::defaultName((band.lo + band.hi) / 2);
                    band.lo = std::min(a, bb);
                    band.hi = std::max(a, bb);
                    if (band.name == oldDefault) band.name = bandtrend::defaultName((band.lo + band.hi) / 2);
                    setTrendBand(int(k), band);
                }
            });
        });
        // name above the tallest shown point in the band
        double top = -std::numeric_limits<double>::infinity();
        for (const auto& sp : shown)
            for (size_t j = 0; j < d.x.size(); ++j)
                if (d.x[j] >= tb.band.lo && d.x[j] <= tb.band.hi && std::isfinite(sp.second[j])) top = std::max(top, sp.second[j]);
        if (std::isfinite(top)) {
            auto* t = m_trendSpectra->addText(tb.band.name, (tb.band.lo + tb.band.hi) / 2, top, QPointF(0.5, 1.15));
            t->setColor(tb.color);
            QFont f = m_trendSpectra->font();
            f.setBold(true);
            t->setFont(f);
        }
    }
    if (first) m_trendSpectra->autoRange();
}

void AnalysisPage::drawTrends() {
    using namespace bandtrend;
    m_trendPlot->clear();
    const int n = m_data.rows();
    const size_t nb = m_trendBands.size();
    m_trendSeries.assign(nb, {});
    m_trendFits.assign(nb, {});
    // time axis
    Vec tsec, tplot;
    QString unit = QStringLiteral("s");
    double scale = 1;
    if (m_data.hasTimes && n > 0) {
        std::tie(unit, scale) = timeUnit(m_data.times.back() - m_data.times.front());
        tsec = m_data.times;
        for (double t : tsec) tplot.push_back(t / scale);
        m_trendPlot->setLabel(Axis::Bottom, QStringLiteral("Time (%1)").arg(unit));
    } else {
        for (int i = 0; i < n; ++i) {
            tsec.push_back(i + 1);
            tplot.push_back(i + 1);
        }
        unit = QStringLiteral("spectra");
        m_trendPlot->setLabel(Axis::Bottom, QStringLiteral("Spectrum"));
    }
    const auto norm = TrendNormalisation(m_trendNorm->currentIndex());
    const int ref = norm == TrendNormalisation::Ratio ? m_trendRef->currentIndex() : -1;
    const auto model = Kinetic(m_trendKinetic->currentIndex());
    const PlotPalette& pal = m_trendPlot->plotPalette();
    QStringList head{QStringLiteral("Spectrum"), m_data.hasTimes ? QStringLiteral("Time (%1)").arg(unit) : QStringLiteral("Index")};
    std::vector<Vec> cols{Vec(tplot.begin(), tplot.begin() + std::min<std::ptrdiff_t>(n, std::ptrdiff_t(tplot.size())))};
    QStringList fitText, notes;
    std::vector<Quantity> plotted;
    bool pending = false;
    for (size_t b = 0; b < nb; ++b) {
        const TrendBand& tb = m_trendBands[b];
        static const std::vector<Measure> none;
        // rows of other settings (a band just edited): nothing until measured again
        const std::vector<Measure>& rows = m_trendCache[b].key == trendKey(tb.band) ? m_trendCache[b].rows : none;
        pending = pending || rows.size() < size_t(n);
        if (int(b) == ref) continue;
        Series s = extract(rows, tb.band.quantity);
        if (intensityLike(tb.band.quantity)) {
            if (ref >= 0 && ref < int(nb)) {
                const TrendBand& rb = m_trendBands[size_t(ref)];
                const std::vector<Measure>& rr = m_trendCache[size_t(ref)].key == trendKey(rb.band) ? m_trendCache[size_t(ref)].rows : none;
                if (intensityLike(rb.band.quantity)) s = ratio(s, extract(rr, rb.band.quantity));
                else notes << QStringLiteral("the reference band must be a height or an area");
            } else if (norm == TrendNormalisation::First) {
                s = relativeToFirst(s);
            }
        } else if (norm != TrendNormalisation::None) {
            notes << QStringLiteral("%1: %2 not normalised").arg(tb.band.name, quantityNames()[int(tb.band.quantity)].toLower());
        }
        const size_t len = std::min(s.value.size(), tplot.size());
        s.value.resize(len);
        s.error.resize(len, kNaN);
        const Vec tp(tplot.begin(), tplot.begin() + std::ptrdiff_t(len)), ts(tsec.begin(), tsec.begin() + std::ptrdiff_t(len));
        KineticFit f = fitKinetic(model, ts, s.value, s.error);
        QString name = tb.band.name;
        if (model != Kinetic::None) {
            if (f.ok) name += QStringLiteral(":  ") + f.summary(unit, scale);
            fitText << QStringLiteral("<b>%1</b>: %2").arg(tb.band.name.toHtmlEscaped(),
                                                           f.ok ? f.summary(unit, scale).toHtmlEscaped() : f.message.toHtmlEscaped());
        }
        bool hasErr = false;
        for (double e : s.error) hasErr = hasErr || (std::isfinite(e) && e > 0);
        if (len > 0) {
            if (hasErr) m_trendPlot->addErrorBars(tp, s.value, s.error, QPen(faded(tb.color, 170), 1.0));
            const bool fitted = model != Kinetic::None && f.ok;    // fitted: points + model curve
            auto* c = m_trendPlot->addCurve(tp, s.value, QPen(fitted ? faded(tb.color, 80) : tb.color, fitted ? 1.0 : 1.6), name);
            c->setMarker(Marker::Circle, len > 150 ? 3.5 : 5.5, QBrush(tb.color));
            c->setDecimation(false);
            if (fitted) {
                Vec fx, fy;
                const double a = ts.front(), z = ts.back();
                for (int k = 0; k <= 240; ++k) {
                    const double t = a + (z - a) * k / 240.0;
                    fx.push_back(t / scale);
                    fy.push_back(f(t));
                }
                m_trendPlot->addCurve(fx, fy, QPen(tb.color, 1.8, Qt::DashLine))->setAutoScale(false);
            }
        }
        plotted.push_back(tb.band.quantity);
        const QString u = quantityUnit(tb.band.quantity);
        head << (u.isEmpty() ? tb.band.name : QStringLiteral("%1 (%2)").arg(tb.band.name, u));
        cols.push_back(s.value);
        if (hasErr) {
            head << QStringLiteral("± %1").arg(tb.band.name);
            cols.push_back(s.error);
        }
        m_trendSeries[b] = s;
        m_trendFits[b] = f;
    }
    // y axis title
    QString ylabel = QStringLiteral("Value");
    bool same = !plotted.empty();
    for (Quantity q : plotted) same = same && q == plotted.front();
    const bool normalised = norm != TrendNormalisation::None && same && intensityLike(plotted.front());
    if (normalised && ref >= 0 && ref < int(nb))
        ylabel = QStringLiteral("%1 / %2").arg(quantityNames()[int(plotted.front())], m_trendBands[size_t(ref)].band.name);
    else if (normalised)
        ylabel = QStringLiteral("%1 (first = 1)").arg(quantityNames()[int(plotted.front())]);
    else if (same) {
        const QString u = quantityUnit(plotted.front());
        ylabel = u.isEmpty() ? quantityNames()[int(plotted.front())] : QStringLiteral("%1 (%2)").arg(quantityNames()[int(plotted.front())], u);
    }
    m_trendPlot->setLabel(Axis::Left, ylabel);
    if (!plotted.empty() && std::all_of(plotted.begin(), plotted.end(), [](Quantity q) { return intensityLike(q); }) &&
        norm == TrendNormalisation::None)
        m_trendPlot->addHLine(0, QPen(faded(pal.reference, 120), 1, Qt::DashLine))->setAutoScale(false);
    std::vector<int> decimals(cols.size(), -1);
    decimals[0] = !m_data.hasTimes ? 0 : scale >= 3600 ? 3 : scale >= 60 ? 2 : 1;
    m_trendModel->set(head, QStringList(m_data.names.mid(0, n)), cols, decimals);

    QStringList info;
    if (m_trendBands.empty())
        info << QStringLiteral("Add a band: from the peaks of the mean spectrum (Add band) or by right-clicking in the spectra.");
    else if (n == 0)
        info << QStringLiteral("No spectra.");
    if (m_trendBusy || (pending && n > 0))
        info << QStringLiteral("Fitting peaks in the background…");
    if (!fitText.isEmpty()) info << fitText.join(QStringLiteral("; "));
    notes.removeDuplicates();
    if (!notes.isEmpty()) info << QStringLiteral("Note: ") + notes.join(QStringLiteral("; ")).toHtmlEscaped();
    m_trendInfo->setText(info.join(QStringLiteral("<br>")));
    m_trendInfo->setStyleSheet(QString());
}

bandtrend::Series AnalysisPage::trendSeries(int i) const {
    return i >= 0 && size_t(i) < m_trendSeries.size() ? m_trendSeries[size_t(i)] : bandtrend::Series{};
}

bandtrend::KineticFit AnalysisPage::trendFit(int i) const {
    return i >= 0 && size_t(i) < m_trendFits.size() ? m_trendFits[size_t(i)] : bandtrend::KineticFit{};
}

void AnalysisPage::fillAddBandMenu() {
    QMenu* m = m_addBand->menu();
    m->clear();
    std::vector<peaks::Peak> pk = trendPeaks();
    std::sort(pk.begin(), pk.end(), [](const peaks::Peak& a, const peaks::Peak& b) { return a.height > b.height; });
    if (pk.size() > 16) pk.resize(16);
    std::sort(pk.begin(), pk.end(), [](const peaks::Peak& a, const peaks::Peak& b) { return a.position < b.position; });
    if (pk.empty()) m->addAction(QStringLiteral("(no peaks in the mean spectrum)"))->setEnabled(false);
    for (const peaks::Peak& p : pk)
        m->addAction(QStringLiteral("%1 cm⁻¹   (FWHM %2)").arg(p.position, 0, 'f', 0).arg(p.fwhm, 0, 'f', 1), this,
                     [this, p] { addBand(bandtrend::bandAround(p, m_data.x)); });
    m->addSeparator();
    m->addAction(QStringLiteral("Right-click in the spectra for a band anywhere"))->setEnabled(false);
}

void AnalysisPage::trendSpectraMenu(const QPointF& data, const QPoint& globalPos) {
    QMenu m(this);
    const double x = data.x();
    const std::vector<peaks::Peak>& pk = trendPeaks();
    const peaks::Peak* near = nullptr;
    for (const peaks::Peak& p : pk)
        if (!near || std::abs(p.position - x) < std::abs(near->position - x)) near = &p;
    if (near && std::abs(near->position - x) < 25) {
        const peaks::Peak p = *near;
        m.addAction(QStringLiteral("Add band at peak %1 cm⁻¹").arg(p.position, 0, 'f', 0), this,
                    [this, p] { addBand(bandtrend::bandAround(p, m_data.x)); });
    }
    m.addAction(QStringLiteral("Add band here (%1 ± 15 cm⁻¹)").arg(x, 0, 'f', 0), this,
                [this, x] { addTrendBand(x - 15, x + 15); });
    for (size_t b = 0; b < m_trendBands.size(); ++b)
        if (x >= m_trendBands[b].band.lo && x <= m_trendBands[b].band.hi) {
            const int id = m_trendBands[b].id;
            m.addAction(QStringLiteral("Remove band %1").arg(m_trendBands[b].band.name), this, [this, id] {
                for (size_t k = 0; k < m_trendBands.size(); ++k)
                    if (m_trendBands[k].id == id) {
                        removeTrendBand(int(k));
                        return;
                    }
            });
        }
    m.addSeparator();
    m.addAction(QStringLiteral("Full view"), m_trendSpectra, &PlotWidget::autoRange);
    m.exec(globalPos);
}

// ============================================================================================ export
PlotWidget* AnalysisPage::plot(const QString& name) const {
    for (const PlotEntry& e : m_plots)
        if (e.name == name) return e.plot;
    return nullptr;
}

QStringList AnalysisPage::plotNames() const {
    QStringList n;
    for (const PlotEntry& e : m_plots) n << e.name;
    return n;
}

bool AnalysisPage::exportPlot(const QString& name, const QString& path) {
    PlotWidget* p = plot(name);
    if (!p) {
        m_error = QStringLiteral("no plot %1").arg(name);
        return false;
    }
    const QSize size = p->isVisible() && p->width() > 200 ? p->size() : QSize(900, 560);
    QString title;
    for (const PlotEntry& e : m_plots)
        if (e.name == name) title = e.title;
    const bool ok = path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive) ? p->exportSvg(path, size, title)
                                                                               : p->exportPng(path, size, 2.0);
    if (!ok) m_error = QStringLiteral("cannot write %1").arg(path);
    return ok;
}

bool AnalysisPage::exportCsv(const QString& kind, const QString& path) {
    const QStringList common{QStringLiteral("LibreRamen analysis: %1").arg(kind),
                             QStringLiteral("spectra: %1, points: %2").arg(m_data.rows()).arg(m_data.cols())};
    auto none = [this](const QString& what) {
        m_error = QStringLiteral("No %1 results to export").arg(what);
        return false;
    };
    const QString tcol = m_data.hasTimes ? QStringLiteral("time_s") : QStringLiteral("index");
    if (kind == QLatin1String("scores") || kind == QLatin1String("loadings") || kind == QLatin1String("variance")) {
        if (!m_pca) return none(QStringLiteral("PCA"));
        const PcaResult& r = *m_pca;
        QStringList c = common;
        c << QStringLiteral("scaling: %1").arg(m_pcaScaling->currentText());
        std::vector<Vec> cols;
        QStringList head;
        if (kind == QLatin1String("scores")) {
            head << QStringLiteral("name") << tcol;
            cols.push_back(m_data.times);
            for (int j = 0; j < r.components; ++j) {
                head << QStringLiteral("PC%1").arg(j + 1);
                cols.push_back(col(r.scores, j));
            }
            head << QStringLiteral("Q") << QStringLiteral("T2");
            cols.push_back(r.q);
            cols.push_back(r.t2);
            c << QStringLiteral("Q limit (95 %): %1, T2 limit (95 %): %2").arg(r.qLimit).arg(r.t2Limit);
            return writeCsv(path, c, head, m_data.names, cols, m_error);
        }
        if (kind == QLatin1String("loadings")) {
            head << QStringLiteral("raman_shift_cm1");
            cols.push_back(m_data.x);
            for (int j = 0; j < r.components; ++j) {
                head << QStringLiteral("PC%1").arg(j + 1);
                cols.push_back(col(r.loadings, j));
            }
            return writeCsv(path, c, head, {}, cols, m_error);
        }
        head << QStringLiteral("component") << QStringLiteral("singular_value") << QStringLiteral("explained")
             << QStringLiteral("cumulative");
        cols = {seq(int(r.explained.size())), r.singularValues, r.explained, r.cumulative};
        return writeCsv(path, c, head, {}, cols, m_error);
    }
    if (kind == QLatin1String("efa")) {
        if (!m_efa) return none(QStringLiteral("EFA"));
        QStringList head{QStringLiteral("name"), tcol};
        std::vector<Vec> cols{m_data.times};
        for (Eigen::Index j = 0; j < m_efa->forward.cols(); ++j) {
            head << QStringLiteral("forward_%1").arg(j + 1);
            cols.push_back(col(m_efa->forward, j));
        }
        for (Eigen::Index j = 0; j < m_efa->backward.cols(); ++j) {
            head << QStringLiteral("backward_%1").arg(j + 1);
            cols.push_back(col(m_efa->backward, j));
        }
        QStringList c = common;
        c << QStringLiteral("threshold: %1, noise: %2").arg(m_efa->threshold).arg(m_efa->noise);
        return writeCsv(path, c, head, m_data.names, cols, m_error);
    }
    if (kind == QLatin1String("pure")) {
        if (!m_simplisma && !m_opa) return none(QStringLiteral("pure-variable"));
        Matrix S;
        std::vector<int> sel;
        if (m_simplisma) {
            S = m_simplisma->S;
            sel = m_simplisma->pureVariables;
        } else {
            sel = m_opa->selected;
            if (m_pureMethod->currentIndex() == 1) S = m_opa->estimate;
            else {
                const Matrix& C = m_opa->estimate;
                S = Eigen::CompleteOrthogonalDecomposition<Matrix>(C.transpose() * C).solve(C.transpose() * m_data.D);
            }
        }
        QStringList head{QStringLiteral("raman_shift_cm1")};
        std::vector<Vec> cols{m_data.x};
        QStringList c = common;
        c << QStringLiteral("method: %1").arg(m_pureMethod->currentText());
        QStringList s;
        for (int v : sel) s << QString::number(v);
        c << QStringLiteral("selected indices: %1").arg(s.join(' '));
        for (Eigen::Index j = 0; j < S.rows(); ++j) {
            head << QStringLiteral("estimate_%1").arg(j + 1);
            cols.push_back(rowv(S, j));
        }
        return writeCsv(path, c, head, {}, cols, m_error);
    }
    if (kind == QLatin1String("spectra") || kind == QLatin1String("concentrations")) {
        if (!m_mcr) return none(QStringLiteral("MCR-ALS"));
        const McrResult& r = *m_mcr;
        QStringList c = common;
        c << r.message << QStringLiteral("lack of fit %: %1, explained variance %: %2").arg(r.lof).arg(r.r2);
        QStringList cons;
        if (m_mcrUsed.nonNegC) cons << QStringLiteral("non-negative C");
        if (m_mcrUsed.nonNegS) cons << QStringLiteral("non-negative S");
        if (m_mcrUsed.unimodalC) cons << QStringLiteral("unimodal C");
        if (m_mcrUsed.closure) cons << QStringLiteral("closure %1").arg(m_mcrUsed.closureTotal);
        if (!m_mcrUsed.fixedSpectra.empty()) cons << QStringLiteral("%1 fixed spectra").arg(m_mcrUsed.fixedSpectra.size());
        c << QStringLiteral("constraints: %1").arg(cons.join(QStringLiteral(", ")));
        const QStringList names = componentNames(int(r.S.rows()));
        for (size_t j = 0; j < m_bands.size(); ++j)
            c << QStringLiteral("ambiguity %1: f %2, min %3, max %4")
                     .arg(names.value(int(j)))
                     .arg(m_bands[j].fOptimal)
                     .arg(m_bands[j].fMin)
                     .arg(m_bands[j].fMax);
        if (kind == QLatin1String("spectra")) {
            QStringList head{QStringLiteral("raman_shift_cm1")};
            std::vector<Vec> cols{m_data.x};
            for (Eigen::Index j = 0; j < r.S.rows(); ++j) {
                head << names.value(int(j));
                cols.push_back(rowv(r.S, j));
            }
            return writeCsv(path, c, head, {}, cols, m_error);
        }
        QStringList head{QStringLiteral("name"), tcol};
        std::vector<Vec> cols{m_data.times};
        for (Eigen::Index j = 0; j < r.C.cols(); ++j) {
            head << names.value(int(j));
            cols.push_back(col(r.C, j));
        }
        return writeCsv(path, c, head, m_data.names, cols, m_error);
    }
    if (kind == QLatin1String("nnls")) {
        if (!m_nnls && m_nnlsAll.empty()) return none(QStringLiteral("NNLS"));
        QStringList c = common;
        c << QStringLiteral("components: %1").arg(m_nnlsSource->currentText());
        if (!m_nnlsAll.empty()) {
            QStringList head{QStringLiteral("name"), tcol};
            std::vector<Vec> cols{m_data.times};
            for (size_t j = 0; j < m_nnlsAll[0].coefficients.size(); ++j) {
                head << m_nnlsNames.value(int(j));
                Vec a;
                for (const auto& f : m_nnlsAll) a.push_back(f.coefficients[j]);
                cols.push_back(a);
            }
            Vec r2, rn;
            for (const auto& f : m_nnlsAll) {
                r2.push_back(f.r2);
                rn.push_back(f.residualNorm);
            }
            head << QStringLiteral("R2") << QStringLiteral("residual_norm");
            cols.push_back(r2);
            cols.push_back(rn);
            return writeCsv(path, c, head, m_data.names, cols, m_error);
        }
        const NnlsFit& f = *m_nnls;
        QStringList amounts;
        for (size_t j = 0; j < f.coefficients.size(); ++j)
            amounts << QStringLiteral("%1 = %2").arg(m_nnlsNames.value(int(j))).arg(f.coefficients[j], 0, 'g', 10);
        c << QStringLiteral("target: %1").arg(m_nnlsTarget->currentText()) << QStringLiteral("amounts: %1").arg(amounts.join(", "))
          << QStringLiteral("R2: %1").arg(f.r2, 0, 'g', 10);
        const int target = m_nnlsTarget->currentIndex() - 1;
        QStringList head{QStringLiteral("raman_shift_cm1"), QStringLiteral("spectrum"), QStringLiteral("fit"),
                         QStringLiteral("residual"), QStringLiteral("background")};
        std::vector<Vec> cols{m_data.x, m_data.row(target), f.fitted, f.residual, f.background};
        return writeCsv(path, c, head, {}, cols, m_error);
    }
    if (kind == QLatin1String("trends")) {
        if (m_trendBands.empty() || m_data.empty()) return none(QStringLiteral("peak trend"));
        using namespace bandtrend;
        QStringList c = common;
        const auto norm = TrendNormalisation(m_trendNorm->currentIndex());
        c << QStringLiteral("normalisation: %1%2")
                 .arg(m_trendNorm->currentText())
                 .arg(norm == TrendNormalisation::Ratio ? QStringLiteral(" (%1)").arg(m_trendRef->currentText()) : QString());
        QStringList head{QStringLiteral("name"), tcol};
        std::vector<Vec> cols{m_data.times};
        if (!m_data.hasTimes) {
            cols[0].clear();
            for (int i = 0; i < m_data.rows(); ++i) cols[0].push_back(i + 1);
        }
        for (size_t b = 0; b < m_trendBands.size(); ++b) {
            const Band& band = m_trendBands[b].band;
            QString def = QStringLiteral("band %1: %2-%3 cm-1, %4, local baseline %5")
                              .arg(band.name)
                              .arg(band.lo, 0, 'f', 1)
                              .arg(band.hi, 0, 'f', 1)
                              .arg(quantityKey(band.quantity))
                              .arg(band.localBaseline ? "on" : "off");
            if (needsFit(band.quantity)) {
                QStringList g;
                for (const peaks::Peak& p : band.guesses) g << QString::number(p.position, 'f', 1);
                def += QStringLiteral(", profile %1, start peaks %2, main %3").arg(band.profile, g.join(' ')).arg(band.main + 1);
            }
            c << def;
            const KineticFit f = trendFit(int(b));
            if (f.model != Kinetic::None)
                c << (f.ok ? QStringLiteral("fit %1: %2 (A %3 ± %4, B %5 ± %6, rate %7 ± %8 per s, t0 %9 s)")
                                 .arg(band.name, f.summary())
                                 .arg(f.a, 0, 'g', 8).arg(f.aErr, 0, 'g', 3).arg(f.b, 0, 'g', 8).arg(f.bErr, 0, 'g', 3)
                                 .arg(f.rate, 0, 'g', 8).arg(f.rateErr, 0, 'g', 3).arg(f.t0)
                           : QStringLiteral("fit %1: %2").arg(band.name, f.message));
            const Series s = trendSeries(int(b));
            if (!s.value.empty()) {
                head << band.name;
                cols.push_back(s.value);
                head << band.name + QStringLiteral(" error");
                cols.push_back(s.error);
            }
            // the raw measurements
            const std::vector<Measure>& rows = m_trendCache[b].rows;
            std::vector<Quantity> qs{Quantity::Height, Quantity::Area, Quantity::Centroid, Quantity::Fwhm};
            if (needsFit(band.quantity)) qs.insert(qs.end(), {Quantity::FitArea, Quantity::FitHeight, Quantity::FitPosition, Quantity::FitFwhm});
            for (Quantity q : qs) {
                const Series e = extract(rows, q);
                head << QStringLiteral("%1 %2").arg(band.name, quantityKey(q));
                cols.push_back(e.value);
                if (needsFit(q)) {
                    head << QStringLiteral("%1 %2_err").arg(band.name, quantityKey(q));
                    cols.push_back(e.error);
                }
            }
            if (needsFit(band.quantity)) {
                Vec r2;
                for (const Measure& m : rows) r2.push_back(m.fitR2);
                head << QStringLiteral("%1 fit_r2").arg(band.name);
                cols.push_back(r2);
            }
        }
        return writeCsv(path, c, head, m_data.names, cols, m_error);
    }
    m_error = QStringLiteral("unknown export %1").arg(kind);
    return false;
}

}  // namespace lr::gui
