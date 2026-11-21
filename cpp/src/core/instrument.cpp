// Shared helpers of the Instrument interface (Python: instrument.Cora.shift_of, load_user_calibration,
// _spectrum, _mean).
#include "instrument.h"

#include <QStringList>
#include <algorithm>
#include <numeric>

#include "calibrate.h"
#include "errors.h"
#include "pyformat.h"

namespace lr {

namespace {
QString joined(const std::vector<QString>& v) {
    QStringList l;
    for (const auto& s : v) l << s;
    return l.join(", ");
}
}  // namespace

double Instrument::shiftOf(int ch, double p) const {
    const auto it = userCal_.find(ch);
    if (it != userCal_.end()) return it->second.poly(p);
    const auto jt = cal_.channels.find(ch);
    if (jt == cal_.channels.end()) throw Error(QString("no calibration for %1 nm").arg(ch).toStdString());
    return jt->second.shift(p);
}

QString Instrument::calibrationLabel(int ch) const {
    const auto it = userCal_.find(ch);
    if (it != userCal_.end()) return QString("user %1 (%2)").arg(it->second.date, joined(it->second.standards));
    return QString("factory (%1)").arg(cal_.source);
}

void Instrument::selectFactoryCalibration(const QString& serial) {
    if (serial.isEmpty() || serial == QLatin1String("?")) {
        loadUserCalibration();
        return;
    }
    if (hasStoredFactory(serial)) {
        try {
            Calibration c = loadIni(factoryIniPath(serial));
            if (!c.channels.empty()) {
                c.serial = serial;
                c.source = factoryIniPath(serial);
                cal_ = std::move(c);
                log(QString("Factory calibration of S/N %1 from %2").arg(serial, cal_.source));
            }
        } catch (const std::exception& e) {
            log(QString("Could not read the factory calibration of S/N %1: %2").arg(serial, e.what()));
        }
    } else if (serial != cal_.serial) {
        log(QString("No factory calibration for S/N %1: using the built-in values of S/N %2, which are only "
                    "approximate for this instrument. Load its BS_Device_Settings.ini with Instrument > Load "
                    "factory calibration (the file is in the vendor software's BS_System folder).")
                .arg(serial, cal_.serial));
        cal_.source = QString("built-in values of S/N %1, not this instrument").arg(cal_.serial);
        cal_.serial = serial;                      // user calibrations belong to this instrument
    }
    userCal_.clear();
    loadUserCalibration();
}

void Instrument::loadUserCalibration() {
    for (int ch : {532, 1064}) {
        const auto c = calib::load(ch, cal_.serial);
        if (!c) continue;
        userCal_[ch] = *c;
        log(QString("Using calibration for %1 nm from %2 (%3, RMS %4 cm-1)")
                .arg(ch).arg(c->date, joined(c->standards), py::fixed(c->rms, 2)));
    }
}

Spectrum Instrument::makeSpectrum(int ch, double exposure_s, int averages, const Vec& pixels, const Vec& sig,
                                  const Vec& bg, double power_mw) const {
    const size_t n = std::min({pixels.size(), sig.size(), bg.size()});
    struct Row {
        double shift, sig, bg, px;
    };
    std::vector<Row> rows(n);
    for (size_t i = 0; i < n; ++i) rows[i] = {shiftOf(ch, pixels[i]), sig[i], bg[i], pixels[i]};
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {   // Python sorts the tuples
        if (a.shift != b.shift) return a.shift < b.shift;
        if (a.sig != b.sig) return a.sig < b.sig;
        if (a.bg != b.bg) return a.bg < b.bg;
        return a.px < b.px;
    });
    Spectrum sp;
    sp.channel = ch;
    sp.exposure_s = exposure_s;
    sp.averages = averages;
    for (const Row& r : rows) {
        sp.shift.push_back(r.shift);
        sp.intensity.push_back(r.sig - r.bg);
        sp.signal.push_back(r.sig);
        sp.background.push_back(r.bg);
        sp.pixels.push_back(r.px);
    }
    const auto c = cal_.channels.find(ch);
    sp.setMeta("laser_nm", py::str(c != cal_.channels.end() ? c->second.laser_nm : 0.0));
    sp.setMeta("calibration", calibrationLabel(ch));
    if (ch == 1064) {
        sp.setMeta("power_mw", py::str(power_mw > 0 ? power_mw : 50.0));   // Cora.power_mw[1064] default
        sp.setMeta("note", "1064 nm pixel orientation not yet verified");
    } else {
        sp.setMeta("power", "set on the instrument (not computer-controlled)");
    }
    return sp;
}

Vec meanFrames(const std::vector<Vec>& frames) {
    if (frames.empty()) return {};
    size_t n = frames[0].size();
    for (const Vec& f : frames) n = std::min(n, f.size());          // zip(*frames)
    Vec m(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        double s = 0.0;                                             // sum() in frame order, as Python
        for (const Vec& f : frames) s += f[i];
        m[i] = s / double(frames.size());
    }
    return m;
}

}  // namespace lr
