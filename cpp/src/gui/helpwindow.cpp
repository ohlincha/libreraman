#include "helpwindow.h"

#include <QDesktopServices>
#include <algorithm>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QKeySequence>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QVBoxLayout>

namespace lr::gui {

namespace {
const QString kGitHubDocs = QStringLiteral("https://github.com/ohlincha/libreramen/blob/main/docs/");
}

HelpBrowser::HelpBrowser(QWidget* parent) : QTextBrowser(parent) {
    setOpenLinks(false);                                // follow() decides: here or in the web browser
    connect(this, &QTextBrowser::anchorClicked, this, &HelpBrowser::follow);
    document()->setDocumentMargin(16);
}

QVariant HelpBrowser::loadResource(int type, const QUrl& name) {
    QVariant v = QTextBrowser::loadResource(type, name);
    if (type != QTextDocument::ImageResource)
        return v;
    QImage img;
    if (v.canConvert<QImage>())
        img = v.value<QImage>();
    else if (v.typeId() == QMetaType::QByteArray)
        img.loadFromData(v.toByteArray());
    if (img.isNull())
        return v;
    const int maxW = std::max(480, std::min(1000, viewport()->width() - 48));   // screenshots are 1400 px wide
    if (img.width() > maxW)
        img = img.scaledToWidth(maxW, Qt::SmoothTransformation);
    return img;
}

QUrl HelpBrowser::externalUrl(const QUrl& link) const {
    if (link.scheme() == QLatin1String("http") || link.scheme() == QLatin1String("https") ||
        link.scheme() == QLatin1String("mailto"))
        return link;
    const QUrl u = source().resolved(link);
    if (u.scheme() == QLatin1String("qrc") && QFile::exists(QLatin1Char(':') + u.path()))
        return {};                                      // built in: shown here
    // a file of the repository that is not built in (README.md, ../cpp/...): its page on GitHub
    QUrl gh(kGitHubDocs);
    gh = gh.resolved(QUrl(link.path()));
    gh.setFragment(link.fragment());
    return gh;
}

void HelpBrowser::follow(const QUrl& link) {
    if (link.path().isEmpty() && link.hasFragment()) {   // #anchor in the same document
        scrollToAnchor(link.fragment());
        return;
    }
    const QUrl ext = externalUrl(link);
    if (ext.isEmpty()) {
        setSource(source().resolved(link));
        return;
    }
    m_lastExternal = ext;
    if (m_openExternally)
        QDesktopServices::openUrl(ext);
}

HelpWindow::HelpWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setWindowTitle(QStringLiteral("LibreRamen help"));
    resize(980, 860);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    auto* bar = new QHBoxLayout;
    m_back = new QPushButton(QStringLiteral("◀  Back"));
    m_back->setEnabled(false);
    auto* qs = new QPushButton(QStringLiteral("Quick start"));
    auto* man = new QPushButton(QStringLiteral("Manual"));
    bar->addWidget(m_back);
    bar->addSpacing(12);
    bar->addWidget(qs);
    bar->addWidget(man);
    bar->addStretch();
    v->addLayout(bar);
    m_browser = new HelpBrowser;
    v->addWidget(m_browser, 1);
    connect(m_back, &QPushButton::clicked, m_browser, &QTextBrowser::backward);
    connect(m_browser, &QTextBrowser::backwardAvailable, m_back, &QWidget::setEnabled);
    connect(qs, &QPushButton::clicked, this, [this] { showDoc(Doc::QuickStart); });
    connect(man, &QPushButton::clicked, this, [this] { showDoc(Doc::Manual); });
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, &QWidget::close);
}

QUrl HelpWindow::docUrl(Doc d) {
    return QUrl(d == Doc::QuickStart ? QStringLiteral("qrc:/help/QUICKSTART.md") : QStringLiteral("qrc:/help/MANUAL.md"));
}

void HelpWindow::showDoc(Doc d) {
    const QUrl u = docUrl(d);
    if (m_browser->source() != u)
        m_browser->setSource(u, QTextDocument::MarkdownResource);
    m_browser->verticalScrollBar()->setValue(0);
}

}  // namespace lr::gui
