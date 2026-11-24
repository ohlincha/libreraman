// Library page smoke test (offscreen): open/create a library, demo entries, browse/filter, entry display,
// "Add to library", "Match last spectrum" with all three methods, read-only and error states.
// With LIBRERAMEN_PREVIEW_DIR set, the page is rendered to library-*.png files there (light and dark).
#include "gui/librarypage.h"
#include "gui/plotwidget.h"
#include "gui/theme.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

#include "core/library.h"
#include "core/simulator.h"

using namespace lr;
using namespace lr::gui;

namespace {

Spectrum measure(SimCora& sim, const QString& sample, double exposure = 0.5) {
    sim.setSample(sample);
    AcquireOptions o;
    o.exposure_s = exposure;
    Spectrum sp = sim.acquire(o);
    sp.setMeta("sample", sample);
    return sp;
}

void savePreview(QWidget& w, const QString& name) {
    const QString dir = qEnvironmentVariable("LIBRERAMEN_PREVIEW_DIR");
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    QApplication::processEvents();
    const QString path = dir + '/' + name;
    QVERIFY(w.grab().save(path));
    qInfo("wrote %s", qPrintable(path));
}

}  // namespace

class TestLibraryPage : public QObject {
    Q_OBJECT
    QTemporaryDir tmp;

private slots:
    void initTestCase() {
        QVERIFY(tmp.isValid());
        theme::apply(*qApp, theme::Mode::Light);
    }

    void page() {
        LibraryPage page;
        page.resize(1180, 760);
        page.show();
        QVERIFY(page.libraryPath().isEmpty());
        QCOMPARE(page.visibleEntries(), 0);
        savePreview(page, "library-empty.png");

        QSignalSpy changed(&page, &LibraryPage::libraryChanged);
        const QString path = tmp.filePath("lib.sqlite");
        QVERIFY(page.setLibraryPath(path));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(page.libraryPath(), QFileInfo(path).absoluteFilePath());
        library::createDemo(page.library());
        page.refresh();
        const int all = page.visibleEntries();
        QCOMPARE(all, page.library().count());
        QVERIFY(all > 20);

        // search
        page.setSearchText("ccl4");
        QCOMPARE(page.visibleEntries(), 2);           // own spectrum (formula) + literature band list (alias tag)
        page.setSearchText("");
        QCOMPARE(page.visibleEntries(), all);

        // select an entry: details and plot
        qint64 ethanol = 0, ccl4 = 0, calcite = 0;
        for (const auto& e : page.library().find()) {
            if (e.name == "Calcite") calcite = e.id;
            if (e.source != library::Source::Own) continue;
            if (e.name == "Ethanol") ethanol = e.id;
            if (e.name == "Carbon tetrachloride") ccl4 = e.id;
            
        }
        page.selectEntry(ethanol);
        QCOMPARE(page.selectedEntry(), ethanol);
        QVERIFY(!page.plot()->items().empty());
        const auto labels = page.findChildren<QLabel*>();
        bool licenceShown = false;
        for (QLabel* l : labels) licenceShown = licenceShown || l->text().contains("LibreRamen demo data (MIT)");
        QVERIFY(licenceShown);
        savePreview(page, "library-browse.png");
        page.selectEntry(calcite);                    // band list: sticks
        QCOMPARE(page.selectedEntry(), calcite);
        QVERIFY(!page.plot()->items().empty());
        savePreview(page, "library-bandlist.png");

        // "Add to library" without dialog
        SimCora sim(defaultCalibration(), [](const QString&) {}, 1000.0, 5);
        sim.connect();
        sim.initialize();
        sim.pressArm();
        const Spectrum sp = measure(sim, "chloroform");
        const qint64 id = page.addSpectrum(sp, false);
        QVERIFY(id > 0);
        QCOMPARE(page.visibleEntries(), all + 1);
        QCOMPARE(page.selectedEntry(), id);
        QCOMPARE(page.library().get(id)->source, library::Source::Own);

        // "Match last spectrum"
        QSignalSpy matched(&page, &LibraryPage::matched);
        const Spectrum q = measure(sim, "ethanol");
        QCOMPARE(page.method(), LibraryPage::Method::Spectrum);   // the default
        page.setMethod(LibraryPage::Method::Spectrum);
        auto hits = page.match(q);
        QVERIFY(!hits.empty());
        QCOMPARE(hits[0].name, QString("Ethanol"));
        QVERIFY(hits[0].score > 0.95);
        QCOMPARE(matched.count(), 1);
        QCOMPARE(matched.at(0).at(0).toString(), QString("Ethanol"));
        savePreview(page, "library-match.png");

        page.setMethod(LibraryPage::Method::Peaks);
        hits = page.runMatch();
        QVERIFY(!hits.empty());
        QCOMPARE(hits[0].name.toLower(), QString("ethanol"));
        QVERIFY(hits[0].matched > 0);
        savePreview(page, "library-peaks.png");

        // mixture: ethanol + carbon tetrachloride measured separately and added
        const Spectrum a = measure(sim, "ethanol", 1.0), b = measure(sim, "carbon tetrachloride", 1.0);
        Spectrum mix = a;
        for (size_t i = 0; i < mix.intensity.size(); ++i) mix.intensity[i] = 0.5 * a.intensity[i] + 0.5 * b.intensity[i];
        mix.setMeta("sample", "mixture");
        page.setMethod(LibraryPage::Method::Mixture);
        page.match(mix);
        QVERIFY(!page.mixtures().empty());
        const auto& best = page.mixtures()[0];
        QCOMPARE(int(best.components.size()), 2);
        QList<qint64> ids;
        for (const auto& c : best.components) ids << c.id;
        QVERIFY(ids.contains(ethanol));
        QVERIFY(ids.contains(ccl4));
        QVERIFY(best.hqi > 0.98);
        savePreview(page, "library-mixture.png");

        // the hit list double-click equivalent: back to details
        page.setMethod(LibraryPage::Method::Spectrum);
        page.runMatch();
        page.selectHit(1);
        page.showDetailsTab();
        QCOMPARE(page.selectedEntry(), id);           // details tab shows the selection again

        // read-only
        QVERIFY(page.setLibraryPath(path, true));
        QVERIFY(page.library().readOnly());
        QCOMPARE(page.addSpectrum(sp, false), qint64(0));
        int fromFile = 0;
        for (QPushButton* b : page.findChildren<QPushButton*>()) {
            if (b->text() == "Delete" || b->text() == "Import" || b->text() == "From file…") QVERIFY(!b->isEnabled());
            if (b->text() == "From file…") ++fromFile;
        }
        QCOMPARE(fromFile, 1);                         // "Add spectrum from file" next to Add…
        QCOMPARE(page.visibleEntries(), all + 1);
        savePreview(page, "library-readonly.png");

        // errors stay in the page header
        QSignalSpy msg(&page, &LibraryPage::message);
        QVERIFY(!page.setLibraryPath(tmp.filePath("missing.sqlite"), true));
        QCOMPARE(msg.count(), 1);
        QVERIFY(msg.at(0).at(0).toString().contains("not found"));
    }

    void dark() {
        if (qEnvironmentVariable("LIBRERAMEN_PREVIEW_DIR").isEmpty()) QSKIP("preview only");
        theme::apply(*qApp, theme::Mode::Dark);
        LibraryPage page;
        page.resize(1180, 760);
        page.show();
        QVERIFY(page.setLibraryPath(tmp.filePath("dark.sqlite")));
        library::createDemo(page.library());
        page.refresh();
        SimCora sim(defaultCalibration(), [](const QString&) {}, 1000.0, 9);
        sim.connect();
        sim.initialize();
        sim.pressArm();
        page.match(measure(sim, "zinc sulfide"));
        savePreview(page, "library-dark-match.png");
        theme::apply(*qApp, theme::Mode::Light);
    }
};

QTEST_MAIN(TestLibraryPage)
#include "gui_test_librarypage.moc"
