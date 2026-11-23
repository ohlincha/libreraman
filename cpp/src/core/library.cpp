#include "library.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QTextStream>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <numeric>
#include <unordered_map>

#include "calibration.h"
#include "errors.h"
#include "peaks.h"
#include "processing.h"
#include "simulator.h"
#include "standards.h"

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef LIBRERAMEN_VERSION
#define LIBRERAMEN_VERSION "dev"
#endif

namespace lr::library {

namespace {

[[noreturn]] void fail(const QString& msg) { throw Error(msg.toStdString()); }

QString nowIso() {
    const QDateTime n = QDateTime::currentDateTime();
    return n.toOffsetFromUtc(n.offsetFromUtc()).toString(Qt::ISODateWithMs);
}
QString isoOf(const QDateTime& t) { return t.isValid() ? t.toString(Qt::ISODateWithMs) : nowIso(); }

// ---- schema and migrations: kMigrations[v] upgrades a file from version v to v + 1.
const std::vector<std::vector<const char*>>& migrations() {
    static const std::vector<std::vector<const char*>> m = {
        {   // 0 -> 1
            "CREATE TABLE IF NOT EXISTS info (key TEXT PRIMARY KEY, value TEXT)",
            "CREATE TABLE entries ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " name TEXT NOT NULL,"
            " formula TEXT NOT NULL DEFAULT '',"
            " cas TEXT NOT NULL DEFAULT '',"
            " class TEXT NOT NULL DEFAULT '',"
            " tags TEXT NOT NULL DEFAULT '',"
            " source TEXT NOT NULL CHECK (source IN ('own','rruff','literature','computed','vendor')),"
            " source_id TEXT NOT NULL DEFAULT '',"
            " licence TEXT NOT NULL DEFAULT '',"
            " laser_nm REAL,"
            " conditions TEXT NOT NULL DEFAULT '',"
            " operator TEXT NOT NULL DEFAULT '',"
            " notes TEXT NOT NULL DEFAULT '',"
            " created TEXT NOT NULL,"
            " modified TEXT NOT NULL,"
            " provenance TEXT NOT NULL DEFAULT '{}')",
            "CREATE TABLE spectra ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,"
            " label TEXT NOT NULL DEFAULT '',"
            " processed INTEGER NOT NULL DEFAULT 0,"
            " n INTEGER NOT NULL,"
            " xmin REAL, xmax REAL,"
            " x BLOB NOT NULL, y BLOB NOT NULL)",
            "CREATE TABLE peaks ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,"
            " position REAL NOT NULL,"
            " intensity REAL NOT NULL DEFAULT 0,"
            " width REAL NOT NULL DEFAULT 0,"
            " label TEXT NOT NULL DEFAULT '')",
            "CREATE INDEX entries_name ON entries(name COLLATE NOCASE)",
            "CREATE INDEX entries_source ON entries(source)",
            "CREATE INDEX entries_class ON entries(class COLLATE NOCASE)",
            "CREATE INDEX spectra_entry ON spectra(entry_id)",
            "CREATE INDEX peaks_entry ON peaks(entry_id)",
        },
    };
    return m;
}

std::atomic<int> g_connections{0};

QString escapeLike(QString s) {
    s.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
    return '%' + s + '%';
}

double parseLaser(const QString& s) {           // "532", "532 nm", "532.1"
    static const QRegularExpression re(R"(([0-9]+(?:\.[0-9]+)?))");
    const auto m = re.match(s);
    return m.hasMatch() ? m.captured(1).toDouble() : 0.0;
}

// Null QStrings are bound as SQL NULL: bind text through this.
QString txt(const QString& s) { return s.isNull() ? QStringLiteral("") : s; }

QString fmt(double v) { return QString::number(v, 'g', 12); }

// Peaks below this shift are not picked (laser line, notch-filter edge, anti-Stokes side).
constexpr double kMinPeakShift = 50.0;

// Baseline of the automatic peak tables, recorded in info 'peak_baseline' as "method:strength" (older files:
// "snip4" = 0.3.0/0.3.1, nothing = classic SNIP, both at strength 5). Tables picked with an older setting are
// re-picked once when the library is opened for writing (issue #4); edited tables are kept.
const QString kPeakBaseline = QStringLiteral("arpls:6");

// Peaks of a spectrum for the peak table: arPLS baseline (kPeakBaseline), noise-based picking, relative heights.
std::vector<PeakEntry> autoPeaks(const Vec& x, const Vec& y, const QString& spec = kPeakBaseline) {
    if (x.size() < 16) return {};
    const QStringList ms = (spec.isEmpty() ? QStringLiteral("snip") : spec).split(QLatin1Char(':'));
    Vec base = processing::baseline(x, y, ms[0], ms.size() > 1 ? ms[1].toDouble() : 5.0);
    Vec yc(y.size());
    for (size_t i = 0; i < y.size(); ++i) yc[i] = y[i] - base[i];
    std::vector<peaks::Peak> pk = peaks::pick(x, yc, 5.0, 2.0, false, kMinPeakShift);
    std::sort(pk.begin(), pk.end(), [](const peaks::Peak& a, const peaks::Peak& b) { return a.height > b.height; });
    if (pk.size() > 40) pk.resize(40);
    const double hmax = pk.empty() ? 1.0 : std::max(pk.front().height, 1e-300);
    std::vector<PeakEntry> out;
    for (const auto& p : pk) out.push_back({p.position, p.height / hmax, p.fwhm, {}});
    std::sort(out.begin(), out.end(), [](const PeakEntry& a, const PeakEntry& b) { return a.position < b.position; });
    return out;
}

// Fill empty fields of e from defaults d (name, formula, ... ; laser if 0; provenance keys missing in e).
void fillFrom(Entry& e, const Entry& d) {
    auto f = [](QString& a, const QString& b) {
        if (a.isEmpty()) a = b;
    };
    f(e.name, d.name);
    f(e.formula, d.formula);
    f(e.cas, d.cas);
    f(e.cls, d.cls);
    f(e.sourceId, d.sourceId);
    f(e.licence, d.licence);
    f(e.conditions, d.conditions);
    f(e.op, d.op);
    f(e.notes, d.notes);
    if (e.tags.isEmpty()) e.tags = d.tags;
    if (e.laserNm <= 0) e.laserNm = d.laserNm;
    for (auto it = d.provenance.begin(); it != d.provenance.end(); ++it)
        if (!e.provenance.contains(it.key())) e.provenance.insert(it.key(), it.value());
}

void sortXY(Vec& x, Vec& y) {
    const size_t n = std::min(x.size(), y.size());
    x.resize(n);
    y.resize(n);
    if (std::is_sorted(x.begin(), x.end())) return;
    std::vector<size_t> idx(n);
    std::iota(idx.begin(), idx.end(), size_t(0));
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return x[a] < x[b]; });
    Vec xs(n), ys(n);
    for (size_t i = 0; i < n; ++i) {
        xs[i] = x[idx[i]];
        ys[i] = y[idx[i]];
    }
    x.swap(xs);
    y.swap(ys);
}

}  // namespace

// ------------------------------------------------------------------------------------------------ sources
const std::vector<Source>& allSources() {
    static const std::vector<Source> s = {Source::Own, Source::Rruff, Source::Literature, Source::Computed,
                                          Source::Vendor};
    return s;
}

QString sourceKey(Source s) {
    switch (s) {
    case Source::Own: return QStringLiteral("own");
    case Source::Rruff: return QStringLiteral("rruff");
    case Source::Literature: return QStringLiteral("literature");
    case Source::Computed: return QStringLiteral("computed");
    case Source::Vendor: return QStringLiteral("vendor");
    }
    return QStringLiteral("own");
}

QString sourceLabel(Source s) {
    switch (s) {
    case Source::Own: return QStringLiteral("Own");
    case Source::Rruff: return QStringLiteral("RRUFF");
    case Source::Literature: return QStringLiteral("Literature");
    case Source::Computed: return QStringLiteral("Computed");
    case Source::Vendor: return QStringLiteral("Vendor");
    }
    return QStringLiteral("Own");
}

Source sourceFromKey(const QString& key) {
    const QString k = key.trimmed().toLower();
    for (Source s : allSources())
        if (k == sourceKey(s) || k == sourceLabel(s).toLower()) return s;
    fail(QStringLiteral("unknown library source '%1'").arg(key));
}

const SpectrumData* Entry::preferredSpectrum() const {
    for (const auto& s : spectra)
        if (s.processed && !s.x.empty()) return &s;
    for (const auto& s : spectra)
        if (!s.x.empty()) return &s;
    return nullptr;
}

// ------------------------------------------------------------------------------------------------ blobs
QByteArray toBlob(const Vec& v) {
    QByteArray b(int(v.size() * 8), Qt::Uninitialized);
    char* p = b.data();
    for (double d : v) {
        quint64 u;
        std::memcpy(&u, &d, 8);
        qToLittleEndian(u, p);
        p += 8;
    }
    return b;
}

Vec fromBlob(const QByteArray& b) {
    const size_t n = size_t(b.size()) / 8;
    Vec v(n);
    const char* p = b.constData();
    for (size_t i = 0; i < n; ++i, p += 8) {
        const quint64 u = qFromLittleEndian<quint64>(p);
        std::memcpy(&v[i], &u, 8);
    }
    return v;
}

// ------------------------------------------------------------------------------------------------ Impl
struct Library::Impl {
    QSqlDatabase db;
    QString conn;
    QString path;
    bool readOnly = false;
    bool network = false;
    QString journal;
    int version = 0;
    int txDepth = 0;

    QSqlQuery query() const { return QSqlQuery(db); }

    void exec(QSqlQuery& q) const {
        if (!q.exec()) fail(QStringLiteral("library database error: %1").arg(q.lastError().text()));
    }
    QSqlQuery exec(const QString& sql) const {
        QSqlQuery q(db);
        if (!q.exec(sql)) fail(QStringLiteral("library database error: %1").arg(q.lastError().text()));
        return q;
    }
    QVariant scalar(const QString& sql) const {
        QSqlQuery q = exec(sql);
        return q.next() ? q.value(0) : QVariant();
    }
    void requireOpen() const {
        if (!db.isOpen()) fail(QStringLiteral("no library is open"));
    }
    void requireWritable() const {
        requireOpen();
        if (readOnly) fail(QStringLiteral("the library %1 is open read-only").arg(QDir::toNativeSeparators(path)));
    }

    // Nested transactions: only the outermost one talks to SQLite.
    struct Tx {
        Impl& d;
        bool done = false;
        explicit Tx(Impl& impl) : d(impl) {
            if (d.txDepth++ == 0 && !d.db.transaction())
                fail(QStringLiteral("library database error: %1").arg(d.db.lastError().text()));
        }
        void commit() {
            done = true;
            if (--d.txDepth == 0 && !d.db.commit())
                fail(QStringLiteral("library database error: %1").arg(d.db.lastError().text()));
        }
        ~Tx() {
            if (!done && --d.txDepth == 0) d.db.rollback();
        }
        Tx(const Tx&) = delete;
        Tx& operator=(const Tx&) = delete;
    };

    void migrate() {
        const auto& m = migrations();
        if (version >= int(m.size())) return;
        if (readOnly)
            fail(QStringLiteral("the library %1 must be upgraded (schema %2 -> %3) but is open read-only")
                     .arg(QDir::toNativeSeparators(path)).arg(version).arg(Library::kSchemaVersion));
        Tx tx(*this);
        const int from = version;
        for (int v = from; v < int(m.size()); ++v) {
            for (const char* sql : m[size_t(v)]) exec(QString::fromLatin1(sql));
            exec(QStringLiteral("PRAGMA user_version = %1").arg(v + 1));
        }
        QSqlQuery q(db);
        q.prepare("INSERT OR REPLACE INTO info(key, value) VALUES (?, ?)");
        auto put = [&](const QString& k, const QString& v) {
            q.addBindValue(k);
            q.addBindValue(v);
            exec(q);
        };
        if (from == 0) {
            put("format", "LibreRamen spectral library");
            put("created", nowIso());
            put("created_by", QStringLiteral("LibreRamen %1").arg(LIBRERAMEN_VERSION));
        }
        const QString hist = scalar("SELECT value FROM info WHERE key = 'schema_history'").toString();
        put("schema_history", (hist.isEmpty() ? QString() : hist + "; ") +
                                  QStringLiteral("%1->%2 %3 (LibreRamen %4)").arg(from).arg(m.size()).arg(nowIso(),
                                                                                                   LIBRERAMEN_VERSION));
        tx.commit();
        version = int(m.size());
    }

    // Issue #4: peak tables used to be picked on a classic-SNIP baseline. Re-pick, once, every table that is
    // still exactly what the old automatic picking gives (edited or imported peak lists are left alone), then
    // record the method. Read-only files keep their tables.
    void upgradePeaks() {
        if (readOnly) return;
        // peak tables picked automatically with the previous default (none recorded: classic SNIP, <= 0.2.4;
        // "snip4": 0.3.x) are re-picked with the current one; edited tables are kept
        const QString prev = scalar("SELECT value FROM info WHERE key = 'peak_baseline'").toString();
        if (prev == kPeakBaseline) return;
        Tx tx(*this);
        std::vector<qint64> ids;
        for (QSqlQuery q = exec("SELECT id FROM entries"); q.next();) ids.push_back(q.value(0).toLongLong());
        QSqlQuery qs(db), qp(db), del(db), ins(db);
        qs.prepare("SELECT x, y FROM spectra WHERE entry_id = ? ORDER BY id LIMIT 1");
        qp.prepare("SELECT position, intensity, width, label FROM peaks WHERE entry_id = ? ORDER BY position");
        del.prepare("DELETE FROM peaks WHERE entry_id = ?");
        ins.prepare("INSERT INTO peaks(entry_id, position, intensity, width, label) VALUES (?,?,?,?,?)");
        auto same = [](double a, double b, double tol) { return std::abs(a - b) <= tol * std::max(1.0, std::abs(a)); };
        for (qint64 id : ids) {
            qs.addBindValue(id);
            exec(qs);
            if (!qs.next()) continue;
            const Vec x = fromBlob(qs.value(0).toByteArray()), y = fromBlob(qs.value(1).toByteArray());
            qs.finish();
            std::vector<PeakEntry> stored;
            qp.addBindValue(id);
            exec(qp);
            while (qp.next())
                stored.push_back({qp.value(0).toDouble(), qp.value(1).toDouble(), qp.value(2).toDouble(),
                                  qp.value(3).toString()});
            qp.finish();
            const std::vector<PeakEntry> old = autoPeaks(x, y, prev);
            bool automatic = !stored.empty() && stored.size() == old.size();
            for (size_t i = 0; automatic && i < old.size(); ++i)
                automatic = stored[i].label.isEmpty() && same(stored[i].position, old[i].position, 1e-9) &&
                            same(stored[i].intensity, old[i].intensity, 1e-9) &&
                            same(stored[i].width, old[i].width, 1e-9);
            if (!automatic) continue;
            del.addBindValue(id);
            exec(del);
            for (const PeakEntry& p : autoPeaks(x, y)) {
                ins.addBindValue(id);
                ins.addBindValue(p.position);
                ins.addBindValue(p.intensity);
                ins.addBindValue(p.width);
                ins.addBindValue(QString(""));
                exec(ins);
            }
        }
        QSqlQuery put(db);
        put.prepare("INSERT OR REPLACE INTO info(key, value) VALUES ('peak_baseline', ?)");
        put.addBindValue(kPeakBaseline);
        exec(put);
        tx.commit();
    }

    Entry readEntry(const QSqlQuery& q) const {   // columns of kEntryCols
        Entry e;
        e.id = q.value(0).toLongLong();
        e.name = q.value(1).toString();
        e.formula = q.value(2).toString();
        e.cas = q.value(3).toString();
        e.cls = q.value(4).toString();
        const QString tags = q.value(5).toString();
        for (const QString& t : tags.split(',', Qt::SkipEmptyParts))
            if (!t.trimmed().isEmpty()) e.tags << t.trimmed();
        try {
            e.source = sourceFromKey(q.value(6).toString());
        } catch (const Error&) {
            e.source = Source::Own;
        }
        e.sourceId = q.value(7).toString();
        e.licence = q.value(8).toString();
        e.laserNm = q.value(9).isNull() ? 0.0 : q.value(9).toDouble();
        e.conditions = q.value(10).toString();
        e.op = q.value(11).toString();
        e.notes = q.value(12).toString();
        e.created = QDateTime::fromString(q.value(13).toString(), Qt::ISODateWithMs);
        e.modified = QDateTime::fromString(q.value(14).toString(), Qt::ISODateWithMs);
        e.provenance = QJsonDocument::fromJson(q.value(15).toString().toUtf8()).object();
        e.spectrumCount = q.value(16).toInt();
        e.peakCount = q.value(17).toInt();
        return e;
    }

    void bindEntry(QSqlQuery& q, const Entry& e) const {
        q.bindValue(":name", e.name.trimmed().isEmpty() ? QStringLiteral("unnamed") : e.name.trimmed());
        q.bindValue(":formula", txt(e.formula.trimmed()));
        q.bindValue(":cas", txt(e.cas.trimmed()));
        q.bindValue(":class", txt(e.cls.trimmed()));
        QStringList tags;
        for (const QString& t : e.tags)
            if (!t.trimmed().isEmpty()) tags << QString(t).remove(',').trimmed();
        q.bindValue(":tags", txt(tags.join(", ")));
        q.bindValue(":source", sourceKey(e.source));
        q.bindValue(":source_id", txt(e.sourceId));
        q.bindValue(":licence", txt(e.licence));
        q.bindValue(":laser_nm", e.laserNm > 0 ? QVariant(e.laserNm) : QVariant(QMetaType(QMetaType::Double)));
        q.bindValue(":conditions", txt(e.conditions));
        q.bindValue(":operator", txt(e.op));
        q.bindValue(":notes", txt(e.notes));
        q.bindValue(":provenance", QString::fromUtf8(QJsonDocument(e.provenance).toJson(QJsonDocument::Compact)));
    }

    void writeData(qint64 id, const Entry& e) {
        QSqlQuery qs(db);
        qs.prepare("INSERT INTO spectra(entry_id, label, processed, n, xmin, xmax, x, y) VALUES (?,?,?,?,?,?,?,?)");
        for (const SpectrumData& s0 : e.spectra) {
            Vec x = s0.x, y = s0.y;
            sortXY(x, y);
            if (x.empty()) continue;
            qs.addBindValue(id);
            qs.addBindValue(txt(s0.label));
            qs.addBindValue(s0.processed ? 1 : 0);
            qs.addBindValue(qint64(x.size()));
            qs.addBindValue(x.front());
            qs.addBindValue(x.back());
            qs.addBindValue(toBlob(x));
            qs.addBindValue(toBlob(y));
            exec(qs);
        }
        QSqlQuery qp(db);
        qp.prepare("INSERT INTO peaks(entry_id, position, intensity, width, label) VALUES (?,?,?,?,?)");
        for (const PeakEntry& p : e.peaks) {
            if (!std::isfinite(p.position)) continue;
            qp.addBindValue(id);
            qp.addBindValue(p.position);
            qp.addBindValue(std::isfinite(p.intensity) ? p.intensity : 0.0);
            qp.addBindValue(std::isfinite(p.width) ? p.width : 0.0);
            qp.addBindValue(txt(p.label));
            exec(qp);
        }
    }

    qint64 insert(Entry e) {
        QSqlQuery q(db);
        q.prepare("INSERT INTO entries(name, formula, cas, class, tags, source, source_id, licence, laser_nm, conditions,"
                  " operator, notes, created, modified, provenance) VALUES (:name, :formula, :cas, :class, :tags,"
                  " :source, :source_id, :licence, :laser_nm, :conditions, :operator, :notes, :created, :modified,"
                  " :provenance)");
        bindEntry(q, e);
        const QString now = nowIso();
        q.bindValue(":created", e.created.isValid() ? isoOf(e.created) : now);
        q.bindValue(":modified", now);
        exec(q);
        const qint64 id = q.lastInsertId().toLongLong();
        writeData(id, e);
        return id;
    }

    // WHERE clause (and bind values) of a filter on entries (alias-free).
    QString where(const Filter& f, std::vector<QVariant>& binds) const {
        QStringList w;
        if (!f.text.trimmed().isEmpty()) {
            const QString t = escapeLike(f.text.trimmed());
            QStringList ors;
            for (const char* c : {"name", "formula", "cas", "class", "tags", "notes", "source_id"}) {
                ors << QStringLiteral("%1 LIKE ? ESCAPE '\\'").arg(QLatin1String(c));
                binds.push_back(t);
            }
            w << '(' + ors.join(" OR ") + ')';
        }
        if (!f.cls.trimmed().isEmpty()) {
            w << "class = ? COLLATE NOCASE";
            binds.push_back(f.cls.trimmed());
        }
        if (f.source) {
            w << "source = ?";
            binds.push_back(sourceKey(*f.source));
        }
        if (f.laserNm > 0) {
            QString c = "(laser_nm BETWEEN ? AND ?";
            binds.push_back(f.laserNm - f.laserTol);
            binds.push_back(f.laserNm + f.laserTol);
            if (f.includeUnknownLaser) c += " OR laser_nm IS NULL OR laser_nm <= 0";
            w << c + ')';
        }
        if (f.spectraOnly) w << "EXISTS (SELECT 1 FROM spectra s WHERE s.entry_id = entries.id)";
        return w.isEmpty() ? QString() : " WHERE " + w.join(" AND ");
    }
};

namespace {
const char* kEntryCols =
    "id, name, formula, cas, class, tags, source, source_id, licence, laser_nm, conditions, operator, notes, created,"
    " modified, provenance,"
    " (SELECT COUNT(*) FROM spectra s WHERE s.entry_id = entries.id),"
    " (SELECT COUNT(*) FROM peaks p WHERE p.entry_id = entries.id)";
}

// ------------------------------------------------------------------------------------------------ Library
Library::Library() : d(std::make_unique<Impl>()) {}

Library::~Library() { close(); }

bool Library::isNetworkPath(const QString& path) {
    const QString native = QDir::toNativeSeparators(path);
    if (native.startsWith(QLatin1String("\\\\")) || path.startsWith(QLatin1String("//"))) return true;   // UNC
#ifdef Q_OS_WIN
    const QString abs = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (abs.size() >= 2 && abs[1] == QLatin1Char(':')) {
        const std::wstring root = abs.left(2).toStdWString() + L"\\";
        if (GetDriveTypeW(root.c_str()) == DRIVE_REMOTE) return true;
    }
#endif
    QFileInfo fi(path);
    const QString dir = fi.exists() ? fi.absoluteFilePath() : fi.absolutePath();
    const QStorageInfo st(dir);
    if (!st.isValid()) return false;
    const QString fs = QString::fromLatin1(st.fileSystemType()).toLower();
    static const QStringList remote = {"nfs", "nfs4", "cifs", "smb", "smb2", "smb3", "smbfs", "afpfs", "afs",
                                       "ncpfs", "9p", "davfs", "webdav", "fuse.sshfs", "sshfs", "fuse.davfs2",
                                       "fuse.rclone", "glusterfs", "fuse.glusterfs", "ceph", "lustre", "gpfs"};
    return remote.contains(fs);
}

void Library::open(const QString& path, OpenMode mode, bool create) {
    close();
    if (path.isEmpty()) fail(QStringLiteral("no library file given"));
    const QFileInfo fi(path);
    const QString abs = fi.absoluteFilePath();
    const bool exists = fi.exists();
    if (!exists && (mode == OpenMode::ReadOnly || !create))
        fail(QStringLiteral("library file not found: %1").arg(QDir::toNativeSeparators(abs)));
    if (exists && fi.isDir()) fail(QStringLiteral("%1 is a folder, not a library file").arg(QDir::toNativeSeparators(abs)));
    if (!exists && !QDir().mkpath(fi.absolutePath()))
        fail(QStringLiteral("cannot create the folder %1").arg(QDir::toNativeSeparators(fi.absolutePath())));
    if (!QSqlDatabase::isDriverAvailable("QSQLITE"))
        fail(QStringLiteral("the Qt SQLite driver (qsqlite plugin) is not available"));

    d->network = isNetworkPath(abs);
    d->readOnly = mode == OpenMode::ReadOnly;
    d->conn = QStringLiteral("libreramen-library-%1").arg(++g_connections);
    d->db = QSqlDatabase::addDatabase("QSQLITE", d->conn);
    d->db.setDatabaseName(abs);
    QString opts = QStringLiteral("QSQLITE_BUSY_TIMEOUT=%1").arg(d->network ? 15000 : 5000);
    if (d->readOnly) opts += ";QSQLITE_OPEN_READONLY";
    d->db.setConnectOptions(opts);
    d->path = abs;
    if (!d->db.open()) {
        const QString err = d->db.lastError().text();
        close();
        fail(QStringLiteral("cannot open the library %1: %2").arg(QDir::toNativeSeparators(abs), err));
    }
    try {
        QSqlQuery q(d->db);
        if (!q.exec("PRAGMA user_version") || !q.next())
            fail(QStringLiteral("%1 is not a LibreRamen library (%2)")
                     .arg(QDir::toNativeSeparators(abs), q.lastError().text()));
        d->version = q.value(0).toInt();
        q.finish();                     // an active statement keeps a read transaction open
        if (d->version > kSchemaVersion)
            fail(QStringLiteral("the library %1 was written by a newer LibreRamen (schema %2; this version reads up "
                                "to %3)").arg(QDir::toNativeSeparators(abs)).arg(d->version).arg(kSchemaVersion));
        if (d->version == 0) {          // new file, or some other SQLite database
            const int tables = d->scalar("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table'"
                                         " AND name NOT LIKE 'sqlite_%'").toInt();
            if (tables > 0)
                fail(QStringLiteral("%1 is an SQLite file but not a LibreRamen library")
                         .arg(QDir::toNativeSeparators(abs)));
        }
        d->exec("PRAGMA foreign_keys = ON");
        if (!d->readOnly) {
            // WAL is faster and lets readers work during writes, but needs shared memory, which network file
            // systems do not provide reliably: use the rollback journal there.
            const QString want = d->network ? "DELETE" : "WAL";
            d->journal = d->scalar("PRAGMA journal_mode = " + want).toString().toLower();
            if (d->network) d->exec("PRAGMA synchronous = FULL");
        } else {
            d->journal = d->scalar("PRAGMA journal_mode").toString().toLower();
        }
        d->migrate();
        d->upgradePeaks();
    } catch (...) {
        close();
        throw;
    }
}

void Library::close() {
    if (!d || d->conn.isEmpty()) return;
    if (d->db.isOpen()) {
        if (!d->readOnly && d->journal == "wal") {
            // Leave a self-contained file in rollback-journal mode (can then be copied to a network drive or
            // opened read-only); silently stays WAL if another connection still uses it.
            QSqlQuery q(d->db);
            q.exec("PRAGMA busy_timeout = 0");
            q.exec("PRAGMA journal_mode = DELETE");
        }
        d->db.close();
    }
    d->db = QSqlDatabase();
    QSqlDatabase::removeDatabase(d->conn);
    d->conn.clear();
    d->path.clear();
    d->journal.clear();
    d->version = 0;
    d->txDepth = 0;
    d->readOnly = false;
    d->network = false;
}

bool Library::isOpen() const { return d->db.isOpen(); }
bool Library::readOnly() const { return d->readOnly; }
QString Library::path() const { return d->path; }
bool Library::onNetwork() const { return d->network; }
QString Library::journalMode() const { return d->journal; }
int Library::schemaVersion() const { return d->version; }

QString Library::info(const QString& key) const {
    d->requireOpen();
    QSqlQuery q(d->db);
    q.prepare("SELECT value FROM info WHERE key = ?");
    q.addBindValue(key);
    d->exec(q);
    return q.next() ? q.value(0).toString() : QString();
}

void Library::setInfo(const QString& key, const QString& value) {
    d->requireWritable();
    QSqlQuery q(d->db);
    q.prepare("INSERT OR REPLACE INTO info(key, value) VALUES (?, ?)");
    q.addBindValue(key);
    q.addBindValue(value);
    d->exec(q);
}

qint64 Library::add(Entry e) {
    d->requireWritable();
    Impl::Tx tx(*d);
    const qint64 id = d->insert(std::move(e));
    tx.commit();
    return id;
}

void Library::update(const Entry& e) { update(e, false); }

void Library::update(const Entry& e, bool keepData) {
    d->requireWritable();
    Impl::Tx tx(*d);
    QSqlQuery q(d->db);
    q.prepare("UPDATE entries SET name = :name, formula = :formula, cas = :cas, class = :class, tags = :tags,"
              " source = :source, source_id = :source_id, licence = :licence, laser_nm = :laser_nm,"
              " conditions = :conditions, operator = :operator, notes = :notes, modified = :modified,"
              " provenance = :provenance WHERE id = :id");
    d->bindEntry(q, e);
    q.bindValue(":modified", nowIso());
    q.bindValue(":id", e.id);
    d->exec(q);
    if (q.numRowsAffected() != 1) fail(QStringLiteral("library entry %1 not found").arg(e.id));
    if (!keepData) {
        for (const char* t : {"spectra", "peaks"}) {
            QSqlQuery del(d->db);
            del.prepare(QStringLiteral("DELETE FROM %1 WHERE entry_id = ?").arg(QLatin1String(t)));
            del.addBindValue(e.id);
            d->exec(del);
        }
        d->writeData(e.id, e);
    }
    tx.commit();
}

void Library::remove(qint64 id) { remove(std::vector<qint64>{id}); }

void Library::remove(const std::vector<qint64>& ids) {
    d->requireWritable();
    Impl::Tx tx(*d);
    QSqlQuery q(d->db);
    q.prepare("DELETE FROM entries WHERE id = ?");      // spectra and peaks: ON DELETE CASCADE
    for (qint64 id : ids) {
        q.addBindValue(id);
        d->exec(q);
    }
    tx.commit();
}

std::optional<Entry> Library::get(qint64 id, bool withData) const {
    d->requireOpen();
    QSqlQuery q(d->db);
    q.prepare(QStringLiteral("SELECT %1 FROM entries WHERE id = ?").arg(QLatin1String(kEntryCols)));
    q.addBindValue(id);
    d->exec(q);
    if (!q.next()) return std::nullopt;
    Entry e = d->readEntry(q);
    if (withData) {
        QSqlQuery qs(d->db);
        qs.prepare("SELECT id, label, processed, x, y FROM spectra WHERE entry_id = ? ORDER BY id");
        qs.addBindValue(id);
        d->exec(qs);
        while (qs.next())
            e.spectra.push_back({qs.value(0).toLongLong(), qs.value(1).toString(), qs.value(2).toInt() != 0,
                                 fromBlob(qs.value(3).toByteArray()), fromBlob(qs.value(4).toByteArray())});
        QSqlQuery qp(d->db);
        qp.prepare("SELECT position, intensity, width, label FROM peaks WHERE entry_id = ? ORDER BY position");
        qp.addBindValue(id);
        d->exec(qp);
        while (qp.next())
            e.peaks.push_back({qp.value(0).toDouble(), qp.value(1).toDouble(), qp.value(2).toDouble(),
                               qp.value(3).toString()});
    }
    return e;
}

std::vector<Entry> Library::find(const Filter& f, bool withData) const {
    d->requireOpen();
    std::vector<QVariant> binds;
    const QString where = d->where(f, binds);
    QString sql = QStringLiteral("SELECT %1 FROM entries%2 ORDER BY name COLLATE NOCASE, id")
                      .arg(QLatin1String(kEntryCols), where);
    if (f.limit > 0) sql += QStringLiteral(" LIMIT %1").arg(f.limit);
    QSqlQuery q(d->db);
    q.setForwardOnly(true);
    q.prepare(sql);
    for (const QVariant& b : binds) q.addBindValue(b);
    d->exec(q);
    std::vector<Entry> out;
    while (q.next()) out.push_back(d->readEntry(q));
    if (!withData || out.empty()) return out;

    std::unordered_map<qint64, size_t> index;
    for (size_t i = 0; i < out.size(); ++i) index[out[i].id] = i;
    // one query each for all spectra and peaks of the selected entries
    const QString sub = QStringLiteral("SELECT id FROM entries%1").arg(where) +
                        (f.limit > 0 ? QStringLiteral(" ORDER BY name COLLATE NOCASE, id LIMIT %1").arg(f.limit)
                                     : QString());
    QSqlQuery qs(d->db);
    qs.setForwardOnly(true);
    qs.prepare("SELECT entry_id, id, label, processed, x, y FROM spectra WHERE entry_id IN (" + sub + ") ORDER BY id");
    for (const QVariant& b : binds) qs.addBindValue(b);
    d->exec(qs);
    while (qs.next()) {
        auto it = index.find(qs.value(0).toLongLong());
        if (it == index.end()) continue;
        out[it->second].spectra.push_back({qs.value(1).toLongLong(), qs.value(2).toString(), qs.value(3).toInt() != 0,
                                           fromBlob(qs.value(4).toByteArray()), fromBlob(qs.value(5).toByteArray())});
    }
    QSqlQuery qp(d->db);
    qp.setForwardOnly(true);
    qp.prepare("SELECT entry_id, position, intensity, width, label FROM peaks WHERE entry_id IN (" + sub +
               ") ORDER BY entry_id, position");
    for (const QVariant& b : binds) qp.addBindValue(b);
    d->exec(qp);
    while (qp.next()) {
        auto it = index.find(qp.value(0).toLongLong());
        if (it == index.end()) continue;
        out[it->second].peaks.push_back({qp.value(1).toDouble(), qp.value(2).toDouble(), qp.value(3).toDouble(),
                                         qp.value(4).toString()});
    }
    return out;
}

int Library::count() const {
    d->requireOpen();
    return d->scalar("SELECT COUNT(*) FROM entries").toInt();
}

QStringList Library::classes() const {
    d->requireOpen();
    QSqlQuery q = d->exec("SELECT DISTINCT class FROM entries WHERE class <> '' ORDER BY class COLLATE NOCASE");
    QStringList out;
    while (q.next()) {
        const QString c = q.value(0).toString();
        if (!out.contains(c, Qt::CaseInsensitive)) out << c;
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ import
qint64 Library::addSpectrum(const Spectrum& sp, Entry meta) {
    fileio::DataSpectrum ds = fileio::fromSpectrum(sp, meta.name);
    if (meta.laserNm <= 0) {
        const double l = parseLaser(sp.metaValue("laser_nm"));
        meta.laserNm = l > 0 ? l : double(sp.channel);
    }
    if (meta.conditions.isEmpty()) {
        QStringList c;
        c << QStringLiteral("%1 nm").arg(sp.channel) << QStringLiteral("exposure %1 s").arg(sp.exposure_s)
          << QStringLiteral("%1 average%2").arg(sp.averages).arg(sp.averages == 1 ? "" : "s");
        const QString p = sp.metaValue("power_mw");
        if (!p.isEmpty()) c << QStringLiteral("%1 mW").arg(p);
        meta.conditions = c.join(", ");
    }
    if (!meta.provenance.contains("measured")) meta.provenance.insert("measured", iso(sp.t));
    return addSpectrum(ds, std::move(meta));
}

qint64 Library::addSpectrum(const fileio::DataSpectrum& sp0, Entry meta) {
    d->requireWritable();
    fileio::DataSpectrum sp = sp0;
    sp.sorted();
    Vec x = sp.x, y = sp.y;
    sortXY(x, y);
    if (x.size() < 2) fail(QStringLiteral("the spectrum has no data"));
    Entry e = std::move(meta);
    if (e.name.trimmed().isEmpty()) e.name = sp.name.isEmpty() ? QStringLiteral("spectrum") : sp.name;
    if (e.laserNm <= 0) {
        for (const char* k : {"laser_nm", "laser", "laser wavelength", "excitation", "channel"}) {
            const double l = parseLaser(sp.metaValue(QLatin1String(k)));
            if (l > 100) {
                e.laserNm = l;
                break;
            }
        }
    }
    if (e.op.isEmpty()) e.op = sp.metaValue("operator", sp.metaValue("Operator"));
    if (e.notes.isEmpty()) e.notes = sp.metaValue("notes");
    if (e.licence.isEmpty() && e.source == Source::Own) e.licence = QStringLiteral("own data");
    if (e.sourceId.isEmpty() && !sp.source.isEmpty()) e.sourceId = QFileInfo(sp.source).fileName();
    QJsonObject meta2;
    for (const auto& kv : sp.meta) meta2.insert(kv.first, kv.second);
    if (!meta2.isEmpty() && !e.provenance.contains("metadata")) e.provenance.insert("metadata", meta2);
    if (!sp.source.isEmpty() && !e.provenance.contains("file")) e.provenance.insert("file", sp.source);
    if (!e.provenance.contains("added"))
        e.provenance.insert("added", QStringLiteral("%1, LibreRamen %2").arg(nowIso(), LIBRERAMEN_VERSION));
    if (e.spectra.empty()) e.spectra.push_back({0, QStringLiteral("raw"), false, x, y});
    if (e.peaks.empty()) e.peaks = autoPeaks(x, y);
    return add(std::move(e));
}

std::vector<qint64> Library::importFile(const QString& path, const Entry& meta) {
    d->requireWritable();
    std::vector<qint64> ids;
    Impl::Tx tx(*d);
    if (isRruffFile(path)) {
        Entry e = entryFromRruff(readRruff(path));
        fillFrom(e, meta);
        ids.push_back(d->insert(std::move(e)));
    } else {
        const std::vector<fileio::DataSpectrum> sps = fileio::read(path);
        for (const auto& sp : sps) {
            Entry e = meta;
            if (sps.size() > 1) e.name.clear();
            e.provenance.insert("imported", QStringLiteral("%1 from %2").arg(nowIso(), QFileInfo(path).fileName()));
            ids.push_back(addSpectrum(sp, std::move(e)));
        }
    }
    tx.commit();
    return ids;
}

std::vector<qint64> Library::importRruffFolder(const QString& dir, QStringList* errors) {
    d->requireWritable();
    const QDir qd(dir);
    if (!qd.exists()) fail(QStringLiteral("folder not found: %1").arg(QDir::toNativeSeparators(dir)));
    std::vector<qint64> ids;
    Impl::Tx tx(*d);
    for (const QFileInfo& fi : qd.entryInfoList({"*.txt", "*.TXT"}, QDir::Files, QDir::Name)) {
        try {
            if (!isRruffFile(fi.absoluteFilePath())) {
                if (errors) *errors << QStringLiteral("%1: not a RRUFF file").arg(fi.fileName());
                continue;
            }
            ids.push_back(d->insert(entryFromRruff(readRruff(fi.absoluteFilePath()))));
        } catch (const Error& e) {
            if (errors) *errors << QStringLiteral("%1: %2").arg(fi.fileName(), QString::fromUtf8(e.what()));
        }
    }
    tx.commit();
    return ids;
}

std::vector<qint64> Library::importBandList(const QString& path, const Entry& meta) {
    d->requireWritable();
    const auto lists = readBandList(path);
    std::vector<qint64> ids;
    Impl::Tx tx(*d);
    for (const auto& b : lists) {
        Entry e;
        e.name = b.name;
        e.cls = b.cls;
        e.sourceId = b.reference;
        e.licence = b.licence;
        e.source = meta.source == Source::Own ? Source::Literature : meta.source;
        e.peaks = b.bands;
        fillFrom(e, meta);
        e.provenance.insert("imported", QStringLiteral("%1 from %2").arg(nowIso(), QFileInfo(path).fileName()));
        ids.push_back(d->insert(std::move(e)));
    }
    tx.commit();
    return ids;
}

// ------------------------------------------------------------------------------------------------ export
void Library::exportEntry(qint64 id, const QString& path) const {
    const auto e = get(id, true);
    if (!e) fail(QStringLiteral("library entry %1 not found").arg(id));
    const SpectrumData* s = e->preferredSpectrum();
    if (!s) {
        BandListEntry b{e->name, e->cls, e->sourceId, e->licence, e->peaks};
        writeBandList(path, {b});
        return;
    }
    fileio::DataSpectrum ds;
    ds.name = e->name;
    ds.x = s->x;
    ds.y = s->y;
    auto put = [&](const QString& k, const QString& v) {
        if (!v.isEmpty()) ds.setMeta(k, v);
    };
    put("sample", e->name);
    put("formula", e->formula);
    put("cas", e->cas);
    put("class", e->cls);
    put("tags", e->tags.join(", "));
    put("library_source", sourceLabel(e->source));
    put("source_id", e->sourceId);
    put("licence", e->licence);
    if (e->laserNm > 0) put("laser_nm", fmt(e->laserNm));
    put("conditions", e->conditions);
    put("operator", e->op);
    put("notes", QString(e->notes).replace('\n', ' '));
    put("created", e->created.toString(Qt::ISODate));
    put("spectrum", s->label + (s->processed ? " (processed)" : ""));
    if (!e->peaks.empty()) {
        QStringList p;
        for (const auto& k : e->peaks) p << QString::number(k.position, 'f', 1);
        put("peaks", p.join(" "));
    }
    if (!e->provenance.isEmpty())
        put("provenance", QString::fromUtf8(QJsonDocument(e->provenance).toJson(QJsonDocument::Compact)));
    const QString suf = QFileInfo(path).suffix().toLower();
    if (suf == "jdx" || suf == "dx" || suf == "jcamp")
        fileio::writeJcamp(path, ds);
    else
        fileio::writeCsv(path, ds);
}

int Library::exportTo(const std::vector<qint64>& ids, const QString& otherPath) const {
    d->requireOpen();
    if (QFileInfo(otherPath).absoluteFilePath() == d->path) fail(QStringLiteral("cannot export a library into itself"));
    Library other;
    other.open(otherPath, OpenMode::ReadWrite, true);
    Impl::Tx tx(*other.d);
    int n = 0;
    for (qint64 id : ids) {
        auto e = get(id, true);
        if (!e) continue;
        e->provenance.insert("copied_from", QStringLiteral("%1 #%2, %3").arg(QFileInfo(d->path).fileName()).arg(id)
                                                .arg(nowIso()));
        e->id = 0;
        other.d->insert(std::move(*e));
        ++n;
    }
    tx.commit();
    return n;
}

// ------------------------------------------------------------------------------------------------ RRUFF
bool isRruffFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(4096);
    QByteArray t = head.trimmed();
    if (t.startsWith("\xEF\xBB\xBF")) t = t.mid(3);
    return t.startsWith("##NAMES=") || head.contains("##RRUFFID=");
}

RruffSpectrum readRruff(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        fail(QStringLiteral("cannot read %1").arg(QDir::toNativeSeparators(path)));
    QTextStream in(&f);
    RruffSpectrum r;
    static const QRegularExpression sep(R"([,;\s]+)");
    bool ended = false;
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.startsWith(QChar(0xFEFF))) line = line.mid(1);
        if (line.isEmpty()) continue;
        if (line.startsWith("##")) {
            const int eq = line.indexOf('=');
            const QString key = (eq < 0 ? line.mid(2) : line.mid(2, eq - 2)).trimmed().toUpper();
            const QString val = eq < 0 ? QString() : line.mid(eq + 1).trimmed();
            if (key == "END") {
                if (!r.x.empty()) ended = true;
                continue;
            }
            r.header.emplace_back(key, val);
            if (key == "NAMES") r.name = val.section(',', 0, 0).trimmed();
            else if (key == "RRUFFID") r.rruffId = val;
            else if (key == "IDEAL CHEMISTRY") r.formula = QString(val).remove('_').remove('^');
            else if (key == "LOCALITY") r.locality = val;
            else if (key == "OWNER") r.owner = val;
            else if (key == "SOURCE") r.sourceName = val;
            else if (key == "DESCRIPTION") r.description = val;
            else if (key == "STATUS") r.status = val;
            else if (key == "URL") r.url = val;
            else if (key.contains("LASER") || key.contains("WAVELENGTH")) r.laserNm = parseLaser(val);
            continue;
        }
        if (ended || line.startsWith('#')) continue;
        const QStringList parts = line.split(sep, Qt::SkipEmptyParts);
        if (parts.size() < 2) continue;
        bool okx = false, oky = false;
        const double x = parts[0].toDouble(&okx), y = parts[1].toDouble(&oky);
        if (okx && oky && std::isfinite(x) && std::isfinite(y)) {
            r.x.push_back(x);
            r.y.push_back(y);
        }
    }
    if (r.x.size() < 2) fail(QStringLiteral("%1: no spectrum data in the RRUFF file").arg(QFileInfo(path).fileName()));
    sortXY(r.x, r.y);
    // File names: Name__R050031-1__Raman__532__0__unoriented__Raman_Data_Processed__12345.txt
    const QString base = QFileInfo(path).completeBaseName();
    const QStringList tok = base.split("__");
    if (r.name.isEmpty() && !tok.isEmpty()) r.name = QString(tok[0]).replace('_', ' ');
    if (r.rruffId.isEmpty() && tok.size() > 1 && tok[1].startsWith('R')) r.rruffId = tok[1];
    if (r.laserNm <= 0)
        for (int i = 0; i + 1 < tok.size(); ++i)
            if (tok[i].compare("Raman", Qt::CaseInsensitive) == 0) {
                const double l = tok[i + 1].toDouble();
                if (l > 200 && l < 2000) {
                    r.laserNm = l;
                    break;
                }
            }
    r.processed = base.contains("Processed", Qt::CaseInsensitive);
    for (const auto& kv : r.header)
        if (kv.first.contains("PROCESS")) r.processed = true;
    if (r.name.isEmpty()) r.name = base;
    return r;
}

Entry entryFromRruff(const RruffSpectrum& r) {
    Entry e;
    e.name = r.name;
    e.formula = r.formula;
    e.cls = QStringLiteral("mineral");
    e.source = Source::Rruff;
    e.sourceId = r.rruffId;
    e.licence = QStringLiteral("RRUFF Project (rruff.info), free for research and education; cite Lafuente et al. "
                               "(2015), Highlights in Mineralogical Crystallography, 1-30. Do not redistribute "
                               "without checking the RRUFF terms.");
    e.laserNm = r.laserNm;
    QStringList notes;
    if (!r.locality.isEmpty()) notes << "Locality: " + r.locality;
    if (!r.description.isEmpty()) notes << "Description: " + r.description;
    if (!r.status.isEmpty()) notes << "Status: " + r.status;
    e.notes = notes.join('\n');
    QJsonObject h;
    for (const auto& kv : r.header) h.insert(kv.first, kv.second);
    e.provenance.insert("rruff_header", h);
    if (!r.url.isEmpty()) e.provenance.insert("url", r.url);
    if (!r.owner.isEmpty()) e.provenance.insert("owner", r.owner);
    if (!r.sourceName.isEmpty()) e.provenance.insert("sample_source", r.sourceName);
    e.provenance.insert("imported", QStringLiteral("%1, LibreRamen %2").arg(nowIso(), LIBRERAMEN_VERSION));
    e.spectra.push_back({0, r.processed ? QStringLiteral("RRUFF processed") : QStringLiteral("RRUFF raw"),
                         r.processed, r.x, r.y});
    e.peaks = autoPeaks(r.x, r.y);
    return e;
}

// ------------------------------------------------------------------------------------------------ band lists
namespace {

double strengthOf(const QString& s0, bool* ok) {
    const QString s = s0.trimmed().toLower();
    *ok = true;
    if (s == "vw") return 0.1;
    if (s == "w") return 0.25;
    if (s == "m") return 0.5;
    if (s == "s" || s == "vs") return 1.0;
    return s.toDouble(ok);
}

QStringList splitCsvLine(const QString& line) {   // name may be quoted; returns {name, rest}
    QString t = line.trimmed();
    if (t.startsWith('"')) {
        QString name;
        int i = 1;
        for (; i < t.size(); ++i) {
            if (t[i] == '"') {
                if (i + 1 < t.size() && t[i + 1] == '"') {
                    name += '"';
                    ++i;
                    continue;
                }
                break;
            }
            name += t[i];
        }
        QString rest = t.mid(i + 1).trimmed();
        if (rest.startsWith(',')) rest = rest.mid(1);
        return {name, rest};
    }
    const int c = t.indexOf(',');
    if (c < 0) return {t};
    return {t.left(c).trimmed(), t.mid(c + 1)};
}

}  // namespace

std::vector<BandListEntry> readBandList(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        fail(QStringLiteral("cannot read %1").arg(QDir::toNativeSeparators(path)));
    QTextStream in(&f);
    std::vector<BandListEntry> out;
    QString cls, ref, lic;
    int lineNo = 0;
    static const QRegularExpression sep(R"([;,\s]+)");
    while (!in.atEnd()) {
        QString line = in.readLine();
        ++lineNo;
        if (line.startsWith(QChar(0xFEFF))) line = line.mid(1);
        const QString t = line.trimmed();
        if (t.isEmpty()) continue;
        if (t.startsWith('#')) {
            const QString c = t.mid(1).trimmed();
            const int colon = c.indexOf(':');
            if (colon > 0) {
                const QString k = c.left(colon).trimmed().toLower(), v = c.mid(colon + 1).trimmed();
                if (k == "class") cls = v;
                else if (k == "source" || k == "reference") ref = v;
                else if (k == "licence" || k == "license") lic = v;
            }
            continue;
        }
        const QStringList nr = splitCsvLine(t);
        if (nr.size() < 2 || nr[0].isEmpty()) {
            if (out.empty() && nr.size() < 2) continue;
            fail(QStringLiteral("%1, line %2: expected name,bands").arg(QFileInfo(path).fileName()).arg(lineNo));
        }
        if (out.empty() && nr[0].compare("name", Qt::CaseInsensitive) == 0) continue;   // header
        BandListEntry b{nr[0], cls, ref, lic, {}};
        QString rest = nr[1].trimmed();
        if (rest.startsWith('"') && rest.endsWith('"') && rest.size() >= 2) rest = rest.mid(1, rest.size() - 2);
        for (const QString& tok : rest.split(sep, Qt::SkipEmptyParts)) {
            QString pos = tok, st;
            const int c = tok.indexOf(':');
            if (c >= 0) {
                pos = tok.left(c);
                st = tok.mid(c + 1);
            }
            bool ok = false;
            const double p = pos.toDouble(&ok);
            if (!ok || !std::isfinite(p))
                fail(QStringLiteral("%1, line %2: '%3' is not a band position")
                         .arg(QFileInfo(path).fileName()).arg(lineNo).arg(tok));
            double inten = 0;
            if (!st.isEmpty()) {
                bool okS = false;
                inten = strengthOf(st, &okS);
                if (!okS) inten = 0;
            }
            b.bands.push_back({p, inten, 0, {}});
        }
        if (b.bands.empty())
            fail(QStringLiteral("%1, line %2: no bands for '%3'").arg(QFileInfo(path).fileName()).arg(lineNo).arg(b.name));
        std::sort(b.bands.begin(), b.bands.end(), [](const PeakEntry& a, const PeakEntry& c) { return a.position < c.position; });
        out.push_back(std::move(b));
    }
    return out;
}

void writeBandList(const QString& path, const std::vector<BandListEntry>& entries) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        fail(QStringLiteral("cannot write %1").arg(QDir::toNativeSeparators(path)));
    QTextStream o(&f);
    o << "# LibreRamen band list (name,band;band;... in cm-1; optional :relative strength)\n";
    QString cls, ref, lic;
    for (const auto& b : entries) {
        if (b.cls != cls) o << "# class: " << (cls = b.cls) << "\n";
        if (b.reference != ref) o << "# source: " << (ref = b.reference) << "\n";
        if (b.licence != lic) o << "# licence: " << (lic = b.licence) << "\n";
        QString name = b.name;
        if (name.contains(',') || name.contains('"')) name = '"' + name.replace('"', "\"\"") + '"';
        QStringList bands;
        for (const auto& p : b.bands)
            bands << (p.intensity > 0 ? QStringLiteral("%1:%2").arg(fmt(p.position), fmt(p.intensity)) : fmt(p.position));
        o << name << "," << bands.join(';') << "\n";
    }
}

// ------------------------------------------------------------------------------------------------ demo
std::pair<Vec, Vec> simulatorSpectrum(const QString& sample) {
    const SimData& sd = simulatorData();
    const ChannelCal& cal = defaultCalibration().channels.at(532);
    for (const auto& s : sd.samples) {
        if (s.first != sample) continue;
        Vec x(s.second.size()), y = s.second;
        for (size_t i = 0; i < x.size(); ++i) x[i] = cal.shift(double(i));
        sortXY(x, y);
        return {x, y};
    }
    fail(QStringLiteral("unknown simulator sample: %1").arg(sample));
}

void createDemo(Library& lib, bool withStandards) {
    struct Info {
        const char *sample, *name, *formula, *cas, *cls;
    };
    static const Info infos[] = {
        {"carbon tetrachloride", "Carbon tetrachloride", "CCl4", "56-23-5", "solvent"},
        {"chloroform", "Chloroform", "CHCl3", "67-66-3", "solvent"},
        {"ethanol", "Ethanol", "C2H5OH", "64-17-5", "solvent"},
        {"water", "Water", "H2O", "7732-18-5", "solvent"},
        {"zinc sulfide", "Zinc sulfide", "ZnS", "1314-98-3", "inorganic"},
    };
    const SimData& sd = simulatorData();
    for (const auto& s : sd.samples) {
        Entry e;
        e.name = s.first;
        for (const Info& i : infos)
            if (s.first == QLatin1String(i.sample)) {
                e.name = QString::fromUtf8(i.name);
                e.formula = QString::fromUtf8(i.formula);
                e.cas = QString::fromUtf8(i.cas);
                e.cls = QString::fromUtf8(i.cls);
            }
        e.source = Source::Own;
        e.sourceId = QStringLiteral("simulator:%1").arg(s.first);
        e.licence = QStringLiteral("LibreRamen demo data (MIT), recorded on the development Cora 5000");
        e.laserNm = defaultCalibration().channels.at(532).laser_nm;
        e.conditions = QStringLiteral("532 nm, counts per second, dark subtracted");
        e.op = QStringLiteral("LibreRamen");
        e.notes = QStringLiteral("Demo entry generated from the simulator recordings (resource simulator.bin).");
        e.tags = QStringList{"demo"};
        e.provenance.insert("generator", QStringLiteral("library::createDemo, LibreRamen %1").arg(LIBRERAMEN_VERSION));
        e.provenance.insert("calibration", QStringLiteral("factory (built-in), S/N %1").arg(defaultCalibration().serial));
        auto [x, y] = simulatorSpectrum(s.first);
        e.peaks = autoPeaks(x, y);
        e.spectra.push_back({0, QStringLiteral("raw"), false, std::move(x), std::move(y)});
        lib.add(std::move(e));
    }
    if (!withStandards) return;
    for (const auto& st : standards::all()) {
        Entry e;
        // display name: "toluene" -> "Toluene", short acronyms upper case ("dmso" -> "DMSO", "ptfe" -> "PTFE")
        e.name = st.name;
        if (e.name.size() <= 4 && !e.name.contains(' ')) e.name = e.name.toUpper();
        else if (!e.name.isEmpty()) e.name[0] = e.name[0].toUpper();
        e.cls = QStringLiteral("standard");
        e.source = Source::Literature;
        e.sourceId = st.source == "ASTM" ? QStringLiteral("ASTM E1840 (as commonly quoted)")
                                         : QStringLiteral("literature values");
        e.licence = QStringLiteral("LibreRamen standards table (MIT); band positions are factual reference values");
        e.notes = st.note;
        e.tags = st.aka;
        e.tags << "standard";
        e.provenance.insert("generator", QStringLiteral("library::createDemo from standards.h"));
        for (const auto& b : standards::bands(st.name)) e.peaks.push_back({b.position, b.strength / 3.0, 0, {}});
        lib.add(std::move(e));
    }
}

// ------------------------------------------------------------------------------------------- reference set
namespace {
constexpr int kReferenceVersion = 2;               // bump when resources/reference changes
const char* const kReferenceFiles[] = {":/core/reference/battery.json", ":/core/reference/solvents.json"};

std::vector<Entry> referenceSet() {
    std::vector<Entry> out;
    for (const char* path : kReferenceFiles) {
        QFile f(QString::fromLatin1(path));
        if (!f.open(QIODevice::ReadOnly)) throw Error(std::string("reference data missing: ") + path);
        const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
        const QString set = QFileInfo(QString::fromLatin1(path)).baseName();
        for (const QJsonValue& v : arr) {
            const QJsonObject o = v.toObject();
            Entry e;
            e.name = o["name"].toString();
            e.formula = o["formula"].toString();
            e.cas = o["cas"].toString();
            e.cls = o["cls"].toString();
            for (const QJsonValue& t : o["tags"].toArray()) e.tags << t.toString();
            e.tags << QStringLiteral("reference");
            e.source = Source::Literature;
            e.laserNm = o["laser_nm"].toDouble();
            QStringList refs;
            const QJsonArray src = o["sources"].toArray();
            for (const QJsonValue& s : src) {
                const QJsonObject so = s.toObject();
                refs << so["ref"].toString() + QStringLiteral(" — ") + so["doi_or_url"].toString() +
                            (so["covers"].toString().isEmpty() ? QString() : QStringLiteral(" (") + so["covers"].toString() + ')');
            }
            if (!src.isEmpty()) e.sourceId = src.first().toObject()["doi_or_url"].toString();
            e.licence = QStringLiteral("Band positions from the cited literature (factual data); compiled for LibreRamen (MIT)");
            QString notes = o["notes"].toString();
            if (!o["rruff_id"].toString().isEmpty())
                notes += QStringLiteral("\nRRUFF: %1 (import from rruff.info yourself; not redistributable)").arg(o["rruff_id"].toString());
            notes += QStringLiteral("\nConfidence: %1\nSources:\n%2").arg(o["confidence"].toString(), refs.join('\n'));
            e.notes = notes.trimmed();
            e.provenance.insert("reference_key", set + ':' + e.name);
            e.provenance.insert("reference_set", set);
            e.provenance.insert("generator", QStringLiteral("LibreRamen reference library v%1").arg(kReferenceVersion));
            for (const QJsonValue& pv : o["peaks"].toArray()) {
                const QJsonObject p = pv.toObject();
                e.peaks.push_back({p["position"].toDouble(), p["intensity"].toDouble(), p["width"].toDouble(),
                                   p["label"].toString()});
            }
            std::sort(e.peaks.begin(), e.peaks.end(), [](const PeakEntry& a, const PeakEntry& b) { return a.position < b.position; });
            out.push_back(std::move(e));
        }
    }
    return out;
}
}  // namespace

int referenceVersion() { return kReferenceVersion; }
int referenceCount() { return int(referenceSet().size()); }

int addReferenceEntries(Library& lib) {
    if (lib.readOnly()) throw Error("the library is open read-only");
    QSet<QString> have;
    Filter f;
    f.source = Source::Literature;
    for (const Entry& e : lib.find(f)) {
        const QString k = e.provenance.value("reference_key").toString();
        if (!k.isEmpty()) have.insert(k);
    }
    int added = 0;
    for (Entry& e : referenceSet()) {
        if (have.contains(e.provenance.value("reference_key").toString())) continue;   // kept as the user left it
        lib.add(std::move(e));
        ++added;
    }
    lib.setInfo(QStringLiteral("reference_version"), QString::number(kReferenceVersion));
    return added;
}

}  // namespace lr::library
