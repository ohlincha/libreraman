// Built-in band-assignment table (resources/band_assignments.csv).
#include <QtTest>

#include "core/bandassign.h"

using namespace lr;

class TestBandAssign : public QObject {
    Q_OBJECT
private slots:
    void tableLoads() {
        const auto& all = bands::all();
        QVERIFY(all.size() > 250);
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
        QVERIFY(inorg > 100);
    }
    void candidates() {
        auto has = [](const std::vector<const bands::Band*>& v, const QString& group) {
            return std::any_of(v.begin(), v.end(), [&](const bands::Band* b) { return b->group.contains(group); });
        };
        // calcite only with the inorganic table; C=O only with the organic one
        QVERIFY(has(bands::candidates(1086, false, true), "calcite"));
        QVERIFY(!has(bands::candidates(1086, true, false), "calcite"));
        QVERIFY(has(bands::candidates(1715, true, false), "ketone"));
        QVERIFY(!has(bands::candidates(1715, false, true), "ketone"));
        // both: a mixed sample
        const auto both = bands::candidates(1003, true, true);
        QVERIFY(has(both, "Phenylalanine"));
        QVERIFY(has(both, "gypsum"));
        // ranges containing the position first, then narrowest: phenylalanine (1000-1006) before gypsum (1006-1009)
        QVERIFY(both.front()->group.contains("Phenylalanine"));
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
