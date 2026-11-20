// Page registry and the placeholder page (see page.h).
#include "page.h"

#include "analysispagehost.h"
#include "calibrationpage.h"
#include "datapagehost.h"
#include "librarypagehost.h"

#include <QEvent>
#include <QLabel>
#include <QVBoxLayout>
#include <map>

namespace lr::gui {

namespace {

// Built-in pages. Add the real pages here when they are merged, e.g.
//   {QStringLiteral("Data"), [](const PageContext& c) -> Page* { return new DataPage(c); }},
std::map<QString, PageFactory> builtinPageFactories() {
    return {
        {QStringLiteral("Data"), [](const PageContext& c) -> Page* { return new DataPageHost(c); }},
        {QStringLiteral("Library"), [](const PageContext& c) -> Page* { return new LibraryPageHost(c); }},
        {QStringLiteral("Analysis"), [](const PageContext& c) -> Page* { return new AnalysisPageHost(c); }},
        {QStringLiteral("Calibration"), [](const PageContext& c) -> Page* { return new CalibrationPage(c); }},
    };
}

std::map<QString, PageFactory>& registry() {
    static std::map<QString, PageFactory> r = builtinPageFactories();
    return r;
}

struct PlaceholderText {
    const char* area;
    const char* text;
};

// Python MainWindow: Placeholder("Library", ...), ...
const PlaceholderText kPlaceholders[] = {
    {"Data", "Your spectra: open files, compare, process, export."},
    {"Library", "Spectral library (open databases, literature, your own spectra) and matching."},
    {"Analysis", "PCA, EFA, OPA, SIMPLISMA, MCR-ALS, NNLS; comparison with computed spectra."},
    {"Calibration", "Calibration against reference standards (wizard), quick checks, history. "
                    "Already available on the command line: python3 -m libreramen calibrate"},
};

}  // namespace

PlaceholderPage::PlaceholderPage(const QString& title, const QString& text, QWidget* parent)
    : Page(parent), m_title(title) {
    auto* v = new QVBoxLayout(this);
    auto* lab = new QLabel(QStringLiteral("<h2>%1</h2><p>%2</p><p style='color:#888'>Coming in a later version; "
                                          "see docs/SPECIFICATION.md.</p>")
                               .arg(title.toHtmlEscaped(), text.toHtmlEscaped()),
                           this);
    lab->setWordWrap(true);
    lab->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    v->addWidget(lab);
}

namespace {
bool g_dialogs = true;
}

bool dialogsEnabled() { return g_dialogs; }
void setDialogsEnabled(bool on) { g_dialogs = on; }

const QStringList& pageAreas() {
    static const QStringList a = {QStringLiteral("Measure"), QStringLiteral("Data"), QStringLiteral("Library"),
                                  QStringLiteral("Analysis"), QStringLiteral("Calibration")};
    return a;
}

void registerPage(const QString& area, PageFactory factory) { registry()[area] = std::move(factory); }

Page* createPage(const QString& area, const PageContext& ctx) {
    const auto it = registry().find(area);
    Page* p = nullptr;
    if (it != registry().end() && it->second) {
        p = it->second(ctx);
    } else {
        QString text;
        for (const auto& t : kPlaceholders)
            if (area == QLatin1String(t.area))
                text = QString::fromUtf8(t.text);
        p = new PlaceholderPage(area, text);
    }
    if (p)
        p->setController(ctx.controller);
    return p;
}

SpectrumBatcher::SpectrumBatcher(QWidget* page, int visibleMs, Deliver deliver)
    : QObject(page), m_page(page), m_visibleMs(visibleMs), m_deliver(std::move(deliver)) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &SpectrumBatcher::flush);
    page->installEventFilter(this);
}

void SpectrumBatcher::add(const lr::Spectrum& sp) {
    m_pending.push_back(sp);
    schedule();
}

void SpectrumBatcher::schedule() {
    if (m_pending.empty() || !m_page->isVisible() || m_timer.isActive())
        return;
    const qint64 interval = std::max<qint64>(m_visibleMs, kDuty * (m_lastCostMs + (m_extraCost ? m_extraCost() : 0)));
    const qint64 wait = m_since.isValid() ? interval - m_since.elapsed() : 0;
    m_timer.start(int(std::max<qint64>(0, wait)));
}

void SpectrumBatcher::flush() {
    m_timer.stop();
    if (m_pending.empty())
        return;
    std::vector<lr::Spectrum> batch;
    batch.swap(m_pending);
    ++m_deliveries;
    QElapsedTimer cost;
    cost.start();
    m_deliver(std::move(batch));
    m_lastCostMs = cost.elapsed();
    m_since.start();                           // measured from the end of the (possibly long) update
}

bool SpectrumBatcher::eventFilter(QObject* watched, QEvent* e) {
    if (watched == m_page && e->type() == QEvent::Show)
        schedule();
    return QObject::eventFilter(watched, e);
}

QString spectrumDisplayName(const lr::Spectrum& sp) {
    QString name = sp.metaValue(QStringLiteral("sample"), QStringLiteral("spectrum")) + QLatin1Char(' ') +
                   sp.t.toString(QStringLiteral("HH:mm:ss"));
    const QString idx = sp.metaValue(QStringLiteral("series_index"));
    if (!idx.isEmpty())
        name += QStringLiteral(" #") + idx;
    return name;
}

}  // namespace lr::gui
