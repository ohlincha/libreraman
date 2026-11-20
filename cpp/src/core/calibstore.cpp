// TEMPORARY minimal implementation of Poly::operator(), calib::configDir() and calib::load() so that the
// numerics branch links and can be tested on its own; replaced by the engine's calibstore.cpp at merge.
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

#include "calibrate.h"

namespace lr {

double Poly::operator()(double x) const {          // calibrate.polyval
    const double t = (x - x0) / scale;
    double s = 0.0;
    for (size_t i = 0; i < coeff.size(); ++i) s += coeff[i] * std::pow(t, double(i));
    return s;
}

namespace calib {

QString configDir() {                               // calibrate.config_dir
    const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    const QString base = xdg.isEmpty() ? QDir::homePath() + "/.config" : xdg;
    return base + "/libreramen";
}

std::optional<UserCalibration> load(int channel, const QString& serial, const QString& path0) {
    const QString path = path0.isEmpty()
        ? configDir() + QString("/calibration_%1_%2.json").arg(serial).arg(channel) : path0;
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject p = o.value("poly").toObject();
    UserCalibration u;
    for (const QJsonValue& c : p.value("coeff").toArray()) u.poly.coeff.push_back(c.toDouble());
    u.poly.x0 = p.value("x0").toDouble();
    u.poly.scale = p.value("scale").toDouble(1.0);
    u.date = o.value("date").toString();
    for (const QJsonValue& s : o.value("standards").toArray()) u.standards.push_back(s.toString());
    u.rms = o.value("rms").toDouble();
    return u;
}

}  // namespace calib
}  // namespace lr
