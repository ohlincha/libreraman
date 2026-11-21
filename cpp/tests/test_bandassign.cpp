// Built-in band-assignment table (resources/band_assignments.csv).
#include <QtTest>

#include "core/bandassign.h"

using namespace lr;

class TestBandAssign : public QObject {
    Q_OBJECT
private slots:
    void tableLoads() {
        const auto& all = bands::all();
        QVERIFY(all.size() > 180);
        int org = 0, inorg = 0;
        for (const bands::Band& b : all) {
            QVERIFY(!b.group.isEmpty());
            QVERIFY(b.lo <= b.hi);
            QVERIFY(b.lo >= 0 && b.hi < 5000);
            QVERIFY(b.category == "organic" || b.category == "inorganic" || b.category == "both");
            org += b.category == "organic";
            inorg += b.category == "inorganic";
        }
        QVERIFY(org > 100);
        QVERIFY(inorg > 50);
    }
    void candidates() {
        auto has = [](const std::vector<const bands::Band*>& v, const QString& group) {
            return std::any_of(v.begin(), v.end(), [&](const bands::Band* b) { return b->group.contains(group); });
        };
        // carbonate only with the inorganic table; C=O only with the organic one
        QVERIFY(has(bands::candidates(1086, false, true), "carbonate"));
        QVERIFY(!has(bands::candidates(1086, true, false), "carbonate"));
        QVERIFY(has(bands::candidates(1715, true, false), "ketone"));
        QVERIFY(!has(bands::candidates(1715, false, true), "ketone"));
        // both: a mixed sample
        const auto both = bands::candidates(1003, true, true);
        QVERIFY(has(both, "Benzene"));
        QVERIFY(has(both, "sulfate"));
        // functional groups and ion classes only: compounds are identified with the Library
        {
            std::vector<const bands::Band*> allp;
            for (const bands::Band& b : bands::all()) allp.push_back(&b);
            QVERIFY(!has(allp, "gypsum") && !has(allp, "calcite") && !has(allp, "Toluene"));
            QVERIFY(has(allp, "N₂ (gas)") && has(allp, "CH₃"));   // subscripts
        }
        // ranges containing the position first, then the narrowest
        auto inside = [](const bands::Band* b) { return 1003 >= b->lo && 1003 <= b->hi; };
        for (std::size_t i = 1; i < both.size(); ++i)
            if (inside(both[i - 1]) == inside(both[i]))
                QVERIFY(both[i - 1]->width() <= both[i]->width());
            else
                QVERIFY(inside(both[i - 1]));
    }
    void parse() {
        const auto v = bands::parse("# comment\ncategory,group,mode,lo,hi,raman,note\n"
                                    "organic,X,ν,10,20,s,a note, with comma\ninorganic,Y,ν,0,0,,inactive\n");
        QCOMPARE(int(v.size()), 1);
        QCOMPARE(v[0].note, QString("a note, with comma"));
    }
};

QTEST_MAIN(TestBandAssign)
#include "test_bandassign.moc"
