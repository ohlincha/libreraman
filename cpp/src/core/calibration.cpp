#include "calibration.h"

#include <QFile>
#include <QRegularExpression>
#include <QStringDecoder>
#include <cmath>

#include "errors.h"

namespace lr {

double ChannelCal::wavelength(double p) const {
    double s = 0, pk = 1;
    for (double c : wl_coeff) { s += c * pk; pk *= p; }
    return s;
}

double ChannelCal::shift(double p) const { return 1e7 / laser_nm - 1e7 / wavelength(p); }

const Calibration& defaultCalibration() {
    static const Calibration cal = [] {
        Calibration c;
        c.serial = "99002169";
        const std::vector<double> nist = {0.0971937, 0.228325, -0.0586762, 0.216023, -0.0977171, 0.0115596};
        ChannelCal a;
        a.laser_nm = 532.100;
        a.wl_coeff = {514.006252760493, 0.096682105209, -0.000006593998, 0.000000000058};
        a.pwr_coeff[0] = 0.289022; a.pwr_coeff[1] = 0.008822;
        a.pwr_max_mw = 490.0; a.tec_wait_s = 3.0; a.nist_coeff = nist;
        ChannelCal b;
        b.laser_nm = 1064.010;
        b.wl_coeff = {1067.709823464664, 1.417819808954, 0.000004218663, -0.000000348208};
        b.pwr_coeff[0] = 0.275808; b.pwr_coeff[1] = 0.002209;
        b.pwr_max_mw = 450.0; b.tec_wait_s = 60.0; b.nist_coeff = nist;
        c.channels[532] = a;
        c.channels[1064] = b;
        return c;
    }();
    return cal;
}

Calibration loadIni(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) throw Error(("cannot read " + path).toStdString());
    const QString text = QString::fromLatin1(f.readAll());
    std::map<QString, std::map<QString, QString>> ini;      // section -> key -> value (case-sensitive keys)
    QString sec;
    for (QString line : text.split(QRegularExpression("[\r\n]+"))) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(';') || line.startsWith('#')) continue;
        if (line.startsWith('[') && line.endsWith(']')) { sec = line.mid(1, line.size() - 2); continue; }
        const int eq = line.indexOf('=');
        if (eq > 0) ini[sec][line.left(eq).trimmed()] = line.mid(eq + 1).trimmed();
    }
    auto get = [&](const QString& s, const QString& k, const QString& d) {
        auto it = ini.find(s);
        if (it == ini.end()) return d;
        auto jt = it->second.find(k);
        return jt == it->second.end() ? d : jt->second;
    };
    Calibration cal;
    cal.serial = get("SYSTEM", "Sys_SN", "");
    cal.source = path;
    const int n = get("SYSTEM", "Sys_Laser_Count", "2").toInt();
    for (int i = 0; i < n; ++i) {
        const QString s = QString("LASER_%1").arg(i);
        if (!ini.count(s)) continue;
        auto f = [&](const QString& k, double d = 0.0) { return get(s, k, QString::number(d, 'g', 17)).toDouble(); };
        ChannelCal c;
        c.laser_nm = f("LS_WL");
        for (int k = 0; k < 4; ++k) c.wl_coeff.push_back(f(QString("LS_WL_Coeff_%1").arg(k)));
        c.pwr_coeff[0] = f("LS_Pwr_Coeff_0");
        c.pwr_coeff[1] = f("LS_Pwr_Coeff_1");
        c.pwr_max_mw = f("LS_Pwr_Max");
        c.tec_wait_s = get("SYSTEM", QString("Sys_TEC_Wait_Time_%1").arg(i), "3").toDouble();
        for (int k = 0; k < 6; ++k) {
            const QString key = QString("NIST_Coeff_%1").arg(k, 2, 10, QChar('0'));
            if (ini[s].count(key)) c.nist_coeff.push_back(f(key));
        }
        c.nist_range[0] = f("NIST_WL_Min", 195);
        c.nist_range[1] = f("NIST_WL_Max", 3500);
        cal.channels[c.laser_nm < 800 ? 532 : 1064] = c;
    }
    return cal;
}

}  // namespace lr
