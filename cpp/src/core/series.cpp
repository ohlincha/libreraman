#include "series.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QTextStream>
#include <QThread>
#include <cmath>
#include <map>

#include "errors.h"
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

struct Run {
    Instrument& inst;
    const SeriesSettings& s;
    const std::atomic<bool>& stop;
    const SeriesCallbacks& cb;
    QString folder, table;
    double start = 0.0;
    std::vector<SeriesColumn> cols;

    void log(const QString& m) const {
        if (cb.log) cb.log(m);
    }
    void progress(const SeriesProgress& p) const {
        if (cb.progress) cb.progress(p);
    }
    void makeFolder() {
        folder = QDir(s.folder).filePath(safeName(s.sample) + "_series_" +
                                         QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
        if (!QDir().mkpath(folder)) throw Error(("cannot create " + folder).toStdString());
        table = QDir(folder).filePath("series.csv");
    }
    // Saves the spectrum, rewrites series.csv (atomically), emits it.
    void record(Spectrum& sp, int k, const QString& mode = {}) {
        sp.setMeta("sample", s.sample);
        sp.setMeta("series_index", QString::number(k + 1));
        sp.setMeta("series_elapsed_s", py::str(std::round((epoch(sp.t) - start) * 10.0) / 10.0));
        if (!mode.isEmpty()) sp.setMeta("mode", mode);
        sp.saveCsv(QDir(folder).filePath(QString("%1_%2.csv").arg(k + 1, 4, 10, QChar('0')).arg(sp.t.toString("HHmmss"))));
        cols.push_back({epoch(sp.t) - start, sp.intensity});
        QSaveFile f(table);                   // controller: write series.csv.tmp, os.replace
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) throw Error(("cannot write " + table).toStdString());
        {
            QTextStream o(&f);
            const QString header = "# LibreRamen time series, started " + iso(fromEpoch(start)) +
                                   "\n# sample: " + s.sample + "\n# exposure_s: " + py::str(s.exposure_s) +
                                   "\n# averages: " + QString::number(s.averages) +
                                   "\n# interval_s: " + py::str(s.interval_s) + "\n# background: " + s.background +
                                   "\n";
            writeTable(o, header, cols, sp.shift);
        }
        if (!f.commit()) throw Error(("cannot write " + table).toStdString());
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
    Run r{inst, s, stop, cb, {}, {}, 0.0, {}};
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
