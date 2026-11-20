#include "datapage.h"

#include "plotwidget.h"
#include "theme.h"

#include "core/errors.h"
#include "core/pyformat.h"

#include <QAbstractItemView>
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
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace lr::gui {

namespace {

const char* OPEN_FILTER =
    "Spectra (*.csv *.txt *.spc *.jdx *.dx *.dat);;LibreRamen / vendor CSV (*.csv);;"
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
        processing::Settings s;            // Measure page defaults: SNIP, strength 5 (slider 50)
        s.baseline = QStringLiteral("snip");
        s.strength = 5.0;
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
    title->setToolTip(tr("<p>Double-click a name to rename it; right-click for colour and removal.</p>"));
    lv->addWidget(title);
    auto* list = new SpectrumList;
    list->onDrop = [this](const QStringList& p) { openFiles(p); };
    m_list = list;
    connect(m_list, &QListWidget::itemChanged, this, &DataPage::onItemChanged);
    connect(m_list, &QListWidget::currentRowChanged, this, [this] {
        if (!m_batch) updatePeaks();
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
    m_baseline = new QCheckBox(tr("baseline correction"));
    m_baseline->setChecked(true);
    m_baseline->setToolTip(tr("<p>Baseline method and strength of the Measure page</p>"));
    connect(m_baseline, &QCheckBox::toggled, this, &DataPage::redraw);
    bar->addWidget(m_baseline);
    m_diff = new QCheckBox(tr("difference A − B"));
    m_diff->setToolTip(tr("<p>First ticked spectrum minus the second, both processed as shown</p>"));
    connect(m_diff, &QCheckBox::toggled, this, &DataPage::redraw);
    bar->addWidget(m_diff);
    bar->addSpacing(6);
    auto* offLabel = new QLabel(tr("offset"));
    bar->addWidget(offLabel);
    m_offset = new QSlider(Qt::Horizontal);
    m_offset->setRange(0, 100);
    m_offset->setValue(40);
    m_offset->setMaximumWidth(100);
    m_offset->setToolTip(tr("<p>Vertical offset between stacked spectra</p>"));
    connect(m_offset, &QSlider::valueChanged, this, &DataPage::redraw);
    bar->addWidget(m_offset);
    bar->addStretch();
    auto* calc = new QPushButton(tr("Calculator…"));
    calc->setToolTip(tr("<p>New spectrum = a·A + b·B + c (scaling, offset, weighted difference)</p>"));
    connect(calc, &QPushButton::clicked, this, &DataPage::calculatorDialog);
    bar->addWidget(calc);
    auto* exp = new QToolButton;
    exp->setText(tr("Export ▾"));
    exp->setPopupMode(QToolButton::InstantPopup);
    auto* m = new QMenu(exp);
    m->addAction(tr("Ticked spectra as CSV…"), this, [this] { exportDialog("csv"); });
    m->addAction(tr("Ticked spectra as JCAMP-DX…"), this, [this] { exportDialog("jdx"); });
    m->addAction(tr("Ticked spectra as one table (common axis)…"), this, [this] { exportDialog("table"); });
    m->addSeparator();
    m->addAction(tr("Plot as PNG…"), this, [this] { exportDialog("png"); });
    m->addAction(tr("Plot as SVG…"), this, [this] { exportDialog("svg"); });
    exp->setMenu(m);
    bar->addWidget(exp);
    rv->addLayout(bar);

    m_stack = new QStackedWidget;
    m_plot = new PlotWidget;
    m_plot->setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    m_plot->setLabel(Axis::Left, tr("Intensity"));
    m_plot->setGrid(true, true);
    m_plot->setLegendVisible(true);
    m_plot->setHeadroom(0.08);              // Python rescale_y(0.08)
    m_plot->setToolTip(PlotWidget::navigationHelp());
    m_stack->addWidget(m_plot);
    m_heat = new PlotWidget;
    m_heat->setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    m_heat->setLabel(Axis::Left, tr("spectrum #"));
    m_heat->setGrid(false, false);
    m_heat->setWheelMode(WheelMode::ZoomX);
    m_stack->addWidget(m_heat);
    rv->addWidget(m_stack, 3);

    auto* prow = new QHBoxLayout;
    prow->addWidget(new QLabel(tr("<b>Peaks</b> of the current spectrum")));
    prow->addStretch();
    prow->addWidget(new QLabel(tr("fit profile")));
    m_profile = new QComboBox;
    m_profile->addItems({"pvoigt", "voigt", "lorentz", "gauss"});   // Python order (default pvoigt)
    prow->addWidget(m_profile);
    auto* fit = new QPushButton(tr("Fit peaks in view"));
    fit->setToolTip(tr("<p>Fits the peaks of the current spectrum inside the visible x range (zoom in first). "
                       "Drag the shaded fit window or its edges to fit another range.</p>"));
    connect(fit, &QPushButton::clicked, this, [this] {
        QString err;
        if (!fitView(&err) && !err.isEmpty()) info(tr("Fit"), err);
    });
    prow->addWidget(fit);
    auto* clr = new QPushButton(tr("Clear fit"));
    connect(clr, &QPushButton::clicked, this, [this] {
        clearFit();
        updatePeaks();
    });
    prow->addWidget(clr);
    rv->addLayout(prow);
    m_table = new QTableWidget(0, 6);
    m_table->setHorizontalHeaderLabels({QStringLiteral("position (cm⁻¹)"), QStringLiteral("±"), tr("height"),
                                        QStringLiteral("FWHM (cm⁻¹)"), tr("area"), tr("S/N or η")});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
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

void DataPage::addSpectrum(const fileio::DataSpectrum& sp, bool checked) {
    addEntry(sp, checked);
    redraw();
}

void DataPage::addEntry(const fileio::DataSpectrum& sp, bool checked) {
    m_entries.push_back({sp, QColor()});
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
    const int r = m_list->row(it);
    QMenu menu(this);
    menu.addAction(tr("Rename"), this, [this, it] { m_list->editItem(it); });
    menu.addAction(tr("Colour…"), this, [this, r] {
        const QColor c = QColorDialog::getColor(colorOf(r), this, tr("Spectrum colour"));
        if (c.isValid()) setColor(r, c);
    });
    QAction* autoColor = menu.addAction(tr("Automatic colour"), this, [this, r] { setColor(r, QColor()); });
    autoColor->setEnabled(m_entries[size_t(r)].color.isValid());
    menu.addSeparator();
    menu.addAction(tr("Remove"), this, [this, r] {
        std::vector<int> rows;
        for (const QModelIndex& i : m_list->selectionModel()->selectedRows()) rows.push_back(i.row());
        if (std::find(rows.begin(), rows.end(), r) == rows.end()) rows = {r};
        removeRows(rows);
    });
    menu.exec(m_list->viewport()->mapToGlobal(pos));
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
void DataPage::setBaselineCorrection(bool on) { m_baseline->setChecked(on); }
bool DataPage::baselineCorrection() const { return m_baseline->isChecked(); }
void DataPage::setDifference(bool on) { m_diff->setChecked(on); }
bool DataPage::difference() const { return m_diff->isChecked(); }
void DataPage::setOffset(int percent) { m_offset->setValue(percent); }
int DataPage::offset() const { return m_offset->value(); }
void DataPage::setFitProfile(const QString& shape) { m_profile->setCurrentText(shape); }
QString DataPage::fitText() const { return m_fitLabel->text(); }

processing::Settings DataPage::settings() const {
    processing::Settings s = m_settingsFn();
    if (!m_baseline->isChecked()) s.baseline = QStringLiteral("none");
    s.normalize = m_norm->currentText();
    return s;
}

std::pair<Vec, Vec> DataPage::processed(const fileio::DataSpectrum& sp, QString* error) const {
    try {
        return {sp.x, processing::process(sp.x, sp.y, settings()).first};
    } catch (const std::exception& e) {
        if (error) *error = sp.name + ": " + QString::fromUtf8(e.what());
        return {sp.x, sp.y};
    }
}

QString DataPage::describeProcessing() const {
    const processing::Settings ps = m_settingsFn();
    return QStringLiteral("baseline=%1 strength=%2 despike=%3 smooth=%4 normalize=%5")
        .arg(m_baseline->isChecked() ? ps.baseline : QStringLiteral("none"), py::fixed(ps.strength, 1),
             py::str(ps.despike), py::str(ps.smooth), m_norm->currentText());
}

void DataPage::updateSwatches() {
    const QSignalBlocker block(m_list);
    for (int i = 0; i < m_list->count(); ++i) m_list->item(i)->setIcon(swatch(colorOf(i)));
}

void DataPage::redraw() {
    if (m_batch) {
        m_dirty = true;
        return;
    }
    m_dirty = false;
    clearFit();
    m_plot->clear();
    m_heat->clear();
    m_heatItem = nullptr;
    m_note->clear();
    const std::vector<int> rows = tickedRows();
    const View mode = view();
    const bool diff = m_diff->isChecked() && rows.size() >= 2;
    m_stack->setCurrentIndex(mode == View::HeatMap && !diff ? 1 : 0);
    m_offset->setEnabled(mode == View::Stacked && !diff);
    updateSwatches();
    if (rows.empty()) {
        updatePeaks();
        return;
    }
    QStringList problems;
    std::vector<std::pair<Vec, Vec>> data;
    for (int r : rows) {
        QString err;
        data.push_back(processed(m_entries[size_t(r)].sp, &err));
        if (!err.isEmpty()) problems << err;
    }
    if (!problems.isEmpty()) {
        m_note->setText(tr("processing failed, shown unprocessed: %1").arg(problems.join("; ")));
        m_note->setToolTip(problems.join('\n'));
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
            m_note->setText(tr("heat map: the ticked spectra have no common x range"));
        }
    } else {
        double span = 0;
        for (const auto& d : data) {
            const double s = percentile(d.second, 99.5) - percentile(d.second, 1);
            if (std::isfinite(s)) span = std::max(span, s);
        }
        if (span == 0) span = 1.0;
        const double step = mode == View::Stacked ? span * m_offset->value() / 100.0 : 0.0;
        for (std::size_t k = 0; k < data.size(); ++k) {
            Vec y = data[k].second;
            if (step != 0)
                for (double& v : y) v += double(k) * step;
            m_plot->addCurve(data[k].first, std::move(y), QPen(colorOf(rows[k]), 1.3), nameOf(int(k)));
        }
    }
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
}

int DataPage::heightDecimals() const {
    return m_norm->currentText() == QLatin1String("none") ? 0 : 3;   // normalised: 0..1 (Python: always 0)
}

void DataPage::changeEvent(QEvent* e) {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_plot) redraw();      // theme switch: new series colours
}

void DataPage::updatePeaks() {
    m_table->setRowCount(0);
    const int r = currentRow();
    if (r < 0 || r >= count()) return;
    const auto [x, y] = processed(m_entries[size_t(r)].sp);
    try {
        const int dh = heightDecimals();
        for (const peaks::Peak& p : peaks::pick(x, y, 8.0, 2.0, false, 150.0))
            fillPeakRow({{p.position, 1}, {NaN, 2}, {p.height, dh}, {p.fwhm, 1}, {p.area, dh}, {p.snr, 2}});
    } catch (const std::exception&) {
        // too few points etc.: no peak table
    }
}

void DataPage::clearFit() {
    for (PlotItem* it : m_fitItems) m_plot->removeItem(it);
    m_fitItems.clear();
    if (m_fitRegion) m_plot->removeItem(m_fitRegion);
    m_fitRegion = nullptr;
    m_fitLabel->clear();
}

bool DataPage::fitView(QString* error) {
    if (currentRow() < 0) return false;
    const Range v = m_plot->xRange();
    if (v.hi - v.lo > 1200) {
        if (error) *error = tr("Zoom in on the band(s) to fit first (visible range < 1200 cm⁻¹).");
        return false;
    }
    return fitRange(v.lo, v.hi, error);
}

bool DataPage::fitRange(double lo, double hi, QString* error) {
    const int row = currentRow();
    if (row < 0 || row >= count()) return false;
    if (lo > hi) std::swap(lo, hi);
    const auto [x, y] = processed(m_entries[size_t(row)].sp);
    peaks::FitResult res;
    try {
        res = peaks::fit(x, y, {lo, hi}, std::nullopt, m_profile->currentText());
    } catch (const std::exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        if (m_fitRegion) {                      // a dragged fit window: say why there is no fit now
            for (PlotItem* it : m_fitItems) m_plot->removeItem(it);
            m_fitItems.clear();
            m_fitLabel->setText(tr("no fit in %1–%2 cm⁻¹: %3")
                                    .arg(py::fixed(lo, 0), py::fixed(hi, 0), QString::fromUtf8(e.what())));
        }
        return false;
    }
    for (PlotItem* it : m_fitItems) m_plot->removeItem(it);
    m_fitItems.clear();
    const PlotPalette& pal = m_plot->plotPalette();
    Curve* model = m_plot->addCurve(res.x, res.model, QPen(pal.label, 1.2, Qt::DashLine));
    model->setAutoScale(false);
    model->setZValue(5);
    m_fitItems.push_back(model);
    double xm = 0;
    for (double v : res.x) xm += v;
    xm /= double(std::max<std::size_t>(1, res.x.size()));
    for (std::size_t k = 0; k < res.components.size(); ++k) {
        Vec yc = res.components[k];
        for (std::size_t i = 0; i < yc.size() && i < res.x.size(); ++i)
            yc[i] += res.baseline.first + res.baseline.second * (res.x[i] - xm);
        Curve* c = m_plot->addCurve(res.x, std::move(yc), QPen(pal.seriesColor(int(k) + 3), 1));
        c->setAutoScale(false);
        c->setZValue(4);
        m_fitItems.push_back(c);
    }
    if (!m_fitRegion) {
        m_fitRegion = m_plot->addRegion(lo, hi);
        m_fitRegion->setAutoScale(false);
        QColor shade = pal.region;
        shade.setAlphaF(shade.alphaF() * 0.6);
        m_fitRegion->setBrush(shade);
        connect(m_fitRegion, &RegionItem::regionChangeFinished, this,
                [this](double a, double b) {
                    QString err;
                    fitRange(a, b, &err);
                },
                Qt::QueuedConnection);
    } else {
        m_fitRegion->setRegion(lo, hi);
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
    return true;
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
    if (tickedRows().empty()) return;
    if (kind == "table") {
        const QString p = QFileDialog::getSaveFileName(this, tr("Save table"), "spectra.csv", "CSV (*.csv)");
        if (!p.isEmpty()) exportTable(p);
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
