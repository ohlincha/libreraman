#include "series.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <QTextStream>
#include <QElapsedTimer>
#include <QThread>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "errors.h"
#include "fileio.h"
#include "pyformat.h"

namespace lr {

namespace {

double now() { return QDateTime::currentMSecsSinceEpoch() / 1000.0; }   // time.time()
double epoch(const QDateTime& t) { return t.toMSecsSinceEpoch() / 1000.0; }
QDateTime fromEpoch(double t) { return QDateTime::fromMSecsSinceEpoch(qint64(std::floor(t * 1000.0))); }
void sleepS(double s) {
    if (s > 0) QThread::usleep(static_cast<unsigned long>(s * 1e6));
}

// Replaces background and intensity with a background measured once (dark(), data order).
void applyBackground(Spectrum& sp, const Vec& pixels, const Vec& bg) {
    std::map<long long, double> byPixel;                      // controller: dict(zip(pixel_numbers(), bg_once))
    for (size_t i = 0; i < pixels.size() && i < bg.size(); ++i) byPixel[std::llround(pixels[i])] = bg[i];
    sp.background.clear();
    sp.intensity.clear();
    for (size_t i = 0; i < sp.pixels.size(); ++i) {
        const auto it = byPixel.find(std::llround(sp.pixels[i]));
        const double b = it == byPixel.end() ? 0.0 : it->second;
        sp.background.push_back(b);
        sp.intensity.push_back(sp.signal[i] - b);
    }
}

// Header + "raman_shift_cm1,t=..s,..." + one row per shift (series.py / controller _write_table).
void writeTable(QTextStream& o, const QString& header, const std::vector<SeriesColumn>& cols, const Vec& shift) {
    o << header;
    o << "raman_shift_cm1";
    for (const auto& c : cols) o << ",t=" << py::fixed(c.elapsed_s, 1) << "s";
    o << "\n";
    for (size_t i = 0; i < shift.size(); ++i) {
        o << py::fixed(shift[i], 2);
        for (const auto& c : cols) o << "," << (i < c.intensity.size() ? py::fixed(c.intensity[i], 1) : QString());
        o << "\n";
    }
}

}  // namespace

QString safeName(const QString& name) {
    const QString n = name.isEmpty() ? QStringLiteral("sample") : name;
    QString out;
    for (QChar c : n) out += (c.isLetterOrNumber() || c == '-' || c == '_' || c == '.') ? c : QChar('_');
    return out.left(60);
}

// ----------------------------------------------------------------------------- series.py
std::vector<SeriesColumn> runSeries(Instrument& inst, const QString& path, double exposure_s, double interval_s,
                                    int count, int averages, const QString& background, double power_mw,
                                    const std::atomic<bool>* stop,
                                    const std::function<void(int, double, const Spectrum&)>& onSpectrum) {
    const double t0 = now();
    Vec bgOnce;
    const bool once = background == "once";
    if (once) bgOnce = inst.dark(exposure_s, averages);
    const Vec pixels = inst.pixelNumbers(inst.channel());
    const QString header = "# Cora 5000 time series, started " + iso(fromEpoch(t0)) + "\n# channel: " +
                           QString::number(inst.channel()) + " nm\n# exposure_s: " + py::str(exposure_s) +
                           "\n# averages: " + QString::number(averages) + "\n# interval_s: " + py::str(interval_s) +
                           "\n# background: " + background + "\n";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        throw Error(("cannot write " + path).toStdString());
    {
        QTextStream o(&f);
        o << header;
    }
    f.flush();
    std::vector<SeriesColumn> cols;
    for (int k = 0; k < count; ++k) {
        const double target = t0 + k * interval_s;
        while (now() < target) {
            if (stop && *stop) return cols;
            sleepS(std::min(0.05, std::max(0.0, target - now())));
        }
        AcquireOptions o;
        o.exposure_s = exposure_s;
        o.averages = averages;
        o.background = background == "each";
        o.power_mw = power_mw;
        o.stop = stop;
        Spectrum sp;
        try {
            sp = inst.acquire(o);
        } catch (const Stopped&) {
            return cols;
        }
        if (once) applyBackground(sp, pixels, bgOnce);
        const double elapsed = epoch(sp.t) - t0;
        cols.push_back({elapsed, sp.intensity});
        if (onSpectrum) onSpectrum(k, elapsed, sp);
        // rewrite the table each time so the file is always complete
        f.seek(0);
        f.resize(0);
        {
            QTextStream o(&f);
            writeTable(o, header, cols, sp.shift);
        }
        f.flush();
    }
    return cols;
}

// ----------------------------------------------------------------------------- GUI controller series
namespace {

// series.csv, series.jdx and series.spc, rewritten by a thread of their own (see series.h).
class SeriesFileWriter {
public:
    SeriesFileWriter(const QString& folder, const SeriesSettings::Files& files, const QString& csvHeader,
                     const QString& title, const Meta& fileMeta)
        : m_folder(folder), m_files(files), m_header(csvHeader) {
        if (files.jdx) m_jdx.emplace(fileio::MultiEncoder::Format::Jcamp, title, fileMeta);
        if (files.spc) m_spc.emplace(fileio::MultiEncoder::Format::Spc, title, fileMeta);
        m_thread = std::thread([this] { loop(); });
    }
    ~SeriesFileWriter() { finish(); }
    SeriesFileWriter(const SeriesFileWriter&) = delete;
    SeriesFileWriter& operator=(const SeriesFileWriter&) = delete;

    // Worker thread: queue a saved spectrum (copied).
    void add(const Spectrum& sp, double elapsed_s) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back({sp, elapsed_s});
        }
        m_cv.notify_one();
    }
    // Writes everything still queued, then stops the thread (idempotent).
    void finish() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_done = true;
        }
        m_cv.notify_one();
        if (m_thread.joinable()) m_thread.join();
    }
    // First write error not yet reported (empty if none).
    QString takeError() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        QString e = m_error;
        if (!e.isEmpty()) m_errorTaken = true;
        m_error.clear();
        return e;
    }

private:
    struct Item {
        Spectrum sp;
        double elapsed_s;
    };

    void loop() {
        using Clock = std::chrono::steady_clock;
        Clock::time_point notBefore = Clock::now();
        bool pending = false;                                 // spectra added since the last write
        for (;;) {
            std::deque<Item> batch;
            bool done;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (pending)                                  // rate limit, unless the series ends
                    m_cv.wait_until(lock, notBefore, [this] { return m_done; });
                else
                    m_cv.wait(lock, [this] { return m_done || !m_queue.empty(); });
                batch.swap(m_queue);
                done = m_done;
            }
            for (Item& it : batch) encode(it);
            pending = pending || !batch.empty();
            if (pending && (done || Clock::now() >= notBefore)) {
                const auto t0 = Clock::now();
                write();
                pending = false;
                const auto t1 = Clock::now();
                notBefore = t1 + 2 * (t1 - t0);
            }
            if (done) {
                const std::lock_guard<std::mutex> lock(m_mutex);
                if (m_queue.empty()) return;
            }
        }
    }

    // Formats one spectrum once: a column of the table rows, a JCAMP block, an SPC sub-file.
    void encode(const Item& it) {
        const Spectrum& sp = it.sp;
        if (m_files.csv) {
            if (m_rows.empty() || sp.shift != m_shift) {      // first spectrum (or another axis): rebuild
                m_shift = sp.shift;
                m_rows.assign(m_shift.size(), QByteArray());
                for (size_t i = 0; i < m_shift.size(); ++i) {
                    m_rows[i] = py::fixed(m_shift[i], 2).toUtf8();
                    for (const SeriesColumn& c : m_cols)
                        m_rows[i] += ',' + (i < c.intensity.size() ? py::fixed(c.intensity[i], 1).toUtf8() : QByteArray());
                }
            }
            m_cols.push_back({it.elapsed_s, sp.intensity});
            m_colHeader += ",t=" + py::fixed(it.elapsed_s, 1).toUtf8() + "s";
            for (size_t i = 0; i < m_rows.size(); ++i) {
                m_rows[i] += ',';
                if (i < sp.intensity.size()) m_rows[i] += py::fixed(sp.intensity[i], 1).toUtf8();
            }
        }
        if (m_jdx || m_spc) {
            const QString el = sp.metaValue("series_elapsed_s");
            fileio::DataSpectrum d =
                fileio::fromSpectrum(sp, sp.metaValue("sample", "spectrum") + " t=" + el + " s");
            d.setMeta("acquired", iso(sp.t));
            try {
                if (m_jdx) m_jdx->add(d);
                if (m_spc) m_spc->add(d);
            } catch (const std::exception& e) {
                setError(QString::fromUtf8(e.what()));
            }
        }
    }

    void write() {
        if (m_files.csv && !m_rows.empty()) {
            const QString path = QDir(m_folder).filePath("series.csv");
            QByteArray b = m_header.toUtf8() + "raman_shift_cm1" + m_colHeader + '\n';
            qsizetype size = b.size();
            for (const QByteArray& r : m_rows) size += r.size() + 1;
            b.reserve(size);
            for (const QByteArray& r : m_rows) {
                b += r;
                b += '\n';
            }
            QSaveFile f(path);                               // controller: write series.csv.tmp, os.replace
            if (!f.open(QIODevice::WriteOnly | QIODevice::Text) || f.write(b) != b.size() || !f.commit())
                setError("cannot write " + path);
        }
        for (auto* e : {m_jdx ? &*m_jdx : nullptr, m_spc ? &*m_spc : nullptr}) {
            if (!e || e->count() == 0) continue;
            const QString path =
                QDir(m_folder).filePath(e->format() == fileio::MultiEncoder::Format::Jcamp ? "series.jdx" : "series.spc");
            try {
                e->save(path);
            } catch (const std::exception& ex) {
                setError(QString::fromUtf8(ex.what()));
            }
        }
    }

    void setError(const QString& e) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_error.isEmpty() && !m_errorTaken) m_error = e;  // reported once per series
    }

    const QString m_folder;
    const SeriesSettings::Files m_files;
    const QString m_header;
    // writer thread only
    Vec m_shift;
    std::vector<SeriesColumn> m_cols;
    std::vector<QByteArray> m_rows;                           // series.csv rows without the line end
    QByteArray m_colHeader;
    std::optional<fileio::MultiEncoder> m_jdx, m_spc;
    // shared
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Item> m_queue;
    bool m_done = false;
    QString m_error;
    bool m_errorTaken = false;
    std::thread m_thread;
};

struct Run {
    Instrument& inst;
    const SeriesSettings& s;
    const std::atomic<bool>& stop;
    const SeriesCallbacks& cb;
    QString folder;
    double start = 0.0;
    std::unique_ptr<SeriesFileWriter> writer;

    ~Run() {
        if (writer) writer->finish();                         // also when the series failed
    }
    void log(const QString& m) const {
        if (cb.log) cb.log(m);
    }
    void progress(const SeriesProgress& p) const {
        if (cb.progress) cb.progress(p);
    }
    void makeFolder() {
        const QString base = QDir(s.folder).filePath(safeName(s.sample) + "_series_" +
                                                     QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
        folder = base;                                  // never reuse a folder (two series in the same second)
        for (int k = 2; QFileInfo::exists(folder); ++k) folder = base + "_" + QString::number(k);
        if (!QDir().mkpath(folder)) throw Error(("cannot create " + folder).toStdString());
        if (!s.files.csv && !s.files.jdx && !s.files.spc) return;
        const QString started = iso(fromEpoch(start));
        const QString header = "# LibreRaman time series, started " + started + "\n# sample: " + s.sample +
                               "\n# exposure_s: " + py::str(s.exposure_s) +
                               "\n# averages: " + QString::number(s.averages) + "\n# interval_s: " +
                               py::str(s.interval_s) + "\n# background: " + s.background + "\n";
        Meta meta = {{"sample", s.sample},
                     {"started", started},
                     {"exposure_s", py::str(s.exposure_s)},
                     {"averages", QString::number(s.averages)}};
        if (s.laserStaysOn) meta.emplace_back("mode", "as fast as possible, laser on, background once");
        else meta.emplace_back("interval_s", py::str(s.interval_s));
        meta.emplace_back("background", s.laserStaysOn ? QString("once") : s.background);
        meta.emplace_back("software", "LibreRaman " LIBRERAMAN_VERSION);
        writer = std::make_unique<SeriesFileWriter>(folder, s.files, header, s.sample + " time series", meta);
    }
    // Saves the spectrum, hands it to the series files, emits it.
    void record(Spectrum& sp, int k, const QString& mode = {}) {
        sp.setMeta("sample", s.sample);
        sp.setMeta("series_index", QString::number(k + 1));
        sp.setMeta("series_elapsed_s", py::str(std::round((epoch(sp.t) - start) * 10.0) / 10.0));
        if (!mode.isEmpty()) sp.setMeta("mode", mode);
        sp.saveCsv(QDir(folder).filePath(QString("%1_%2.csv").arg(k + 1, 4, 10, QChar('0')).arg(sp.t.toString("HHmmss"))));
        if (writer) {
            writer->add(sp, epoch(sp.t) - start);
            const QString e = writer->takeError();
            if (!e.isEmpty()) log("Series files: " + e + " (the spectra are saved one by one)");
        }
        if (cb.spectrum) cb.spectrum(sp);
    }
    // Start delay (with progress every 0.25 s); false if stopped.
    bool waitUntil(double target, int k, int total, const QDateTime& end) {
        while (now() < target && !stop) {
            SeriesProgress p;
            p.n = k;
            p.total = total;
            p.next_in_s = target - now();
            p.elapsed_s = now() - start;
            p.end = end;
            progress(p);
            sleepS(std::min(0.25, std::max(0.0, target - now())));
        }
        return !stop;
    }
    void finish(int k, int total) {
        if (writer) {
            writer->finish();
            const QString e = writer->takeError();
            if (!e.isEmpty()) log("Series files: " + e + " (the spectra are saved one by one)");
        }
        log(QString("Series ") + (stop ? "stopped" : "finished") + ": " + QString::number(k) + " spectra in " + folder);
        SeriesProgress p;
        p.n = k;
        p.total = total;
        p.done = true;
        p.elapsed_s = now() - start;
        progress(p);
    }
};

}  // namespace

QString runTimeSeries(Instrument& inst, const SeriesSettings& s, const std::atomic<bool>& stop,
                      const SeriesCallbacks& cb, const std::atomic<bool>* pause) {
    Run r{inst, s, stop, cb, {}, 0.0, {}};
    const double t0 = now();
    if (s.start_delay_s > 0) r.log(QString("Series starts in %1 s").arg(py::fixed(s.start_delay_s, 0)));
    r.start = t0 + s.start_delay_s;

    if (s.laserStaysOn) {                     // controller _series_fastest
        // (The Python version ignores the start delay in this mode; here it is honoured.)
        const int total = s.stopByCount ? s.count : 0;
        if (!r.waitUntil(r.start, 0, total, {})) {
            r.finish(0, total);
            return r.folder;
        }
        r.start = now();
        r.makeFolder();
        const Vec bg = inst.dark(s.exposure_s, s.averages);
        int k = 0;
        AcquireOptions o;
        o.exposure_s = s.exposure_s;
        o.averages = s.averages;
        o.power_mw = s.power_mw;
        o.stop = &stop;
        inst.acquireLaserOn(
            o, bg,
            [&] {
                if (s.stopByCount && k >= s.count) return false;
                if (!s.stopByCount && now() - r.start > s.duration_s) return false;
                return true;
            },
            [&](Spectrum& sp) {
                r.record(sp, k, "as fast as possible, laser on, background once");
                ++k;
                SeriesProgress p;
                p.n = k;
                p.total = s.stopByCount ? s.count : k;
                p.elapsed_s = now() - r.start;
                r.progress(p);
            });
        r.finish(k, s.stopByCount ? s.count : k);
        return r.folder;
    }

    // controller _series
    const int total = s.stopByCount ? s.count : std::max(1, int(std::floor(s.duration_s / s.interval_s)) + 1);
    const QDateTime end = fromEpoch(r.start + (total - 1) * s.interval_s);
    r.makeFolder();
    const Vec pixels = inst.pixelNumbers(inst.channel());
    Vec bgOnce;
    int k = 0;
    while (k < total && !stop) {
        const double target = r.start + k * s.interval_s;
        if (!r.waitUntil(target, k, total, end)) break;
        if (now() - target > 1.0)
            r.log(QString("Behind schedule by %1 s (spectrum %2)").arg(py::fixed(now() - target, 1)).arg(k + 1));
        Status st = inst.status();
        const bool userPause = pause && *pause;
        if (!st.armed || st.compartment_open || userPause) {
            const bool disarmed = !st.armed || st.compartment_open;
            r.log(disarmed ? "Paused: not armed (compartment opened?). Press ARM on the instrument to continue."
                           : "Paused");
            while (!stop) {
                st = inst.status();
                if (st.armed && !st.compartment_open && !(pause && *pause)) {
                    r.log(disarmed ? "Armed again: continuing" : "Continuing");
                    break;
                }
                SeriesProgress p;
                p.n = k;
                p.total = total;
                p.paused = true;
                p.elapsed_s = now() - r.start;
                r.progress(p);
                sleepS(0.5);
            }
            if (stop) break;
        }
        if (s.background == "once" && bgOnce.empty()) bgOnce = inst.dark(s.exposure_s, s.averages);
        AcquireOptions o;
        o.exposure_s = s.exposure_s;
        o.averages = s.averages;
        o.background = s.background == "each";
        o.power_mw = s.power_mw;
        o.prebleach_s = s.prebleach_s;
        o.stop = &stop;
        Spectrum sp;
        try {
            sp = inst.acquire(o);
        } catch (const Stopped&) {
            break;
        }
        if (!bgOnce.empty()) applyBackground(sp, pixels, bgOnce);
        r.record(sp, k);
        ++k;
        SeriesProgress p;
        p.n = k;
        p.total = total;
        p.elapsed_s = now() - r.start;
        p.end = end;
        p.next_in_s = s.interval_s;
        r.progress(p);
    }
    r.finish(k, total);
    return r.folder;
}

}  // namespace lr
