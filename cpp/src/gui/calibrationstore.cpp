#include "calibrationstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

#include <algorithm>
#include <cmath>

#include "core/errors.h"

namespace lr::gui::calstore {

namespace {

const char* const kStamp = "yyyyMMdd-HHmmss";

QByteArray readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeAll(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);                                 // atomic: never a half-written active calibration
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        throw lr::Error(("cannot write " + path).toStdString());
}

QString num(double v, int decimals) { return std::isfinite(v) ? QString::number(v, 'f', decimals) : QString(); }

double toNum(const QString& s) {
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok ? v : qQNaN();
}

// CSV field: quoted if it contains a separator or quote
QString field(const QString& s) {
    if (!s.contains(QLatin1Char(',')) && !s.contains(QLatin1Char('"')))
        return s;
    return QLatin1Char('"') + QString(s).replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
}

QStringList splitCsv(const QString& line) {
    QStringList out;
    QString cur;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (quoted) {
            if (c == QLatin1Char('"') && i + 1 < line.size() && line[i + 1] == QLatin1Char('"')) {
                cur += c;
                ++i;
            } else if (c == QLatin1Char('"')) {
                quoted = false;
            } else {
                cur += c;
            }
        } else if (c == QLatin1Char('"')) {
            quoted = true;
        } else if (c == QLatin1Char(',')) {
            out << cur;
            cur.clear();
        } else {
            cur += c;
        }
    }
    out << cur;
    return out;
}

const QStringList kLogColumns = {
    QStringLiteral("time"),          QStringLiteral("serial"),           QStringLiteral("channel_nm"),
    QStringLiteral("event"),         QStringLiteral("standards"),        QStringLiteral("bands"),
    QStringLiteral("deviation_mean_cm1"), QStringLiteral("deviation_rms_cm1"), QStringLiteral("fit_rms_cm1"),
    QStringLiteral("degree"),        QStringLiteral("dark_counts"),      QStringLiteral("exposure_s"),
    QStringLiteral("note")};

}  // namespace

QString activePath(const QString& serial, int channel) {
    return QDir(calib::configDir()).filePath(QStringLiteral("calibration_%1_%2.json").arg(serial).arg(channel));
}

QString historyPath(const QString& serial, int channel, const QDateTime& t) {
    const QString base = QStringLiteral("calibration_%1_%2_%3").arg(serial).arg(channel).arg(t.toString(QLatin1String(kStamp)));
    QString path = QDir(calib::configDir()).filePath(base + QStringLiteral(".json"));
    for (int k = 2; QFileInfo::exists(path); ++k)
        path = QDir(calib::configDir()).filePath(base + QStringLiteral("_%1.json").arg(k));
    return path;
}

std::optional<HistoryEntry> readEntry(const QString& path) {
    const QByteArray data = readAll(path);
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;
    const QJsonObject o = doc.object();
    if (!o.value(QLatin1String("poly")).toObject().value(QLatin1String("coeff")).isArray())
        return std::nullopt;
    HistoryEntry e;
    e.path = path;
    e.date = o.value(QLatin1String("date")).toString();
    e.channel = o.value(QLatin1String("channel")).toInt();
    for (const QJsonValue& v : o.value(QLatin1String("standards")).toArray())
        e.standards << v.toString();
    e.bands = int(o.value(QLatin1String("residuals")).toArray().size());
    e.rms = o.value(QLatin1String("rms")).toDouble();
    e.degree = o.value(QLatin1String("degree")).toInt(int(o.value(QLatin1String("poly")).toObject().value(QLatin1String("coeff")).toArray().size()) - 1);
    e.kind = o.value(QLatin1String("kind")).toString(e.bands == 1 ? QStringLiteral("offset") : QStringLiteral("polynomial"));
    static const QRegularExpression stamp(QStringLiteral("_(\\d{8}-\\d{6})(?:_\\d+)?\\.json$"));
    const auto m = stamp.match(path);
    e.time = m.hasMatch() ? QDateTime::fromString(m.captured(1), QLatin1String(kStamp))
                          : QDateTime::fromString(e.date, QStringLiteral("yyyy-MM-dd HH:mm"));
    if (!e.time.isValid())
        e.time = QFileInfo(path).lastModified();
    return e;
}

std::vector<HistoryEntry> history(const QString& serial, int channel) {
    std::vector<HistoryEntry> out;
    const QString active = activePath(serial, channel);
    const QByteArray activeData = readAll(active);
    const QDir dir(calib::configDir());
    const QString prefix = QStringLiteral("calibration_%1_%2_").arg(serial).arg(channel);
    bool archived = false;
    for (const QString& name : dir.entryList({prefix + QStringLiteral("*.json")}, QDir::Files)) {
        auto e = readEntry(dir.filePath(name));
        if (!e)
            continue;
        e->active = !activeData.isEmpty() && readAll(e->path) == activeData;
        archived = archived || e->active;
        out.push_back(*e);
    }
    if (!activeData.isEmpty() && !archived) {
        if (auto e = readEntry(active)) {
            e->active = true;
            e->activeOnly = true;
            out.push_back(*e);
        }
    }
    std::sort(out.begin(), out.end(), [](const HistoryEntry& a, const HistoryEntry& b) {
        return a.time != b.time ? a.time > b.time : a.path > b.path;
    });
    return out;
}

QString accept(const calib::Result& r, int channel, const QString& serial, const QString& kind,
               const QStringList& spectra, double laserNmEquiv) {
    const QString hist = historyPath(serial, channel, QDateTime::currentDateTime());
    calib::save(r, channel, serial, hist);
    QJsonObject o = QJsonDocument::fromJson(readAll(hist)).object();
    o[QLatin1String("kind")] = kind;
    if (!spectra.isEmpty())
        o[QLatin1String("spectra")] = QJsonArray::fromStringList(spectra);
    if (laserNmEquiv > 0)
        o[QLatin1String("laser_nm_equiv")] = laserNmEquiv;
    o[QLatin1String("software")] = QStringLiteral("LibreRamen " LIBRERAMEN_VERSION);
    const QByteArray data = QJsonDocument(o).toJson(QJsonDocument::Indented);
    writeAll(hist, data);
    writeAll(activePath(serial, channel), data);
    return hist;
}

void makeActive(const QString& historyFile, int channel, const QString& serial) {
    const QByteArray data = readAll(historyFile);
    if (data.isEmpty() || !readEntry(historyFile))
        throw lr::Error(("not a calibration file: " + historyFile).toStdString());
    if (QFileInfo(historyFile).absoluteFilePath() == QFileInfo(activePath(serial, channel)).absoluteFilePath())
        return;
    writeAll(activePath(serial, channel), data);
}

bool revertToFactory(int channel, const QString& serial) {
    const QString active = activePath(serial, channel);
    if (!QFileInfo::exists(active))
        return false;
    for (const HistoryEntry& e : history(serial, channel))
        if (e.activeOnly) {                            // keep it: archive under its own date
            QDateTime t = QDateTime::fromString(e.date, QStringLiteral("yyyy-MM-dd HH:mm"));
            if (!t.isValid())
                t = QFileInfo(active).lastModified();
            writeAll(historyPath(serial, channel, t), readAll(active));
        }
    if (!QFile::remove(active))
        throw lr::Error(("cannot remove " + active).toStdString());
    return true;
}

// ------------------------------------------------------------------------------------------------ log
QString logPath(const QString& serial) {
    return QDir(calib::configDir()).filePath(QStringLiteral("instrument-log_%1.csv").arg(serial));
}

void appendLog(const LogEntry& e) {
    const QString path = logPath(e.serial);
    QDir().mkpath(QFileInfo(path).absolutePath());
    const bool fresh = !QFileInfo::exists(path) || QFileInfo(path).size() == 0;
    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text))
        throw lr::Error(("cannot write " + path).toStdString());
    QTextStream o(&f);
    if (fresh)
        o << kLogColumns.join(QLatin1Char(',')) << "\n";
    const QStringList row = {e.time.toString(Qt::ISODate),
                             field(e.serial),
                             QString::number(e.channel),
                             field(e.event),
                             field(e.standards.join(QStringLiteral("; "))),
                             QString::number(e.bands),
                             num(e.devMean, 3),
                             num(e.devRms, 3),
                             num(e.fitRms, 3),
                             e.degree >= 0 ? QString::number(e.degree) : QString(),
                             num(e.dark, 1),
                             num(e.exposure, 3),
                             field(e.note)};
    o << row.join(QLatin1Char(',')) << "\n";
}

std::vector<LogEntry> readLog(const QString& serial) {
    std::vector<LogEntry> out;
    QFile f(logPath(serial));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (int i = 1; i < lines.size(); ++i) {           // line 0: header
        const QStringList c = splitCsv(lines[i]);
        if (c.size() < kLogColumns.size())
            continue;
        LogEntry e;
        e.time = QDateTime::fromString(c[0], Qt::ISODate);
        if (!e.time.isValid())
            continue;
        e.serial = c[1];
        e.channel = c[2].toInt();
        e.event = c[3];
        e.standards = c[4].isEmpty() ? QStringList() : c[4].split(QStringLiteral("; "));
        e.bands = c[5].toInt();
        e.devMean = toNum(c[6]);
        e.devRms = toNum(c[7]);
        e.fitRms = toNum(c[8]);
        e.degree = c[9].isEmpty() ? -1 : c[9].toInt();
        e.dark = toNum(c[10]);
        e.exposure = toNum(c[11]);
        e.note = c[12];
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(), [](const LogEntry& a, const LogEntry& b) { return a.time < b.time; });
    return out;
}

// ------------------------------------------------------------------------------------------------ check
CheckResult check(const std::vector<std::pair<QString, std::vector<calib::Match>>>& matches,
                  const calib::ShiftFn& current) {
    CheckResult r;
    double s = 0, ss = 0;
    for (const auto& [name, ms] : matches)
        for (const calib::Match& m : ms) {
            Deviation d;
            d.standard = name;
            d.reference = m.reference;
            d.pixel = m.pixel;
            d.strength = m.strength;
            d.measured = current(m.pixel);
            r.bands.push_back(d);
            s += d.deviation();
            ss += d.deviation() * d.deviation();
            r.maxAbs = std::max(r.maxAbs, std::abs(d.deviation()));
        }
    if (!r.bands.empty()) {
        r.mean = s / double(r.bands.size());
        r.rms = std::sqrt(ss / double(r.bands.size()));
    }
    return r;
}

std::optional<qint64> daysSince(const QString& date, const QDateTime& now) {
    QDateTime t = QDateTime::fromString(date, QStringLiteral("yyyy-MM-dd HH:mm"));
    if (!t.isValid())
        t = QDateTime::fromString(date, Qt::ISODate);
    if (!t.isValid())
        t = QDateTime(QDate::fromString(date.left(10), QStringLiteral("yyyy-MM-dd")), QTime(0, 0));
    if (!t.isValid())
        return std::nullopt;
    return t.date().daysTo(now.date());
}

double equivalentLaserNm(double laserNm, double offset) { return 1e7 / (1e7 / laserNm + offset); }

}  // namespace lr::gui::calstore
