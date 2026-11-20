#include "fileio.h"

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QStringList>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <optional>

#include "errors.h"

namespace lr::fileio {

namespace {

[[noreturn]] void fail(const QString& msg) { throw Error(msg.toStdString()); }

// Python float(s): surrounding whitespace, inf/infinity/nan, underscores between digits.
std::optional<double> num(const QString& s0) {
    QString s = s0.trimmed().toLower();
    if (s.isEmpty()) return std::nullopt;
    QString body = s;
    double sign = 1.0;
    if (body.startsWith('+') || body.startsWith('-')) {
        sign = body.startsWith('-') ? -1.0 : 1.0;
        body = body.mid(1);
    }
    if (body == "inf" || body == "infinity") return sign * INFINITY;
    if (body == "nan") return std::nan("");
    static const QRegularExpression re(
        R"(^[+-]?(?:\d(?:_?\d)*(?:\.(?:\d(?:_?\d)*)?)?|\.\d(?:_?\d)*)(?:e[+-]?\d(?:_?\d)*)?$)");
    if (!re.match(s).hasMatch()) return std::nullopt;
    s.remove('_');
    bool ok = false;
    const double v = QLocale::c().toDouble(s, &ok);
    return v;            // !ok only on overflow/underflow, where Qt returns +-inf / 0 like Python
}

double toFloat(const QString& s) {                  // float(s), raising like Python
    const auto v = num(s);
    if (!v) fail(QString("could not convert string to float: '%1'").arg(s));
    return *v;
}

QString fixed(double v, int prec) {                 // f"{v:.{prec}f}"
    QString s = QString::number(v, 'f', prec);
    if (std::signbit(v) && !std::isnan(v) && !s.startsWith('-')) s.prepend('-');
    return s;
}

// str(float) of Python: shortest repr, exponent outside 1e-4 <= |v| < 1e16
QString pyRepr(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    if (v == 0.0) return std::signbit(v) ? "-0.0" : "0.0";
    QString e;
    for (int p = 0; p <= 17; ++p) {
        e = QString::number(v, 'e', p);
        if (e.toDouble() == v) break;
    }
    const int epos = int(e.indexOf('e'));
    QString mant = e.left(epos);
    const int exp = e.mid(epos + 1).toInt();
    const bool neg = mant.startsWith('-');
    if (neg) mant = mant.mid(1);
    QString digits = mant;
    digits.remove('.');
    while (digits.size() > 1 && digits.endsWith('0')) digits.chop(1);
    QString out;
    if (exp >= -4 && exp < 16) {
        if (exp >= 0) {
            QString ip = digits.left(exp + 1), fp = digits.mid(exp + 1);
            while (ip.size() < exp + 1) ip += '0';
            out = ip + "." + (fp.isEmpty() ? QString("0") : fp);
        } else {
            out = "0." + QString(-exp - 1, '0') + digits;
        }
    } else {
        out = digits.left(1) + (digits.size() > 1 ? "." + digits.mid(1) : QString()) + "e" + (exp < 0 ? "-" : "+") +
              QString::number(std::abs(exp)).rightJustified(2, '0');
    }
    return neg ? "-" + out : out;
}

void setMetaIn(Meta& meta, const QString& k, const QString& v) {
    for (auto& kv : meta)
        if (kv.first == k) { kv.second = v; return; }
    meta.emplace_back(k, v);
}

QString metaIn(const Meta& meta, const QString& k, const QString& fallback) {
    for (const auto& kv : meta)
        if (kv.first == k) return kv.second;
    return fallback;
}

QStringList splitLines(const QString& text) {       // str.splitlines()
    static const QRegularExpression re("\r\n|[\r\n\v\f\x1c\x1d\x1e\u0085  ]");
    QStringList lines = text.split(re);
    if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    return lines;
}

QStringList csvRow(const QString& line) {           // csv.reader, excel dialect, one line
    QStringList out;
    if (line.isEmpty()) return out;
    QString cur;
    enum { Start, Field, Quoted } st = Start;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (st == Quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else st = Field;
            } else {
                cur += c;
            }
        } else if (c == ',') {
            out << cur;
            cur.clear();
            st = Start;
        } else if (c == '"' && st == Start) {
            st = Quoted;
        } else {
            cur += c;
            st = Field;
        }
    }
    out << cur;
    return out;
}

QString csvField(const QString& s) {                // csv.writer, QUOTE_MINIMAL
    if (s.contains(',') || s.contains('"') || s.contains('\r') || s.contains('\n'))
        return '"' + QString(s).replace("\"", "\"\"") + '"';
    return s;
}

QByteArray readAll(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) fail(QString("cannot read %1").arg(path));
    return f.readAll();
}

QString baseName(const QString& path) { return QFileInfo(path).fileName(); }

// ---- LibreRamen CSV (single spectrum or series table)
std::vector<DataSpectrum> readOwn(const QStringList& lines, const QString& base, const QString& path) {
    Meta meta;
    std::vector<QStringList> rows;
    for (const QString& line : lines) {
        if (line.startsWith('#')) {
            const QString t = line.mid(1).trimmed();
            const int c = int(t.indexOf(':'));
            if (c >= 0 && c + 1 < t.size()) setMetaIn(meta, t.left(c).trimmed(), t.mid(c + 1).trimmed());
        } else if (!line.trimmed().isEmpty()) {
            rows.push_back(csvRow(line));
        }
    }
    if (rows.empty()) fail(QString("%1: no data").arg(baseName(path)));
    QStringList hdr;
    for (const QString& h : rows[0]) hdr << h.trimmed();
    std::vector<QStringList> data;
    for (size_t i = 1; i < rows.size(); ++i)
        if (!rows[i].isEmpty() && num(rows[i][0])) data.push_back(rows[i]);
    auto cell = [&](const QStringList& r, int j) {
        if (j >= r.size()) fail(QString("%1: row too short").arg(baseName(path)));
        return r[j];
    };
    const int xi = hdr.contains("raman_shift_cm1") ? int(hdr.indexOf("raman_shift_cm1")) : 0;
    Vec x;
    for (const QStringList& r : data) x.push_back(toFloat(cell(r, xi)));
    std::vector<DataSpectrum> out;
    if (hdr.size() > 2 && hdr[1].startsWith("t=")) {          // series table
        for (int j = 1; j < hdr.size(); ++j) {
            DataSpectrum d;
            d.name = base + " " + hdr[j];
            d.x = x;
            for (const QStringList& r : data) d.y.push_back(num(cell(r, j)).value_or(0.0));
            d.meta = meta;
            setMetaIn(d.meta, "time", hdr[j]);
            d.source = path;
            out.push_back(std::move(d.sorted()));
        }
        return out;
    }
    auto column = [&](const QString& name) -> std::optional<Vec> {
        const int j = int(hdr.indexOf(name));
        if (j < 0) return std::nullopt;
        Vec v;
        for (const QStringList& r : data) {
            const auto n = num(cell(r, j));
            if (!n) return std::nullopt;
            v.push_back(*n);
        }
        return v;
    };
    auto y = column("intensity");
    if (!y) y = column("difference");                         // early test files
    if (!y) fail(QString("%1: no intensity column").arg(baseName(path)));
    auto sig = column("signal");
    if (!sig) sig = column("laser");
    DataSpectrum d;
    d.name = metaIn(meta, "sample", base);
    if (d.name.isEmpty()) d.name = base;
    d.x = x;
    d.y = *y;
    d.meta = meta;
    d.source = path;
    d.signal = sig.value_or(Vec());
    d.background = column("background").value_or(Vec());
    out.push_back(std::move(d.sorted()));
    return out;
}

// ---- vendor CSV / TXT
std::vector<DataSpectrum> readVendor(const QStringList& lines, const QString& base, const QString& path) {
    static const QRegularExpression sep("[,\t]");
    Meta meta;
    Vec x, y;
    bool inData = false;
    for (const QString& line : lines) {
        QStringList parts;
        for (const QString& p : line.split(sep)) parts << p.trimmed();
        if (!inData) {
            if (parts[0].startsWith("Spectrum_Raman_Shift") || parts[0].toLower().startsWith("raman")) {
                inData = true;
            } else if (parts[0].endsWith(':')) {
                QStringList v;
                for (int i = 1; i < parts.size(); ++i)
                    if (!parts[i].isEmpty()) v << parts[i];
                setMetaIn(meta, parts[0].chopped(1), v.join(' '));
            }
            continue;
        }
        if (parts.size() >= 2 && num(parts[0]) && num(parts[1])) {
            x.push_back(*num(parts[0]));
            y.push_back(*num(parts[1]));
        } else if (!x.empty()) {
            break;
        }
    }
    if (x.empty()) fail(QString("%1: the file contains no spectrum data").arg(baseName(path)));
    setMetaIn(meta, "format", "vendor text");
    DataSpectrum d{base, x, y, meta, path, {}, {}};
    return {std::move(d.sorted())};
}

// ---- any text with two numeric columns
std::vector<DataSpectrum> readColumns(const QStringList& lines, const QString& base, const QString& path) {
    static const QRegularExpression sep("[,;\t ]+");
    Vec x, y;
    for (const QString& line : lines) {
        const QStringList parts = line.trimmed().split(sep, Qt::SkipEmptyParts);
        if (parts.size() >= 2 && num(parts[0]) && num(parts[1])) {
            x.push_back(*num(parts[0]));
            y.push_back(*num(parts[1]));
        }
    }
    if (x.empty()) fail(QString("%1: no numeric columns found").arg(path));
    DataSpectrum d{base, x, y, {{"format", "columns"}}, path, {}, {}};
    return {std::move(d.sorted())};
}

Vec linspace(double a, double b, long n) {          // numpy.linspace
    Vec v(size_t(std::max(n, 0L)));
    if (n == 1) v[0] = a;
    if (n < 2) return v;
    const double step = (b - a) / double(n - 1);
    for (long i = 0; i < n; ++i) v[size_t(i)] = double(i) * step + a;
    v[size_t(n - 1)] = b;
    return v;
}

// ---- Galactic SPC (new format, 0x4B)
std::vector<DataSpectrum> readSpc(const QString& path, const QString& base) {
    const QByteArray b = readAll(path);
    const auto* u = reinterpret_cast<const uchar*>(b.constData());
    const qsizetype size = b.size();
    auto need = [&](qsizetype end) {
        if (end > size) fail(QString("%1: truncated SPC file").arg(path));
    };
    need(512);
    const int flags = u[0], ver = u[1], fexp = u[3];
    if (ver != 0x4B) fail(QString("%1: only the new Galactic SPC format (0x4B) is supported").arg(path));
    const quint32 npts = qFromLittleEndian<quint32>(u + 4);
    double first, last;
    {
        const quint64 a = qFromLittleEndian<quint64>(u + 8), c = qFromLittleEndian<quint64>(u + 16);
        std::memcpy(&first, &a, 8);
        std::memcpy(&last, &c, 8);
    }
    const quint32 nsub = qFromLittleEndian<quint32>(u + 24);
    QByteArray cb = b.mid(88, 130);
    const qsizetype nul = cb.indexOf('\0');
    if (nul >= 0) cb.truncate(nul);
    const QString comment = QString::fromLatin1(cb);
    Meta meta = {{"format", "SPC"}, {"comment", comment}};
    for (const QString& item : comment.split(',')) {
        const int eq = int(item.indexOf('='));
        if (eq >= 0 && eq + 1 < item.size()) setMetaIn(meta, item.left(eq).trimmed(), item.mid(eq + 1).trimmed());
    }
    auto f32 = [&](qsizetype off) {
        const quint32 w = qFromLittleEndian<quint32>(u + off);
        float f;
        std::memcpy(&f, &w, 4);
        return double(f);
    };
    qsizetype off = 512;
    Vec xs;
    if (flags & 0x80) {                                       // TXVALS: one x array for all sub-files
        need(off + 4 * qsizetype(npts));
        for (quint32 i = 0; i < npts; ++i) xs.push_back(f32(off + 4 * qsizetype(i)));
        off += 4 * qsizetype(npts);
    }
    std::vector<DataSpectrum> out;
    const quint32 count = std::max<quint32>(1, nsub);
    for (quint32 k = 0; k < count; ++k) {
        need(off + 32);
        const int subExp = u[off + 1];
        off += 32;                                            // sub-file header
        const int e = (fexp == 0x80 || subExp == 0x80) ? subExp : fexp;
        need(off + 4 * qsizetype(npts));
        Vec y(npts);
        for (quint32 i = 0; i < npts; ++i) {
            const qsizetype o = off + 4 * qsizetype(i);
            y[i] = e == 0x80 ? f32(o) : double(qFromLittleEndian<qint32>(u + o)) * std::pow(2.0, e - 32);
        }
        off += 4 * qsizetype(npts);
        DataSpectrum d;
        d.name = nsub <= 1 ? base : QString("%1 #%2").arg(base).arg(k + 1);
        d.x = (flags & 0x80) ? xs : linspace(first, last, long(npts));
        d.y = y;
        d.meta = meta;
        d.source = path;
        out.push_back(std::move(d.sorted()));
    }
    return out;
}

// ---- JCAMP-DX
std::vector<DataSpectrum> readJcamp(const QString& text, const QString& base, const QString& path) {
    static const QRegularExpression numRe(R"([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)");
    static const QRegularExpression pairRe(R"(([-+]?[\d.eE+-]+)\s*,\s*([-+]?[\d.eE+-]+))");
    Meta meta;
    Vec x, y;
    enum { None, XyData, Points } mode = None;
    std::optional<double> firstx, lastx;
    std::optional<long> npts;
    double xf = 1.0, yf = 1.0;
    for (const QString& line : splitLines(text)) {
        if (line.startsWith("##")) {
            const QString rest = line.mid(2);
            const int eq = int(rest.indexOf('='));
            const QString k = (eq >= 0 ? rest.left(eq) : rest).trimmed().toUpper();
            const QString v = eq >= 0 ? rest.mid(eq + 1).trimmed() : QString();
            setMetaIn(meta, k, v);
            if (k == "FIRSTX") firstx = toFloat(v);
            else if (k == "LASTX") lastx = toFloat(v);
            else if (k == "NPOINTS") npts = long(toFloat(v));
            else if (k == "XFACTOR") xf = toFloat(v);
            else if (k == "YFACTOR") yf = toFloat(v);
            else if (k == "XYDATA") mode = XyData;
            else if (k == "XYPOINTS" || k == "PEAK TABLE") mode = Points;
            else if (k == "END") mode = None;
            continue;
        }
        if (mode == XyData) {
            Vec vals;
            for (auto it = numRe.globalMatch(line); it.hasNext();) vals.push_back(toFloat(it.next().captured(0)));
            if (vals.size() > 1)
                for (size_t i = 1; i < vals.size(); ++i) y.push_back(vals[i] * yf);
        } else if (mode == Points) {
            for (auto it = pairRe.globalMatch(line); it.hasNext();) {
                const auto m = it.next();
                x.push_back(toFloat(m.captured(1)) * xf);
                y.push_back(toFloat(m.captured(2)) * yf);
            }
        }
    }
    if (x.empty() && !y.empty()) {
        if (!firstx || !lastx) fail(QString("%1: FIRSTX/LASTX missing").arg(baseName(path)));
        const long n = npts && *npts ? *npts : long(y.size());
        x = linspace(*firstx, *lastx, n);
        if (x.size() > y.size()) x.resize(y.size());
    }
    DataSpectrum d;
    d.name = metaIn(meta, "TITLE", base);
    if (d.name.isEmpty()) d.name = base;
    d.x = x;
    d.y = y;
    d.meta = meta;
    d.source = path;
    return {std::move(d.sorted())};
}

void writeText(const QString& path, const QString& text) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) fail(QString("cannot write %1").arg(path));
    f.write(text.toUtf8());
}

double interp(double v, const Vec& xp, const Vec& fp) {  // numpy.interp (clamped at the ends)
    const size_t n = xp.size();
    if (v <= xp[0]) return fp[0];
    if (v >= xp[n - 1]) return fp[n - 1];
    const size_t j = size_t(std::upper_bound(xp.begin(), xp.end(), v) - xp.begin()) - 1;
    if (v == xp[j]) return fp[j];
    const double slope = (fp[j + 1] - fp[j]) / (xp[j + 1] - xp[j]);
    double r = slope * (v - xp[j]) + fp[j];
    if (std::isnan(r)) {
        r = slope * (v - xp[j + 1]) + fp[j + 1];
        if (std::isnan(r) && fp[j] == fp[j + 1]) r = fp[j];
    }
    return r;
}

}  // namespace

// ----------------------------------------------------------------------------- DataSpectrum
DataSpectrum& DataSpectrum::sorted() {
    std::vector<size_t> o(x.size());
    std::iota(o.begin(), o.end(), 0);
    std::stable_sort(o.begin(), o.end(), [&](size_t a, size_t b) { return x[a] < x[b]; });
    if (y.size() < x.size()) fail(QString("%1: x and y differ in length").arg(name));
    auto perm = [&](const Vec& v) {
        Vec r(o.size());
        for (size_t k = 0; k < o.size(); ++k) r[k] = v[o[k]];
        return r;
    };
    y = perm(y);
    x = perm(x);
    if (signal.size() == o.size()) signal = perm(signal);
    if (background.size() == o.size()) background = perm(background);
    return *this;
}

QString DataSpectrum::metaValue(const QString& key, const QString& fallback) const { return metaIn(meta, key, fallback); }

void DataSpectrum::setMeta(const QString& key, const QString& value) { setMetaIn(meta, key, value); }

// ----------------------------------------------------------------------------- reading
std::vector<DataSpectrum> read(const QString& path) {
    const QString base = QFileInfo(path).completeBaseName();
    const QByteArray raw = readAll(path);
    const QByteArray head = raw.left(4);
    if (path.toLower().endsWith(".spc") ||
        (head.size() > 1 && (uchar(head[1]) == 0x4B || uchar(head[1]) == 0x4C || uchar(head[1]) == 0x4D) &&
         uchar(head[0]) < 0x80))
        return readSpc(path, base);
    const QString text = QString::fromUtf8(raw);
    if (text.trimmed().startsWith("##")) return readJcamp(text, base, path);
    const QStringList lines = splitLines(text);
    bool own = !lines.isEmpty() && lines[0].startsWith("# Cora 5000");
    for (int i = 0; i < std::min<qsizetype>(40, lines.size()) && !own; ++i)
        own = lines[i].startsWith("raman_shift_cm1");
    if (own) return readOwn(lines, base, path);
    static const QRegularExpression vendor(R"(^[A-Za-z_]+:\s*[,\t])");
    for (int i = 0; i < std::min<qsizetype>(5, lines.size()); ++i)
        if (vendor.match(lines[i]).hasMatch()) return readVendor(lines, base, path);
    return readColumns(lines, base, path);
}

// ----------------------------------------------------------------------------- writing
void writeCsv(const QString& path, const DataSpectrum& sp) {
    QString t = "# LibreRamen spectrum: " + sp.name + "\n";
    for (const auto& kv : sp.meta) t += "# " + kv.first + ": " + kv.second + "\n";
    t += "raman_shift_cm1,intensity\n";
    for (size_t i = 0; i < std::min(sp.x.size(), sp.y.size()); ++i) t += fixed(sp.x[i], 3) + "," + fixed(sp.y[i], 4) + "\n";
    writeText(path, t);
}

void writeJcamp(const QString& path, const DataSpectrum& sp) {
    if (sp.x.empty()) fail("no data to write");
    QString t = "##TITLE=" + sp.name + "\n##JCAMP-DX=5.01\n##DATA TYPE=RAMAN SPECTRUM\n##ORIGIN=LibreRamen\n";
    for (const auto& kv : sp.meta) t += "##$" + kv.first.toUpper().replace(' ', '_') + "=" + kv.second + "\n";
    t += "##XUNITS=1/CM\n##YUNITS=ARBITRARY UNITS\n##FIRSTX=" + fixed(sp.x.front(), 4) + "\n##LASTX=" +
         fixed(sp.x.back(), 4) + "\n##NPOINTS=" + QString::number(sp.x.size()) +
         "\n##XFACTOR=1\n##YFACTOR=1\n##XYPOINTS=(XY..XY)\n";
    for (size_t i = 0; i < std::min(sp.x.size(), sp.y.size()); ++i) t += fixed(sp.x[i], 4) + ", " + fixed(sp.y[i], 4) + "\n";
    t += "##END=\n";
    writeText(path, t);
}

void writeTable(const QString& path, const std::vector<DataSpectrum>& spectra, double step) {
    if (spectra.empty()) fail("no spectra to write");
    double lo = -INFINITY, hi = INFINITY;
    for (const DataSpectrum& s : spectra) {
        if (s.x.empty()) fail(QString("%1: no data").arg(s.name));
        lo = std::max(lo, s.x.front());
        hi = std::min(hi, s.x.back());
    }
    const double start = std::ceil(lo);
    const long n = std::max(0L, long(std::ceil((hi - start) / step)));      // numpy.arange(start, hi, step)
    QStringList hdr = {"raman_shift_cm1"};
    for (const DataSpectrum& s : spectra) hdr << csvField(s.name);
    QString t = hdr.join(',') + "\r\n";                                     // csv.writer line terminator
    for (long i = 0; i < n; ++i) {
        const double g = start + double(i) * step;
        QStringList row = {fixed(g, 1)};
        for (const DataSpectrum& s : spectra) row << fixed(interp(g, s.x, s.y), 4);
        t += row.join(',') + "\r\n";
    }
    writeText(path, t);
}

DataSpectrum fromSpectrum(const Spectrum& sp, const QString& name) {
    DataSpectrum d;
    d.meta = sp.meta;
    setMetaIn(d.meta, "channel", QString("%1 nm").arg(sp.channel));
    setMetaIn(d.meta, "exposure_s", pyRepr(sp.exposure_s));
    setMetaIn(d.meta, "averages", QString::number(sp.averages));
    d.name = name.isEmpty() ? metaIn(sp.meta, "sample", "spectrum") : name;
    d.x = sp.shift;
    d.y = sp.intensity;
    d.source = metaIn(sp.meta, "file", "");
    d.signal = sp.signal;
    d.background = sp.background;
    return d.sorted();
}

std::pair<int, std::vector<std::pair<QString, QString>>> selftest(const QString& folder) {
    int ok = 0;
    std::vector<std::pair<QString, QString>> bad;
    const QStringList ext = {".csv", ".txt", ".spc", ".jdx", ".dx"};
    for (QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
        const QString p = it.next();
        if (std::none_of(ext.begin(), ext.end(), [&](const QString& e) { return p.toLower().endsWith(e); })) continue;
        try {
            const auto s = read(p);
            if (s.empty() || s[0].x.size() <= 10) {
                bad.emplace_back(p, QString());
                continue;
            }
            ++ok;
        } catch (const std::exception& e) {
            bad.emplace_back(p, QString::fromStdString(e.what()).left(80));
        }
    }
    return {ok, bad};
}

}  // namespace lr::fileio
