#include "librarypage.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "core/errors.h"
#include "core/fileio.h"
#include "core/processing.h"
#include "plotwidget.h"
#include "theme.h"

namespace lr::gui {

using library::Entry;
using library::Source;

namespace {

constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kScoreRole = Qt::UserRole + 2;
constexpr char kMethodKey[] = "library/matchMethod";

QColor sourceColor(Source s) {
    switch (s) {
    case Source::Own: return QColor("#2f9e8f");
    case Source::Rruff: return QColor("#4f7cc0");
    case Source::Literature: return QColor("#c9922b");
    case Source::Computed: return QColor("#8e6cc0");
    case Source::Vendor: return QColor("#7b8594");
    }
    return QColor("#7b8594");
}

QIcon dotIcon(const QColor& c) {
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QRectF(1.5, 1.5, 9, 9));
    return QIcon(pm);
}

QString esc(const QString& s) { return s.toHtmlEscaped(); }

QString laserText(double nm) { return nm > 0 ? QStringLiteral("%1 nm").arg(nm, 0, 'g', 6) : QStringLiteral("–"); }

Vec normalisedMax(const Vec& y) {
    double m = 0;
    for (double v : y) m = std::max(m, std::abs(v));
    Vec out = y;
    if (m > 0)
        for (double& v : out) v /= m;
    return out;
}

// Sticks (band list) as one curve: baseline with a vertical line per band.
void sticks(const std::vector<library::PeakEntry>& peaks, Vec& x, Vec& y) {
    x.clear();
    y.clear();
    for (const auto& p : peaks) {
        const double h = p.intensity > 0 ? p.intensity : 1.0;
        x.insert(x.end(), {p.position, p.position, p.position});
        y.insert(y.end(), {0.0, h, 0.0});
    }
}

// Score column: a light bar behind the number.
class ScoreDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        const QVariant v = idx.data(kScoreRole);
        if (v.isValid()) {
            const double s = std::clamp(v.toDouble(), 0.0, 1.0);
            QRectF r = QRectF(opt.rect).adjusted(3, 4, -3, -4);
            r.setWidth(r.width() * s);
            QColor c = theme::actionColor(theme::Action::Measure);
            c.setAlpha(theme::isDark() ? 110 : 60);
            p->save();
            p->setPen(Qt::NoPen);
            p->setBrush(c);
            p->drawRoundedRect(r, 2, 2);
            p->restore();
        }
        QStyledItemDelegate::paint(p, opt, idx);
    }
};

// ---------------------------------------------------------------------------------------------- entry dialog
class EntryDialog : public QDialog {
public:
    EntryDialog(QWidget* parent, const QString& title, const Entry& e, const QStringList& classes, bool hasSpectrum)
        : QDialog(parent) {
        setWindowTitle(title);
        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        m_name = new QLineEdit(e.name);
        m_formula = new QLineEdit(e.formula);
        m_cas = new QLineEdit(e.cas);
        m_cas->setPlaceholderText(QStringLiteral("e.g. 64-17-5"));
        m_class = new QComboBox;
        m_class->setEditable(true);
        m_class->addItems(classes);
        m_class->setCurrentText(e.cls);
        m_tags = new QLineEdit(e.tags.join(", "));
        m_tags->setPlaceholderText(QStringLiteral("comma separated"));
        m_source = new QComboBox;
        for (Source s : library::allSources()) m_source->addItem(dotIcon(sourceColor(s)), library::sourceLabel(s));
        m_source->setCurrentIndex(int(std::find(library::allSources().begin(), library::allSources().end(), e.source) -
                                      library::allSources().begin()));
        m_sourceId = new QLineEdit(e.sourceId);
        m_sourceId->setPlaceholderText(QStringLiteral("database ID, DOI, file …"));
        m_licence = new QLineEdit(e.licence);
        m_licence->setPlaceholderText(QStringLiteral("e.g. own data, CC BY 4.0, for internal use only"));
        m_laser = new QDoubleSpinBox;
        m_laser->setRange(0, 2000);
        m_laser->setDecimals(1);
        m_laser->setSuffix(QStringLiteral(" nm"));
        m_laser->setSpecialValueText(QStringLiteral("unknown / not applicable"));
        m_laser->setValue(e.laserNm);
        m_conditions = new QLineEdit(e.conditions);
        m_operator = new QLineEdit(e.op);
        m_notes = new QPlainTextEdit(e.notes);
        m_notes->setFixedHeight(70);
        m_bands = new QLineEdit;
        QStringList b;
        for (const auto& p : e.peaks) b << QString::number(p.position, 'f', 1);
        m_bands->setText(b.join(", "));
        m_bands->setPlaceholderText(hasSpectrum ? QStringLiteral("picked automatically if empty")
                                                : QStringLiteral("band positions in cm⁻¹, e.g. 459, 314, 218"));
        form->addRow(QStringLiteral("Name"), m_name);
        form->addRow(QStringLiteral("Formula"), m_formula);
        form->addRow(QStringLiteral("CAS"), m_cas);
        form->addRow(QStringLiteral("Class"), m_class);
        form->addRow(QStringLiteral("Tags"), m_tags);
        form->addRow(QStringLiteral("Source"), m_source);
        form->addRow(QStringLiteral("Source ID"), m_sourceId);
        form->addRow(QStringLiteral("Licence"), m_licence);
        form->addRow(QStringLiteral("Laser"), m_laser);
        form->addRow(QStringLiteral("Conditions"), m_conditions);
        form->addRow(QStringLiteral("Operator"), m_operator);
        form->addRow(QStringLiteral("Notes"), m_notes);
        form->addRow(QStringLiteral("Bands"), m_bands);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(bb, &QDialogButtonBox::accepted, this, [this] {
            if (m_name->text().trimmed().isEmpty()) {
                m_name->setFocus();
                return;
            }
            accept();
        });
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
        auto* v = new QVBoxLayout(this);
        v->addLayout(form);
        v->addWidget(bb);
        resize(520, sizeHint().height());
    }

    // Applies the fields to e. Bands: returns false if they cannot be parsed.
    bool apply(Entry& e, bool* bandsChanged) const {
        e.name = m_name->text().trimmed();
        e.formula = m_formula->text().trimmed();
        e.cas = m_cas->text().trimmed();
        e.cls = m_class->currentText().trimmed();
        e.tags.clear();
        for (const QString& t : m_tags->text().split(',', Qt::SkipEmptyParts))
            if (!t.trimmed().isEmpty()) e.tags << t.trimmed();
        e.source = library::allSources()[size_t(std::max(0, m_source->currentIndex()))];
        e.sourceId = m_sourceId->text().trimmed();
        e.licence = m_licence->text().trimmed();
        e.laserNm = m_laser->value();
        e.conditions = m_conditions->text().trimmed();
        e.op = m_operator->text().trimmed();
        e.notes = m_notes->toPlainText().trimmed();
        // bands: keep intensities/labels of unchanged positions
        std::vector<library::PeakEntry> peaks;
        static const QRegularExpression sep(QStringLiteral("[,;\\s]+"));
        for (const QString& t : m_bands->text().split(sep, Qt::SkipEmptyParts)) {
            bool ok = false;
            const double v = t.toDouble(&ok);
            if (!ok) return false;
            library::PeakEntry p{v, 0, 0, {}};
            for (const auto& old : e.peaks)
                if (std::abs(old.position - v) < 0.05) p = old;
            peaks.push_back(p);
        }
        std::sort(peaks.begin(), peaks.end(),
                  [](const library::PeakEntry& a, const library::PeakEntry& b) { return a.position < b.position; });
        bool changed = peaks.size() != e.peaks.size();
        for (size_t i = 0; !changed && i < peaks.size(); ++i) changed = std::abs(peaks[i].position - e.peaks[i].position) > 0.05;
        if (bandsChanged) *bandsChanged = changed;
        e.peaks = std::move(peaks);
        return true;
    }

private:
    QLineEdit *m_name, *m_formula, *m_cas, *m_tags, *m_sourceId, *m_licence, *m_conditions, *m_operator, *m_bands;
    QComboBox *m_class, *m_source;
    QDoubleSpinBox* m_laser;
    QPlainTextEdit* m_notes;
};

QPushButton* menuButton(const QString& text, QMenu* menu) {
    auto* b = new QPushButton(text);
    b->setMenu(menu);
    return b;
}

// Table item sorting case-insensitively by text, or numerically by Qt::UserRole when set.
class SortItem : public QTableWidgetItem {
public:
    using QTableWidgetItem::QTableWidgetItem;
    bool operator<(const QTableWidgetItem& o) const override {
        const QVariant a = data(Qt::UserRole), b = o.data(Qt::UserRole);
        if (a.isValid() && b.isValid()) return a.toDouble() < b.toDouble();
        return text().compare(o.text(), Qt::CaseInsensitive) < 0;
    }
};

}  // namespace

// ================================================================================================ LibraryPage
LibraryPage::LibraryPage(QWidget* parent) : QWidget(parent), m_lib(std::make_unique<library::Library>()) {
    buildUi();
    updateHeader();
    updateFilters();
    fillTable();
    showNothing();
}

LibraryPage::~LibraryPage() = default;

QString LibraryPage::defaultLibraryPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/library.sqlite");
}

void LibraryPage::buildUi() {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(10, 8, 10, 8);
    v->setSpacing(6);
    v->addWidget(buildHeader());

    auto* split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    split->addWidget(buildBrowser());

    auto* right = new QWidget;
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(2);
    m_plotTitle = new QLabel;
    m_plotTitle->setTextFormat(Qt::RichText);
    rv->addWidget(m_plotTitle);
    auto* rsplit = new QSplitter(Qt::Vertical);
    rsplit->setChildrenCollapsible(false);
    m_plot = new PlotWidget;
    m_plot->setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    m_plot->setLabel(Axis::Left, QStringLiteral("Intensity"));
    m_plot->setHeadroom(0.12);
    m_plot->setToolTip(PlotWidget::navigationHelp());
    rsplit->addWidget(m_plot);
    m_tabs = new QTabWidget;
    m_tabs->addTab(buildDetails(), QStringLiteral("Details"));
    m_tabs->addTab(buildMatchPanel(), QStringLiteral("Match"));
    rsplit->addWidget(m_tabs);
    rsplit->setStretchFactor(0, 3);
    rsplit->setStretchFactor(1, 2);
    rsplit->setSizes({420, 300});
    rv->addWidget(rsplit, 1);
    split->addWidget(right);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    split->widget(0)->setMinimumWidth(360);
    split->setSizes({500, 680});
    v->addWidget(split, 1);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int i) {
        if (i == 0) onSelectionChanged();
        else if (m_results->currentRow() >= 0) showHitOverlay(m_results->currentRow());
    });
}

QWidget* LibraryPage::buildHeader() {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    auto* text = new QVBoxLayout;
    text->setSpacing(0);
    m_libName = new QLabel;
    QFont f = m_libName->font();
    f.setPointSizeF(f.pointSizeF() * 1.25);
    f.setBold(true);
    m_libName->setFont(f);
    m_libInfo = new QLabel;
    m_libInfo->setTextFormat(Qt::RichText);
    m_libInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text->addWidget(m_libName);
    text->addWidget(m_libInfo);
    h->addLayout(text, 1);

    auto* libMenu = new QMenu(this);
    libMenu->addAction(QStringLiteral("New library…"), this, &LibraryPage::newLibrary);
    libMenu->addAction(QStringLiteral("Open…"), this, [this] { openLibrary(false); });
    libMenu->addAction(QStringLiteral("Open read-only…"), this, [this] { openLibrary(true); });
    libMenu->addSeparator();
    libMenu->addAction(QStringLiteral("Add demo entries (simulator spectra, standards)"), this, &LibraryPage::addDemo);
    libMenu->addAction(QStringLiteral("Add reference entries (literature band positions, %1 compounds)")
                           .arg(library::referenceCount()),
                       this, &LibraryPage::addReference);
    m_libMenu = menuButton(QStringLiteral("Library"), libMenu);

    auto* imp = new QMenu(this);
    imp->addAction(QStringLiteral("Spectrum files (CSV, TXT, SPC, JCAMP-DX, RRUFF)…"), this, &LibraryPage::importFiles);
    imp->addAction(QStringLiteral("RRUFF folder…"), this, &LibraryPage::importRruff);
    imp->addAction(QStringLiteral("Band list (CSV)…"), this, &LibraryPage::importBandList);
    m_importMenu = menuButton(QStringLiteral("Import"), imp);

    auto* exp = new QMenu(this);
    exp->addAction(QStringLiteral("Selected entries to files…"), this, &LibraryPage::exportToFiles);
    exp->addAction(QStringLiteral("Selected entries to another library…"), this, &LibraryPage::exportToLibrary);
    m_exportMenu = menuButton(QStringLiteral("Export"), exp);

    for (QPushButton* b : {m_libMenu, m_importMenu, m_exportMenu}) h->addWidget(b, 0, Qt::AlignTop);
    return w;
}

QWidget* LibraryPage::buildBrowser() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("Search name, formula, CAS, tags, notes…"));
    m_search->setClearButtonEnabled(true);
    v->addWidget(m_search);
    auto* fl = new QHBoxLayout;
    fl->setSpacing(4);
    m_class = new QComboBox;
    m_source = new QComboBox;
    m_laser = new QComboBox;
    m_source->addItem(QStringLiteral("All sources"));
    for (Source s : library::allSources()) m_source->addItem(dotIcon(sourceColor(s)), library::sourceLabel(s));
    m_laser->addItem(QStringLiteral("Any laser"), 0.0);
    for (double nm : {532.0, 785.0, 1064.0}) m_laser->addItem(QStringLiteral("%1 nm").arg(nm), nm);
    for (QComboBox* c : {m_class, m_source, m_laser}) {
        c->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        fl->addWidget(c);
    }
    m_class->setToolTip(QStringLiteral("Class"));
    m_source->setToolTip(QStringLiteral("Source of the entries"));
    m_laser->setToolTip(QStringLiteral("Laser wavelength (±10 nm; entries without a laser are included)"));
    fl->addStretch();
    v->addLayout(fl);

    m_table = new QTableWidget(0, 4);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Class"), QStringLiteral("Laser"),
                                        QStringLiteral("Source · licence")});
    m_table->setTextElideMode(Qt::ElideRight);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 8);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->sortByColumn(0, Qt::AscendingOrder);
    v->addWidget(m_table, 1);

    auto* bl = new QHBoxLayout;
    m_add = new QPushButton(QStringLiteral("Add…"));
    m_add->setToolTip(QStringLiteral("New entry from a band list you type in (spectra: “From file…”, Import, or "
                                     "“Add to library…” on the Measure and Data pages)"));
    m_addFile = new QPushButton(QStringLiteral("From file…"));
    m_addFile->setToolTip(QStringLiteral("Add spectra from files (CSV, vendor text, SPC, JCAMP-DX) as new entries "
                                         "(as Import ▸ Spectrum files)"));
    m_edit = new QPushButton(QStringLiteral("Edit…"));
    m_delete = new QPushButton(QStringLiteral("Delete"));
    m_count = new QLabel;
    m_count->setStyleSheet(QStringLiteral("color: %1;").arg(theme::mutedText().name()));
    bl->addWidget(m_add);
    bl->addWidget(m_addFile);
    bl->addWidget(m_edit);
    bl->addWidget(m_delete);
    bl->addStretch();
    bl->addWidget(m_count);
    v->addLayout(bl);

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(150);
    connect(m_searchTimer, &QTimer::timeout, this, &LibraryPage::fillTable);
    connect(m_search, &QLineEdit::textChanged, m_searchTimer, qOverload<>(&QTimer::start));
    for (QComboBox* c : {m_class, m_source, m_laser})
        connect(c, qOverload<int>(&QComboBox::currentIndexChanged), this, &LibraryPage::fillTable);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &LibraryPage::onSelectionChanged);
    connect(m_table, &QTableWidget::itemDoubleClicked, this, [this] { editEntry(); });
    connect(m_add, &QPushButton::clicked, this, &LibraryPage::addEntry);
    connect(m_addFile, &QPushButton::clicked, this, &LibraryPage::importFiles);
    connect(m_edit, &QPushButton::clicked, this, &LibraryPage::editEntry);
    connect(m_delete, &QPushButton::clicked, this, &LibraryPage::deleteEntries);
    return w;
}

QWidget* LibraryPage::buildDetails() {
    m_details = new QLabel;
    m_details->setTextFormat(Qt::RichText);
    m_details->setWordWrap(true);
    m_details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_details->setContentsMargins(8, 6, 8, 6);
    auto* scroll = new QScrollArea;
    scroll->setWidget(m_details);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

QWidget* LibraryPage::buildMatchPanel() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(5);

    auto* ql = new QHBoxLayout;
    m_queryLabel = new QLabel;
    m_queryLabel->setTextFormat(Qt::RichText);
    auto* load = new QPushButton(QStringLiteral("Load query…"));
    load->setToolTip(QStringLiteral("Use a spectrum file as the query"));
    connect(load, &QPushButton::clicked, this, &LibraryPage::loadQueryFile);
    ql->addWidget(m_queryLabel, 1);
    ql->addWidget(load);
    v->addLayout(ql);

    auto* ol = new QHBoxLayout;
    ol->setSpacing(6);
    m_method = new QComboBox;
    m_method->addItems({QStringLiteral("Full spectrum"), QStringLiteral("Peaks"), QStringLiteral("Mixture")});
    m_method->setItemData(0, QStringLiteral("Hit quality index (correlation) of the preprocessed spectra"), Qt::ToolTipRole);
    m_method->setItemData(1, QStringLiteral("Peak positions against the entries' peak and band lists"), Qt::ToolTipRole);
    m_method->setItemData(2, QStringLiteral("Non-negative least squares with 1–3 of the best full-spectrum hits"),
                          Qt::ToolTipRole);
    m_baseline = new QComboBox;
    for (const auto& [key, desc] : processing::baselines())
        if (key != "polynomial") m_baseline->addItem(key == "none" ? QStringLiteral("no baseline") : key.toUpper(), key);
    m_baseline->setCurrentIndex(std::max(0, m_baseline->findData(QStringLiteral("arpls"))));
    m_baseline->setToolTip(QStringLiteral("Baseline removed from query and library spectra before comparing"));
    m_deriv = new QComboBox;
    m_deriv->addItems({QStringLiteral("no derivative"), QStringLiteral("1st derivative"), QStringLiteral("2nd derivative")});
    m_tol = new QDoubleSpinBox;
    m_tol->setRange(1, 50);
    m_tol->setValue(8);
    m_tol->setSuffix(QStringLiteral(" cm⁻¹"));
    m_tol->setPrefix(QStringLiteral("± "));
    m_tol->setToolTip(QStringLiteral("Peak position tolerance"));
    m_topN = new QSpinBox;
    m_topN->setRange(1, 200);
    m_topN->setValue(10);
    m_topN->setPrefix(QStringLiteral("top "));
    m_useFilters = new QCheckBox(QStringLiteral("Filters"));
    m_useFilters->setToolTip(QStringLiteral("Only search entries passing the class/source/laser filters of the list"));
    m_matchButton = new QPushButton(QStringLiteral("Match"));
    m_matchButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: white; font-weight: bold; border-radius: 4px; padding: 4px 16px; }"
        "QPushButton:pressed { background: %2; } QPushButton:disabled { background: %3; }")
                                     .arg(theme::actionColor(theme::Action::Measure).name(),
                                          theme::actionPressedColor(theme::Action::Measure).name(),
                                          theme::disabledButton().name()));
    for (QComboBox* c : {m_method, m_baseline, m_deriv}) {
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(7);
    }
    m_method->setMinimumContentsLength(11);
    m_deriv->setMinimumContentsLength(12);
    for (QWidget* c : std::initializer_list<QWidget*>{m_method, m_baseline, m_deriv, m_tol, m_topN, m_useFilters})
        ol->addWidget(c);
    ol->addStretch();
    ol->addWidget(m_matchButton);
    v->addLayout(ol);

    m_results = new QTableWidget(0, 5);
    m_results->setHorizontalHeaderLabels({QStringLiteral("#"), QStringLiteral("Name"), QStringLiteral("Score"),
                                          QStringLiteral("Source"), QStringLiteral("Licence")});
    m_results->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_results->verticalHeader()->hide();
    m_results->verticalHeader()->setDefaultSectionSize(m_results->fontMetrics().height() + 8);
    m_results->horizontalHeader()->setStretchLastSection(true);
    m_results->horizontalHeader()->setHighlightSections(false);
    m_results->setItemDelegateForColumn(2, new ScoreDelegate(m_results));
    m_results->setWordWrap(false);
    v->addWidget(m_results, 1);
    m_resultInfo = new QLabel;
    m_resultInfo->setStyleSheet(QStringLiteral("color: %1;").arg(theme::mutedText().name()));
    m_resultInfo->setWordWrap(true);
    v->addWidget(m_resultInfo);

    auto methodChanged = [this] {
        const auto m = method();
        m_tol->setVisible(m == Method::Peaks);
        m_baseline->setEnabled(m != Method::Peaks);
        m_deriv->setEnabled(m != Method::Peaks);
    };
    // Peaks is the default: it covers every entry, including the literature band lists
    m_method->setCurrentIndex(std::clamp(QSettings().value(QLatin1String(kMethodKey), int(Method::Peaks)).toInt(), 0, 2));
    connect(m_method, qOverload<int>(&QComboBox::currentIndexChanged), this, methodChanged);
    connect(m_method, qOverload<int>(&QComboBox::activated), this,
            [](int i) { QSettings().setValue(QLatin1String(kMethodKey), i); });
    methodChanged();
    connect(m_matchButton, &QPushButton::clicked, this, [this] { runMatch(); });
    connect(m_results, &QTableWidget::itemSelectionChanged, this, [this] {
        if (m_results->currentRow() >= 0) showHitOverlay(m_results->currentRow());
    });
    connect(m_results, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* it) {
        const qint64 id = m_results->item(it->row(), 0)->data(kIdRole).toLongLong();
        if (id > 0) {
            selectEntry(id);
            showDetailsTab();
        }
    });
    return w;
}

// ------------------------------------------------------------------------------------------------ state
QString LibraryPage::libraryPath() const { return m_lib->isOpen() ? m_lib->path() : QString(); }

bool LibraryPage::setLibraryPath(const QString& path, bool readOnly) {
    try {
        m_lib->open(path, readOnly ? library::OpenMode::ReadOnly : library::OpenMode::ReadWrite, !readOnly);
    } catch (const Error& e) {
        const QString msg = QString::fromUtf8(e.what());
        updateHeader(msg);
        fillTable();
        emit message(msg);
        return false;
    }
    // the default library carries the shipped reference entries (literature band lists), new ones included
    if (!readOnly && QFileInfo(path) == QFileInfo(defaultLibraryPath()) &&
        m_lib->info(QStringLiteral("reference_version")).toInt() < library::referenceVersion()) {
        try {
            if (const int n = library::addReferenceEntries(*m_lib))
                emit message(QStringLiteral("Added %1 reference entries (literature band positions) to the library").arg(n));
        } catch (const Error& e) {
            emit message(QStringLiteral("Reference entries not added: %1").arg(QString::fromUtf8(e.what())));
        }
    }
    m_matcherDirty = true;
    m_hits.clear();
    m_mixtures.clear();
    fillResults();
    updateHeader();
    updateFilters();
    fillTable();
    emit libraryChanged(m_lib->path());
    return true;
}

void LibraryPage::updateHeader(const QString& error) {
    const QString muted = theme::mutedText().name();
    if (!m_lib->isOpen()) {
        m_libName->setText(QStringLiteral("No library open"));
        m_libInfo->setText(error.isEmpty()
                               ? QStringLiteral("<span style='color:%1'>Create or open a library file "
                                                "(Library ▸ New / Open). It can be shared on a network drive.</span>")
                                     .arg(muted)
                               : QStringLiteral("<span style='color:%1'>%2</span>")
                                     .arg(theme::warningText().name(), esc(error)));
    } else {
        const QFileInfo fi(m_lib->path());
        m_libName->setText(fi.completeBaseName());
        QStringList badges;
        auto badge = [](const QString& text, const QString& color) {
            return QStringLiteral("<span style='background:%1; color:white;'>&nbsp;%2&nbsp;</span>").arg(color, text);
        };
        if (m_lib->readOnly()) badges << badge(QStringLiteral("read-only"), QStringLiteral("#7b8594"));
        if (m_lib->onNetwork()) badges << badge(QStringLiteral("network"), QStringLiteral("#4f7cc0"));
        QString info = QStringLiteral("<span style='color:%1'>%2</span>")
                           .arg(muted, esc(QDir::toNativeSeparators(m_lib->path())));
        if (!badges.isEmpty()) info += QStringLiteral("&nbsp;&nbsp;") + badges.join(QStringLiteral("&nbsp;"));
        if (!error.isEmpty())
            info += QStringLiteral("<br><span style='color:%1'>%2</span>").arg(theme::warningText().name(), esc(error));
        m_libInfo->setText(info);
    }
    const bool writable = m_lib->isOpen() && !m_lib->readOnly();
    m_importMenu->setEnabled(writable);
    m_exportMenu->setEnabled(m_lib->isOpen());
    m_add->setEnabled(writable);
    m_addFile->setEnabled(writable);
    m_edit->setEnabled(writable);
    m_delete->setEnabled(writable);
    m_libMenu->menu()->actions().last()->setEnabled(writable);
}

void LibraryPage::updateFilters() {
    const QString cur = m_class->currentIndex() > 0 ? m_class->currentText() : QString();
    QSignalBlocker b(m_class);
    m_class->clear();
    m_class->addItem(QStringLiteral("All classes"));
    if (m_lib->isOpen()) m_class->addItems(m_lib->classes());
    const int i = cur.isEmpty() ? 0 : m_class->findText(cur);
    m_class->setCurrentIndex(std::max(0, i));
}

library::Filter LibraryPage::currentFilter(bool withText) const {
    library::Filter f;
    if (withText) f.text = m_search->text().trimmed();
    if (m_class->currentIndex() > 0) f.cls = m_class->currentText();
    if (m_source->currentIndex() > 0) f.source = library::allSources()[size_t(m_source->currentIndex() - 1)];
    f.laserNm = m_laser->currentData().toDouble();
    return f;
}

void LibraryPage::fillTable() {
    const qint64 keep = selectedEntry();
    QSignalBlocker block(m_table);
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);
    if (!m_lib->isOpen()) {
        m_count->clear();
        m_table->setSortingEnabled(true);
        return;
    }
    std::vector<Entry> entries;
    try {
        entries = m_lib->find(currentFilter(true));
    } catch (const Error& e) {
        updateHeader(QString::fromUtf8(e.what()));
    }
    m_table->setRowCount(int(entries.size()));
    for (int r = 0; r < int(entries.size()); ++r) {
        const Entry& e = entries[size_t(r)];
        auto* name = new SortItem(e.name);
        name->setData(kIdRole, e.id);
        QString tip = e.name;
        if (!e.formula.isEmpty()) tip += QStringLiteral("  ") + e.formula;
        if (e.spectrumCount == 0) tip += QStringLiteral("\nband list: %1 bands").arg(e.peakCount);
        name->setToolTip(tip);
        auto* laser = new SortItem(e.laserNm > 0 ? QStringLiteral("%1").arg(e.laserNm, 0, 'g', 6) : QStringLiteral("–"));
        laser->setData(Qt::UserRole, e.laserNm);
        laser->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        const QString lic = e.licence.isEmpty() ? QStringLiteral("licence not stated") : e.licence;
        auto* src = new SortItem(dotIcon(sourceColor(e.source)),
                                 QStringLiteral("%1 · %2").arg(library::sourceLabel(e.source), lic));
        src->setToolTip(QStringLiteral("Source: %1%2\nLicence: %3")
                            .arg(library::sourceLabel(e.source),
                                 e.sourceId.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(e.sourceId), lic));
        if (e.licence.isEmpty()) src->setForeground(theme::warningText());
        auto* cls = new SortItem(e.cls);
        if (e.spectrumCount == 0) {
            QFont f = name->font();
            f.setItalic(true);
            name->setFont(f);
        }
        m_table->setItem(r, 0, name);
        m_table->setItem(r, 1, cls);
        m_table->setItem(r, 2, laser);
        m_table->setItem(r, 3, src);
    }
    m_table->setSortingEnabled(true);
    if (!m_columnsSized && !entries.empty()) {       // once; afterwards the user's widths are kept
        m_columnsSized = true;
        m_table->resizeColumnsToContents();
        const int caps[] = {180, 120, 50};
        for (int c = 0; c < 3; ++c) m_table->setColumnWidth(c, std::min(m_table->columnWidth(c) + 6, caps[c]));
    }
    const int total = m_lib->count();
    m_count->setText(int(entries.size()) == total ? QStringLiteral("%1 entries").arg(total)
                                                  : QStringLiteral("%1 of %2 entries").arg(entries.size()).arg(total));
    block.unblock();
    if (keep > 0) selectEntry(keep);
    else onSelectionChanged();
}

int LibraryPage::visibleEntries() const { return m_table->rowCount(); }

void LibraryPage::setSearchText(const QString& text) {
    m_search->setText(text);
    m_searchTimer->stop();
    fillTable();
}

std::vector<qint64> LibraryPage::selectedIds() const {
    std::vector<qint64> ids;
    for (const QModelIndex& i : m_table->selectionModel()->selectedRows(0))
        ids.push_back(i.data(kIdRole).toLongLong());
    return ids;
}

qint64 LibraryPage::selectedEntry() const {
    const int r = m_table->currentRow();
    if (r < 0 || !m_table->item(r, 0) || !m_table->item(r, 0)->isSelected()) {
        const auto ids = selectedIds();
        return ids.empty() ? 0 : ids.front();
    }
    return m_table->item(r, 0)->data(kIdRole).toLongLong();
}

void LibraryPage::selectEntry(qint64 id) {
    for (int r = 0; r < m_table->rowCount(); ++r)
        if (m_table->item(r, 0)->data(kIdRole).toLongLong() == id) {
            m_table->selectRow(r);
            m_table->scrollToItem(m_table->item(r, 0));
            onSelectionChanged();
            return;
        }
}

void LibraryPage::showMatchTab() { m_tabs->setCurrentIndex(1); }
void LibraryPage::showDetailsTab() { m_tabs->setCurrentIndex(0); }

void LibraryPage::refresh() {
    if (!m_lib->isOpen()) return;
    m_matcherDirty = true;
    updateFilters();
    fillTable();
}

void LibraryPage::onSelectionChanged() {
    const qint64 id = selectedEntry();
    if (id > 0) showEntry(id);
    else showNothing();
}

// ------------------------------------------------------------------------------------------------ display
void LibraryPage::showNothing() {
    m_plot->clear();
    m_plotTitle->setText(QStringLiteral("<span style='color:%1'>Select an entry to see its spectrum</span>")
                             .arg(theme::mutedText().name()));
    m_details->setText(QStringLiteral("<p style='color:%1'>No entry selected.</p>").arg(theme::mutedText().name()));
}

void LibraryPage::showEntry(qint64 id) {
    std::optional<Entry> e;
    try {
        e = m_lib->get(id, true);
    } catch (const Error& err) {
        emit message(QString::fromUtf8(err.what()));
    }
    if (!e) {
        showNothing();
        return;
    }
    const QString muted = theme::mutedText().name();
    auto row = [&](const QString& k, const QString& v) {
        return QStringLiteral("<tr><td style='color:%1; padding-right:12px; white-space:nowrap'>%2</td><td>%3</td></tr>")
            .arg(muted, k, v);
    };
    const QColor sc = sourceColor(e->source);
    QString html = QStringLiteral("<table cellspacing='0' cellpadding='2'>");
    html += row(QStringLiteral("Source"),
                QStringLiteral("<span style='color:%1; font-weight:bold'>●</span> <b>%2</b>%3")
                    .arg(sc.name(), library::sourceLabel(e->source),
                         e->sourceId.isEmpty() ? QString() : QStringLiteral(" &nbsp;%1").arg(esc(e->sourceId))));
    html += row(QStringLiteral("Licence"), e->licence.isEmpty()
                                               ? QStringLiteral("<span style='color:%1'>not stated</span>")
                                                     .arg(theme::warningText().name())
                                               : esc(e->licence));
    if (!e->formula.isEmpty()) html += row(QStringLiteral("Formula"), esc(e->formula));
    if (!e->cas.isEmpty()) html += row(QStringLiteral("CAS"), esc(e->cas));
    if (!e->cls.isEmpty()) html += row(QStringLiteral("Class"), esc(e->cls));
    if (!e->tags.isEmpty()) html += row(QStringLiteral("Tags"), esc(e->tags.join(", ")));
    html += row(QStringLiteral("Laser"), laserText(e->laserNm));
    if (!e->conditions.isEmpty()) html += row(QStringLiteral("Conditions"), esc(e->conditions));
    if (!e->op.isEmpty()) html += row(QStringLiteral("Operator"), esc(e->op));
    QStringList data;
    for (const auto& s : e->spectra)
        data << QStringLiteral("%1 (%2 points, %3–%4 cm⁻¹)%5")
                    .arg(esc(s.label.isEmpty() ? QStringLiteral("spectrum") : s.label))
                    .arg(s.x.size())
                    .arg(s.x.empty() ? 0.0 : s.x.front(), 0, 'f', 0)
                    .arg(s.x.empty() ? 0.0 : s.x.back(), 0, 'f', 0)
                    .arg(s.processed ? QStringLiteral(", processed") : QString());
    if (!e->peaks.empty()) {
        QStringList p;
        for (const auto& k : e->peaks) p << QString::number(k.position, 'f', 0);
        data << QStringLiteral("%1 bands: %2").arg(e->peaks.size()).arg(p.join(", "));
    }
    html += row(QStringLiteral("Data"), data.join(QStringLiteral("<br>")));
    if (!e->notes.isEmpty()) html += row(QStringLiteral("Notes"), esc(e->notes).replace('\n', QStringLiteral("<br>")));
    html += row(QStringLiteral("Added"), e->created.toString(QStringLiteral("yyyy-MM-dd HH:mm")) +
                                             (e->modified.isValid() && e->modified.secsTo(e->created) < -60
                                                  ? QStringLiteral(" &nbsp;(edited %1)")
                                                        .arg(e->modified.toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                                                  : QString()));
    if (!e->provenance.isEmpty()) {
        QStringList keys = e->provenance.keys();
        html += row(QStringLiteral("Provenance"),
                    QStringLiteral("<span style='color:%1'>%2</span>").arg(muted, esc(keys.join(", "))));
    }
    html += QStringLiteral("</table>");
    m_details->setText(html);
    if (!e->provenance.isEmpty())
        m_details->setToolTip(QString::fromUtf8(QJsonDocument(e->provenance).toJson(QJsonDocument::Indented)).left(3000));
    plotEntry(*e);
}

void LibraryPage::plotEntry(const Entry& e) {
    m_plot->clear();
    m_plot->clearHomeXRange();
    m_plot->setLegendVisible(false);
    const PlotPalette& pal = m_plot->plotPalette();
    QString title = QStringLiteral("<b>%1</b>").arg(esc(e.name));
    if (!e.formula.isEmpty()) title += QStringLiteral(" &nbsp;%1").arg(esc(e.formula));
    title += QStringLiteral(" &nbsp;<span style='color:%1'>●</span> <span style='color:%2'>%3</span>")
                 .arg(sourceColor(e.source).name(), theme::mutedText().name(), library::sourceLabel(e.source));
    m_plotTitle->setText(title);
    if (const library::SpectrumData* s = e.preferredSpectrum()) {
        m_plot->setLabel(Axis::Left, s->processed ? QStringLiteral("Intensity (processed)") : QStringLiteral("Intensity"));
        m_plot->addCurve(s->x, s->y, QPen(pal.main, 1.3));
        if (!e.peaks.empty()) {
            std::vector<library::PeakEntry> top = e.peaks;
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.intensity > b.intensity; });
            if (top.size() > 12) top.resize(12);
            Vec pos;
            for (const auto& p : top) pos.push_back(p.position);
            std::sort(pos.begin(), pos.end());
            m_plot->addPeakLabels()->setPeaks(pos, s->x, s->y);
        }
        // start above the laser line / notch edge when the data include it (double-click: this view)
        if (s->x.front() < 50 && s->x.back() > 400) {
            m_plot->setHomeXRange(50, s->x.back());
            m_plot->autoRange();
            return;
        }
    } else if (!e.peaks.empty()) {
        Vec x, y;
        sticks(e.peaks, x, y);
        m_plot->setLabel(Axis::Left, QStringLiteral("Relative strength"));
        auto* c = m_plot->addCurve(x, y, QPen(sourceColor(e.source), 1.8));
        c->setDecimation(false);
        std::vector<PeakLabel> labels;
        for (const auto& p : e.peaks)
            labels.push_back({p.position, p.intensity > 0 ? p.intensity : 1.0, QString::number(p.position, 'f', 0)});
        m_plot->addPeakLabels()->setLabels(std::move(labels));
        const double lo = e.peaks.front().position, hi = e.peaks.back().position;
        const double pad = std::max(40.0, 0.08 * (hi - lo));
        m_plot->setHomeXRange(lo - pad, hi + pad);
    }
    m_plot->autoRange();
}

// ------------------------------------------------------------------------------------------------ matching
match::Preprocess LibraryPage::currentPreprocess() const {
    match::Preprocess p;
    p.baseline = m_baseline->currentData().toString();
    p.derivative = m_deriv->currentIndex();
    if (p.derivative > 0) p.smoothWindow = 9;
    return p;
}

void LibraryPage::ensureMatcher() {
    const match::Preprocess p = currentPreprocess();
    if (m_matcherDirty) {
        m_matcher = match::Matcher(p);
        m_matcher.load(*m_lib);
        m_matcherDirty = false;
        return;
    }
    const match::Preprocess& o = m_matcher.preprocess();
    if (o.baseline != p.baseline || o.derivative != p.derivative || o.smoothWindow != p.smoothWindow)
        m_matcher.setPreprocess(p);
}

void LibraryPage::setMethod(Method m) { m_method->setCurrentIndex(int(m)); }

LibraryPage::Method LibraryPage::method() const { return Method(std::clamp(m_method->currentIndex(), 0, 2)); }

void LibraryPage::setQuery(const Vec& x, const Vec& y, const QString& name) {
    m_qx = x;
    m_qy = y;
    m_qname = name.isEmpty() ? QStringLiteral("spectrum") : name;
    const double lo = x.empty() ? 0 : *std::min_element(x.begin(), x.end());
    const double hi = x.empty() ? 0 : *std::max_element(x.begin(), x.end());
    m_queryLabel->setText(QStringLiteral("Query: <b>%1</b> <span style='color:%2'>(%3 points, %4–%5 cm⁻¹)</span>")
                              .arg(esc(m_qname), theme::mutedText().name())
                              .arg(x.size())
                              .arg(lo, 0, 'f', 0)
                              .arg(hi, 0, 'f', 0));
    m_matchButton->setEnabled(true);
}

std::vector<match::Hit> LibraryPage::match(const Spectrum& sp) {
    QString name = sp.metaValue(QStringLiteral("sample"));
    if (name.isEmpty()) name = QStringLiteral("last spectrum");
    name += QStringLiteral(", ") + sp.t.toString(QStringLiteral("HH:mm:ss"));
    setQuery(sp.shift, sp.intensity, name);
    showMatchTab();
    return runMatch();
}

std::vector<match::Hit> LibraryPage::runMatch() {
    m_hits.clear();
    m_mixtures.clear();
    if (m_qx.size() < 16) {
        m_resultInfo->setText(QStringLiteral("No query: use “Match last spectrum” on the Measure page or Load query…"));
        fillResults();
        emit matched({}, 0);
        return {};
    }
    if (!ensureLibrary()) return {};
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        ensureMatcher();
        m_hitsMethod = method();
        const library::Filter f = m_useFilters->isChecked() ? currentFilter(false) : library::Filter{};
        const int n = m_topN->value();
        if (m_hitsMethod == Method::Spectrum) {
            m_hits = m_matcher.search(m_qx, m_qy, n, f);
        } else if (m_hitsMethod == Method::Peaks) {
            match::PeakOptions o;
            o.tolerance = m_tol->value();
            m_hits = m_matcher.searchPeaks(m_qx, m_qy, o, n, f);
        } else {
            m_mixtures = m_matcher.searchMixtures(m_qx, m_qy, 8, 3, n, f);
        }
    } catch (const Error& e) {
        m_resultInfo->setText(QString::fromUtf8(e.what()));
    }
    QApplication::restoreOverrideCursor();
    fillResults();
    if (m_results->rowCount() > 0) selectHit(0);
    QString best;
    double score = 0;
    if (!m_hits.empty()) {
        best = m_hits[0].name;
        score = m_hits[0].score;
    } else if (!m_mixtures.empty()) {
        QStringList names;
        for (const auto& c : m_mixtures[0].components) names << c.name;
        best = names.join(QStringLiteral(" + "));
        score = m_mixtures[0].hqi;
    }
    emit matched(best, score);
    return m_hits;
}

void LibraryPage::fillResults() {
    QSignalBlocker b(m_results);
    m_results->setRowCount(0);
    const bool mix = !m_mixtures.empty();
    m_results->setHorizontalHeaderLabels({QStringLiteral("#"), mix ? QStringLiteral("Components") : QStringLiteral("Name"),
                                          mix || m_hitsMethod == Method::Spectrum ? QStringLiteral("HQI")
                                                                                  : QStringLiteral("Score"),
                                          mix ? QStringLiteral("Fractions") : QStringLiteral("Source"),
                                          mix ? QStringLiteral("Residual") : QStringLiteral("Licence")});
    auto scoreItem = [](double s) {
        auto* it = new QTableWidgetItem(QString::number(100 * s, 'f', 1));
        it->setData(kScoreRole, s);
        it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return it;
    };
    int r = 0;
    for (const match::Hit& h : m_hits) {
        m_results->insertRow(r);
        auto* num = new QTableWidgetItem(QString::number(r + 1));
        num->setData(kIdRole, h.id);
        num->setTextAlignment(Qt::AlignCenter);
        auto* name = new QTableWidgetItem(h.bandList ? h.name + QStringLiteral(" (band list)") : h.name);
        QString tip = h.name;
        if (h.bandList)
            tip += QStringLiteral("\ncompared with a spectrum drawn from the literature band list (%1 cm⁻¹ wide bands, "
                                  "taken from the query)")
                       .arg(m_matcher.bandWidth(), 0, 'f', 0);
        if (h.matched >= 0)
            tip += QStringLiteral("\n%1 of %2 library bands matched (%3 query peaks)")
                       .arg(h.matched)
                       .arg(h.libraryPeaks)
                       .arg(h.queryPeaks);
        tip += QStringLiteral("\ncompared %1–%2 cm⁻¹").arg(h.lo, 0, 'f', 0).arg(h.hi, 0, 'f', 0);
        name->setToolTip(tip);
        auto* src = new QTableWidgetItem(dotIcon(sourceColor(h.source)), library::sourceLabel(h.source));
        auto* lic = new QTableWidgetItem(h.licence.isEmpty() ? QStringLiteral("not stated") : h.licence);
        lic->setToolTip(h.licence);
        m_results->setItem(r, 0, num);
        m_results->setItem(r, 1, name);
        m_results->setItem(r, 2, scoreItem(h.score));
        m_results->setItem(r, 3, src);
        m_results->setItem(r, 4, lic);
        ++r;
    }
    for (const match::Mixture& m : m_mixtures) {
        m_results->insertRow(r);
        auto* num = new QTableWidgetItem(QString::number(r + 1));
        num->setTextAlignment(Qt::AlignCenter);
        QStringList names, fr;
        for (const auto& c : m.components) {
            names << c.name;
            fr << QStringLiteral("%1 %").arg(100 * c.fraction, 0, 'f', 0);
        }
        m_results->setItem(r, 0, num);
        m_results->setItem(r, 1, new QTableWidgetItem(names.join(QStringLiteral(" + "))));
        m_results->setItem(r, 2, scoreItem(m.hqi));
        m_results->setItem(r, 3, new QTableWidgetItem(fr.join(QStringLiteral(" / "))));
        m_results->setItem(r, 4, new QTableWidgetItem(QString::number(m.residual, 'f', 3)));
        ++r;
    }
    m_results->resizeColumnsToContents();
    m_results->setColumnWidth(0, 34);
    m_results->setColumnWidth(1, std::clamp(m_results->columnWidth(1) + 10, 140, 300));
    m_results->setColumnWidth(2, 70);
    if (m_qx.empty()) {
        m_queryLabel->setText(QStringLiteral("<span style='color:%1'>No query yet: “Match last spectrum” on the Measure "
                                             "page, or load a file.</span>")
                                  .arg(theme::mutedText().name()));
        m_matchButton->setEnabled(false);
    }
    if (!m_qx.empty() && m_hits.empty() && m_mixtures.empty() && m_resultInfo->text().isEmpty())
        m_resultInfo->setText(QStringLiteral("No results."));
    else if (!m_hits.empty() || !m_mixtures.empty()) {
        QString info = m_hitsMethod == Method::Spectrum
                           ? QStringLiteral("HQI = squared correlation after resampling, %1 baseline%2; 100 = identical "
                                            "shape.")
                                 .arg(m_matcher.preprocess().baseline.toUpper(),
                                      m_matcher.preprocess().derivative > 0 ? QStringLiteral(", derivative") : QString())
                           : m_hitsMethod == Method::Peaks
                                 ? QStringLiteral("Score from peak positions within ±%1 cm⁻¹ (100 = all peaks match).")
                                       .arg(m_tol->value())
                                 : QStringLiteral("Non-negative least squares on the top hits; fractions refer to "
                                                  "normalised reference spectra.");
        m_resultInfo->setText(info + QStringLiteral(" Searched %1 entries.").arg(m_matcher.size()));
    }
}

void LibraryPage::selectHit(int row) {
    if (row < 0 || row >= m_results->rowCount()) return;
    m_results->selectRow(row);
    showHitOverlay(row);
}

void LibraryPage::showHitOverlay(int row) {
    const PlotPalette& pal = m_plot->plotPalette();
    m_plot->clear();
    m_plot->clearHomeXRange();
    m_plot->setLegendVisible(true);
    m_plot->setLabel(Axis::Left, QStringLiteral("Normalised intensity"));
    const QString muted = theme::mutedText().name();
    if (row < int(m_mixtures.size()) && m_hits.empty()) {
        const match::Mixture& m = m_mixtures[size_t(row)];
        m_plot->addCurve(m.x, normalisedMax(m.query), QPen(pal.main, 1.4), QStringLiteral("Query"));
        double qm = 0;
        for (double v : m.query) qm = std::max(qm, std::abs(v));
        auto scaled = [&](const Vec& v) {
            Vec o = v;
            if (qm > 0)
                for (double& a : o) a /= qm;
            return o;
        };
        QPen fp(pal.reference, 1.4, Qt::DashLine);
        m_plot->addCurve(m.x, scaled(m.fit), fp, QStringLiteral("Fit (HQI %1)").arg(100 * m.hqi, 0, 'f', 1));
        for (size_t k = 0; k < m.parts.size(); ++k) {
            auto* c = m_plot->addCurve(m.x, scaled(m.parts[k]), QPen(pal.seriesColor(int(k) + 1), 1.1),
                                       QStringLiteral("%1 (%2 %)").arg(m.components[k].name)
                                           .arg(100 * m.components[k].fraction, 0, 'f', 0));
            c->setOpacity(0.85);
        }
        QStringList names;
        for (const auto& c : m.components) names << esc(c.name);
        m_plotTitle->setText(QStringLiteral("<b>%1</b> &nbsp;<span style='color:%2'>fitted to %3</span>")
                                 .arg(names.join(QStringLiteral(" + ")), muted, esc(m_qname)));
        m_plot->autoRange();
        return;
    }
    if (row >= int(m_hits.size())) return;
    const match::Hit& h = m_hits[size_t(row)];
    const match::Prepared q = match::prepare(m_qx, m_qy, m_matcher.preprocess());
    const match::Prepared* ref = m_matcher.prepared(h.id);
    const QColor hitColor = pal.seriesColor(1);
    m_plot->addCurve(q.xs(), normalisedMax(q.y), QPen(pal.main, 1.4), QStringLiteral("Query: %1").arg(m_qname));
    if (m_hitsMethod == Method::Peaks || !ref) {
        auto e = m_lib->get(h.id, true);
        if (e && !e->peaks.empty()) {
            Vec x, y;
            std::vector<library::PeakEntry> pk;
            for (const auto& p : e->peaks)
                if (p.position >= q.xFirst() - 20 && p.position <= q.xLast() + 20) pk.push_back(p);
            sticks(pk, x, y);
            for (double& v : y) v = -0.35 * v;          // drawn downwards, below the query
            auto* c = m_plot->addCurve(x, y, QPen(hitColor, 1.6), QStringLiteral("%1 bands").arg(h.name));
            c->setDecimation(false);
        }
    } else {
        Vec rx = ref->xs(), ry = normalisedMax(ref->y);
        Vec cx, cy;
        for (size_t i = 0; i < rx.size(); ++i)
            if (rx[i] >= h.lo && rx[i] <= h.hi) {
                cx.push_back(rx[i]);
                cy.push_back(ry[i]);
            }
        m_plot->addCurve(cx, normalisedMax(cy), QPen(hitColor, 1.2), h.bandList ? QStringLiteral("Hit: %1 (drawn from band list)").arg(h.name) : QStringLiteral("Hit: %1").arg(h.name));
    }
    m_plotTitle->setText(QStringLiteral("<b>%1</b> &nbsp;<span style='color:%2'>%3 %4 &nbsp;vs. query %5</span>")
                             .arg(esc(h.name), muted,
                                  m_hitsMethod == Method::Spectrum ? QStringLiteral("HQI") : QStringLiteral("score"),
                                  QString::number(100 * h.score, 'f', 1), esc(m_qname)));
    m_plot->autoRange();
}

// ------------------------------------------------------------------------------------------------ actions
bool LibraryPage::ensureLibrary() {
    if (m_lib->isOpen()) return true;
    return setLibraryPath(defaultLibraryPath());
}

bool LibraryPage::requireWritable() {
    if (!ensureLibrary()) return false;
    if (m_lib->readOnly()) {
        QMessageBox::information(this, QStringLiteral("Library"),
                                 QStringLiteral("This library is open read-only. Open it normally (Library ▸ Open…) "
                                                "to change it."));
        return false;
    }
    return true;
}

void LibraryPage::libraryModified() {
    m_matcherDirty = true;
    updateFilters();
    fillTable();
}

void LibraryPage::report(const QString& title, const QString& text, const QStringList& errors) {
    emit message(text);
    if (errors.isEmpty()) return;
    QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::Ok, this);
    box.setDetailedText(errors.join('\n'));
    box.exec();
}

qint64 LibraryPage::addSpectrum(const Spectrum& sp, bool ask) {
    if (ask ? !requireWritable() : (!ensureLibrary() || m_lib->readOnly())) return 0;
    Entry meta;
    meta.name = sp.metaValue(QStringLiteral("sample"));
    meta.op = sp.metaValue(QStringLiteral("operator"));
    meta.notes = sp.metaValue(QStringLiteral("notes"));
    meta.source = Source::Own;
    meta.licence = QStringLiteral("own data");
    if (ask) {
        Entry shown = meta;
        shown.laserNm = sp.channel;
        const QString l = sp.metaValue(QStringLiteral("laser_nm"));
        if (!l.isEmpty()) shown.laserNm = l.toDouble();
        shown.conditions = QStringLiteral("%1 nm, exposure %2 s, %3 average(s)")
                               .arg(sp.channel).arg(sp.exposure_s).arg(sp.averages);
        EntryDialog dlg(this, QStringLiteral("Add to library"), shown, m_lib->classes(), true);
        if (dlg.exec() != QDialog::Accepted) return 0;
        bool bandsChanged = false;
        if (!dlg.apply(shown, &bandsChanged)) {
            QMessageBox::warning(this, QStringLiteral("Add to library"), QStringLiteral("The band list is not valid."));
            return 0;
        }
        if (!bandsChanged) shown.peaks.clear();       // picked automatically
        meta = shown;
    }
    try {
        const qint64 id = m_lib->addSpectrum(sp, meta);
        libraryModified();
        selectEntry(id);
        emit message(QStringLiteral("Added “%1” to the library").arg(m_lib->get(id, false)->name));
        return id;
    } catch (const Error& e) {
        if (ask) QMessageBox::warning(this, QStringLiteral("Add to library"), QString::fromUtf8(e.what()));
        emit message(QString::fromUtf8(e.what()));
        return 0;
    }
}

void LibraryPage::newLibrary() {
    const QString start = m_lib->isOpen() ? QFileInfo(m_lib->path()).absolutePath() : QDir::homePath();
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("New library"), start + QStringLiteral("/library.sqlite"),
                                                QStringLiteral("LibreRamen library (*.sqlite)"));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".sqlite");
    if (QFileInfo::exists(path) && !QFile::remove(path)) {
        QMessageBox::warning(this, QStringLiteral("New library"), QStringLiteral("Cannot replace %1").arg(path));
        return;
    }
    if (!setLibraryPath(path)) QMessageBox::warning(this, QStringLiteral("New library"), m_libInfo->text());
}

void LibraryPage::openLibrary(bool readOnly) {
    const QString start = m_lib->isOpen() ? QFileInfo(m_lib->path()).absolutePath() : QDir::homePath();
    const QString path = QFileDialog::getOpenFileName(this, readOnly ? QStringLiteral("Open library read-only")
                                                                     : QStringLiteral("Open library"),
                                                      start, QStringLiteral("LibreRamen library (*.sqlite *.db);;All files (*)"));
    if (path.isEmpty()) return;
    if (!setLibraryPath(path, readOnly))
        QMessageBox::warning(this, QStringLiteral("Open library"), QStringLiteral("Could not open %1.").arg(path));
}

void LibraryPage::addReference() {
    if (!requireWritable()) return;
    try {
        const int n = library::addReferenceEntries(*m_lib);
        emit message(n ? QStringLiteral("Added %1 reference entries").arg(n)
                       : QStringLiteral("The library already has all reference entries"));
    } catch (const Error& e) {
        QMessageBox::warning(this, QStringLiteral("Reference entries"), QString::fromUtf8(e.what()));
    }
    libraryModified();
}

void LibraryPage::addDemo() {
    if (!requireWritable()) return;
    try {
        library::createDemo(*m_lib);
    } catch (const Error& e) {
        QMessageBox::warning(this, QStringLiteral("Demo entries"), QString::fromUtf8(e.what()));
    }
    libraryModified();
}

void LibraryPage::importFiles() {
    if (!requireWritable()) return;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Import spectra"), QDir::homePath(),
        QStringLiteral("Spectra (*.csv *.txt *.spc *.jdx *.dx);;All files (*)"));
    if (files.isEmpty()) return;
    QStringList errors;
    int n = 0;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (const QString& f : files) {
        try {
            n += int(m_lib->importFile(f).size());
        } catch (const Error& e) {
            errors << QStringLiteral("%1: %2").arg(QFileInfo(f).fileName(), QString::fromUtf8(e.what()));
        }
    }
    QApplication::restoreOverrideCursor();
    libraryModified();
    report(QStringLiteral("Import"), QStringLiteral("Imported %1 entries; %2 files failed.").arg(n).arg(errors.size()),
           errors);
}

void LibraryPage::importRruff() {
    if (!requireWritable()) return;
    const auto ok = QMessageBox::information(
        this, QStringLiteral("Import RRUFF spectra"),
        QStringLiteral("Import RRUFF text files you have downloaded from rruff.info.\n\n"
                       "RRUFF data are free for research and education; cite Lafuente et al. (2015). LibreRamen "
                       "keeps the source and licence with every entry and never redistributes them. Check the "
                       "RRUFF terms before sharing a library that contains them."),
        QMessageBox::Ok | QMessageBox::Cancel);
    if (ok != QMessageBox::Ok) return;
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Folder with RRUFF .txt files"),
                                                          QDir::homePath());
    if (dir.isEmpty()) return;
    QStringList errors;
    std::vector<qint64> ids;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        ids = m_lib->importRruffFolder(dir, &errors);
    } catch (const Error& e) {
        errors << QString::fromUtf8(e.what());
    }
    QApplication::restoreOverrideCursor();
    libraryModified();
    report(QStringLiteral("Import RRUFF"),
           QStringLiteral("Imported %1 RRUFF spectra; %2 files skipped.").arg(ids.size()).arg(errors.size()), errors);
}

void LibraryPage::importBandList() {
    if (!requireWritable()) return;
    const QString f = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import band list"), QDir::homePath(), QStringLiteral("Band lists (*.csv *.txt);;All files (*)"));
    if (f.isEmpty()) return;
    try {
        const auto ids = m_lib->importBandList(f);
        libraryModified();
        report(QStringLiteral("Import band list"), QStringLiteral("Imported %1 band lists.").arg(ids.size()));
    } catch (const Error& e) {
        QMessageBox::warning(this, QStringLiteral("Import band list"), QString::fromUtf8(e.what()));
    }
}

void LibraryPage::exportToFiles() {
    const auto ids = selectedIds();
    if (ids.empty()) {
        QMessageBox::information(this, QStringLiteral("Export"), QStringLiteral("Select the entries to export first."));
        return;
    }
    QString dir;
    QString single;
    if (ids.size() == 1) {
        const auto e = m_lib->get(ids[0], false);
        single = QFileDialog::getSaveFileName(this, QStringLiteral("Export entry"),
                                              QDir::homePath() + '/' + e->name + QStringLiteral(".csv"),
                                              QStringLiteral("CSV (*.csv);;JCAMP-DX (*.jdx)"));
        if (single.isEmpty()) return;
    } else {
        dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Export entries to folder"), QDir::homePath());
        if (dir.isEmpty()) return;
    }
    QStringList errors;
    int n = 0;
    for (qint64 id : ids) {
        try {
            QString path = single;
            if (path.isEmpty()) {
                QString name = m_lib->get(id, false)->name;
                name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9 _.,()+-]")), QStringLiteral("_"));
                path = QStringLiteral("%1/%2 (%3).csv").arg(dir, name).arg(id);
            }
            m_lib->exportEntry(id, path);
            ++n;
        } catch (const Error& e) {
            errors << QString::fromUtf8(e.what());
        }
    }
    report(QStringLiteral("Export"), QStringLiteral("Exported %1 entries (source and licence included).").arg(n), errors);
}

void LibraryPage::exportToLibrary() {
    const auto ids = selectedIds();
    if (ids.empty()) {
        QMessageBox::information(this, QStringLiteral("Export"), QStringLiteral("Select the entries to export first."));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Copy entries to library"), QDir::homePath(),
                                                      QStringLiteral("LibreRamen library (*.sqlite)"), nullptr,
                                                      QFileDialog::DontConfirmOverwrite);
    if (path.isEmpty()) return;
    try {
        const int n = m_lib->exportTo(ids, path);
        report(QStringLiteral("Export"), QStringLiteral("Copied %1 entries to %2.").arg(n).arg(QDir::toNativeSeparators(path)));
    } catch (const Error& e) {
        QMessageBox::warning(this, QStringLiteral("Export"), QString::fromUtf8(e.what()));
    }
}

void LibraryPage::addEntry() {
    if (!requireWritable()) return;
    Entry e;
    e.source = Source::Literature;
    EntryDialog dlg(this, QStringLiteral("New library entry (band list)"), e, m_lib->classes(), false);
    if (dlg.exec() != QDialog::Accepted) return;
    if (!dlg.apply(e, nullptr) || e.peaks.empty()) {
        QMessageBox::warning(this, QStringLiteral("New entry"),
                             QStringLiteral("Give the band positions (cm⁻¹), separated by commas."));
        return;
    }
    try {
        const qint64 id = m_lib->add(e);
        libraryModified();
        selectEntry(id);
    } catch (const Error& err) {
        QMessageBox::warning(this, QStringLiteral("New entry"), QString::fromUtf8(err.what()));
    }
}

void LibraryPage::editEntry() {
    const qint64 id = selectedEntry();
    if (id <= 0 || !requireWritable()) return;
    auto e = m_lib->get(id, true);
    if (!e) return;
    EntryDialog dlg(this, QStringLiteral("Edit library entry"), *e, m_lib->classes(), !e->spectra.empty());
    if (dlg.exec() != QDialog::Accepted) return;
    bool bandsChanged = false;
    if (!dlg.apply(*e, &bandsChanged)) {
        QMessageBox::warning(this, QStringLiteral("Edit entry"), QStringLiteral("The band list is not valid."));
        return;
    }
    try {
        m_lib->update(*e);              // with data: spectra unchanged, peaks as edited
        libraryModified();
        selectEntry(id);
    } catch (const Error& err) {
        QMessageBox::warning(this, QStringLiteral("Edit entry"), QString::fromUtf8(err.what()));
    }
}

void LibraryPage::deleteEntries() {
    const auto ids = selectedIds();
    if (ids.empty() || !requireWritable()) return;
    const QString what = ids.size() == 1 ? QStringLiteral("“%1”").arg(m_lib->get(ids[0], false)->name)
                                         : QStringLiteral("%1 entries").arg(ids.size());
    if (QMessageBox::question(this, QStringLiteral("Delete"),
                              QStringLiteral("Delete %1 from the library? This cannot be undone.").arg(what)) !=
        QMessageBox::Yes)
        return;
    try {
        m_lib->remove(ids);
    } catch (const Error& e) {
        QMessageBox::warning(this, QStringLiteral("Delete"), QString::fromUtf8(e.what()));
    }
    libraryModified();
}

void LibraryPage::loadQueryFile() {
    const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Query spectrum"), QDir::homePath(),
                                                   QStringLiteral("Spectra (*.csv *.txt *.spc *.jdx *.dx);;All files (*)"));
    if (f.isEmpty()) return;
    try {
        if (library::isRruffFile(f)) {
            const auto r = library::readRruff(f);
            setQuery(r.x, r.y, r.name);
        } else {
            const auto sps = fileio::read(f);
            if (sps.empty()) throw Error("no spectrum in the file");
            setQuery(sps[0].x, sps[0].y, sps[0].name);
        }
        runMatch();
    } catch (const Error& e) {
        QMessageBox::warning(this, QStringLiteral("Query spectrum"), QString::fromUtf8(e.what()));
    }
}

}  // namespace lr::gui
