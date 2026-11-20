// Computed Raman spectra panel (see computedpanel.h).
#include "computedpanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "core/errors.h"
#include "core/processing.h"
#include "plotwidget.h"
#include "theme.h"

namespace lr::gui {

using namespace lr::computed;

namespace {

constexpr double kStackOffset = 1.3;        // experimental spectrum above the computed ones (Stacked)

QString num(double v, int decimals) {
    if (!std::isfinite(v)) return {};
    QString t = QString::number(v, 'f', decimals);
    if (t.startsWith('-') && t.mid(1).toDouble() == 0.0) t.remove(0, 1);     // "-0.0"
    return t;
}

QDoubleSpinBox* spin(double lo, double hi, double value, int decimals, double step, const QString& suffix = {}) {
    auto* s = new QDoubleSpinBox;
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setSingleStep(step);
    s->setValue(value);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false);
    return s;
}

}  // namespace

// ============================================================================================== set-up
ComputedPanel::ComputedPanel(QWidget* parent) : QWidget(parent) { build(); }

ComputedPanel::~ComputedPanel() = default;

void ComputedPanel::build() {
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(8, 4, 8, 8);
    auto* split = new QSplitter(Qt::Horizontal);
    outer->addWidget(split);

    // ---------------------------------------------------------------- left: structures and settings
    auto* left = new QWidget;
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel(tr("<b>Structures</b> (tick to show)"));
    title->setToolTip(tr("<p>Every frequency calculation in the opened files. The current (selected) structure "
                         "gets the stick spectrum, its own scaling factor and the assignment table.</p>"));
    lv->addWidget(title);
    m_list = new QListWidget;
    m_list->setMinimumHeight(90);
    m_list->setTextElideMode(Qt::ElideMiddle);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_list, &QListWidget::currentRowChanged, this, &ComputedPanel::onCurrentChanged);
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem*) {
        if (!m_updating) redraw();
    });
    lv->addWidget(m_list, 1);
    auto* row = new QHBoxLayout;
    auto button = [&row](const QString& text, const QString& tip = {}) {
        auto* b = new QPushButton(text);
        b->setToolTip(tip);
        row->addWidget(b);
        return b;
    };
    connect(button(tr("Open…"), tr("<p>Gaussian, ORCA (.out, .hess), NWChem, CP2K, Quantum ESPRESSO (dynmat.x), "
                                   "vasp_raman.py, Phonopy-Spectroscopy or CASTEP output</p>")),
            &QPushButton::clicked, this, &ComputedPanel::openFilesDialog);
    connect(button(tr("Folder…"), tr("<p>Every supported output file under a folder (e.g. an ECCE project)</p>")),
            &QPushButton::clicked, this, &ComputedPanel::openFolderDialog);
    connect(button(tr("Remove")), &QPushButton::clicked, this, [this] { removeRow(currentRow()); });
    lv->addLayout(row);
    m_info = new QLabel;
    m_info->setWordWrap(true);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lv->addWidget(m_info);

    auto* spec = new QGroupBox(tr("Intensities"));
    spec->setStyleSheet(theme::groupBoxStyle());
    auto* sf = new QFormLayout(spec);
    m_quantity = new QComboBox;
    m_quantity->addItems({tr("cross-section (laser, T)"), tr("Raman activity")});
    m_quantity->setToolTip(tr("<p><b>cross-section</b>: dσ/dΩ ∝ (ν₀ − νᵢ)⁴ Sᵢ / (νᵢ (1 − exp(−hcνᵢ/kT))) "
                              "(Polavarapu 1990), comparable with a measured spectrum.<br>"
                              "<b>Raman activity</b>: Sᵢ as computed (Å⁴/amu).</p>"));
    sf->addRow(tr("Show"), m_quantity);
    m_laser = spin(200, 2000, 532, 1, 1, " nm");
    sf->addRow(tr("Laser"), m_laser);
    m_temp = spin(1, 2000, 298.15, 2, 5, " K");
    sf->addRow(tr("Temperature"), m_temp);
    lv->addWidget(spec);

    auto* sc = new QGroupBox(tr("Frequency scaling (current structure)"));
    sc->setStyleSheet(theme::groupBoxStyle());
    auto* scf = new QFormLayout(sc);
    m_factor = spin(0.5, 1.5, 1.0, 4, 0.001);
    scf->addRow(tr("Factor"), m_factor);
    m_two = new QCheckBox(tr("two regions"));
    m_two->setToolTip(tr("<p>One factor below and one above the split frequency (e.g. C–H stretches)</p>"));
    scf->addRow(m_two);
    m_split = spin(100, 5000, 2000, 0, 50, " cm⁻¹");
    scf->addRow(tr("Split at"), m_split);
    m_factorHigh = spin(0.5, 1.5, 1.0, 4, 0.001);
    scf->addRow(tr("Factor above"), m_factorHigh);
    m_matching = new QComboBox;
    m_matching->addItems({tr("Hungarian (optimal)"), tr("nearest")});
    m_matching->setToolTip(tr("<p>How computed bands are assigned to experimental peaks: <b>Hungarian</b> "
                              "minimises the total shift over all pairs, <b>nearest</b> pairs the closest "
                              "first.</p>"));
    scf->addRow(tr("Matching"), m_matching);
    m_tolerance = spin(1, 200, 30, 0, 5, " cm⁻¹");
    scf->addRow(tr("Tolerance"), m_tolerance);
    auto* fitRow = new QHBoxLayout;
    auto* fit = new QPushButton(tr("Fit to experimental"));
    fit->setToolTip(tr("<p>Least-squares factor(s) from the assigned band positions</p>"));
    connect(fit, &QPushButton::clicked, this, [this] {
        QString err;
        if (!fitScaling(&err)) warn(tr("Scaling"), err);
    });
    fitRow->addWidget(fit);
    auto* cmp = new QPushButton(tr("Compare all"));
    cmp->setToolTip(tr("<p>Correlation of every structure with the experimental spectrum, each with its own "
                       "fitted scaling</p>"));
    connect(cmp, &QPushButton::clicked, this, [this] {
        QString err;
        if (compareStructures(&err).empty() && !err.isEmpty()) warn(tr("Compare"), err);
    });
    fitRow->addWidget(cmp);
    scf->addRow(fitRow);
    m_fitLabel = new QLabel;
    m_fitLabel->setWordWrap(true);
    scf->addRow(m_fitLabel);
    lv->addWidget(sc);
    for (QDoubleSpinBox* s : {m_factor, m_split, m_factorHigh})
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &ComputedPanel::onScalingEdited);
    connect(m_two, &QCheckBox::toggled, this, &ComputedPanel::onScalingEdited);
    connect(m_matching, qOverload<int>(&QComboBox::currentIndexChanged), this, &ComputedPanel::redraw);
    connect(m_tolerance, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &ComputedPanel::redraw);

    auto* br = new QGroupBox(tr("Broadening and plot"));
    br->setStyleSheet(theme::groupBoxStyle());
    auto* bf = new QFormLayout(br);
    m_shape = new QComboBox;
    m_shape->addItems({"Lorentzian", "Gaussian", "pseudo-Voigt"});
    bf->addRow(tr("Profile"), m_shape);
    m_fwhm = spin(0.5, 200, 8, 1, 0.5, " cm⁻¹");
    bf->addRow(tr("FWHM"), m_fwhm);
    m_eta = spin(0, 1, 0.5, 2, 0.05);
    m_eta->setToolTip(tr("<p>Lorentzian fraction of the pseudo-Voigt profile</p>"));
    bf->addRow(tr("η"), m_eta);
    m_layout = new QComboBox;
    m_layout->addItems({tr("stacked"), tr("overlay"), tr("mirror")});
    bf->addRow(tr("Layout"), m_layout);
    m_sticks = new QCheckBox(tr("sticks"));
    m_sticks->setChecked(true);
    m_baseline = new QCheckBox(tr("baseline-correct experimental"));
    m_baseline->setChecked(true);
    auto* chk = new QHBoxLayout;
    chk->addWidget(m_sticks);
    chk->addWidget(m_baseline);
    bf->addRow(chk);
    lv->addWidget(br);
    for (QDoubleSpinBox* s : {m_laser, m_temp, m_fwhm, m_eta})
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &ComputedPanel::redraw);
    for (QComboBox* c : {m_quantity, m_shape, m_layout})
        connect(c, qOverload<int>(&QComboBox::currentIndexChanged), this, &ComputedPanel::redraw);
    connect(m_shape, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int i) { m_eta->setEnabled(i == 2); });
    m_eta->setEnabled(false);
    connect(m_sticks, &QCheckBox::toggled, this, &ComputedPanel::redraw);
    connect(m_baseline, &QCheckBox::toggled, this, [this](bool on) { setBaselineCorrection(on); });
    split->addWidget(left);

    // ------------------------------------------------------------------- right: plot and assignment
    auto* right = new QWidget;
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    auto* bar = new QHBoxLayout;
    bar->addWidget(new QLabel(tr("<b>Computed and experimental spectra</b> (normalised)")));
    bar->addStretch();
    auto* exp = new QToolButton;
    exp->setText(tr("Export ▾"));
    exp->setPopupMode(QToolButton::InstantPopup);
    auto* m = new QMenu(exp);
    m->addAction(tr("Broadened spectrum as CSV…"), this, [this] { exportDialog("csv"); });
    m->addAction(tr("Broadened spectrum as JCAMP-DX…"), this, [this] { exportDialog("jdx"); });
    m->addAction(tr("Assignment table as CSV…"), this, [this] { exportDialog("table"); });
    m->addSeparator();
    m->addAction(tr("Plot as PNG…"), this, [this] { exportDialog("png"); });
    m->addAction(tr("Plot as SVG…"), this, [this] { exportDialog("svg"); });
    exp->setMenu(m);
    bar->addWidget(exp);
    rv->addLayout(bar);

    m_plot = new PlotWidget;
    m_plot->setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    m_plot->setLabel(Axis::Left, tr("Normalised intensity"));
    m_plot->setGrid(true, true);
    m_plot->setLegendVisible(true);
    m_plot->setHeadroom(0.08);
    m_plot->setToolTip(PlotWidget::navigationHelp());
    rv->addWidget(m_plot, 3);

    auto* trow = new QHBoxLayout;
    trow->addWidget(new QLabel(tr("<b>Band assignment</b> (current structure)")));
    trow->addStretch();
    m_tableNote = new QLabel;
    trow->addWidget(m_tableNote);
    rv->addLayout(trow);
    m_table = new QTableWidget(0, 7);
    m_table->setHorizontalHeaderLabels({tr("computed (cm⁻¹)"), tr("unscaled"), tr("symmetry"), tr("rel. int."),
                                        tr("experimental (cm⁻¹)"), tr("rel. int."), QStringLiteral("Δ (cm⁻¹)")});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 6);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setMaximumHeight(200);
    rv->addWidget(m_table, 1);
    split->addWidget(right);
    split->setStretchFactor(1, 1);
    split->setSizes({300, 900});
    updateInfo();
    redraw();
}

// ========================================================================================= structures
bool ComputedPanel::loadFile(const QString& path, QString* error) {
    try {
        const auto calcs = computed::read(path);
        const int before = count();
        m_updating = true;
        for (const Calculation& c : calcs) addCalculation(c, true);
        m_list->setCurrentRow(before);            // the first structure of the new file
        m_updating = false;
        onCurrentChanged();
        m_plot->autoRange();
        return true;
    } catch (const Error& e) {
        m_updating = false;
        const QString msg = QString::fromStdString(e.what());
        if (error) *error = msg;
        warn(tr("Computed spectrum"), msg);
        return false;
    }
}

QStringList ComputedPanel::loadFiles(const QStringList& paths) {
    QStringList errors;
    m_collecting = true;                      // one message for all files
    for (const QString& p : paths) {
        QString err;
        if (!loadFile(p, &err)) errors << err;
    }
    m_collecting = false;
    if (!errors.isEmpty()) warn(tr("Computed spectra"), errors.join("\n"));
    return errors;
}

int ComputedPanel::loadFolder(const QString& dir) {
    QStringList files;
    try {
        files = findOutputs(dir);
    } catch (const Error& e) {
        warn(tr("Computed spectra"), QString::fromStdString(e.what()));
        return 0;
    }
    if (files.isEmpty()) {
        warn(tr("Computed spectra"), tr("No supported quantum-chemistry output under %1").arg(dir));
        return 0;
    }
    const int before = count();
    loadFiles(files);
    return count() - before;
}

void ComputedPanel::addCalculation(const Calculation& c, bool checked) {
    m_entries.push_back({c, Scaling{}, kNaN});
    auto* it = new QListWidgetItem;
    it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
    const bool was = m_updating;
    m_updating = true;
    it->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    m_list->addItem(it);
    updateItemText(count() - 1);
    m_updating = was;
    if (m_list->currentRow() < 0) m_list->setCurrentRow(0);
    emit calculationsChanged();
    if (!m_updating) redraw();
}

void ComputedPanel::updateItemText(int row) {
    if (row < 0 || row >= count()) return;
    const Entry& e = m_entries[std::size_t(row)];
    QString text = e.calc.name();
    if (std::isfinite(e.score)) text = QString("r %1 · ").arg(e.score, 0, 'f', 3) + text;
    if (!e.calc.warnings.isEmpty()) text = QStringLiteral("⚠ ") + text;
    const bool was = m_updating;
    m_updating = true;
    QListWidgetItem* it = m_list->item(row);
    it->setText(text);
    it->setToolTip("<p>" + e.calc.label().toHtmlEscaped() + "<br>" + e.calc.source.toHtmlEscaped() +
                   (e.calc.warnings.isEmpty() ? QString() : "<br>" + e.calc.warnings.join("<br>").toHtmlEscaped()) +
                   "</p>");
    m_updating = was;
}

int ComputedPanel::currentRow() const { return m_list->currentRow(); }

void ComputedPanel::setCurrentRow(int row) {
    if (row >= 0 && row < count()) m_list->setCurrentRow(row);
}

bool ComputedPanel::isChecked(int row) const {
    return row >= 0 && row < count() && m_list->item(row)->checkState() == Qt::Checked;
}

void ComputedPanel::setChecked(int row, bool on) {
    if (row < 0 || row >= count()) return;
    m_list->item(row)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
}

void ComputedPanel::removeRow(int row) {
    if (row < 0 || row >= count()) return;
    m_updating = true;
    m_entries.erase(m_entries.begin() + row);
    delete m_list->takeItem(row);
    m_updating = false;
    onCurrentChanged();
    emit calculationsChanged();
}

void ComputedPanel::clear() {
    m_updating = true;
    m_entries.clear();
    m_list->clear();
    m_updating = false;
    onCurrentChanged();
    emit calculationsChanged();
}

void ComputedPanel::onCurrentChanged() {
    if (m_updating) return;
    const int r = currentRow();
    m_updating = true;
    const Scaling s = r >= 0 ? m_entries[std::size_t(r)].scaling : Scaling{};
    m_factor->setValue(s.low);
    m_two->setChecked(s.twoRegion());
    m_factorHigh->setValue(s.high);
    if (s.twoRegion()) m_split->setValue(s.split);
    m_updating = false;
    m_fitNote.clear();
    updateInfo();
    redraw();
}

void ComputedPanel::onScalingEdited() {
    m_split->setEnabled(m_two->isChecked());
    m_factorHigh->setEnabled(m_two->isChecked());
    if (m_updating) return;
    const int r = currentRow();
    if (r < 0) return;
    Scaling s;
    s.low = m_factor->value();
    if (m_two->isChecked()) {
        s.high = m_factorHigh->value();
        s.split = m_split->value();
    }
    m_entries[std::size_t(r)].scaling = s;
    m_fitNote.clear();
    redraw();
}

void ComputedPanel::updateInfo() {
    const int r = currentRow();
    m_split->setEnabled(m_two->isChecked());
    m_factorHigh->setEnabled(m_two->isChecked());
    if (r < 0) {
        m_info->setText(tr("<i>Open the output of a frequency calculation with Raman activities.</i>"));
        return;
    }
    const Calculation& c = m_entries[std::size_t(r)].calc;
    const auto b = bands(c, options());
    QString t = "<b>" + c.label().toHtmlEscaped() + "</b><br>";
    t += tr("%1 modes, %2 bands").arg(c.modes.size()).arg(b.size());
    if (c.hasIr()) t += tr(", IR intensities");
    for (const QString& w : c.warnings)
        t += QString("<br><span style='color:%1'>%2</span>").arg(theme::warningText().name(), w.toHtmlEscaped());
    m_info->setText(t);
}

QString ComputedPanel::infoText() const { return m_info->text(); }

// ======================================================================================== experimental
void ComputedPanel::setExperimental(const fileio::DataSpectrum& sp) {
    m_exp = sp;
    m_exp.sorted();
    processExperimental();
    m_plot->autoRange();
    redraw();
}

void ComputedPanel::clearExperimental() {
    m_exp = {};
    processExperimental();
    redraw();
}

void ComputedPanel::setBaselineCorrection(bool on) {
    m_expBaseline = on;
    if (m_baseline->isChecked() != on) m_baseline->setChecked(on);
    processExperimental();
    redraw();
}

void ComputedPanel::processExperimental() {
    m_expX.clear();
    m_expY.clear();
    m_expPeaks.clear();
    if (m_exp.x.size() < 10) return;
    m_expX = m_exp.x;
    m_expY = m_exp.y;
    try {
        if (m_expBaseline) {
            const Vec base = processing::baseline(m_expX, m_expY, "als", 5.0);
            for (std::size_t i = 0; i < m_expY.size(); ++i) m_expY[i] -= base[i];
        }
        m_expPeaks = peaks::pick(m_expX, m_expY, 8.0);
    } catch (const Error&) {
        m_expPeaks.clear();
    }
    const double mx = *std::max_element(m_expY.begin(), m_expY.end());
    if (mx > 0) {
        for (double& v : m_expY) v /= mx;
        for (auto& p : m_expPeaks) p.height /= mx;
    }
    // weak features (< 2 % of the strongest band) are not used for matching
    m_expPeaks.erase(std::remove_if(m_expPeaks.begin(), m_expPeaks.end(),
                                    [](const peaks::Peak& p) { return !(p.height >= 0.02); }),
                     m_expPeaks.end());
}

// ============================================================================================ settings
Options ComputedPanel::options() const {
    Options o;
    o.quantity = m_quantity->currentIndex() == 1 ? Quantity::Activity : Quantity::Intensity;
    o.laserNm = m_laser->value();
    o.temperatureK = m_temp->value();
    const int r = currentRow();
    if (r >= 0) o.scaling = m_entries[std::size_t(r)].scaling;
    return o;
}

void ComputedPanel::setLaser(double nm) { m_laser->setValue(nm); }
void ComputedPanel::setTemperature(double kelvin) { m_temp->setValue(kelvin); }
void ComputedPanel::setQuantity(Quantity q) { m_quantity->setCurrentIndex(q == Quantity::Activity ? 1 : 0); }

Scaling ComputedPanel::scaling(int row) const {
    return row >= 0 && row < count() ? m_entries[std::size_t(row)].scaling : Scaling{};
}

void ComputedPanel::setScaling(int row, const Scaling& s) {
    if (row < 0 || row >= count()) return;
    m_entries[std::size_t(row)].scaling = s;
    if (row == currentRow()) onCurrentChanged();
    else redraw();
}

Broadening ComputedPanel::broadening() const {
    Broadening b;
    b.shape = m_shape->currentIndex() == 1 ? Shape::Gaussian : m_shape->currentIndex() == 2 ? Shape::PseudoVoigt
                                                                                            : Shape::Lorentzian;
    b.fwhm = m_fwhm->value();
    b.eta = m_eta->value();
    return b;
}

void ComputedPanel::setBroadening(const Broadening& b) {
    m_updating = true;
    m_shape->setCurrentIndex(b.shape == Shape::Gaussian ? 1 : b.shape == Shape::PseudoVoigt ? 2 : 0);
    m_fwhm->setValue(b.fwhm);
    m_eta->setValue(b.eta);
    m_updating = false;
    redraw();
}

void ComputedPanel::setTwoRegion(bool on, double split) {
    m_split->setValue(split);
    m_two->setChecked(on);
}

bool ComputedPanel::twoRegion() const { return m_two->isChecked(); }

void ComputedPanel::setPlotLayout(Layout l) { m_layout->setCurrentIndex(int(l)); }
ComputedPanel::Layout ComputedPanel::plotLayout() const { return Layout(m_layout->currentIndex()); }
void ComputedPanel::setSticksVisible(bool on) { m_sticks->setChecked(on); }

// ============================================================================================ analysis
bool ComputedPanel::fitScaling(QString* error) {
    auto fail = [&](const QString& m) {
        if (error) *error = m;
        return false;
    };
    const int r = currentRow();
    if (r < 0) return fail(tr("No structure loaded."));
    if (!hasExperimental()) return fail(tr("No experimental spectrum to fit to."));
    if (m_expPeaks.empty()) return fail(tr("No peaks found in the experimental spectrum."));
    FitOptions fo;
    fo.twoRegion = m_two->isChecked();
    fo.split = m_split->value();
    fo.tolerance = m_tolerance->value();
    fo.matching = m_matching->currentIndex() == 1 ? Matching::Nearest : Matching::Hungarian;
    Options unit = options();
    unit.scaling = Scaling{};
    Vec pos;
    for (const auto& p : m_expPeaks) pos.push_back(p.position);
    try {
        const ScaleFit f = computed::fitScaling(bands(m_entries[std::size_t(r)].calc, unit), pos, fo);
        m_entries[std::size_t(r)].scaling = f.scaling;
        onCurrentChanged();
        m_fitNote = tr("fitted: %1 · %2 bands matched · rms %3 cm⁻¹")
                        .arg(f.scaling.text())
                        .arg(f.matched)
                        .arg(f.rms, 0, 'f', 1);
        m_fitLabel->setText(m_fitNote);
        return true;
    } catch (const Error& e) {
        return fail(QString::fromStdString(e.what()));
    }
}

std::vector<StructureScore> ComputedPanel::compareStructures(QString* error) {
    if (!hasExperimental() || count() == 0) {
        if (error) *error = tr("Load structures and an experimental spectrum first.");
        return {};
    }
    std::vector<Calculation> calcs;
    for (const Entry& e : m_entries) calcs.push_back(e.calc);
    FitOptions fo;
    fo.twoRegion = m_two->isChecked();
    fo.split = m_split->value();
    fo.tolerance = m_tolerance->value();
    fo.matching = m_matching->currentIndex() == 1 ? Matching::Nearest : Matching::Hungarian;
    std::vector<StructureScore> s;
    try {
        s = compare(calcs, options(), broadening(), m_exp, true, fo);
    } catch (const Error& e) {
        if (error) *error = QString::fromStdString(e.what());
        return {};
    }
    for (const StructureScore& x : s) {
        Entry& e = m_entries[std::size_t(x.index)];
        e.score = x.correlation;
        if (x.matched > 0) e.scaling = x.scaling;
        updateItemText(x.index);
    }
    onCurrentChanged();
    if (!s.empty()) {
        m_fitNote = tr("best match: %1 (r = %2)")
                        .arg(m_entries[std::size_t(s.front().index)].calc.name())
                        .arg(s.front().correlation, 0, 'f', 3);
        m_fitLabel->setText(m_fitNote);
    }
    return s;
}

std::vector<AssignmentRow> ComputedPanel::assignment() const {
    const int r = currentRow();
    if (r < 0) return {};
    const auto b = bands(m_entries[std::size_t(r)].calc, options());
    const Matching m = m_matching->currentIndex() == 1 ? Matching::Nearest : Matching::Hungarian;
    std::vector<peaks::Peak> pk;
    if (hasExperimental()) {
        const double lo = m_expX.front(), hi = m_expX.back();
        std::vector<Band> inside;
        for (const Band& x : b)
            if (x.position >= lo && x.position <= hi) inside.push_back(x);
        return computed::assignmentTable(inside, m_expPeaks, m_tolerance->value(), m, 0.01);
    }
    return computed::assignmentTable(b, pk, m_tolerance->value(), m, 0.01);
}

Vec ComputedPanel::grid() const {
    if (hasExperimental() && m_expX.size() >= 2) {
        const double lo = std::ceil(m_expX.front()), hi = std::floor(m_expX.back());
        if (hi > lo) return makeGrid(lo, hi, 1.0);
    }
    double lo = 1e9, hi = -1e9;
    for (int r = 0; r < count(); ++r) {
        if (!isChecked(r) && r != currentRow()) continue;
        Options o = options();
        o.scaling = m_entries[std::size_t(r)].scaling;
        for (const Band& b : bands(m_entries[std::size_t(r)].calc, o)) {
            lo = std::min(lo, b.position);
            hi = std::max(hi, b.position);
        }
    }
    if (hi < lo) return makeGrid(100, 3500, 1.0);
    return makeGrid(std::max(0.0, std::floor(lo - 150)), std::ceil(hi + 150), 1.0);
}

// ============================================================================================ drawing
void ComputedPanel::redraw() {
    if (m_updating) return;
    m_plot->clear();
    const PlotPalette& pal = m_plot->plotPalette();
    const Layout lay = plotLayout();
    const double sign = lay == Layout::Mirror ? -1.0 : 1.0;
    const double expOff = lay == Layout::Stacked && hasExperimental() ? kStackOffset : 0.0;
    const Vec g = grid();
    const Broadening br = broadening();
    const int cur = currentRow();

    // computed spectra (ticked; the current one always)
    int k = 0;
    QColor curColor;
    for (int r = 0; r < count(); ++r) {
        if (!isChecked(r) && r != cur) continue;
        const Entry& e = m_entries[std::size_t(r)];
        Options o = options();
        o.scaling = e.scaling;
        fileio::DataSpectrum s = spectrum(e.calc, o, br, g, true);
        for (double& v : s.y) v *= sign;
        const QColor col = pal.seriesColor(1 + k++);
        QPen pen(col, r == cur ? 1.8 : 1.2);
        if (r != cur) pen.setStyle(Qt::DashLine);
        QString legend = e.calc.name();
        if (e.scaling.twoRegion())
            legend += QString(" ×%1/%2").arg(e.scaling.low, 0, 'f', 3).arg(e.scaling.high, 0, 'f', 3);
        else if (e.scaling.low != 1.0)
            legend += QString(" ×%1").arg(e.scaling.low, 0, 'f', 4);
        m_plot->addCurve(s.x, s.y, pen, legend);
        if (r == cur) curColor = col;
    }
    // sticks of the current structure
    std::vector<Band> curBands;
    if (cur >= 0) {
        curBands = bands(m_entries[std::size_t(cur)].calc, options());
        if (m_sticks->isChecked() && !curBands.empty()) {
            const double mx = maxValue(curBands);
            Vec x, y;
            for (const Band& b : curBands) {
                if (!g.empty() && (b.position < g.front() || b.position > g.back())) continue;
                x.insert(x.end(), {b.position, b.position, b.position});
                y.insert(y.end(), {0.0, sign * (mx > 0 ? b.value / mx : 0.0), std::nan("")});
            }
            QColor sc = curColor.isValid() ? curColor : pal.accent;
            sc.setAlpha(170);
            Curve* st = m_plot->addCurve(x, y, QPen(sc, 1.4));
            st->setDecimation(false);
            st->setAutoScale(true);
        }
    }
    // experimental spectrum, its peak labels and the assignment connectors
    if (hasExperimental()) {
        Vec y = m_expY;
        for (double& v : y) v += expOff;
        m_plot->addCurve(m_expX, y, QPen(pal.main, 1.3), m_exp.name.isEmpty() ? tr("experimental") : m_exp.name);
        Vec pos;
        for (const auto& p : m_expPeaks) pos.push_back(p.position);
        PeakLabels* labels = m_plot->addPeakLabels();
        labels->setPeaks(pos, m_expX, y, 0);
        if (lay == Layout::Stacked && cur >= 0) {
            Vec x2, y2;
            for (const AssignmentRow& a : assignment()) {
                if (a.band < 0 || a.peak < 0) continue;
                x2.insert(x2.end(), {a.computed, a.experimental, std::nan("")});
                y2.insert(y2.end(), {1.04, expOff - 0.04, std::nan("")});
            }
            if (!x2.empty()) {
                QPen pen(pal.reference, 1.0, Qt::DotLine);
                Curve* c = m_plot->addCurve(x2, y2, pen);
                c->setDecimation(false);
                c->setAutoScale(false);
            }
        }
    }
    if (lay == Layout::Mirror) m_plot->addHLine(0, QPen(pal.reference, 1.0));
    updateTable();
    updateInfo();
    m_fitLabel->setText(m_fitNote.isEmpty() && cur >= 0 ? tr("scaling %1").arg(m_entries[std::size_t(cur)].scaling.text())
                                                        : m_fitNote);
}

void ComputedPanel::updateTable() {
    const auto rows = assignment();
    m_table->setRowCount(int(rows.size()));
    const QColor muted = theme::mutedText();
    int matched = 0;
    double ss = 0;
    for (int i = 0; i < int(rows.size()); ++i) {
        const AssignmentRow& a = rows[std::size_t(i)];
        const QStringList cells = {num(a.computed, 1),     num(a.unscaled, 1),        a.symmetry, num(a.computedRel, 2),
                                   num(a.experimental, 1), num(a.experimentalRel, 2), num(a.delta, 1)};
        for (int c = 0; c < cells.size(); ++c) {
            auto* it = new QTableWidgetItem(cells[c]);
            it->setTextAlignment(Qt::AlignCenter);
            if (a.band < 0 || a.peak < 0) it->setForeground(muted);
            m_table->setItem(i, c, it);
        }
        if (a.band >= 0 && a.peak >= 0) {
            ++matched;
            ss += a.delta * a.delta;
        }
    }
    if (!hasExperimental()) m_tableNote->setText(tr("no experimental spectrum"));
    else if (matched) m_tableNote->setText(tr("%1 matched, rms Δ %2 cm⁻¹").arg(matched).arg(std::sqrt(ss / matched), 0, 'f', 1));
    else m_tableNote->setText(tr("no bands matched"));
}

// ============================================================================================== export
bool ComputedPanel::exportSpectrum(const QString& path, QString* error) {
    const int r = currentRow();
    if (r < 0) {
        if (error) *error = tr("No structure loaded.");
        return false;
    }
    try {
        const fileio::DataSpectrum s = spectrum(m_entries[std::size_t(r)].calc, options(), broadening(), grid(), true);
        const QString suf = QFileInfo(path).suffix().toLower();
        if (suf == "jdx" || suf == "dx") fileio::writeJcamp(path, s);
        else fileio::writeCsv(path, s);
        return true;
    } catch (const Error& e) {
        if (error) *error = QString::fromStdString(e.what());
        return false;
    }
}

bool ComputedPanel::exportAssignment(const QString& path, QString* error) {
    try {
        writeAssignmentCsv(path, assignment());
        return true;
    } catch (const Error& e) {
        if (error) *error = QString::fromStdString(e.what());
        return false;
    }
}

bool ComputedPanel::exportPlot(const QString& path) {
    if (path.endsWith(".svg", Qt::CaseInsensitive)) return m_plot->exportSvg(path, QSize(1200, 700));
    return m_plot->exportPng(path, QSize(1000, 600), 2.0);
}

void ComputedPanel::openFilesDialog() {
    const QStringList p = QFileDialog::getOpenFileNames(this, tr("Open quantum-chemistry output"), QDir::homePath(),
                                                        fileFilter());
    if (!p.isEmpty()) loadFiles(p);
}

void ComputedPanel::openFolderDialog() {
    const QString d = QFileDialog::getExistingDirectory(this, tr("Folder with calculations (e.g. ECCE project)"));
    if (!d.isEmpty()) loadFolder(d);
}

void ComputedPanel::exportDialog(const QString& kind) {
    const int r = currentRow();
    const QString base = r >= 0 ? QFileInfo(m_entries[std::size_t(r)].calc.source).completeBaseName() : "computed";
    QString err;
    bool ok = true;
    if (kind == "csv" || kind == "jdx") {
        const QString p = QFileDialog::getSaveFileName(this, tr("Save computed spectrum"), base + "_computed." + kind,
                                                       kind == "csv" ? "CSV (*.csv)" : "JCAMP-DX (*.jdx *.dx)");
        if (p.isEmpty()) return;
        ok = exportSpectrum(p, &err);
    } else if (kind == "table") {
        const QString p = QFileDialog::getSaveFileName(this, tr("Save assignment"), base + "_assignment.csv", "CSV (*.csv)");
        if (p.isEmpty()) return;
        ok = exportAssignment(p, &err);
    } else {
        const QString p = QFileDialog::getSaveFileName(this, tr("Save plot"), base + "." + kind,
                                                       kind == "svg" ? "SVG (*.svg)" : "PNG (*.png)");
        if (p.isEmpty()) return;
        ok = exportPlot(p);
    }
    if (!ok) warn(tr("Export"), err.isEmpty() ? tr("Could not write the file.") : err);
}

void ComputedPanel::warn(const QString& title, const QString& text) {
    if (m_collecting) return;
    emit message(title, text);
    if (m_messages) QMessageBox::warning(this, title, text);
}

}  // namespace lr::gui
