// Tests for core/computed: readers for every supported program (synthetic, truncated output excerpts
// in golden/computed written after the programs' layouts), Raman cross-sections against a hand
// calculation in CGS units, frequency-scaling fits, broadening areas/FWHM, matching and assignment,
// structure comparison and the ECCE folder scan.
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <random>

#include "core/computed.h"
#include "core/errors.h"
#include "core/fileio.h"
#include "golden_util.h"

using namespace lr::computed;
using lr::Vec;
using lr::fileio::DataSpectrum;

namespace {

const QString DIR = QStringLiteral(GOLDEN_DIR "/computed/");

Calculation one(const QString& file) {
    const auto v = read(DIR + file);
    if (v.size() != 1) qFatal("expected one calculation in %s", qPrintable(file));
    return v.front();
}

constexpr double kPi = 3.14159265358979323846;

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

// Experimental-like spectrum: Lorentzian bands at given positions/heights, small offset and noise.
DataSpectrum synthetic(const std::vector<std::pair<double, double>>& bands, double x0, double x1, double fwhm = 6,
                       unsigned seed = 3) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0, 0.002);
    DataSpectrum d;
    d.name = "experimental";
    for (double x = x0; x <= x1; x += 0.5) {
        double y = 0.05;
        for (const auto& [p, h] : bands) y += h / (1 + std::pow((x - p) / (fwhm / 2), 2));
        d.x.push_back(x);
        d.y.push_back(y + noise(rng));
    }
    return d;
}

std::vector<std::pair<double, double>> scaledBands(const Calculation& c, double f, double split = 0, double f2 = 1) {
    Options o;
    o.scaling.low = f;
    o.scaling.high = f2;
    o.scaling.split = split;
    const auto b = bands(c, o);
    const double m = maxValue(b);
    std::vector<std::pair<double, double>> out;
    for (const Band& x : b) out.emplace_back(x.position, x.value / m);
    return out;
}

}  // namespace

class TestComputed : public QObject {
    Q_OBJECT
private slots:
    // A real xtb output (excerpt): GFN2-xTB water, --ohess prints Raman "intensities" that are all zero.
    void xtbReal() {
        const auto calcs = lr::computed::read(DIR + "water_xtb_excerpt.out");
        QCOMPARE(int(calcs.size()), 1);
        const auto& c = calcs[0];
        QCOMPARE(c.format, QStringLiteral("xtb"));
        QCOMPARE(c.program, QStringLiteral("xtb"));
        QCOMPARE(c.version, QStringLiteral("6.7.1"));
        QCOMPARE(c.method, QStringLiteral("GFN2-xTB"));
        QCOMPARE(c.formula, QStringLiteral("H2O"));
        QCOMPARE(int(c.atoms.size()), 3);
        QCOMPARE(int(c.modes.size()), 3);                      // translations/rotations dropped
        QCOMPARE(c.modes[0].frequency, 1538.86);
        QCOMPARE(c.modes[2].frequency, 3651.45);
        QCOMPARE(c.modes[0].ir, 133.29);                       // mode 7 of the printout
        QCOMPARE(c.modes[2].ir, 16.69);
        QVERIFY(!c.hasActivities());                           // all zero -> treated as not computed
        QVERIFY(c.warnings.join(" ").contains("zero"));
    }
    // A real ORCA 6.1.1 run (CCl4, PBE0/def2-SVP, Freq + %elprop Polar 1; host/dir lines anonymised).
    void orcaReal() {
        const auto calcs = lr::computed::read(DIR + "orca611_ccl4_pbe0_real.out");
        QCOMPARE(int(calcs.size()), 1);
        const auto& c = calcs[0];
        QCOMPARE(c.format, QStringLiteral("orca"));
        QCOMPARE(c.version, QStringLiteral("6.1.1"));
        QCOMPARE(c.formula, QStringLiteral("CCl4"));
        QCOMPARE(int(c.modes.size()), 9);
        QVERIFY(c.hasActivities());
        lr::computed::Options opt;
        const auto b = lr::computed::bands(c, opt);           // degenerate modes summed: E, T2, A1, T2
        QCOMPARE(int(b.size()), 4);
        QVERIFY(std::abs(b[0].position - 218.03) < 0.01);
        QVERIFY(std::abs(b[0].activity - 8.087264) < 1e-5);
        QVERIFY(std::abs(b[1].activity - 15.050244) < 1e-5);
        QVERIFY(std::abs(b[2].position - 472.58) < 0.01);
        QVERIFY(std::abs(b[2].activity - 14.864084) < 1e-6);
        QVERIFY(std::abs(c.modes[5].depolarization) < 1e-9);  // the totally symmetric A1 mode
    }
    void detect() {
        const std::vector<std::pair<QString, QString>> cases = {
            {"gaussian_ccl4.log", "gaussian"},       {"gaussian_chcl3_hpmodes.out", "gaussian"},
            {"orca5_chcl3.out", "orca"},             {"orca4_ccl4_twojobs.out", "orca"},
            {"orca6_ccl4.out", "orca"},              {"orca_chcl3.hess", "orca-hess"},
            {"nwchem_ccl4_raman.out", "nwchem"},     {"cp2k_chcl3_vib.out", "cp2k"},
            {"qe_diamond_dynmat.out", "qe"},         {"qe_diamond_ph.out", "qe"},
            {"vasp_raman_ccl4.dat", "vasp-raman"},   {"phonopy_zns_raman_peaktable.dat", "phonopy"},
            {"castep_si_raman.castep", "castep"},    {"not_an_output.txt", ""}};
        for (const auto& [file, fmt] : cases) {
            QFile f(DIR + file);
            QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(file));
            QCOMPARE(detectFormat(QString::fromUtf8(f.readAll()), file), fmt);
        }
    }

    void gaussianLink1() {
        const Calculation c = one("gaussian_ccl4.log");      // opt job + freq=raman job
        QCOMPARE(c.program, QString("Gaussian"));
        QCOMPARE(c.version, QString("16 Rev. C.01"));
        QCOMPARE(c.method, QString("B3LYP"));
        QCOMPARE(c.basis, QString("6-31G(d)"));
        QCOMPARE(c.formula, QString("CCl4"));
        QCOMPARE(int(c.atoms.size()), 5);
        QCOMPARE(c.atoms[1].element, QString("Cl"));
        QCOMPARE(c.atoms[1].position[0], 1.0277);
        QCOMPARE(int(c.modes.size()), 9);
        QCOMPARE(c.modes[0].symmetry, QString("E"));
        QCOMPARE(c.modes[5].symmetry, QString("A1"));
        QCOMPARE(c.modes[5].frequency, 449.87);
        QCOMPARE(c.modes[5].activity, 24.831);
        QCOMPARE(c.modes[5].depolarization, 0.0017);
        QCOMPARE(c.modes[8].ir, 320.453);
        QCOMPARE(int(c.modes[0].displacement.size()), 5);
        QCOMPARE(c.modes[0].displacement[0][0], 0.59);
        QVERIFY(c.warnings.isEmpty());
        QCOMPARE(c.name(), QString("CCl4 B3LYP/6-31G(d) (gaussian_ccl4.log)"));

        const auto b = bands(c, Options{});
        QCOMPARE(int(b.size()), 4);                         // E, T2, A1, T2 (degenerate modes merged)
        QCOMPARE(b[0].symmetry, QString("E"));
        QCOMPARE(int(b[0].modes.size()), 2);
        QVERIFY(near(b[0].activity, 2 * 4.1012, 1e-12));
        QVERIFY(near(b[1].activity, 3 * 3.6230, 1e-12));
        QCOMPARE(b[3].symmetry, QString("T2"));
        QVERIFY(near(b[3].ir, 3 * 320.453, 1e-9));
    }

    void gaussianHpModes() {
        const Calculation c = one("gaussian_chcl3_hpmodes.out");
        QCOMPARE(int(c.modes.size()), 9);                   // the high-precision block is not read twice
        QCOMPARE(c.formula, QString("CHCl3"));
        QCOMPARE(c.modes[8].frequency, 3155.304);
        QCOMPARE(c.modes[8].activity, 91.0412);
        QCOMPARE(c.modes[8].symmetry, QString("A1"));
        QCOMPARE(int(c.modes[8].displacement.size()), 5);
        // only the HP block: still read
        QFile f(DIR + "gaussian_chcl3_hpmodes.out");
        QVERIFY(f.open(QIODevice::ReadOnly));
        QString text = QString::fromUtf8(f.readAll());
        const int second = text.lastIndexOf(" Harmonic frequencies");
        text = text.left(second) + " - Thermochemistry -\n";
        const auto hp = readText(text, "hp.out");
        QCOMPARE(int(hp.size()), 1);
        QCOMPARE(int(hp[0].modes.size()), 9);
        QCOMPARE(hp[0].modes[2].activity, 4.5871);
        QCOMPARE(hp[0].modes[2].symmetry, QString("A1"));
        QCOMPARE(int(hp[0].modes[2].displacement.size()), 5);
    }

    void orca() {
        const Calculation c = one("orca5_chcl3.out");
        QCOMPARE(c.program, QString("ORCA"));
        QCOMPARE(c.version, QString("5.0.4"));
        QCOMPARE(c.method, QString("B3LYP"));
        QCOMPARE(c.basis, QString("def2-TZVP"));
        QCOMPARE(c.formula, QString("CHCl3"));
        QCOMPARE(int(c.modes.size()), 9);                   // modes 0..5 (0.00 cm-1) dropped
        QCOMPARE(c.modes[8].frequency, 3167.93);
        QCOMPARE(c.modes[8].activity, 91.0412);
        QCOMPARE(c.modes[8].depolarization, 0.2903);
        QCOMPARE(c.modes[4].ir, 176.82);                    // km/mol column (ORCA 5)
        QVERIFY(c.modes[0].symmetry.isEmpty());              // ORCA prints no irreps

        const auto two = read(DIR + "orca4_ccl4_twojobs.out");   // ORCA 4, $new_job
        QCOMPARE(int(two.size()), 2);
        QCOMPARE(two[0].method, QString("BP86"));
        QCOMPARE(two[0].basis, QString("def2-SVP"));
        QCOMPARE(two[1].method, QString("B3LYP"));
        QCOMPARE(two[1].basis, QString("def2-TZVP"));
        QCOMPARE(two[0].modes[5].frequency, 429.63);
        QCOMPARE(two[1].modes[5].frequency, 455.72);
        QCOMPARE(two[0].modes[8].ir, 320.453);              // T**2 column (ORCA 4)
        QCOMPARE(two[1].name(), QString("CCl4 B3LYP/def2-TZVP (orca4_ccl4_twojobs.out) #2"));

        const Calculation six = one("orca6_ccl4.out");
        QCOMPARE(six.version, QString("6.0.1"));
        QCOMPARE(six.method, QString("r2SCAN-3c"));
        QCOMPARE(six.basis, QString("def2-mTZVPP"));
        QCOMPARE(six.modes[5].activity, 24.831);

        const Calculation h = one("orca_chcl3.hess");
        QCOMPARE(h.format, QString("orca-hess"));
        QCOMPARE(int(h.modes.size()), 9);
        QCOMPARE(h.modes[3].activity, 11.2034);
        QCOMPARE(h.modes[4].ir, 176.82);
        QCOMPARE(h.formula, QString("CHCl3"));
        QVERIFY(near(h.atoms[0].position[2], 0.4496, 1e-6));    // bohr -> Å
    }

    void nwchem() {
        const Calculation c = one("nwchem_ccl4_raman.out");
        QCOMPARE(c.program, QString("NWChem"));
        QCOMPARE(c.version, QString("7.0.2"));
        QCOMPARE(c.method, QString("B3LYP"));
        QCOMPARE(c.basis, QString("6-31G*"));
        QCOMPARE(c.formula, QString("CCl4"));
        QCOMPARE(int(c.modes.size()), 9);
        QCOMPARE(c.modes[5].frequency, 450.05);
        QCOMPARE(c.modes[5].activity, 24.831);
        QCOMPARE(c.modes[8].ir, 320.453);
    }

    void cp2k() {
        const Calculation c = one("cp2k_chcl3_vib.out");
        QCOMPARE(c.program, QString("CP2K"));
        QCOMPARE(c.version, QString("2023.1"));
        QCOMPARE(c.method, QString("PBE"));
        QCOMPARE(c.basis, QString("DZVP-MOLOPT-SR-GTH"));
        QCOMPARE(c.formula, QString("CHCl3"));
        QCOMPARE(int(c.modes.size()), 9);
        QCOMPARE(c.imaginaryModes(), 1);
        QCOMPARE(c.modes[0].frequency, -25.3124);
        QCOMPARE(c.modes[5].ir, 170.1129);
        QVERIFY(!c.hasActivities());
        QCOMPARE(c.warnings.size(), 2);
        QVERIFY(c.warnings[0].contains("imaginary"));
        QVERIFY(c.warnings[1].contains("no Raman activities"));
        const auto b = bands(c, Options{});
        QCOMPARE(int(b.size()), 6);                         // imaginary mode not plotted; unit activities
        QCOMPARE(b[0].activity, 2.0);
    }

    void quantumEspresso() {
        const Calculation d = one("qe_diamond_dynmat.out");
        QCOMPARE(d.program, QString("Quantum ESPRESSO"));
        QCOMPARE(d.version, QString("DYNMAT 7.2"));
        QCOMPARE(int(d.modes.size()), 3);                   // acoustic modes dropped
        QCOMPARE(d.modes[0].frequency, 1293.21);
        QCOMPARE(d.modes[0].activity, 84.3412);
        QCOMPARE(d.modes[0].depolarization, 0.75);
        QVERIFY(d.warnings.isEmpty());

        const Calculation p = one("qe_diamond_ph.out");
        QCOMPARE(int(p.modes.size()), 3);                   // -1.35 cm-1 acoustic modes are not "imaginary"
        QCOMPARE(p.imaginaryModes(), 0);
        QCOMPARE(p.modes[0].symmetry, QString("T_2g"));
        QCOMPARE(p.modes[0].frequency, 1293.212342);
        QCOMPARE(p.method, QString("PBE"));
        QCOMPARE(p.formula, QString("C2"));
        QVERIFY(!p.hasActivities());
    }

    void vaspPhonopyCastep() {
        const Calculation v = one("vasp_raman_ccl4.dat");
        QCOMPARE(v.program, QString("VASP"));
        QCOMPARE(int(v.modes.size()), 9);                   // near-zero translations/rotations dropped
        QCOMPARE(v.modes[0].frequency, 209.77);             // sorted ascending
        QCOMPARE(v.modes[5].activity, 24.831);
        QVERIFY(v.warnings.isEmpty());

        const Calculation p = one("phonopy_zns_raman_peaktable.dat");
        QCOMPARE(p.program, QString("Phonopy-Spectroscopy"));
        QCOMPARE(int(p.modes.size()), 1);
        QCOMPARE(p.modes[0].frequency, 271.412);
        QCOMPARE(p.modes[0].activity, 15.6213);
        QCOMPARE(p.modes[0].symmetry, QString("T2"));

        const Calculation c = one("castep_si_raman.castep");
        QCOMPARE(c.program, QString("CASTEP"));
        QCOMPARE(c.version, QString("20.11"));
        QCOMPARE(c.method, QString("LDA"));
        QCOMPARE(c.basis, QString("PW 400 eV"));
        QCOMPARE(c.formula, QString("Si2"));
        QCOMPARE(int(c.modes.size()), 3);
        QCOMPARE(c.modes[0].frequency, 508.456281);
        QCOMPARE(c.modes[0].activity, 31.2341213);
        QCOMPARE(c.modes[0].ir, 0.0);
        QCOMPARE(c.modes[0].symmetry, QString("T2g"));   // irrep letter b from the character table
    }

    void errors() {
        try {
            read(DIR + "not_an_output.txt");
            QFAIL("no exception");
        } catch (const lr::Error& e) {
            QVERIFY2(QString(e.what()).contains("not a recognised quantum-chemistry output"), e.what());
        }
        try {
            read(DIR + "gaussian_opt_only.log");
            QFAIL("no exception");
        } catch (const lr::Error& e) {
            QVERIFY2(QString(e.what()).contains("no vibrational frequencies found (Gaussian output: run freq=raman)"),
                     e.what());
        }
        QVERIFY(golden::throws<lr::Error>([&] { read(DIR + "does_not_exist.log"); }));
        QVERIFY(golden::throws<lr::Error>([&] { readText("x", "x", "unknown"); }));
    }

    void hill() {
        QCOMPARE(hillFormula({"Cl", "C", "H", "Cl", "Cl"}), QString("CHCl3"));
        QCOMPARE(hillFormula({"O", "H", "H"}), QString("H2O"));
        QCOMPARE(hillFormula({"S", "Zn"}), QString("SZn"));
        QCOMPARE(hillFormula({}), QString());
    }

    void crossSection() {
        // hand calculation in CGS units (erg, cm, g): dσ/dΩ = (2π)^4/45 · h/(8π² c ν) (ν0-ν)^4 S / (1 - e^(-hcν/kT))
        const double h = 6.62607015e-27, c = 2.99792458e10, k = 1.380649e-16, amu = 1.66053906660e-24;
        const double pi = 3.14159265358979323846;
        for (const auto& [nu, S, laser, T] : std::vector<std::array<double, 4>>{
                 {449.87, 24.831, 532.0, 298.15}, {213.62, 4.1012, 785.0, 77.0}, {3155.3, 91.04, 1064.0, 300.0}}) {
            const double nu0 = 1e7 / laser;
            const double sCgs = S * 1e-32 / amu;
            const double hand = std::pow(2 * pi, 4) / 45 * h / (8 * pi * pi * c * nu) * std::pow(nu0 - nu, 4) * sCgs /
                                (1 - std::exp(-h * c * nu / (k * T)));             // cm²/sr
            const double got = ramanCrossSection(nu, S, laser, T);                // m²/sr
            QVERIFY2(std::abs(got / (hand * 1e-4) - 1) < 1e-12, qPrintable(QString::number(got)));
        }
        // the 459 cm-1 band of CCl4 at 532 nm is a few 10^-34 m²/sr (literature ~5e-30 cm²/sr order)
        const double s = ramanCrossSection(459, 24.8, 532, 298.15);
        QVERIFY(s > 1e-35 && s < 1e-32);
        QCOMPARE(ramanCrossSection(0, 1, 532, 298), 0.0);
        QCOMPARE(ramanCrossSection(-20, 1, 532, 298), 0.0);
        QCOMPARE(ramanCrossSection(20000, 1, 532, 298), 0.0);   // beyond the laser line
        // relative intensities: proportionality with (ν0-ν)^4 S / (ν (1-e^-x))
        const double r = ramanCrossSection(218, 1, 532, 298.15) / ramanCrossSection(459, 1, 532, 298.15);
        auto f = [&](double nu) {
            return std::pow(1e7 / 532 - nu, 4) / (nu * (1 - std::exp(-h * c * nu / (k * 298.15))));
        };
        QVERIFY(std::abs(r / (f(218) / f(459)) - 1) < 1e-12);

        // bands(): activity vs intensity
        const Calculation calc = one("gaussian_ccl4.log");
        Options o;
        o.quantity = Quantity::Activity;
        const auto a = bands(calc, o);
        QCOMPARE(a[2].value, a[2].activity);
        o.quantity = Quantity::Intensity;
        const auto b = bands(calc, o);
        QCOMPARE(b[2].value, ramanCrossSection(449.87, 24.831, 532, 298.15));
        // the low-frequency E band gains relative to the A1 band (1/ν and Boltzmann factor)
        QVERIFY(b[0].value / b[2].value > a[0].value / a[2].value);
    }

    void scaling() {
        Scaling s;
        QCOMPARE(s.apply(1000), 1000.0);
        s.low = 0.96;
        QCOMPARE(s.apply(1000), 960.0);
        QCOMPARE(s.text(), QString("0.9600"));
        s.split = 2000;
        s.high = 0.95;
        QCOMPARE(s.apply(1999), 0.96 * 1999);
        QCOMPARE(s.apply(3000), 0.95 * 3000);
        QVERIFY(s.text().contains("2000"));
    }

    void fitSingle() {
        const Calculation c = one("gaussian_ccl4.log");
        for (double f : {0.97, 1.02}) {
            const DataSpectrum exp = synthetic(scaledBands(c, f), 150, 1000);
            for (Matching m : {Matching::Hungarian, Matching::Nearest}) {
                FitOptions fo;
                fo.matching = m;
                const ScaleFit r = fitScaling(c, Options{}, exp, fo);
                QVERIFY2(near(r.scaling.low, f, 2e-4), qPrintable(QString::number(r.scaling.low, 'f', 6)));
                QVERIFY(!r.scaling.twoRegion());
                QCOMPARE(r.matched, 4);
                QVERIFY(r.rms < 0.5);
            }
        }
    }

    void fitTwoRegion() {
        const Calculation c = one("gaussian_chcl3_hpmodes.out");
        const DataSpectrum exp = synthetic(scaledBands(c, 0.985, 2000, 0.957), 150, 3300);
        FitOptions fo;
        fo.twoRegion = true;
        fo.tolerance = 40;
        const ScaleFit r = fitScaling(c, Options{}, exp, fo);
        QVERIFY(r.scaling.twoRegion());
        QVERIFY2(near(r.scaling.low, 0.985, 3e-4), qPrintable(QString::number(r.scaling.low, 'f', 6)));
        QVERIFY2(near(r.scaling.high, 0.957, 3e-4), qPrintable(QString::number(r.scaling.high, 'f', 6)));
        QCOMPARE(r.matched, 6);
        // a single factor cannot fit both regions as well
        fo.twoRegion = false;
        const ScaleFit one_ = fitScaling(c, Options{}, exp, fo);
        QVERIFY(one_.rms > r.rms);
        // no experimental peaks: error
        QVERIFY(golden::throws<lr::Error>([&] { fitScaling(bands(c, Options{}), Vec{}, fo); }));
    }

    void matching() {
        // Kuhn-Munkres: 3x3 with known optimum 1+2+3... cost
        const std::vector<double> cost = {4, 1, 3,   //
                                          2, 0, 5,   //
                                          3, 2, 2};
        QCOMPARE(hungarian(cost, 3, 3), std::vector<int>({1, 0, 2}));   // 1 + 2 + 2 = 5
        // greedy nearest vs Hungarian
        const Vec a = {100, 106}, b = {104, 112};
        const auto n = assign(a, b, 7, Matching::Nearest);
        QCOMPARE(int(n.size()), 1);
        QCOMPARE(n[0], std::make_pair(1, 0));
        const auto h = assign(a, b, 7, Matching::Hungarian);
        QCOMPARE(int(h.size()), 2);
        QCOMPARE(h[0], std::make_pair(0, 0));
        QCOMPARE(h[1], std::make_pair(1, 1));
        // tolerance respected, empty inputs
        QVERIFY(assign({100}, {200}, 30).empty());
        QVERIFY(assign({}, {1}, 30).empty());
        // more rows than columns
        QCOMPARE(int(assign({100, 101, 102}, {101}, 5).size()), 1);
    }

    void broadening() {
        Band b;
        b.position = 500;
        b.value = 2.5;
        const Vec g = makeGrid(0, 1000, 0.05);
        QCOMPARE(int(g.size()), 20001);
        for (Shape s : {Shape::Lorentzian, Shape::Gaussian, Shape::PseudoVoigt}) {
            Broadening br;
            br.shape = s;
            br.fwhm = 10;
            const Vec y = broaden({b}, g, br);
            double area = 0;
            for (std::size_t i = 1; i < g.size(); ++i) area += 0.5 * (y[i] + y[i - 1]) * (g[i] - g[i - 1]);
            // Lorentzian tails beyond ±500 cm-1 hold 2/π·atan(5/500) of the area
            const double expect = s == Shape::Gaussian ? 2.5
                                  : s == Shape::Lorentzian
                                      ? 2.5 * (2 / kPi) * std::atan(500 / 5.0)
                                      : 2.5 * (0.5 + 0.5 * (2 / kPi) * std::atan(500 / 5.0));
            QVERIFY2(near(area, expect, 1e-6), qPrintable(shapeName(s) + " " + QString::number(area, 'f', 8)));
            // FWHM from the half-maximum crossings
            const std::size_t imax = std::size_t(std::max_element(y.begin(), y.end()) - y.begin());
            QVERIFY(near(g[imax], 500, 1e-9));
            const double half = y[imax] / 2;
            auto cross = [&](int dir) {
                std::size_t i = imax;
                while (y[i + dir] > half) i += dir;
                const std::size_t j = i + dir;
                return g[i] + (half - y[i]) * (g[j] - g[i]) / (y[j] - y[i]);
            };
            QVERIFY2(near(cross(1) - cross(-1), 10.0, 0.01), qPrintable(shapeName(s)));
        }
        QVERIFY(golden::throws<lr::Error>([&] { makeGrid(10, 0, 1); }));
    }

    void spectrumAndSticks() {
        const Calculation c = one("gaussian_ccl4.log");
        Options o;
        o.scaling.low = 0.98;
        o.quantity = Quantity::Activity;
        const DataSpectrum d = spectrum(c, o, Broadening{}, makeGrid(100, 1000, 1.0));
        QCOMPARE(int(d.x.size()), 901);
        QCOMPARE(*std::max_element(d.y.begin(), d.y.end()), 1.0);
        QCOMPARE(d.metaValue("computed"), QString("yes"));
        QCOMPARE(d.metaValue("method"), QString("B3LYP"));
        QCOMPARE(d.metaValue("scaling"), QString("0.9800"));
        QCOMPARE(d.metaValue("laser_nm"), QString("532"));
        // the strongest band (A1) is at the maximum
        const auto imax = std::max_element(d.y.begin(), d.y.end()) - d.y.begin();
        QVERIFY(near(d.x[imax], 0.98 * 449.87, 1.0));
        // written and read back by fileio
        QTemporaryDir tmp;
        lr::fileio::writeCsv(tmp.filePath("c.csv"), d);
        const auto back = lr::fileio::read(tmp.filePath("c.csv"));
        QCOMPARE(int(back.size()), 1);
        QCOMPARE(int(back[0].x.size()), 901);
        lr::fileio::writeJcamp(tmp.filePath("c.jdx"), d);
        QCOMPARE(int(lr::fileio::read(tmp.filePath("c.jdx")).front().x.size()), 901);

        const auto [sx, sy] = sticks(bands(c, o));
        QCOMPARE(int(sx.size()), 4);
        QCOMPARE(*std::max_element(sy.begin(), sy.end()), 1.0);
        QVERIFY(near(sx[2], 0.98 * 449.87, 1e-9));
    }

    void assignment() {
        const Calculation c = one("gaussian_ccl4.log");
        Options o;
        o.scaling.low = 1.02;
        const auto b = bands(c, o);
        // experimental CCl4: 218, 314, 459, 762/791 (Fermi doublet) and an impurity band at 1000
        std::vector<lr::peaks::Peak> pk(6);
        const double pos[] = {218, 314, 459, 762, 791, 1000}, ht[] = {0.5, 0.6, 1.0, 0.12, 0.13, 0.05};
        for (int i = 0; i < 6; ++i) {
            pk[i].position = pos[i];
            pk[i].height = ht[i];
        }
        const auto rows = assignmentTable(b, pk, 30);
        QCOMPARE(int(rows.size()), 6);                      // 4 matched + 1 unmatched peak (+1 of the doublet)
        QCOMPARE(rows[0].experimental, 218.0);
        QVERIFY(near(rows[0].computed, 1.02 * 213.62, 1e-9));
        QVERIFY(near(rows[0].delta, 1.02 * 213.62 - 218, 1e-9));
        QCOMPARE(rows[0].symmetry, QString("E"));
        QCOMPARE(rows[2].symmetry, QString("A1"));
        QCOMPARE(rows[2].experimentalRel, 1.0);
        int unmatchedPeaks = 0, matched = 0;
        for (const auto& r : rows) {
            if (r.band < 0) ++unmatchedPeaks;
            if (r.band >= 0 && r.peak >= 0) ++matched;
        }
        QCOMPARE(matched, 4);
        QCOMPARE(unmatchedPeaks, 2);
        QCOMPARE(rows.back().experimental, 1000.0);
        QVERIFY(std::isnan(rows.back().computed));
        // T2 at 785.5 goes to the closer component of the doublet (791)
        QCOMPARE(rows[4].experimental, 791.0);
        QVERIFY(rows[4].band >= 0);

        const QString text = assignmentText(rows);
        QVERIFY(text.startsWith("computed_cm-1"));
        QCOMPARE(text.count('\n'), 7);
        QTemporaryDir tmp;
        writeAssignmentCsv(tmp.filePath("a.csv"), rows);
        QFile f(tmp.filePath("a.csv"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 7);
        QCOMPARE(lines[1].split(',').size(), 7);
    }

    void comparison() {
        Vec x, y;
        for (int i = 0; i < 100; ++i) {
            x.push_back(i);
            y.push_back(std::sin(i * 0.1));
        }
        QVERIFY(near(correlation(x, y, x, y), 1.0, 1e-12));
        Vec y2;
        for (double v : y) y2.push_back(-2 * v + 3);
        QVERIFY(near(correlation(x, y, x, y2), -1.0, 1e-12));

        // experimental spectrum made from the ORCA 6 CCl4 bands (×0.985): CCl4 calculations beat CHCl3
        const Calculation ccl4 = one("orca6_ccl4.out");
        const DataSpectrum exp = synthetic(scaledBands(ccl4, 0.985), 150, 3200, 8);
        std::vector<Calculation> calcs = {one("orca5_chcl3.out"), one("gaussian_ccl4.log"), ccl4};
        const auto s = compare(calcs, Options{}, Broadening{}, exp, true);
        QCOMPARE(int(s.size()), 3);
        // both CCl4 calculations have the same activities and proportional frequencies: with fitted
        // scaling they agree equally well; CHCl3 comes last
        QCOMPARE(s[2].index, 0);
        QVERIFY(s[0].correlation > 0.99 && s[1].correlation > 0.99);
        QVERIFY(s[2].correlation < 0.5);
        for (const auto& x : s)
            if (x.index == 2) QVERIFY(near(x.scaling.low, 0.985, 1e-3));
            else if (x.index == 1) QVERIFY(near(x.scaling.low, 0.985 * 1.022, 1e-3));
        // without fitting (factor 1): the experiment is Gaussian ×1.0067 but ORCA 6 ×0.985, so the
        // Gaussian calculation is closer
        const auto u = compare(calcs, Options{}, Broadening{}, exp, false);
        QCOMPARE(u[0].index, 1);
        QCOMPARE(u[0].scaling.low, 1.0);
    }

    void findOutputsInFolder() {
        const QStringList files = findOutputs(DIR);
        QCOMPARE(files.size(), 16);                         // all fixtures except the plain note
        QVERIFY(!files.contains(DIR + "not_an_output.txt"));
        QTemporaryDir tmp;
        QDir(tmp.path()).mkpath("project/calc1");
        QFile::copy(DIR + "orca5_chcl3.out", tmp.filePath("project/calc1/chcl3.out"));
        QFile::copy(DIR + "not_an_output.txt", tmp.filePath("project/readme.txt"));
        QCOMPARE(findOutputs(tmp.path()), QStringList({tmp.filePath("project/calc1/chcl3.out")}));
        QVERIFY(findOutputs(tmp.path(), false).isEmpty());
        QVERIFY(golden::throws<lr::Error>([&] { findOutputs(tmp.filePath("nope")); }));
    }
};

QTEST_GUILESS_MAIN(TestComputed)
#include "test_computed.moc"
