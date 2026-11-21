#pragma once
// Wavelength calibration and laser data (Python: calibration.py), from the vendor's BS_Device_Settings.ini.
//   wavelength (nm) = c0 + c1 p + c2 p^2 + c3 p^3 for calibration pixel p
//   Raman shift (cm^-1) = 1e7/laser_nm - 1e7/wavelength
#include <QString>
#include <map>
#include <vector>

namespace lr {

struct ChannelCal {
    double laser_nm = 532.1;
    std::vector<double> wl_coeff;
    double pwr_coeff[2] = {0.0, 0.0};   // 1064: driver current (A) = c0 + c1 * P(mW)
    double pwr_max_mw = 0.0;
    double tec_wait_s = 3.0;
    std::vector<double> nist_coeff;
    double nist_range[2] = {195.0, 3500.0};

    double wavelength(double p) const;
    double shift(double p) const;
};

struct Calibration {
    QString serial;
    std::map<int, ChannelCal> channels;  // 532 / 1064
    QString source = "built-in";
};

const Calibration& defaultCalibration();          // the development instrument (S/N 99002169)
Calibration loadIni(const QString& path);         // vendor ini (latin-1)

// Factory calibrations of other instruments: the vendor's BS_Device_Settings.ini, stored per serial number as
// <config dir>/factory_<serial>.ini (config dir as for the user calibrations).
QString factoryIniPath(const QString& serial);
Calibration importFactoryIni(const QString& path);   // reads it (needs Sys_SN), stores the copy; returns it
bool hasStoredFactory(const QString& serial);

}  // namespace lr
