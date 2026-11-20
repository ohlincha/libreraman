"""
Wavelength calibration and laser data, from the vendor's BS_System/BS_Device_Settings.ini.

Wavelength (nm) = c0 + c1*p + c2*p^2 + c3*p^3 for calibration pixel p;
Raman shift (cm^-1) = 1e7/laser_nm - 1e7/wavelength_nm.
"""

import configparser
from dataclasses import dataclass, field


@dataclass
class ChannelCal:
    laser_nm: float
    wl_coeff: tuple
    pwr_coeff: tuple = (0.0, 0.0)     # 1064: driver current (A) = c0 + c1 * P(mW)
    pwr_max_mw: float = 0.0
    tec_wait_s: float = 3.0
    nist_coeff: tuple = ()
    nist_range: tuple = (195.0, 3500.0)

    def wavelength(self, p):
        return sum(c * p ** k for k, c in enumerate(self.wl_coeff))

    def shift(self, p):
        return 1e7 / self.laser_nm - 1e7 / self.wavelength(p)


@dataclass
class Calibration:
    serial: str = ""
    channels: dict = field(default_factory=dict)    # 532 / 1064 -> ChannelCal
    source: str = "built-in"


# Values of the development instrument (serial 99002169), used when no ini file is given.
DEFAULT = Calibration(
    serial="99002169",
    channels={
        532: ChannelCal(532.100, (514.006252760493, 0.096682105209, -0.000006593998, 0.000000000058),
                        (0.289022, 0.008822), 490.0, 3.0,
                        (0.0971937, 0.228325, -0.0586762, 0.216023, -0.0977171, 0.0115596)),
        1064: ChannelCal(1064.010, (1067.709823464664, 1.417819808954, 0.000004218663, -0.000000348208),
                         (0.275808, 0.002209), 450.0, 60.0,
                         (0.0971937, 0.228325, -0.0586762, 0.216023, -0.0977171, 0.0115596)),
    })


def load_ini(path):
    cp = configparser.ConfigParser(strict=False, interpolation=None)
    cp.optionxform = str
    with open(path, encoding="latin-1") as fh:
        cp.read_file(fh)
    cal = Calibration(serial=cp.get("SYSTEM", "Sys_SN", fallback=""), source=path)
    for i in range(int(cp.get("SYSTEM", "Sys_Laser_Count", fallback="2"))):
        sec = f"LASER_{i}"
        if sec not in cp:
            continue
        s = cp[sec]
        f = lambda k, d=0.0: float(s.get(k, d))  # noqa: E731
        wl = f("LS_WL")
        ch = 532 if wl < 800 else 1064
        tec = float(cp.get("SYSTEM", f"Sys_TEC_Wait_Time_{i}", fallback="3"))
        nist = tuple(f(f"NIST_Coeff_{k:02d}") for k in range(6) if f"NIST_Coeff_{k:02d}" in s)
        cal.channels[ch] = ChannelCal(
            wl, tuple(f(f"LS_WL_Coeff_{k}") for k in range(4)),
            (f("LS_Pwr_Coeff_0"), f("LS_Pwr_Coeff_1")), f("LS_Pwr_Max"), tec, nist,
            (f("NIST_WL_Min", 195), f("NIST_WL_Max", 3500)))
    return cal
