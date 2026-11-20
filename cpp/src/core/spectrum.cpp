#include "spectrum.h"

#include <QFile>
#include <QTextStream>

#include "errors.h"
#include "pyformat.h"

namespace lr {

QString Spectrum::metaValue(const QString& key, const QString& fallback) const {
    for (const auto& kv : meta)
        if (kv.first == key) return kv.second;
    return fallback;
}

void Spectrum::setMeta(const QString& key, const QString& value) {
    for (auto& kv : meta)
        if (kv.first == key) { kv.second = value; return; }
    meta.emplace_back(key, value);
}

void Spectrum::saveCsv(const QString& path) const {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        throw Error(("cannot write " + path).toStdString());
    QTextStream o(&f);
    o << "# Cora 5000 spectrum, " << iso(t) << "\n";
    o << "# channel: " << channel << " nm\n# exposure_s: " << py::str(exposure_s) << "\n# averages: " << averages << "\n";
    for (const auto& kv : meta) o << "# " << kv.first << ": " << kv.second << "\n";
    o << "raman_shift_cm1,intensity,signal,background\n";
    for (size_t i = 0; i < shift.size(); ++i) {
        o << py::fixed(shift[i], 2) << "," << py::fixed(intensity[i], 1) << ",";
        if (i < signal.size()) o << py::str(signal[i]);
        o << ",";
        if (i < background.size()) o << py::str(background[i]);
        o << "\n";
    }
}

QString iso(const QDateTime& t) {
    return t.toOffsetFromUtc(t.offsetFromUtc()).toString(Qt::ISODate);
}

}  // namespace lr
