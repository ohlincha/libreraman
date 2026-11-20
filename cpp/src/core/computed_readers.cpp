// Readers for quantum-chemistry output with vibrational (Raman) data; see computed.h for the formats.
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include "computed.h"
#include "errors.h"

namespace lr::computed {

namespace {

constexpr double kBohr = 0.529177210903;      // Å
constexpr double kThzToCm = 33.35640951981521;
constexpr double kMevToCm = 8.065543937;

[[noreturn]] void fail(const QString& msg) { throw Error(msg.toStdString()); }

QStringList splitLines(const QString& text) {
    QStringList l = text.split('\n');
    for (QString& s : l)
        if (s.endsWith('\r')) s.chop(1);
    return l;
}

const QRegularExpression& wsRe() {
    static const QRegularExpression re("\\s+");
    return re;
}

QStringList tokens(const QString& s) { return s.split(wsRe(), Qt::SkipEmptyParts); }

bool toNum(QString s, double& v) {
    if (s.isEmpty()) return false;
    s.replace('D', 'E').replace('d', 'e');
    bool ok = false;
    v = s.toDouble(&ok);
    return ok && std::isfinite(v);
}

double num(const QString& s) {
    double v;
    return toNum(s, v) ? v : kNaN;
}

bool isNum(const QString& s) {
    double v;
    return toNum(s, v);
}

bool isInt(const QString& s) {
    bool ok = false;
    s.toInt(&ok);
    return ok;
}

Vec numbers(const QString& s) {             // all numeric tokens of a line, in order
    Vec out;
    for (const QString& t : tokens(s)) {
        double v;
        if (toNum(t, v)) out.push_back(v);
    }
    return out;
}

bool isDashes(const QString& t) {
    return t.size() >= 3 && std::all_of(t.begin(), t.end(), [](QChar c) { return c == '-' || c == ' ' || c == '='; });
}

const QStringList& elementSymbols() {
    static const QStringList s = {
        "X",  "H",  "He", "Li", "Be", "B",  "C",  "N",  "O",  "F",  "Ne", "Na", "Mg", "Al", "Si", "P",  "S",
        "Cl", "Ar", "K",  "Ca", "Sc", "Ti", "V",  "Cr", "Mn", "Fe", "Co", "Ni", "Cu", "Zn", "Ga", "Ge", "As",
        "Se", "Br", "Kr", "Rb", "Sr", "Y",  "Zr", "Nb", "Mo", "Tc", "Ru", "Rh", "Pd", "Ag", "Cd", "In", "Sn",
        "Sb", "Te", "I",  "Xe", "Cs", "Ba", "La", "Ce", "Pr", "Nd", "Pm", "Sm", "Eu", "Gd", "Tb", "Dy", "Ho",
        "Er", "Tm", "Yb", "Lu", "Hf", "Ta", "W",  "Re", "Os", "Ir", "Pt", "Au", "Hg", "Tl", "Pb", "Bi", "Po",
        "At", "Rn", "Fr", "Ra", "Ac", "Th", "Pa", "U"};
    return s;
}

QString elementOf(int z) { return z > 0 && z < elementSymbols().size() ? elementSymbols()[z] : QString("X"); }

// "CL" / "cl1" / "C_a" -> "Cl" / "Cl" / "C"
QString normElement(const QString& tag) {
    QString letters;
    for (QChar c : tag) {
        if (!c.isLetter()) break;
        letters += c;
    }
    if (letters.isEmpty()) return {};
    QString two = letters.left(2);
    two = two.left(1).toUpper() + two.mid(1).toLower();
    if (two.size() == 2 && elementSymbols().contains(two)) return two;
    return letters.left(1).toUpper();
}

void finish(Calculation& c) {
    // periodic codes: acoustic modes at Gamma / translations come out as small (often negative) numbers
    // without Raman activity; drop them so that they do not count as imaginary modes
    if (c.format == "qe" || c.format == "castep" || c.format == "phonopy" || c.format == "vasp-raman") {
        double amax = 0;
        for (const Mode& m : c.modes)
            if (std::isfinite(m.activity)) amax = std::max(amax, m.activity);
        c.modes.erase(std::remove_if(c.modes.begin(), c.modes.end(),
                                     [&](const Mode& m) {
                                         return std::abs(m.frequency) < 20.0 &&
                                                (!std::isfinite(m.activity) || m.activity <= 1e-3 * amax);
                                     }),
                      c.modes.end());
    }
    c.modes.erase(std::remove_if(c.modes.begin(), c.modes.end(),
                                 [](const Mode& m) { return !std::isfinite(m.frequency) || std::abs(m.frequency) < 1.0; }),
                  c.modes.end());
    std::stable_sort(c.modes.begin(), c.modes.end(), [](const Mode& a, const Mode& b) { return a.frequency < b.frequency; });
    if (c.formula.isEmpty() && !c.atoms.empty()) {
        QStringList el;
        for (const Atom& a : c.atoms) el << a.element;
        c.formula = hillFormula(el);
    }
    const int im = c.imaginaryModes();
    if (im > 0) {
        QStringList f;
        for (const Mode& m : c.modes)
            if (m.frequency < 0) f << QString::number(-m.frequency, 'f', 1) + "i";
        c.warnings << QString("%1 imaginary frequenc%2 (%3 cm⁻¹): not a minimum")
                          .arg(im)
                          .arg(im == 1 ? "y" : "ies")
                          .arg(f.join(", "));
    }
    bool allZero = !c.modes.empty();
    for (const Mode& m : c.modes)
        if (!std::isfinite(m.activity) || m.activity != 0.0) allZero = false;
    if (allZero) {                         // printed but not computed (e.g. xtb --ohess, Gaussian freq without raman)
        for (Mode& m : c.modes) m.activity = kNaN;
        c.warnings << "all Raman activities in this file are zero (not computed in this run): positions only "
                      "(unit activities)";
    } else if (!c.modes.empty() && !c.hasActivities()) {
        c.warnings << "no Raman activities in this file: positions only (unit activities)";
    }
}

// Mode with a given 1-based / 0-based number in a map, created on demand.
Mode& modeAt(std::map<int, Mode>& m, int k) { return m[k]; }

std::vector<Mode> modesOf(const std::map<int, Mode>& m) {
    std::vector<Mode> out;
    for (const auto& kv : m) out.push_back(kv.second);
    return out;
}

// ============================================================================================ Gaussian
std::vector<Calculation> readGaussian(const QStringList& L) {
    struct Section {
        Calculation c;
        bool hp = false;
        int job = 0;
    };
    std::vector<Section> secs;
    int job = 0;
    QString version, routeMethod, routeBasis, scfMethod, stdBasis, formula;
    std::vector<Atom> stdGeom, inGeom;
    static const QRegularExpression verRe("Gaussian\\s+(\\d+):\\s+\\S*?Rev([A-Z]\\.\\d+)");
    static const QRegularExpression scfRe("SCF Done:\\s+E\\(([^)]+)\\)");
    static const QRegularExpression labelRe("^\\s*(\\S.*?)\\s+(-{2,3})\\s+(.*)$");

    auto readGeom = [&](int i) {
        std::vector<Atom> g;
        int j = i + 1;
        int dashes = 0;
        for (; j < L.size() && dashes < 2; ++j)
            if (L[j].trimmed().startsWith("---")) ++dashes;
        for (; j < L.size(); ++j) {
            const QString t = L[j].trimmed();
            if (t.startsWith("---")) break;
            const QStringList tk = tokens(t);
            if (tk.size() < 5) break;
            Atom a;
            a.element = elementOf(tk[1].toInt());
            a.position = {num(tk[tk.size() - 3]), num(tk[tk.size() - 2]), num(tk[tk.size() - 1])};
            g.push_back(a);
        }
        return g;
    };

    for (int i = 0; i < L.size(); ++i) {
        const QString& s = L[i];
        const QString t = s.trimmed();
        if (t.startsWith("Entering Link 1")) {
            ++job;
            routeMethod.clear();
            routeBasis.clear();
            scfMethod.clear();
            continue;
        }
        if (version.isEmpty()) {
            const auto m = verRe.match(t);
            if (m.hasMatch()) version = m.captured(1) + " Rev. " + m.captured(2);
        }
        if (t.startsWith('#') && routeMethod.isEmpty()) {
            QString route = t;
            for (int j = i + 1; j < L.size() && !L[j].trimmed().startsWith("---"); ++j) route += " " + L[j].trimmed();
            for (const QString& tok : tokens(route.mid(1))) {
                const int sl = tok.indexOf('/');
                const int eq = tok.indexOf('=');
                if (sl > 0 && (eq < 0 || eq > sl) && !tok.startsWith("freq", Qt::CaseInsensitive) &&
                    !tok.startsWith("opt", Qt::CaseInsensitive)) {
                    routeMethod = tok.left(sl).toUpper();
                    routeBasis = tok.mid(sl + 1);
                    break;
                }
            }
            continue;
        }
        if (t.startsWith("SCF Done:")) {
            const auto m = scfRe.match(t);
            if (m.hasMatch()) {
                QString x = m.captured(1);
                if (x.startsWith("RO") && x.size() > 3) x = x.mid(2);
                else if ((x.startsWith('R') || x.startsWith('U')) && x.size() > 2) x = x.mid(1);
                scfMethod = x;
            }
            continue;
        }
        if (t.startsWith("Standard basis:")) {
            QString b = t.mid(15).trimmed();
            const int p = b.indexOf(" (");
            stdBasis = p > 0 ? b.left(p) : b;
            continue;
        }
        if (t.startsWith("Stoichiometry")) {
            const QStringList tk = tokens(t);
            if (tk.size() > 1) {
                formula = tk[1];
                const int p = formula.indexOf('(');
                if (p > 0) formula = formula.left(p);
            }
            continue;
        }
        if (t.startsWith("Standard orientation:")) {
            stdGeom = readGeom(i);
            continue;
        }
        if (t.startsWith("Input orientation:")) {
            inGeom = readGeom(i);
            continue;
        }
        if (!t.startsWith("Harmonic frequencies (cm**-1)")) continue;

        // ---- one frequency section
        Section sec;
        sec.job = job;
        Calculation& c = sec.c;
        std::vector<int> cur;                  // mode indices of the current block
        bool activitySet = false;
        int j = i + 1;
        for (; j < L.size(); ++j) {
            const QString u = L[j].trimmed();
            if (u.startsWith("Harmonic frequencies") || u.contains("- Thermochemistry -") ||
                u.startsWith("Normal termination") || u.startsWith("Error termination") || u.startsWith("Temperature "))
                break;
            if (u.startsWith("Coord Atom Element:")) {          // HPModes displacements
                for (int k = j + 1; k < L.size(); ++k) {
                    const QStringList tk = tokens(L[k]);
                    if (tk.size() != 3 + int(cur.size()) || !isInt(tk[0]) || !isInt(tk[1])) break;
                    const int coord = tk[0].toInt() - 1, atom = tk[1].toInt() - 1;
                    if (coord < 0 || coord > 2 || atom < 0) break;
                    for (int q = 0; q < int(cur.size()); ++q) {
                        auto& d = c.modes[cur[q]].displacement;
                        if (int(d.size()) <= atom) d.resize(atom + 1, {0, 0, 0});
                        d[atom][coord] = num(tk[3 + q]);
                    }
                    j = k;
                }
                continue;
            }
            if (u.startsWith("Atom") && u.contains("AN")) {     // normal displacements
                for (int k = j + 1; k < L.size(); ++k) {
                    const QStringList tk = tokens(L[k]);
                    if (tk.size() != 2 + 3 * int(cur.size()) || !isInt(tk[0]) || !isInt(tk[1])) break;
                    for (int q = 0; q < int(cur.size()); ++q)
                        c.modes[cur[q]].displacement.push_back(
                            {num(tk[2 + 3 * q]), num(tk[3 + 3 * q]), num(tk[4 + 3 * q])});
                    j = k;
                }
                continue;
            }
            const auto m = labelRe.match(L[j]);
            if (!m.hasMatch()) continue;
            const QString label = m.captured(1).toLower();
            const Vec v = numbers(m.captured(3));
            if (label.startsWith("frequencies")) {
                if (m.captured(2) == "---") sec.hp = true;
                cur.clear();
                activitySet = false;
                QStringList sym;
                if (j > 0) {
                    sym = tokens(L[j - 1]);
                    if (int(sym.size()) != int(v.size()) || std::all_of(sym.begin(), sym.end(), isInt)) sym.clear();
                }
                for (int q = 0; q < int(v.size()); ++q) {
                    Mode md;
                    md.frequency = v[q];
                    if (!sym.isEmpty()) md.symmetry = sym[q] == "?A" ? QString() : sym[q];
                    cur.push_back(int(c.modes.size()));
                    c.modes.push_back(md);
                }
                continue;
            }
            if (cur.empty() || int(v.size()) != int(cur.size())) continue;
            auto put = [&](double Mode::*field) {
                for (int q = 0; q < int(cur.size()); ++q) c.modes[cur[q]].*field = v[q];
            };
            if (label.startsWith("ir inten")) put(&Mode::ir);
            else if ((label.startsWith("raman activ") || label.startsWith("raman1")) && !activitySet) {
                put(&Mode::activity);
                activitySet = true;
            } else if (label.startsWith("depolar") && label.contains("(p)")) put(&Mode::depolarization);
        }
        i = j - 1;
        c.program = "Gaussian";
        c.version = version;
        c.format = "gaussian";
        c.method = !routeMethod.isEmpty() ? routeMethod : scfMethod;
        c.basis = !stdBasis.isEmpty() ? stdBasis : routeBasis;
        c.formula = formula;
        c.atoms = !stdGeom.empty() ? stdGeom : inGeom;
        if (!c.modes.empty()) secs.push_back(sec);
    }

    std::set<int> jobsWithNormal;
    for (const auto& s : secs)
        if (!s.hp) jobsWithNormal.insert(s.job);
    std::vector<Calculation> out;
    for (auto& s : secs)
        if (!s.hp || !jobsWithNormal.count(s.job)) out.push_back(s.c);
    return out;
}

// =============================================================================================== ORCA
std::vector<Calculation> readOrca(const QStringList& L) {
    std::vector<Calculation> out;
    static const QRegularExpression verRe("Program Version\\s+(\\S+)");
    static const QRegularExpression inputRe("^\\|\\s*\\d+>\\s*!(.*)$");
    static const QRegularExpression rowRe("^\\s*(\\d+):\\s+(.*)$");
    static const QRegularExpression basisRe("^(def2-|def2/|cc-p|aug-|6-31|6-311|sto-|ma-|pcseg|pc-|ano-|sarc|x2c|zora-|dkh-|minix|madef2|jun-|may-)",
                                            QRegularExpression::CaseInsensitiveOption);
    static const QSet<QString> jobWords = {
        "OPT", "FREQ", "NUMFREQ", "ANFREQ", "RAMAN", "TIGHTSCF", "VERYTIGHTSCF", "EXTREMESCF", "NORMALSCF", "LOOSESCF",
        "TIGHTOPT", "VERYTIGHTOPT", "LOOSEOPT", "RIJCOSX", "RIJK", "RIJONX", "RI", "NORI", "D3", "D3BJ", "D3ZERO",
        "D4", "DEFGRID1", "DEFGRID2", "DEFGRID3", "GRID4", "GRID5", "GRID6", "GRID7", "FINALGRID5", "FINALGRID6",
        "NOFINALGRID", "LARGEPRINT", "MINIPRINT", "NORMALPRINT", "SMALLPRINT", "KEEPDENS", "NUMGRAD", "UKS", "RKS",
        "UHF", "RHF", "ROHF", "SP", "ENGRAD", "MOREAD", "NOAUTOSTART", "PRINTBASIS", "XYZFILE", "PMODEL", "HUECKEL",
        "SLOWCONV", "VERYSLOWCONV", "KDIIS", "SOSCF", "NOSOSCF", "NOTRAH", "TRAH", "AUTOAUX", "NOPOP", "PRINTMOS"};

    // The input echo at the top lists every job ($new_job); method/basis keywords per job.
    QString version, basis;
    QStringList methods{QString()}, inputBases{QString()};
    std::vector<Atom> geom;
    std::map<int, Mode> modes;
    int section = -1;                      // frequency section (= job with a frequency calculation)
    QString curBasis;
    std::vector<Atom> curGeom;
    auto flush = [&]() {
        if (modes.empty()) return;
        Calculation c;
        c.program = "ORCA";
        c.version = version;
        c.format = "orca";
        const int k = std::clamp(section, 0, int(methods.size()) - 1);
        c.method = methods.value(k);
        c.basis = !curBasis.isEmpty() ? curBasis : inputBases.value(k);
        c.atoms = curGeom;
        c.modes = modesOf(modes);
        out.push_back(c);
        modes.clear();
    };
    auto startSection = [&]() {
        flush();
        ++section;
        curBasis = basis;
        curGeom = geom;
    };
    // rows "  6:   215.43 ..." after a header line (blank/dash/header lines before the first row)
    auto rows = [&](int i, const std::function<void(int, const QString&)>& f) {
        int j = i + 1;
        bool started = false;
        for (; j < L.size(); ++j) {
            const auto m = rowRe.match(L[j]);
            if (m.hasMatch()) {
                started = true;
                f(m.captured(1).toInt(), m.captured(2));
            } else if (started) {
                break;
            } else if (j - i > 12) {
                break;
            }
        }
        return j - 1;
    };

    for (int i = 0; i < L.size(); ++i) {
        const QString t = L[i].trimmed();
        if (version.isEmpty()) {
            const auto m = verRe.match(t);
            if (m.hasMatch()) version = m.captured(1);
        }
        if (t.startsWith('|') && t.contains("> $new_job", Qt::CaseInsensitive)) {
            methods << QString();
            inputBases << QString();
            continue;
        }
        {
            const auto m = inputRe.match(t);
            if (m.hasMatch()) {
                for (const QString& kw : tokens(m.captured(1))) {
                    const QString up = kw.toUpper();
                    if (basisRe.match(kw).hasMatch()) {
                        if (inputBases.last().isEmpty() && !kw.contains('/')) inputBases.last() = kw;
                    } else if (!jobWords.contains(up) && !up.startsWith("PAL") && !up.startsWith("CPCM") &&
                               !up.startsWith("SMD") && methods.last().isEmpty()) {
                        methods.last() = kw;
                    }
                }
                continue;
            }
        }
        if (t.startsWith("Your calculation utilizes the basis:")) {
            basis = t.mid(t.indexOf(':') + 1).trimmed();
            continue;
        }
        if (t == "CARTESIAN COORDINATES (ANGSTROEM)") {
            geom.clear();
            for (int j = i + 2; j < L.size(); ++j) {
                const QStringList tk = tokens(L[j]);
                if (tk.size() != 4 || !isNum(tk[1])) break;
                geom.push_back({normElement(tk[0]), {num(tk[1]), num(tk[2]), num(tk[3])}});
            }
            continue;
        }
        if (t == "VIBRATIONAL FREQUENCIES") {
            startSection();
            i = rows(i, [&](int k, const QString& rest) {
                const Vec v = numbers(rest);
                if (!v.empty()) modeAt(modes, k).frequency = v[0];
            });
            continue;
        }
        if (t == "RAMAN SPECTRUM") {
            i = rows(i, [&](int k, const QString& rest) {
                const Vec v = numbers(rest);
                if (v.size() < 2) return;
                Mode& md = modeAt(modes, k);
                md.frequency = v[0];
                md.activity = v[1];
                if (v.size() > 2) md.depolarization = v[2];
            });
            continue;
        }
        if (t == "IR SPECTRUM") {
            bool km = false;
            for (int j = i + 1; j < std::min<int>(L.size(), i + 6); ++j)
                if (L[j].contains("km/mol", Qt::CaseInsensitive)) km = true;
            i = rows(i, [&](int k, const QString& rest) {
                QString r = rest;
                const int p = r.indexOf('(');
                if (p >= 0) r = r.left(p);
                const Vec v = numbers(r);
                Mode& md = modeAt(modes, k);
                if (!v.empty()) md.frequency = v[0];
                if (km && v.size() > 2) md.ir = v[2];
                else if (!km && v.size() > 1) md.ir = v[1];
            });
            continue;
        }
    }
    flush();
    return out;
}

std::vector<Calculation> readOrcaHess(const QStringList& L) {
    Calculation c;
    c.program = "ORCA";
    c.format = "orca-hess";
    std::map<int, Mode> modes;
    for (int i = 0; i < L.size(); ++i) {
        const QString t = L[i].trimmed();
        if (!t.startsWith('$') || i + 1 >= L.size()) continue;
        const int n = L[i + 1].trimmed().toInt();
        auto row = [&](int k) { return k + i + 2 < L.size() ? numbers(L[k + i + 2]) : Vec{}; };
        if (t == "$vibrational_frequencies") {
            for (int k = 0; k < n; ++k) {
                const Vec v = row(k);
                if (v.size() >= 2) modeAt(modes, int(v[0])).frequency = v[1];
            }
        } else if (t == "$raman_spectrum") {
            for (int k = 0; k < n; ++k) {
                const Vec v = row(k);
                if (v.size() < 2) continue;
                Mode& m = modeAt(modes, k);
                m.frequency = v[0];
                m.activity = v[1];
                if (v.size() > 2) m.depolarization = v[2];
            }
        } else if (t == "$ir_spectrum") {
            for (int k = 0; k < n; ++k) {
                const Vec v = row(k);
                if (v.size() < 2) continue;
                Mode& m = modeAt(modes, k);
                m.frequency = v[0];
                m.ir = v.size() >= 7 ? v[2] : v[1];     // ORCA 5/6: freq eps Int T**2 TX TY TZ; ORCA 4: freq T**2 ...
            }
        } else if (t == "$atoms") {
            for (int k = 0; k < n && k + i + 2 < L.size(); ++k) {
                const QStringList tk = tokens(L[k + i + 2]);
                if (tk.size() < 5) continue;
                c.atoms.push_back({normElement(tk[0]),
                                   {num(tk[2]) * kBohr, num(tk[3]) * kBohr, num(tk[4]) * kBohr}});
            }
        }
    }
    c.modes = modesOf(modes);
    return {c};
}

// ============================================================================================= NWChem
std::vector<Calculation> readNwchem(const QStringList& L) {
    Calculation c;
    c.program = "NWChem";
    c.format = "nwchem";
    static const QRegularExpression verRe("Northwest Computational Chemistry Package \\(NWChem\\)\\s+(\\S+)");
    static const QRegularExpression groupRe("\\[[^\\]]*\\]");
    std::map<int, Mode> ir, raman;
    bool projectedIr = false;
    for (int i = 0; i < L.size(); ++i) {
        const QString t = L[i].trimmed();
        if (c.version.isEmpty()) {
            const auto m = verRe.match(t);
            if (m.hasMatch()) c.version = m.captured(1);
        }
        if (t.startsWith("Output coordinates in angstroms")) {
            c.atoms.clear();
            int j = i + 1;
            while (j < L.size() && !L[j].trimmed().startsWith("----")) ++j;
            for (++j; j < L.size(); ++j) {
                const QStringList tk = tokens(L[j]);
                if (tk.size() < 6 || !isInt(tk[0])) break;
                c.atoms.push_back({normElement(tk[1]), {num(tk[3]), num(tk[4]), num(tk[5])}});
            }
            continue;
        }
        if (t.startsWith("XC Information") && c.method.isEmpty()) {
            for (int j = i + 1; j < std::min<int>(L.size(), i + 6); ++j) {
                const QString u = L[j].trimmed();
                if (u.contains("Method XC")) {
                    c.method = tokens(u).value(0);
                    break;
                }
            }
            continue;
        }
        if (t.startsWith("NWChem SCF Module") && c.method.isEmpty()) c.method = "HF";
        if (t.startsWith("Summary of \"ao basis\"") && c.basis.isEmpty()) {
            for (int j = i + 1; j < std::min<int>(L.size(), i + 8); ++j) {
                if (!L[j].trimmed().startsWith("Tag")) continue;
                if (j + 2 < L.size()) c.basis = tokens(L[j + 2]).value(1);
                break;
            }
            continue;
        }
        if (!t.startsWith("Normal Eigenvalue ||") || i + 2 >= L.size()) continue;
        const QString title = t.mid(t.indexOf("||") + 2).trimmed();
        const QString units = L[i + 1].mid(L[i + 1].indexOf("||") + 2);
        QStringList groups;
        for (auto it = groupRe.globalMatch(units); it.hasNext();) groups << it.next().captured(0);
        const bool isRaman = title.contains("Raman", Qt::CaseInsensitive);
        const bool isIr = title.contains("Infra", Qt::CaseInsensitive);
        if (!isRaman && !isIr) continue;
        int col = int(groups.size()) - 1;
        for (int g = 0; g < groups.size(); ++g) {
            if (isRaman && groups[g].contains("ctivit", Qt::CaseInsensitive)) col = g;
            if (isIr && groups[g].contains("km/mol", Qt::CaseInsensitive)) col = g;
        }
        if (isIr) {
            const bool proj = title.contains("Projected", Qt::CaseInsensitive);
            if (projectedIr && !proj) continue;
            projectedIr = proj;
            ir.clear();
        } else {
            raman.clear();
        }
        int j = i + 2;
        if (isDashes(L[j].trimmed().remove('|'))) ++j;
        for (; j < L.size(); ++j) {
            const QString& r = L[j];
            const int bar = r.indexOf("||");
            if (bar < 0) break;
            const QStringList left = tokens(r.left(bar));
            const Vec right = numbers(r.mid(bar + 2));
            if (left.size() < 2 || !isInt(left[0])) break;
            Mode& m = modeAt(isIr ? ir : raman, left[0].toInt());
            m.frequency = num(left[1]);
            if (col >= 0 && col < int(right.size())) (isIr ? m.ir : m.activity) = right[col];
        }
        i = j - 1;
    }
    std::map<int, Mode> all = raman.empty() ? ir : raman;
    for (auto& [k, m] : all) {
        auto it = ir.find(k);
        if (it != ir.end()) m.ir = it->second.ir;
    }
    c.modes = modesOf(all);
    return {c};
}

// =============================================================================================== CP2K
std::vector<Calculation> readCp2k(const QStringList& L) {
    Calculation c;
    c.program = "CP2K";
    c.format = "cp2k";
    static const QRegularExpression verRe("CP2K version\\s+(\\S+)");
    static const QRegularExpression funcRe("FUNCTIONAL\\|\\s*([A-Za-z0-9+\\-]+):");
    static const QRegularExpression basisRe("Orbital Basis Set\\s+(\\S+)");
    std::vector<int> cur;
    for (int i = 0; i < L.size(); ++i) {
        const QString t = L[i].trimmed();
        if (c.version.isEmpty()) {
            const auto m = verRe.match(t);
            if (m.hasMatch()) c.version = m.captured(1);
        }
        if (c.method.isEmpty()) {
            const auto m = funcRe.match(t);
            if (m.hasMatch()) c.method = m.captured(1).toUpper();
        }
        if (c.basis.isEmpty()) {
            const auto m = basisRe.match(t);
            if (m.hasMatch()) c.basis = m.captured(1);
        }
        if (t.contains("ATOMIC COORDINATES IN", Qt::CaseInsensitive) && t.contains("angstrom", Qt::CaseInsensitive)) {
            c.atoms.clear();
            bool started = false;
            for (int j = i + 1; j < L.size(); ++j) {
                const QStringList tk = tokens(L[j]);
                if (tk.size() >= 7 && isInt(tk[0]) && isInt(tk[1]) && isNum(tk[4])) {
                    started = true;
                    c.atoms.push_back({normElement(tk[2]), {num(tk[4]), num(tk[5]), num(tk[6])}});
                } else if (started) {
                    break;
                } else if (j - i > 6) {
                    break;
                }
            }
            continue;
        }
        if (!t.startsWith("VIB|")) continue;
        const QString body = t.mid(4).trimmed();
        const Vec v = numbers(body);
        if (body.startsWith("Frequency", Qt::CaseInsensitive)) {
            cur.clear();
            for (double f : v) {
                Mode m;
                m.frequency = f;
                cur.push_back(int(c.modes.size()));
                c.modes.push_back(m);
            }
        } else if (!cur.empty() && v.size() == cur.size()) {
            if (body.startsWith("Intensities", Qt::CaseInsensitive) || body.startsWith("IR", Qt::CaseInsensitive)) {
                for (std::size_t q = 0; q < cur.size(); ++q) c.modes[cur[q]].ir = v[q];
            } else if (body.startsWith("Raman", Qt::CaseInsensitive)) {
                for (std::size_t q = 0; q < cur.size(); ++q) c.modes[cur[q]].activity = v[q];
            }
        }
    }
    return {c};
}

// ================================================================================= Quantum ESPRESSO
std::vector<Calculation> readQe(const QStringList& L) {
    Calculation c;
    c.program = "Quantum ESPRESSO";
    c.format = "qe";
    static const QRegularExpression verRe("Program\\s+(\\S+)\\s+v\\.\\s*(\\S+)");
    static const QRegularExpression headRe("^#\\s*mode\\s+\\[cm-1\\]");
    static const QRegularExpression symRe("freq\\s*\\(\\s*(\\d+)\\s*-\\s*(\\d+)\\s*\\)\\s*=\\s*(-?[\\d.]+)\\s*\\[cm-1\\]\\s*-->\\s*(\\S+)");
    static const QRegularExpression phRe("freq\\s*\\(\\s*(\\d+)\\s*\\)\\s*=\\s*(-?[\\d.]+)\\s*\\[THz\\]\\s*=\\s*(-?[\\d.]+)\\s*\\[cm-1\\]");
    static const QRegularExpression siteRe("^\\s*\\d+\\s+(\\S+)\\s+(?:[\\d.]+\\s+)?tau\\(");
    static const QRegularExpression xcRe("Exchange-correlation\\s*=\\s*(\\S+)");
    std::map<int, Mode> table, ph;
    std::map<int, QString> sym;
    bool phDone = false;
    QStringList programs;
    for (int i = 0; i < L.size(); ++i) {
        const QString t = L[i].trimmed();
        {
            const auto m = verRe.match(t);
            if (m.hasMatch() && c.version.isEmpty()) {
                c.version = m.captured(2);
                if (m.captured(2).endsWith(',')) c.version.chop(1);
                programs << m.captured(1);
            }
        }
        if (c.method.isEmpty()) {
            const auto m = xcRe.match(t);
            if (m.hasMatch()) c.method = m.captured(1);
        }
        if (t.startsWith("site n.") && c.atoms.empty()) {
            for (int j = i + 1; j < L.size(); ++j) {
                const auto m = siteRe.match(L[j]);
                if (!m.hasMatch()) break;
                c.atoms.push_back({normElement(m.captured(1)), {0, 0, 0}});
            }
            continue;
        }
        {
            const auto m = symRe.match(t);
            if (m.hasMatch()) {
                QString s = m.captured(4);
                for (int k = m.captured(1).toInt(); k <= m.captured(2).toInt(); ++k) sym[k] = s;
                continue;
            }
        }
        {
            const auto m = phRe.match(t);
            if (m.hasMatch()) {
                const int k = m.captured(1).toInt();
                if (k == 1 && !ph.empty()) phDone = true;     // next q point: keep the first (Gamma)
                if (!phDone) ph[k].frequency = num(m.captured(3));
                continue;
            }
        }
        if (headRe.match(t).hasMatch()) {
            const QStringList h = tokens(t.mid(1));
            // columns after "mode": [cm-1] [THz] then IR / Raman / depol.fact as named
            QStringList cols;
            for (const QString& w : h.mid(1)) cols << w.toLower();
            table.clear();
            for (int j = i + 1; j < L.size(); ++j) {
                const QStringList tk = tokens(L[j]);
                if (tk.size() < 3 || !isInt(tk[0])) break;
                Mode& m = table[tk[0].toInt()];
                for (int q = 0; q < cols.size() && q + 1 < tk.size(); ++q) {
                    const double v = num(tk[q + 1]);
                    if (cols[q] == "[cm-1]") m.frequency = v;
                    else if (cols[q] == "ir") m.ir = v;
                    else if (cols[q] == "raman") m.activity = v;
                    else if (cols[q].startsWith("depol")) m.depolarization = v;
                }
                i = j;
            }
        }
    }
    std::map<int, Mode> all = table.empty() ? ph : table;
    for (auto& [k, m] : all) {
        auto it = sym.find(k);
        if (it != sym.end()) m.symmetry = it->second;
    }
    c.modes = modesOf(all);
    if (!programs.isEmpty()) c.version = programs.first() + " " + c.version;
    return {c};
}

// ======================================================================================== vasp_raman
std::vector<Calculation> readVaspRaman(const QStringList& L) {
    Calculation c;
    c.program = "VASP";
    c.version = "vasp_raman.py";
    c.format = "vasp-raman";
    for (const QString& s : L) {
        const QString t = s.trimmed();
        if (t.isEmpty() || t.startsWith('#')) continue;
        const QStringList tk = tokens(t);
        if (tk.size() < 5 || !isInt(tk[0])) continue;
        Mode m;
        m.frequency = num(tk[1]);
        m.activity = num(tk[4]);
        c.modes.push_back(m);
    }
    return {c};
}

// ============================================================================ Phonopy-Spectroscopy
std::vector<Calculation> readPhonopy(const QStringList& L) {
    Calculation c;
    c.program = "Phonopy-Spectroscopy";
    c.format = "phonopy";
    double unit = 1.0;
    bool header = false;
    for (const QString& s : L) {
        QString t = s.trimmed();
        if (t.startsWith('#')) t = t.mid(1).trimmed();
        if (t.isEmpty()) continue;
        if (!header && t.contains("Freq", Qt::CaseInsensitive)) {
            header = true;
            if (t.contains("THz")) unit = kThzToCm;
            else if (t.contains("meV")) unit = kMevToCm;
            continue;
        }
        QStringList tk = t.contains(',') ? t.split(',') : tokens(t);
        for (QString& x : tk) x = x.trimmed();
        if (tk.size() < 3 || !isInt(tk[0])) continue;
        Vec v;
        QString irrep;
        for (int q = 1; q < tk.size(); ++q) {
            double x;
            if (toNum(tk[q], x)) v.push_back(x);
            else if (irrep.isEmpty() && !tk[q].isEmpty()) irrep = tk[q];
        }
        if (v.size() < 2) continue;
        Mode m;
        m.frequency = v[0] * unit;
        m.activity = v[1];
        m.symmetry = irrep == "None" ? QString() : irrep;
        c.modes.push_back(m);
    }
    return {c};
}

// ============================================================================================= CASTEP
std::vector<Calculation> readCastep(const QStringList& L) {
    std::vector<Calculation> out;
    static const QRegularExpression verRe("CASTEP version\\s+([\\d.]+)");
    static const QRegularExpression funcRe("using functional\\s*:\\s*(.+)$");
    static const QRegularExpression cutRe("plane wave basis set cut-off\\s*:\\s*([\\d.]+)\\s*eV");
    static const QRegularExpression cellRe("^\\s*x\\s+([A-Z][a-z]?)\\S*\\s+\\d+\\s+(-?[\\d.]+)\\s+(-?[\\d.]+)\\s+(-?[\\d.]+)\\s+x\\s*$");
    QString version, method, basis;
    std::vector<Atom> atoms;
    bool cellRead = false;
    std::vector<Calculation> noRaman;
    for (int i = 0; i < L.size(); ++i) {
        const QString t = L[i].trimmed();
        if (version.isEmpty()) {
            const auto m = verRe.match(t);
            if (m.hasMatch()) version = m.captured(1);
        }
        if (method.isEmpty()) {
            const auto m = funcRe.match(t);
            if (m.hasMatch()) {
                const QString f = m.captured(1).trimmed();
                if (f.startsWith("Perdew Burke Ernzerhof")) method = "PBE";
                else if (f.startsWith("PBE for solids")) method = "PBEsol";
                else if (f.startsWith("Local Density")) method = "LDA";
                else method = f;
            }
        }
        if (basis.isEmpty()) {
            const auto m = cutRe.match(t);
            if (m.hasMatch()) basis = "PW " + m.captured(1).section('.', 0, 0) + " eV";
        }
        if (!cellRead) {
            const auto m = cellRe.match(L[i]);
            if (m.hasMatch()) {
                atoms.push_back({m.captured(1), {num(m.captured(2)), num(m.captured(3)), num(m.captured(4))}});
                if (i + 1 < L.size() && !cellRe.match(L[i + 1]).hasMatch()) cellRead = true;
                continue;
            }
        }
        if (!(t.startsWith('+') && t.contains("Frequency") && t.contains("irrep"))) continue;
        const bool hasIr = t.contains("IR intensity");
        const bool hasRaman = t.contains("Raman activity");
        Calculation c;
        c.program = "CASTEP";
        c.version = version;
        c.format = "castep";
        c.method = method;
        c.basis = basis;
        c.atoms = atoms;
        int j = i + 1;
        bool started = false;
        for (; j < L.size(); ++j) {
            QString r = L[j].trimmed();
            if (!r.startsWith('+')) break;
            r = r.mid(1);
            if (r.endsWith('+')) r.chop(1);
            const QStringList tk = tokens(r);
            if (tk.size() >= 2 && isInt(tk[0]) && isNum(tk[1])) {
                started = true;
                Mode m;
                m.frequency = num(tk[1]);
                int q = 2;
                if (q < tk.size() && !isNum(tk[q]) && tk[q] != "Y" && tk[q] != "N") {
                    m.symmetry = tk[q];
                    ++q;
                }
                Vec v;
                for (; q < tk.size(); ++q)
                    if (isNum(tk[q])) v.push_back(num(tk[q]));
                std::size_t k = 0;
                if (hasIr && k < v.size()) m.ir = v[k++];
                if (hasRaman && k < v.size()) m.activity = v[k++];
                c.modes.push_back(m);
            } else if (started && (r.trimmed().startsWith("...") || r.trimmed().startsWith("==="))) {
                break;
            }
        }
        // irrep letters (a, b, ...) are explained by the character table that follows: "+ b T2g  1 | ..."
        static const QRegularExpression repRe("^\\+\\s+([a-z])\\s+([A-Z]\\S*)\\s+\\d+\\s+\\|");
        std::map<QString, QString> reps;
        for (int k = j; k < L.size() && L[k].trimmed().startsWith('+'); ++k) {
            const auto m = repRe.match(L[k].trimmed());
            if (m.hasMatch()) reps[m.captured(1)] = m.captured(2);
        }
        for (Mode& m : c.modes) {
            auto it = reps.find(m.symmetry);
            if (it != reps.end()) m.symmetry = it->second;
            else if (m.symmetry.size() == 1 && m.symmetry[0].isLower()) m.symmetry.clear();
        }
        i = j - 1;
        if (c.modes.empty()) continue;
        (hasRaman ? out : noRaman).push_back(c);
    }
    if (out.empty() && !noRaman.empty()) out.push_back(noRaman.front());
    return out;
}

QString readFile(const QString& path, qint64 maxBytes = -1) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) fail("cannot read " + path + ": " + f.errorString());
    const QByteArray b = maxBytes > 0 ? f.read(maxBytes) : f.readAll();
    QString s = QString::fromUtf8(b);
    if (s.contains(QChar::ReplacementCharacter)) s = QString::fromLatin1(b);
    return s;
}

// ================================================================================================= xtb
// xtb (Grimme group) frequency printout ("--hess"/"--ohess"):
//    projected vibrational frequencies (cm⁻¹)
//   eigval :       -0.00    -0.00     0.00 ...
//    IR intensities (km·mol⁻¹)
//      1: 98.19   2:134.84 ...
//    Raman intensities (Å⁴*amu⁻¹)          (activities; all zero unless polarisability derivatives were computed)
//      1:  0.00   2:  0.00 ...
// The last printout in the file is used. Geometry: "Standard orientation" of an embedded g98-style block
// (ECCE runs) or the "final structure:" xyz block.
std::vector<Calculation> readXtb(const QStringList& L) {
    Calculation c;
    c.program = "xtb";
    c.format = "xtb";
    c.method = "GFN2-xTB";
    static const QRegularExpression verRe("xtb version\\s+(\\S+)");
    static const QRegularExpression gfnRe("--gfn\\s*([012])");
    static const QRegularExpression pairRe("(\\d+)\\s*:\\s*(-?[0-9.]+(?:[eE][-+]?\\d+)?)");
    int start = -1;
    for (int i = 0; i < L.size(); ++i) {
        const QString& l = L[i];
        if (c.version.isEmpty()) {
            const auto m = verRe.match(l);
            if (m.hasMatch()) c.version = m.captured(1);
        }
        if (l.contains("program call")) {
            if (l.contains("--gfnff")) c.method = "GFN-FF";
            else {
                const auto m = gfnRe.match(l);
                if (m.hasMatch()) c.method = QString("GFN%1-xTB").arg(m.captured(1));
            }
        }
        if (l.contains("projected vibrational frequencies")) start = i;
    }
    if (start < 0) return {};
    Vec freq;
    int i = start + 1;
    for (; i < L.size() && L[i].trimmed().startsWith("eigval"); ++i) {
        const QString t = L[i].mid(L[i].indexOf(':') + 1);
        for (const QString& tok : tokens(t)) freq.push_back(num(tok));
    }
    std::vector<Mode> modes(freq.size());
    for (size_t k = 0; k < freq.size(); ++k) modes[k].frequency = freq[k];
    auto readPairs = [&](int from, auto&& set) {        // "k: value" lists until a line without pairs
        for (int j = from; j < L.size(); ++j) {
            auto it = pairRe.globalMatch(L[j]);
            if (!it.hasNext()) break;
            while (it.hasNext()) {
                const auto m = it.next();
                const int k = m.captured(1).toInt() - 1;
                if (k >= 0 && k < int(modes.size())) set(modes[size_t(k)], num(m.captured(2)));
            }
        }
    };
    for (int j = i; j < L.size() && j < i + 400; ++j) {
        if (L[j].contains("IR intensities")) readPairs(j + 1, [](Mode& m, double v) { m.ir = v; });
        if (L[j].contains("Raman intensities") || L[j].contains("Raman activities"))
            readPairs(j + 1, [](Mode& m, double v) { m.activity = v; });
        if (L[j].contains("Thermodynamic Functions")) break;
    }
    c.modes = modes;
    // geometry
    for (int j = 0; j < L.size(); ++j) {
        if (L[j].contains("Standard orientation:")) {
            c.atoms.clear();
            for (int k = j + 5; k < L.size() && !isDashes(L[k].trimmed()); ++k) {
                const QStringList t = tokens(L[k]);
                if (t.size() < 6) break;
                c.atoms.push_back({elementOf(t[1].toInt()), {num(t[3]), num(t[4]), num(t[5])}});
            }
        }
    }
    if (c.atoms.empty()) {
        for (int j = 0; j + 2 < L.size(); ++j) {
            if (!L[j].contains("final structure:")) continue;
            int k = j + 1;
            while (k < L.size() && L[k].trimmed().isEmpty()) ++k;
            if (k >= L.size() || !isInt(L[k].trimmed())) break;
            const int n = L[k].trimmed().toInt();
            for (int a = 0; a < n && k + 2 + a < L.size(); ++a) {
                const QStringList t = tokens(L[k + 2 + a]);
                if (t.size() < 4) break;
                c.atoms.push_back({normElement(t[0]), {num(t[1]), num(t[2]), num(t[3])}});
            }
        }
    }
    return {c};
}

const std::map<QString, QString>& hints() {
    static const std::map<QString, QString> h = {
        {"gaussian", "Gaussian output: run freq=raman"},
        {"orca", "ORCA output: run Freq (or NumFreq) with %elprop Polar 1 end for Raman"},
        {"orca-hess", "ORCA .hess file of a frequency calculation"},
        {"nwchem", "NWChem output: task <theory> raman (or frequencies)"},
        {"cp2k", "CP2K output with &VIBRATIONAL_ANALYSIS"},
        {"qe", "Quantum ESPRESSO dynmat.x output (ph.x with lraman=.true.)"},
        {"vasp-raman", "vasp_raman.py output"},
        {"phonopy", "Phonopy-Spectroscopy peak table"},
        {"castep", "CASTEP .castep of a phonon calculation with calculate_raman : true"},
        {"xtb", "xtb output of a Hessian calculation (--hess or --ohess)"}};
    return h;
}

}  // namespace

// ============================================================================================= public
const QStringList& formats() {
    static const QStringList f = {"gaussian", "orca", "orca-hess", "nwchem", "cp2k", "qe", "vasp-raman", "phonopy", "castep", "xtb"};
    return f;
}

QString fileFilter() {
    return "Quantum-chemistry output (*.log *.out *.output *.hess *.castep *.dat *.txt *.csv);;All files (*)";
}

QString detectFormat(const QString& text, const QString& fileName) {
    static const QRegularExpression qeRe("^#\\s*mode\\s+\\[cm-1\\]\\s+\\[THz\\]", QRegularExpression::MultilineOption);
    static const QRegularExpression vaspRe("^#\\s*mode\\s+freq\\(cm-1\\)\\s+alpha\\s+beta2\\s+activity",
                                           QRegularExpression::MultilineOption);
    if (text.contains("$orca_hessian_file") ||
        (fileName.endsWith(".hess", Qt::CaseInsensitive) && text.contains("$vibrational_frequencies")))
        return "orca-hess";
    if (text.contains("xtb version") && text.contains("projected vibrational frequencies")) return "xtb";
    if (text.contains("Entering Gaussian System") || text.contains("Gaussian, Inc.")) return "gaussian";
    if (text.contains("O   R   C   A") || (text.contains("Program Version") && text.contains("ORCA"))) return "orca";
    if (text.contains("Northwest Computational Chemistry Package")) return "nwchem";
    if (text.contains("CP2K|") || text.contains(" VIB|Frequency")) return "cp2k";
    if (text.contains("CASTEP version") || text.contains("C A S T E P")) return "castep";
    if (text.contains("Program DYNMAT") || text.contains("Program PHONON") || qeRe.match(text).hasMatch()) return "qe";
    if (vaspRe.match(text).hasMatch()) return "vasp-raman";
    // Phonopy-Spectroscopy peak table: a header with frequency and intensity columns
    const QStringList head = splitLines(text.left(4096)).mid(0, 6);
    for (const QString& h : head)
        if (h.contains("Freq", Qt::CaseInsensitive) && (h.contains("Intensity") || h.contains("Activity")) &&
            (h.contains("Irrep") || h.contains("Linewidth") || h.contains("Peak")))
            return "phonopy";
    return {};
}

std::vector<Calculation> readText(const QString& text, const QString& source, const QString& format) {
    const QString fmt = format.isEmpty() ? detectFormat(text, source) : format;
    const QString who = source.isEmpty() ? QStringLiteral("text") : QFileInfo(source).fileName();
    if (fmt.isEmpty())
        fail(who + ": not a recognised quantum-chemistry output (Gaussian, ORCA, NWChem, CP2K, Quantum ESPRESSO, "
                   "vasp_raman.py, Phonopy-Spectroscopy, CASTEP, xtb)");
    if (!formats().contains(fmt)) fail("unknown computed-spectrum format: " + fmt);
    const QStringList L = splitLines(text);
    std::vector<Calculation> calcs;
    if (fmt == "gaussian") calcs = readGaussian(L);
    else if (fmt == "orca") calcs = readOrca(L);
    else if (fmt == "orca-hess") calcs = readOrcaHess(L);
    else if (fmt == "nwchem") calcs = readNwchem(L);
    else if (fmt == "cp2k") calcs = readCp2k(L);
    else if (fmt == "qe") calcs = readQe(L);
    else if (fmt == "vasp-raman") calcs = readVaspRaman(L);
    else if (fmt == "phonopy") calcs = readPhonopy(L);
    else if (fmt == "castep") calcs = readCastep(L);
    else if (fmt == "xtb") calcs = readXtb(L);

    std::vector<Calculation> out;
    for (Calculation& c : calcs) {
        c.source = source;
        finish(c);
        if (!c.modes.empty()) out.push_back(c);
    }
    if (out.empty()) fail(who + ": no vibrational frequencies found (" + hints().at(fmt) + ")");
    for (int k = 0; k < int(out.size()); ++k) {
        out[k].block = k;
        out[k].blocks = int(out.size());
    }
    return out;
}

std::vector<Calculation> read(const QString& path, const QString& format) {
    return readText(readFile(path), path, format);
}

QStringList findOutputs(const QString& dir, bool recursive) {
    static const QStringList ext = {"log", "out", "output", "hess", "castep", "dat", "txt", "csv"};
    QStringList out;
    if (!QFileInfo(dir).isDir()) fail(dir + " is not a folder");
    QDirIterator it(dir, QDir::Files | QDir::Readable,
                    recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        const QString p = it.next();
        const QFileInfo fi(p);
        if (!ext.contains(fi.suffix().toLower())) continue;
        try {
            if (!detectFormat(readFile(p, 256 * 1024), p).isEmpty()) out << p;
        } catch (const Error&) {
        }
    }
    out.sort();
    return out;
}

}  // namespace lr::computed
