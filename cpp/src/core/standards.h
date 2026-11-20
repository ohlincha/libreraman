#pragma once
// Raman shift reference values (cm^-1) for calibration standards (Python: standards.py).
//
// Sources and confidence:
// * "ASTM" - ASTM E1840 (Standard Guide for Raman Shift Standards for Spectrometer Calibration), values as
//   commonly quoted; check them against the current edition before relying on them for accredited work.
// * "lit"  - typical literature values for neat liquids / pure solids at room temperature, generally
//   reliable to about +-1-2 cm^-1.
// Each band: position, relative strength 1-3 (weak/medium/strong; helps the matcher prefer strong bands)
// and tolerance (expected uncertainty of the reference value; bands() fills in the default).
#include <QString>
#include <QStringList>
#include <vector>

namespace lr::standards {

enum Strength { W = 1, M = 2, S = 3 };

struct Band {
    double position;
    int strength;
    double tolerance = -1.0;          // < 0: default of the source (bands())
};

struct Standard {
    QString name;                     // key of the Python STANDARDS dict
    QString kind;                     // "liquid" / "solid"
    QString source;                   // "ASTM" / "lit"
    QString note;                     // may be empty
    QStringList aka;
    std::vector<Band> bands;
};

const std::vector<Standard>& all();                 // STANDARDS, in the Python order
const Standard& find(const QString& name);          // by name or alias, case-insensitive; throws lr::Error
std::vector<Band> bands(const QString& name);       // tolerance filled in: 0.5 (ASTM) or 2.0 (lit)

}  // namespace lr::standards
