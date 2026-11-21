#include "bandassign.h"

#include <QFile>
#include <QStringList>

#include <algorithm>

// The core library is static: its resources must be registered explicitly (outside any namespace).
static void initBandResources() { Q_INIT_RESOURCE(core); }

namespace lr::bands {

QString Band::label() const {
    const QString range = sharp() ? QString::number(lo, 'f', 0)
                                  : QStringLiteral("%1–%2").arg(QString::number(lo, 'f', 0), QString::number(hi, 'f', 0));
    QString s = QStringLiteral("%1: %2  %3 cm⁻¹").arg(group, mode, range);
    if (!raman.isEmpty())
        s += QStringLiteral(" (%1)").arg(raman);
    return s;
}

std::vector<Band> parse(const QString& csvText) {
    std::vector<Band> out;
    bool header = true;
    for (const QString& raw : csvText.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (header) {             // category,group,mode,lo,hi,raman,note
            header = false;
            continue;
        }
        const QStringList f = line.split(QLatin1Char(','));
        if (f.size() < 5)
            continue;
        Band b;
        b.category = f[0].trimmed();
        b.group = f[1].trimmed();
        b.mode = f[2].trimmed();
        bool ok1 = false, ok2 = false;
        b.lo = f[3].toDouble(&ok1);
        b.hi = f[4].toDouble(&ok2);
        if (!ok1 || !ok2 || b.hi <= 0)
            continue;            // no position (e.g. Raman-inactive)
        if (b.lo > b.hi)
            std::swap(b.lo, b.hi);
        b.raman = f.size() > 5 ? f[5].trimmed() : QString();
        b.note = f.size() > 6 ? f.mid(6).join(QLatin1Char(',')).trimmed() : QString();
        out.push_back(b);
    }
    return out;
}

const std::vector<Band>& all() {
    static const std::vector<Band> table = [] {
        initBandResources();
        QFile f(QStringLiteral(":/core/band_assignments.csv"));
        if (!f.open(QIODevice::ReadOnly))
            return std::vector<Band>{};
        return parse(QString::fromUtf8(f.readAll()));
    }();
    return table;
}

std::vector<const Band*> candidates(double pos, bool organic, bool inorganic, double tol) {
    std::vector<const Band*> out;
    for (const Band& b : all()) {
        if (!((organic && b.organic()) || (inorganic && b.inorganic())))
            continue;
        if (pos >= b.lo - tol && pos <= b.hi + tol)
            out.push_back(&b);
    }
    // ranges that contain the position first (others only within tol), then the most specific (narrowest)
    auto inside = [pos](const Band* b) { return pos >= b->lo && pos <= b->hi; };
    std::stable_sort(out.begin(), out.end(), [&](const Band* a, const Band* b) {
        if (inside(a) != inside(b))
            return inside(a);
        return a->width() < b->width();
    });
    return out;
}

}  // namespace lr::bands
