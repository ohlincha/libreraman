// Help ▸ Quick start / Manual (helpwindow.h): both documents and their pictures are built in, the Markdown is
// rendered (headings, tables, images), links between the documents stay in the window, other links go to the
// web browser, and the main window's Help menu opens the window.
#include "gui/controller.h"
#include "gui/helpwindow.h"
#include "gui/mainwindow.h"
#include "gui/page.h"

#include <QAction>
#include <QFile>
#include <QImage>
#include <QMenuBar>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextTable>

using namespace lr::gui;

class GuiTestHelp : public QObject {
    Q_OBJECT
    QTemporaryDir m_home;
private slots:
    void initTestCase() {
        qputenv("HOME", m_home.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", (m_home.path() + QStringLiteral("/cfg")).toUtf8());
        setDialogsEnabled(false);
    }

    // every picture a document refers to is in the resources
    void picturesBuiltIn_data() {
        QTest::addColumn<QString>("doc");
        QTest::newRow("quick start") << QStringLiteral(":/help/QUICKSTART.md");
        QTest::newRow("manual") << QStringLiteral(":/help/MANUAL.md");
    }
    void picturesBuiltIn() {
        QFETCH(QString, doc);
        QFile f(doc);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString md = QString::fromUtf8(f.readAll());
        QVERIFY(md.size() > 2000);
        static const QRegularExpression img(QStringLiteral("!\\[[^\\]]*\\]\\(([^)]+)\\)"));
        int n = 0;
        for (auto it = img.globalMatch(md); it.hasNext(); ++n) {
            const QString file = it.next().captured(1);
            QVERIFY2(!QImage(QStringLiteral(":/help/") + file).isNull(), qPrintable(file));
        }
        QVERIFY(n >= 2);
    }

    void rendersQuickStart() {
        HelpWindow w;
        w.resize(900, 700);
        w.show();
        w.showDoc(HelpWindow::Doc::QuickStart);
        HelpBrowser* b = w.browser();
        QCOMPARE(b->source(), HelpWindow::docUrl(HelpWindow::Doc::QuickStart));
        QTextDocument* d = b->document();
        QVERIFY(d->toPlainText().contains(QStringLiteral("Measure one spectrum")));
        QVERIFY(!d->toPlainText().contains(QStringLiteral("![")));   // Markdown rendered, not shown as text
        int tables = 0, images = 0, headings = 0;
        for (QTextBlock bl = d->begin(); bl.isValid(); bl = bl.next()) {
            if (bl.blockFormat().headingLevel() > 0) ++headings;
            for (auto it = bl.begin(); !it.atEnd(); ++it)
                if (it.fragment().charFormat().isImageFormat()) {
                    ++images;
                    const QString name = it.fragment().charFormat().toImageFormat().name();
                    const QVariant r = b->loadResource(QTextDocument::ImageResource, b->source().resolved(QUrl(name)));
                    const QImage im = r.value<QImage>();
                    QVERIFY2(!im.isNull(), qPrintable(name));
                    QVERIFY(im.width() <= 1000);                 // scaled down to the window
                }
        }
        for (QTextFrame* fr : d->rootFrame()->childFrames())
            if (qobject_cast<QTextTable*>(fr)) ++tables;
        QVERIFY(headings >= 9);
        QVERIFY(tables >= 3);
        QCOMPARE(images, 3);
    }

    void links() {
        HelpWindow w;
        w.showDoc(HelpWindow::Doc::QuickStart);
        HelpBrowser* b = w.browser();
        b->setOpenExternally(false);
        QCOMPARE(b->externalUrl(QUrl(QStringLiteral("MANUAL.md"))), QUrl());
        QCOMPARE(b->externalUrl(QUrl(QStringLiteral("https://github.com/ohlincha/libreraman/releases"))),
                 QUrl(QStringLiteral("https://github.com/ohlincha/libreraman/releases")));
        QCOMPARE(b->externalUrl(QUrl(QStringLiteral("../README.md#install"))),
                 QUrl(QStringLiteral("https://github.com/ohlincha/libreraman/blob/main/README.md#install")));
        emit b->anchorClicked(QUrl(QStringLiteral("MANUAL.md")));           // the manual, in the same window
        QCOMPARE(b->source(), HelpWindow::docUrl(HelpWindow::Doc::Manual));
        QVERIFY(b->document()->toPlainText().contains(QStringLiteral("Focus mode")));
        QVERIFY(b->isBackwardAvailable());
        emit b->anchorClicked(QUrl(QStringLiteral("../README.md")));         // not built in: the web browser
        QCOMPARE(b->source(), HelpWindow::docUrl(HelpWindow::Doc::Manual));
        QCOMPARE(b->lastExternal(), QUrl(QStringLiteral("https://github.com/ohlincha/libreraman/blob/main/README.md")));
    }

    void helpMenu() {
        Controller ctl;
        MainWindow win(&ctl);
        QMenu* help = nullptr;
        QStringList menus;
        for (QAction* a : win.menuBar()->actions()) {
            menus << a->text();
            if (a->text() == QLatin1String("&Help")) help = a->menu();
        }
        QCOMPARE(menus, (QStringList{QStringLiteral("&File"), QStringLiteral("&Measure"), QStringLiteral("&Instrument"),
                                     QStringLiteral("&Tools"), QStringLiteral("&Help")}));
        QVERIFY(help);
        QStringList items;
        for (QAction* a : help->actions()) items << a->text();
        QCOMPARE(items, (QStringList{QStringLiteral("Quick start"), QStringLiteral("Manual")}));
        QVERIFY(!win.helpWindow());
        help->actions().at(1)->trigger();
        QVERIFY(win.helpWindow() && win.helpWindow()->isVisible());
        QCOMPARE(win.helpWindow()->browser()->source(), HelpWindow::docUrl(HelpWindow::Doc::Manual));
        help->actions().at(0)->trigger();                                  // the same window, other document
        QCOMPARE(win.helpWindow()->browser()->source(), HelpWindow::docUrl(HelpWindow::Doc::QuickStart));
        win.close();
    }
};

QTEST_MAIN(GuiTestHelp)
#include "gui_test_help.moc"
