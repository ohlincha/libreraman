#pragma once
// Help ▸ Quick start / Manual: docs/QUICKSTART.md and docs/MANUAL.md with their pictures, compiled into the
// program (resources/help.qrc, prefix /help), shown offline in a QTextBrowser (Markdown via QTextDocument).
// Links between the two documents stay in the window; web links and links to files that are not built in
// (e.g. ../README.md) open in the web browser (local files: their page on GitHub). Pictures wider than the
// window are scaled down.
#include <QTextBrowser>
#include <QWidget>

class QPushButton;

namespace lr::gui {

class HelpBrowser : public QTextBrowser {
    Q_OBJECT
public:
    explicit HelpBrowser(QWidget* parent = nullptr);
    QVariant loadResource(int type, const QUrl& name) override;
    // what a click on a link does (for tests): "" = shown here, else the URL handed to the web browser
    QUrl externalUrl(const QUrl& link) const;
    void setOpenExternally(bool on) { m_openExternally = on; }   // off in tests
    QUrl lastExternal() const { return m_lastExternal; }

private:
    void follow(const QUrl& link);
    bool m_openExternally = true;
    QUrl m_lastExternal;
};

class HelpWindow : public QWidget {
    Q_OBJECT
public:
    enum class Doc { QuickStart, Manual };
    explicit HelpWindow(QWidget* parent = nullptr);
    void showDoc(Doc d);
    HelpBrowser* browser() const { return m_browser; }
    static QUrl docUrl(Doc d);   // qrc:/help/QUICKSTART.md, qrc:/help/MANUAL.md

private:
    HelpBrowser* m_browser;
    QPushButton* m_back;
};

}  // namespace lr::gui
