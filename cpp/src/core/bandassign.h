#pragma once
// Raman group frequencies and characteristic bands (resources/band_assignments.csv, built in as
// :/core/band_assignments.csv): help for labelling the bands of a spectrum.
#include <QString>

#include <vector>

namespace lr::bands {

struct Band {
    QString category;   // organic | inorganic | both
    QString group;      // e.g. "C=O (ketone; saturated)"
    QString mode;       // e.g. "ν(C=O)"
    double lo = 0, hi = 0;  // cm⁻¹; lo == hi: a sharp characteristic band
    QString raman;      // vs, s, m, w, var
    QString note;
    bool organic() const { return category != QLatin1String("inorganic"); }
    bool inorganic() const { return category != QLatin1String("organic"); }
    bool sharp() const { return hi - lo < 4; }
    double width() const { return hi - lo; }
    QString label() const;   // "group: mode lo–hi cm⁻¹ (raman)"
};

// All bands of the built-in table (Raman-inactive rows without a position are left out).
const std::vector<Band>& all();
// Parses the CSV format of the table (comment lines start with '#'; first other line is the header).
std::vector<Band> parse(const QString& csvText);
// Bands of the selected categories whose range contains pos (± tol cm⁻¹): those containing pos itself first,
// then by width (most specific first).
std::vector<const Band*> candidates(double pos, bool organic, bool inorganic, double tol = 5);

}  // namespace lr::bands
