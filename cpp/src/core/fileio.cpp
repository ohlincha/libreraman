#include "fileio.h"

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>
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

// ---- LibreRaman CSV (single spectrum or series table)
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
constexpr int kTMULTI = 0x04, kTRANDM = 0x08, kTORDRD = 0x10, kTXYXYS = 0x40, kTXVALS = 0x80;

double zSeconds(int fztype) {               // factor to seconds of a time z axis, 0 = not a time
    switch (fztype) {
    case 4: return 1.0;                     // XSECS
    case 5: return 60.0;                    // XMINUTS
    case 11: return 86400.0;                // XDAYS
    case 23: return 1e-3;                   // XMSEC
    case 24: return 1e-6;                   // XUSEC
    case 25: return 1e-9;                   // XNSEC
    case 30: return 3600.0;                 // XHOURS
    default: return 0.0;
    }
}

QString gNum(double v) { return QString::number(v, 'g', 7); }   // float32 values, shortest useful form

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
    const int fztype = u[30];
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
    // log block: "key=value" lines (written by MultiEncoder; other programs' logs are read the same way)
    const quint32 flogoff = qFromLittleEndian<quint32>(u + 248);
    if (flogoff >= 512 && qsizetype(flogoff) + 64 <= size) {
        const quint32 sizd = qFromLittleEndian<quint32>(u + flogoff), txto = qFromLittleEndian<quint32>(u + flogoff + 8);
        const qsizetype from = qsizetype(flogoff) + qsizetype(txto);
        const qsizetype to = std::min<qsizetype>(size, qsizetype(flogoff) + qsizetype(sizd));
        if (txto >= 64 && from < to) {
            QByteArray text = b.mid(from, to - from);
            const qsizetype end = text.indexOf('\0');
            if (end >= 0) text.truncate(end);
            for (const QString& line : QString::fromUtf8(text).split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts)) {
                const int eq = int(line.indexOf('='));
                if (eq > 0) setMetaIn(meta, line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
            }
        }
    }
    const bool xyxys = (flags & kTXYXYS) != 0;
    qsizetype off = 512;
    Vec xs;
    if ((flags & kTXVALS) && !xyxys) {                        // TXVALS: one x array for all sub-files
        need(off + 4 * qsizetype(npts));
        for (quint32 i = 0; i < npts; ++i) xs.push_back(f32(off + 4 * qsizetype(i)));
        off += 4 * qsizetype(npts);
    }
    std::vector<DataSpectrum> out;
    Vec zs;
    const quint32 count = std::max<quint32>(1, nsub);
    for (quint32 k = 0; k < count; ++k) {
        need(off + 32);
        const int subExp = u[off + 1];
        zs.push_back(f32(off + 4));
        const quint32 n = xyxys ? qFromLittleEndian<quint32>(u + off + 16) : npts;
        off += 32;                                            // sub-file header
        Vec x;
        if (xyxys) {                                          // TXYXYS: x array in every sub-file
            need(off + 4 * qsizetype(n));
            for (quint32 i = 0; i < n; ++i) x.push_back(f32(off + 4 * qsizetype(i)));
            off += 4 * qsizetype(n);
        }
        const int e = (fexp == 0x80 || subExp == 0x80) ? subExp : fexp;
        need(off + 4 * qsizetype(n));
        Vec y(n);
        for (quint32 i = 0; i < n; ++i) {
            const qsizetype o = off + 4 * qsizetype(i);
            y[i] = e == 0x80 ? f32(o) : double(qFromLittleEndian<qint32>(u + o)) * std::pow(2.0, e - 32);
        }
        off += 4 * qsizetype(n);
        DataSpectrum d;
        d.name = nsub <= 1 ? base : QString("%1 #%2").arg(base).arg(k + 1);
        d.x = xyxys ? x : (flags & kTXVALS) ? xs : linspace(first, last, long(npts));
        d.y = y;
        d.meta = meta;
        d.source = path;
        out.push_back(std::move(d));
    }
    // z values (time series): only when the file says they mean something
    const double fzinc = f32(312);
    const bool zUsed = (flags & kTMULTI) && nsub > 1 &&
                       ((flags & (kTRANDM | kTORDRD)) || fzinc != 0.0 ||
                        std::any_of(zs.begin(), zs.end(), [](double z) { return z != 0.0; }));
    for (size_t k = 0; k < out.size(); ++k) {
        DataSpectrum& d = out[k];
        if (zUsed) {
            // even z (no TORDRD/TRANDM): subtime of the first sub-file + k * fzinc (when fzinc is given)
            const double z = (flags & (kTRANDM | kTORDRD)) || fzinc == 0.0 ? zs[k] : zs[0] + double(k) * fzinc;
            setMetaIn(d.meta, "z", gNum(z));
            if (const double f = zSeconds(fztype); f > 0) {
                setMetaIn(d.meta, "series_elapsed_s", gNum(z * f));
                d.name = QString("%1 t=%2s").arg(base, gNum(z * f));
            }
        }
        d.sorted();
    }
    return out;
}

// ---- JCAMP-DX
struct JcampBlock {
    Meta meta;
    Vec x, y;
    enum { None, XyData, Points } mode = None;
    std::optional<double> firstx, lastx;
    std::optional<long> npts;
    double xf = 1.0, yf = 1.0;
    bool link = false;                                        // ##DATA TYPE=LINK

    void label(const QString& k, const QString& v, const QString& metaKey) {
        setMetaIn(meta, metaKey, v);
        if (k == "FIRSTX") firstx = toFloat(v);
        else if (k == "LASTX") lastx = toFloat(v);
        else if (k == "NPOINTS") npts = long(toFloat(v));
        else if (k == "XFACTOR") xf = toFloat(v);
        else if (k == "YFACTOR") yf = toFloat(v);
        else if (k == "XYDATA") mode = XyData;
        else if (k == "XYPOINTS" || k == "PEAK TABLE") mode = Points;
        else if (k == "END") mode = None;
        else if (k == "DATA TYPE" && v.trimmed().toUpper() == "LINK") link = true;
    }
    void data(const QString& line) {
        static const QRegularExpression numRe(R"([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)");
        static const QRegularExpression pairRe(R"(([-+]?[\d.eE+-]+)\s*,\s*([-+]?[\d.eE+-]+))");
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
    bool hasData() const { return !x.empty() || !y.empty(); }
    DataSpectrum finish(const QString& name, const QString& path) {
        if (x.empty() && !y.empty()) {
            if (!firstx || !lastx) fail(QString("%1: FIRSTX/LASTX missing").arg(baseName(path)));
            const long n = npts && *npts ? *npts : long(y.size());
            x = linspace(*firstx, *lastx, n);
            if (x.size() > y.size()) x.resize(y.size());
        }
        DataSpectrum d;
        d.name = name;
        d.x = x;
        d.y = y;
        d.meta = meta;
        d.source = path;
        return std::move(d.sorted());
    }
};

// "##KEY=value" -> (normalised label, value); false for data lines
bool jcampLabel(const QString& line, QString& k, QString& v) {
    if (!line.startsWith("##")) return false;
    const QString rest = line.mid(2);
    const int eq = int(rest.indexOf('='));
    k = (eq >= 0 ? rest.left(eq) : rest).trimmed().toUpper();
    v = eq >= 0 ? rest.mid(eq + 1).trimmed() : QString();
    return true;
}

std::vector<DataSpectrum> readJcamp(const QString& text, const QString& base, const QString& path) {
    JcampBlock b;
    QString k, v;
    for (const QString& line : splitLines(text)) {
        if (jcampLabel(line, k, v)) b.label(k, v, k);
        else b.data(line);
    }
    QString name = metaIn(b.meta, "TITLE", base);
    if (name.isEmpty()) name = base;
    return {b.finish(name, path)};
}

// Compound file (##BLOCKS=): a LINK block enclosing data blocks. Every ##TITLE= starts a block; a block
// that is still open then (the LINK block) is the parent of the following ones.
std::vector<DataSpectrum> readJcampLinked(const QString& text, const QString& base, const QString& path) {
    auto metaKey = [](const QString& k) { return k.startsWith('$') ? k.mid(1).trimmed().toLower() : k; };
    std::vector<DataSpectrum> out;
    Meta parent;                                              // ##$ labels of the LINK block
    std::optional<JcampBlock> cur;
    QString k, v;
    for (const QString& line : splitLines(text)) {
        if (!jcampLabel(line, k, v)) {
            if (cur) cur->data(line);
            continue;
        }
        if (k == "TITLE") {
            if (cur && (cur->link || !cur->hasData()))         // the enclosing LINK block
                for (const auto& kv : cur->meta)
                    if (kv.first == kv.first.toLower()) setMetaIn(parent, kv.first, kv.second);
            cur.emplace();
            cur->meta = parent;
        }
        if (!cur) continue;                                   // labels after the end of a block
        if (k == "END") {
            if (!cur->link && cur->hasData()) {
                QString name = metaIn(cur->meta, "TITLE", QString());
                if (name.isEmpty()) name = QString("%1 #%2").arg(base).arg(out.size() + 1);
                out.push_back(cur->finish(name, path));
            }
            cur.reset();
            continue;
        }
        cur->label(k, v, metaKey(k));
    }
    if (cur && !cur->link && cur->hasData()) {                // missing ##END=
        QString name = metaIn(cur->meta, "TITLE", QString());
        if (name.isEmpty()) name = QString("%1 #%2").arg(base).arg(out.size() + 1);
        out.push_back(cur->finish(name, path));
    }
    if (out.empty()) fail(QString("%1: no spectra in the JCAMP-DX blocks").arg(baseName(path)));
    return out;
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
    if (text.trimmed().startsWith("##")) {
        static const QRegularExpression blocks(R"(^##\s*BLOCKS\s*=)",
                                               QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
        return blocks.match(text).hasMatch() ? readJcampLinked(text, base, path) : readJcamp(text, base, path);
    }
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
    QString t = "# LibreRaman spectrum: " + sp.name + "\n";
    for (const auto& kv : sp.meta) t += "# " + kv.first + ": " + kv.second + "\n";
    t += "raman_shift_cm1,intensity\n";
    for (size_t i = 0; i < std::min(sp.x.size(), sp.y.size()); ++i) t += fixed(sp.x[i], 3) + "," + fixed(sp.y[i], 4) + "\n";
    writeText(path, t);
}

void writeJcamp(const QString& path, const DataSpectrum& sp) {
    if (sp.x.empty()) fail("no data to write");
    QString t = "##TITLE=" + sp.name + "\n##JCAMP-DX=5.01\n##DATA TYPE=RAMAN SPECTRUM\n##ORIGIN=LibreRaman\n";
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

// ----------------------------------------------------------------------------- several spectra in one file
namespace {

QString oneLine(QString s) {                        // JCAMP values and SPC log lines: no line breaks
    s.replace(QRegularExpression("[\r\n]+"), " ");
    return s;
}

QString labelKey(const QString& key) {              // meta key -> ##$ label name
    QString k = key.toUpper();
    k.replace(' ', '_');
    k.replace('=', '_');
    return oneLine(k);
}

std::optional<double> elapsedOf(const DataSpectrum& sp) {
    if (const auto v = num(sp.metaValue("series_elapsed_s")); v && std::isfinite(*v)) return v;
    static const QRegularExpression re(R"(^\s*t\s*=\s*([-+0-9.eE]+)\s*s\s*$)");
    const auto m = re.match(sp.metaValue("time"));
    if (m.hasMatch())
        if (const auto v = num(m.captured(1)); v && std::isfinite(*v)) return v;
    return std::nullopt;
}

QDateTime acquiredOf(const DataSpectrum& sp) {
    for (const char* key : {"acquired", "time"}) {
        const QDateTime t = QDateTime::fromString(sp.metaValue(key).trimmed(), Qt::ISODateWithMs);
        if (t.isValid()) return t;
    }
    return {};
}

QString longDate(const QDateTime& t) {              // JCAMP-DX 5.01 ##LONGDATE: YYYY/MM/DD HH:MM:SS +UUUU
    const int off = t.offsetFromUtc() / 60;
    return t.toString("yyyy/MM/dd HH:mm:ss") +
           QString(" %1%2%3").arg(off < 0 ? '-' : '+').arg(std::abs(off) / 60, 2, 10, QChar('0')).arg(std::abs(off) % 60, 2, 10, QChar('0'));
}

QByteArray floats(const Vec& v) {
    QByteArray b(qsizetype(v.size()) * 4, '\0');
    auto* p = reinterpret_cast<uchar*>(b.data());
    for (size_t i = 0; i < v.size(); ++i) {
        const float f = float(v[i]);
        quint32 w;
        std::memcpy(&w, &f, 4);
        qToLittleEndian<quint32>(w, p + 4 * i);
    }
    return b;
}

void putF32(uchar* p, double v) {
    const float f = float(v);
    quint32 w;
    std::memcpy(&w, &f, 4);
    qToLittleEndian<quint32>(w, p);
}

}  // namespace

MultiEncoder::MultiEncoder(Format format, const QString& title, const Meta& fileMeta)
    : m_format(format), m_title(title), m_meta(fileMeta) {}

void MultiEncoder::add(const DataSpectrum& sp) {
    const size_t n = std::min(sp.x.size(), sp.y.size());
    if (n == 0) fail(QString("%1: no data to write").arg(sp.name));
    Item it;
    it.npts = quint32(n);
    it.first = sp.x.front();
    it.last = sp.x[n - 1];
    const auto el = elapsedOf(sp);
    it.hasTime = el.has_value();
    it.z = el.value_or(0.0);
    it.counts = sp.metaValue("processing").isEmpty();
    it.acquired = acquiredOf(sp);
    if (m_format == Format::Jcamp) {
        QString t;
        t.reserve(qsizetype(n) * 24 + 1024);
        t += "##TITLE=" + oneLine(sp.name) + "\n##JCAMP-DX=5.01\n##DATA TYPE=RAMAN SPECTRUM\n##BLOCK_ID=" +
             QString::number(m_items.size() + 1) + "\n##ORIGIN=LibreRaman\n##OWNER=" +
             oneLine(sp.metaValue("operator")) + "\n";
        if (it.acquired.isValid()) t += "##LONGDATE=" + longDate(it.acquired) + "\n";
        for (const auto& kv : sp.meta) t += "##$" + labelKey(kv.first) + "=" + oneLine(kv.second) + "\n";
        t += QString("##XUNITS=1/CM\n##YUNITS=") + (it.counts ? "COUNTS" : "ARBITRARY UNITS") + "\n##FIRSTX=" +
             fixed(it.first, 4) + "\n##LASTX=" + fixed(it.last, 4) + "\n##NPOINTS=" + QString::number(n) +
             "\n##FIRSTY=" + fixed(sp.y.front(), 4) + "\n##XFACTOR=1\n##YFACTOR=1\n##XYPOINTS=(XY..XY)\n";
        for (size_t i = 0; i < n; ++i) {
            t += fixed(sp.x[i], 4);
            t += ", ";
            t += fixed(sp.y[i], 4);
            t += '\n';
        }
        t += "##END=\n";
        it.data = t.toUtf8();
    } else {
        it.xs = floats(Vec(sp.x.begin(), sp.x.begin() + std::ptrdiff_t(n)));
        it.data = floats(Vec(sp.y.begin(), sp.y.begin() + std::ptrdiff_t(n)));
        if (!m_items.empty() && it.xs != m_items.front().xs) m_commonX = false;
    }
    m_size += it.data.size() + it.xs.size();
    m_items.push_back(std::move(it));
}

QByteArray MultiEncoder::bytes() const {
    if (m_items.empty()) fail("no spectra to write");
    const bool allTime = std::all_of(m_items.begin(), m_items.end(), [](const Item& i) { return i.hasTime; });
    const bool counts = std::all_of(m_items.begin(), m_items.end(), [](const Item& i) { return i.counts; });
    const QDateTime t0 = m_items.front().acquired;
    if (m_format == Format::Jcamp) {
        QString h = "##TITLE=" + oneLine(m_title) + "\n##JCAMP-DX=5.01\n##DATA TYPE=LINK\n##BLOCKS=" +
                    QString::number(m_items.size()) + "\n##ORIGIN=LibreRaman\n##OWNER=" +
                    oneLine(metaIn(m_meta, "operator", QString())) + "\n";
        if (t0.isValid()) h += "##LONGDATE=" + longDate(t0) + "\n";
        for (const auto& kv : m_meta) h += "##$" + labelKey(kv.first) + "=" + oneLine(kv.second) + "\n";
        QByteArray out = h.toUtf8();
        out.reserve(out.size() + m_size + 8);
        for (const Item& i : m_items) out += i.data;
        out += "##END=\n";
        return out;
    }
    // ---- SPC
    const int n = int(m_items.size());
    auto zOf = [&](int k) { return allTime ? m_items[size_t(k)].z : double(k); };
    QByteArray log;
    if (!m_meta.empty()) {
        QString text;
        for (const auto& kv : m_meta) text += oneLine(kv.first).remove('=') + "=" + oneLine(kv.second) + "\r\n";
        const QByteArray tb = text.toUtf8() + QByteArray(1, '\0');
        log = QByteArray(64, '\0');
        auto* lp = reinterpret_cast<uchar*>(log.data());
        const quint32 sizd = quint32(64 + tb.size());
        qToLittleEndian<quint32>(sizd, lp);                           // logsizd: size on disk
        qToLittleEndian<quint32>((sizd + 4095) / 4096 * 4096, lp + 4);   // logsizm: memory block, 4096 multiple
        qToLittleEndian<quint32>(64, lp + 8);                         // logtxto: text offset
        log += tb;                                                    // logbins, logdsks, logspar: 0
    }
    QByteArray out(512, '\0');
    auto* h = reinterpret_cast<uchar*>(out.data());
    h[0] = uchar(kTMULTI | kTXVALS | (m_commonX ? 0 : kTXYXYS) | (allTime ? kTORDRD : 0));   // ftflgs
    h[1] = 0x4B;                                                      // fversn
    h[2] = 11;                                                        // fexper: Raman
    h[3] = 0x80;                                                      // fexp: float32 y
    qToLittleEndian<quint32>(m_commonX ? m_items.front().npts : 0, h + 4);   // fnpts (XYXYS: directory, none)
    double first = m_items.front().first, last = m_items.front().last;
    if (!m_commonX)
        for (const Item& i : m_items) {
            first = std::min({first, i.first, i.last});
            last = std::max({last, i.first, i.last});
        }
    quint64 w;
    std::memcpy(&w, &first, 8);
    qToLittleEndian<quint64>(w, h + 8);                               // ffirst
    std::memcpy(&w, &last, 8);
    qToLittleEndian<quint64>(w, h + 16);                              // flast
    qToLittleEndian<quint32>(quint32(n), h + 24);                     // fnsub
    h[28] = 13;                                                       // fxtype: Raman shift (cm-1)
    h[29] = counts ? 4 : 0;                                           // fytype: counts / arbitrary
    h[30] = allTime ? 4 : 0;                                          // fztype: seconds / arbitrary
    if (t0.isValid()) {                                               // fdate: packed local time
        const QDate d = t0.date();
        const QTime tm = t0.time();
        qToLittleEndian<quint32>(quint32(d.year()) << 20 | quint32(d.month()) << 16 | quint32(d.day()) << 11 |
                                     quint32(tm.hour()) << 6 | quint32(tm.minute()),
                                 h + 32);
    }
    std::memcpy(h + 45, "LibreRmn", 8);                               // fsource (9 bytes, NUL-terminated)
    const QByteArray cmnt = (m_title.isEmpty() ? QString("LibreRaman") : oneLine(m_title) + ", LibreRaman").toLatin1().left(129);
    std::memcpy(h + 88, cmnt.constData(), size_t(cmnt.size()));      // fcmnt (130 bytes)
    putF32(h + 312, n > 1 ? (zOf(n - 1) - zOf(0)) / double(n - 1) : 0.0);   // fzinc
    out.reserve(512 + m_size + 32 * n + log.size());
    if (m_commonX) out += m_items.front().xs;                         // TXVALS: the shared x array
    for (int k = 0; k < n; ++k) {
        const Item& i = m_items[size_t(k)];
        uchar sub[32] = {};
        sub[1] = 0x80;                                                // subexp: float32
        qToLittleEndian<quint16>(quint16(k), sub + 2);                // subindx
        putF32(sub + 4, zOf(k));                                      // subtime
        putF32(sub + 8, zOf(k + 1 < n ? k + 1 : k));                  // subnext
        if (!m_commonX) qToLittleEndian<quint32>(i.npts, sub + 16);   // subnpts (XYXYS)
        out.append(reinterpret_cast<const char*>(sub), 32);
        if (!m_commonX) out += i.xs;
        out += i.data;
    }
    if (!log.isEmpty()) {
        qToLittleEndian<quint32>(quint32(out.size()), reinterpret_cast<uchar*>(out.data()) + 248);   // flogoff
        out += log;
    }
    return out;
}

void MultiEncoder::save(const QString& path) const {
    const QByteArray b = bytes();
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(b) != b.size() || !f.commit()) fail(QString("cannot write %1").arg(path));
}

void writeJcampMulti(const QString& path, const std::vector<DataSpectrum>& spectra, const QString& title,
                     const Meta& fileMeta) {
    MultiEncoder e(MultiEncoder::Format::Jcamp, title, fileMeta);
    for (const DataSpectrum& s : spectra) e.add(s);
    e.save(path);
}

void writeSpcMulti(const QString& path, const std::vector<DataSpectrum>& spectra, const QString& title,
                   const Meta& fileMeta) {
    MultiEncoder e(MultiEncoder::Format::Spc, title, fileMeta);
    for (const DataSpectrum& s : spectra) e.add(s);
    e.save(path);
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
