// User calibration storage shared with the Python version (calibrate.py: polyval, config_dir, load).
// The rest of calibrate.py is ported in calibrate.cpp.
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

#include "calibrate.h"
#include "errors.h"

namespace lr {

double Poly::operator()(double x) const {     // calibrate.polyval
    const double t = (x - x0) / scale;
    double s = 0.0;
    for (size_t k = 0; k < coeff.size(); ++k) s += coeff[k] * std::pow(t, double(k));
    return s;
}

namespace calib {

QString configDir() {
    // os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config") - on every platform, so the
    // Python and C++ versions share the files.
    const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    const QString base = xdg.isEmpty() ? QDir::homePath() + "/.config" : xdg;
    return QDir::cleanPath(base + "/libreraman");
}

std::optional<UserCalibration> load(int channel, const QString& serial, const QString& path) {
    const QString p = path.isEmpty() ? configDir() + QString("/calibration_%1_%2.json").arg(serial).arg(channel) : path;
    QFile f(p);
    if (!f.exists()) return std::nullopt;
    if (!f.open(QIODevice::ReadOnly)) throw Error(("cannot read " + p).toStdString());
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        throw Error(("invalid calibration file " + p + ": " + err.errorString()).toStdString());
    const QJsonObject o = doc.object();
    const QJsonObject poly = o.value("poly").toObject();
    if (!poly.value("coeff").isArray()) throw Error(("invalid calibration file " + p + ": no polynomial").toStdString());
    UserCalibration c;
    for (const QJsonValue& v : poly.value("coeff").toArray()) c.poly.coeff.push_back(v.toDouble());
    c.poly.x0 = poly.value("x0").toDouble(0.0);
    c.poly.scale = poly.value("scale").toDouble(1.0);
    c.date = o.value("date").toString();
    for (const QJsonValue& v : o.value("standards").toArray()) c.standards.push_back(v.toString());
    c.rms = o.value("rms").toDouble(0.0);
    return c;
}

}  // namespace calib
}  // namespace lr
