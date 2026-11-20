// Tests for core/chemometrics with synthetic data: pure spectra made of Gaussian/Lorentzian bands, known
// concentration profiles (consecutive reaction A -> B -> C, or elution-like Gaussian profiles) and noise.
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

#include <cmath>
#include <random>

#include "core/chemometrics.h"
#include "core/errors.h"
#include "golden_util.h"

using namespace lr;
using namespace lr::chemo;

namespace {

const int kP = 300;                       // variables (keep the Debug build fast)
const int kN = 40;                        // spectra

Vec axis() {
    Vec x;
    for (int i = 0; i < kP; ++i) x.push_back(400.0 + i * 1400.0 / (kP - 1));
    return x;
}

double lorentz(double x, double c, double w) { return 1.0 / (1.0 + std::pow((x - c) / w, 2)); }
double gauss(double x, double c, double w) { return std::exp(-0.5 * std::pow((x - c) / w, 2)); }

// Three overlapping component spectra, each with one selective band (A 520, B 1100, C 1650).
Matrix pureSpectra() {
    const Vec x = axis();
    Matrix S(3, kP);
    for (int j = 0; j < kP; ++j) {
        const double v = x[size_t(j)];
        S(0, j) = 1.0 * lorentz(v, 520, 12) + 0.6 * gauss(v, 880, 25) + 0.4 * lorentz(v, 1450, 15);
        S(1, j) = 0.9 * lorentz(v, 1100, 10) + 0.5 * gauss(v, 900, 30) + 0.5 * lorentz(v, 1460, 18);
        S(2, j) = 1.0 * lorentz(v, 1650, 9) + 0.7 * gauss(v, 860, 20) + 0.3 * lorentz(v, 1440, 20);
    }
    return S;
}

// A -> B -> C, k1 = 0.12, k2 = 0.05 per time unit; times 0..39.
Matrix kinetics() {
    Matrix C(kN, 3);
    const double k1 = 0.12, k2 = 0.05;
    for (int i = 0; i < kN; ++i) {
        const double t = i;
        const double a = std::exp(-k1 * t);
        const double b = k1 / (k2 - k1) * (std::exp(-k1 * t) - std::exp(-k2 * t));
        C(i, 0) = a;
        C(i, 1) = b;
        C(i, 2) = 1.0 - a - b;
    }
    return C;
}

// Elution-like profiles: Gaussians centred at 10, 20, 30 (sigma 3).
Matrix elution() {
    Matrix C(kN, 3);
    for (int i = 0; i < kN; ++i)
        for (int j = 0; j < 3; ++j) C(i, j) = gauss(i, 10.0 + 10.0 * j, 3.0);
    return C;
}

Matrix noisy(const Matrix& D, double level, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, level * D.cwiseAbs().maxCoeff());
    Matrix out = D;
    for (Eigen::Index i = 0; i < out.size(); ++i) out.data()[i] += nd(rng);
    return out;
}

int nearestIndex(const Vec& x, double v) {
    int best = 0;
    for (int i = 1; i < int(x.size()); ++i)
        if (std::abs(x[size_t(i)] - v) < std::abs(x[size_t(best)] - v)) best = i;
    return best;
}

}  // namespace

class TestChemometrics : public QObject {
    Q_OBJECT
private slots:
    void resampleLinear() {
        const Vec x{0, 1, 2, 3}, y{0, 10, 20, 30};
        const Vec r = resample(x, y, {-1, 0.5, 2.25, 5});
        QCOMPARE(r[0], 0.0);
        QCOMPARE(r[1], 5.0);
        QCOMPARE(r[2], 22.5);
        QCOMPARE(r[3], 30.0);
    }

    void buildFromSpectra() {
        // identical axes: used as is; times from "t=..s"
        std::vector<fileio::DataSpectrum> ds(3);
        for (int k = 0; k < 3; ++k) {
            ds[size_t(k)].name = QString("s%1").arg(k);
            ds[size_t(k)].x = {100, 101, 102, 103};
            ds[size_t(k)].y = {double(k), double(k) + 1, double(k) + 2, double(k) + 3};
            ds[size_t(k)].setMeta("time", QString("t=%1s").arg(5.0 * k + 2.5));
        }
        DataMatrix m = buildMatrix(ds);
        QCOMPARE(m.rows(), 3);
        QCOMPARE(m.cols(), 4);
        QVERIFY(m.hasTimes);
        QCOMPARE(m.times[2], 12.5);
        QCOMPARE(m.D(2, 1), 3.0);
        QCOMPARE(m.names[1], QString("s1"));
        // different axes: resampled on the common range, median spacing; descending input sorted
        ds[1].x = {103.5, 102.5, 101.5, 100.5, 99.5};
        ds[1].y = {4, 3, 2, 1, 0};
        ds[2].setMeta("time", "garbage");
        m = buildMatrix(ds);
        QCOMPARE(m.x.front(), 100.0);
        QCOMPARE(m.x.back(), 103.0);
        QCOMPARE(m.cols(), 4);
        QCOMPARE(m.D(1, 0), 0.5);                   // 99.5 -> 0, 100.5 -> 1
        QVERIFY(!m.hasTimes);                       // one spectrum without a valid time: indices
        QCOMPARE(m.times[2], 2.0);
        // no overlap
        ds[1].x = {200, 201, 202, 203, 204};
        QVERIFY(golden::throws<lr::Error>([&] { buildMatrix(ds); }));
    }

    void buildFromAcquisitions() {
        std::vector<Spectrum> sp(2);
        const QDateTime t0 = QDateTime::fromString("2026-11-20T10:00:00+01:00", Qt::ISODate);
        for (int k = 0; k < 2; ++k) {
            sp[size_t(k)].shift = {1, 2, 3};
            sp[size_t(k)].intensity = {1, 2, double(k)};
            sp[size_t(k)].t = t0.addSecs(30 * k);
        }
        const DataMatrix m = buildMatrix(sp);
        QVERIFY(m.hasTimes);
        QCOMPARE(m.times[1], 30.0);
        QCOMPARE(m.names[0], QString("spectrum 1"));
    }

    void readSeriesTable() {
        QTemporaryDir dir;
        const QString path = dir.filePath("series.csv");
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            QTextStream o(&f);
            o << "# LibreRamen time series, started 2026-11-20T10:00:00+01:00\n# channel: 532 nm\n";
            o << "raman_shift_cm1,t=0.0s,t=10.2s,t=20.4s\n";
            for (int i = 0; i < 5; ++i) o << 100 + i << "," << i << "," << 2 * i << "," << 3 * i << "\n";
        }
        const DataMatrix m = readSeries(path);
        QCOMPARE(m.rows(), 3);
        QCOMPARE(m.cols(), 5);
        QVERIFY(m.hasTimes);
        QCOMPARE(m.times[1], 10.2);
        QCOMPARE(m.D(2, 4), 12.0);
    }

    void preprocessing() {
        DataMatrix m;
        m.x = axis();
        m.D = pureSpectra() * 1000.0;
        for (Eigen::Index i = 0; i < m.D.rows(); ++i) {
            m.D.row(i).array() += 200.0;            // offset
            m.names << QString::number(i);
            m.times.push_back(double(i));
        }
        Preprocess p;
        p.xmin = 1000;
        p.xmax = 1200;
        p.normalize = "max";
        DataMatrix out = preprocess(m, p);
        QVERIFY(out.x.front() >= 1000 && out.x.back() <= 1200);
        QVERIFY(out.x.front() - 1000 < 5 && 1200 - out.x.back() < 5);
        for (int i = 0; i < out.rows(); ++i) QVERIFY(std::abs(out.D.row(i).maxCoeff() - 1.0) < 1e-12);
        // baseline removes the offset (SNIP: lower envelope)
        p = {};
        p.baseline = "snip";
        out = preprocess(m, p);
        QVERIFY(out.D.minCoeff() > -5 && out.D.minCoeff() < 50);
        // first derivative of a constant offset is zero; of a band it changes sign at the maximum
        p = {};
        p.derivative = 1;
        out = preprocess(m, p);
        const int j = nearestIndex(out.x, 1650);
        QVERIFY(out.D(2, j - 3) > 0 && out.D(2, j + 3) < 0);
        p.derivative = 3;
        QVERIFY(golden::throws<lr::Error>([&] { preprocess(m, p); }));
        p = {};
        p.xmin = 5000;
        QVERIFY(golden::throws<lr::Error>([&] { preprocess(m, p); }));
        // autoscaling
        const ColumnScaling cs = columnScaling(m.D, Scaling::Autoscale);
        const Matrix X = applyScaling(m.D, cs);
        QVERIFY(X.colwise().mean().cwiseAbs().maxCoeff() < 1e-9);
        const double var = X.col(10).squaredNorm() / double(X.rows() - 1);
        QVERIFY(std::abs(var - 1.0) < 1e-9);
    }

    void pcaRankAndVariance() {
        const Matrix D = noisy(kinetics() * pureSpectra(), 0.002, 1);
        // raw data: three components
        PcaResult r = pca(D, 5, Scaling::None);
        QCOMPARE(r.components, 5);
        QCOMPARE(rankForVariance(r, 0.9995), 3);
        QVERIFY(r.cumulative[2] > 0.9995);
        QVERIFY(r.explained[3] < 1e-4 && r.explained[2] > 1e-2);
        double sum = 0;
        for (double e : r.explained) sum += e;
        QVERIFY(std::abs(sum - 1.0) < 1e-12);
        // closed system (A + B + C = 1): after mean-centring only two components remain
        r = pca(D, 4, Scaling::Center);
        QVERIFY(r.cumulative[1] > 0.99);
        QVERIFY(r.explained[2] < 2.0 * r.explained[3]);     // the third is already at the noise level
        // scores × loadingsᵀ reproduce the centred data up to the residual Q
        const Matrix X = applyScaling(D, r.scaling);
        const Matrix E = X - r.scores * r.loadings.transpose();
        for (int i = 0; i < kN; ++i) QVERIFY(std::abs(E.row(i).squaredNorm() - r.q[size_t(i)]) < 1e-9);
        // orthonormal loadings; mean T² of the calibration samples is k(n-1)/n exactly
        QVERIFY((r.loadings.transpose() * r.loadings - Matrix::Identity(4, 4)).cwiseAbs().maxCoeff() < 1e-10);
        double t2 = 0;
        for (double v : r.t2) t2 += v;
        QVERIFY(std::abs(t2 / kN - 4.0 * (kN - 1) / kN) < 1e-9);
        // limits are finite and positive; the calibration samples are mostly inside
        QVERIFY(r.qLimit > 0 && r.t2Limit > 0 && std::isfinite(r.t2Limit));
        int outside = 0;
        for (double v : r.q) outside += v > r.qLimit;
        QVERIFY(outside <= 6);
        QVERIFY(golden::throws<lr::Error>([&] { pca(D.topRows(1), 2); }));
    }

    void efaWindows() {
        const Matrix D = noisy(elution() * pureSpectra(), 0.002, 2);
        const EfaResult e = efa(D, 3);
        QCOMPARE(e.forward.rows(), Eigen::Index(kN));
        QCOMPARE(e.forward.cols(), Eigen::Index(3));
        QVERIFY(std::isnan(e.forward(0, 1)));       // one spectrum: one singular value
        QVERIFY(std::isnan(e.backward(kN - 1, 1)));
        QVERIFY(e.noise > 0);
        QCOMPARE(int(e.windows.size()), 3);
        for (int j = 0; j < 3; ++j) {
            const double centre = 10.0 + 10.0 * j;
            const auto [first, last] = e.windows[size_t(j)];
            QVERIFY2(first < centre && last > centre, qPrintable(QString("window %1: %2..%3").arg(j).arg(first).arg(last)));
            QVERIFY2(std::abs(first - (centre - 7)) <= 4 && std::abs(last - (centre + 7)) <= 4,
                     qPrintable(QString("window %1: %2..%3").arg(j).arg(first).arg(last)));
        }
        QVERIFY(e.windows[0].first < e.windows[1].first && e.windows[1].first < e.windows[2].first);
        // the full-matrix singular values: forward row n-1 equals backward row 0
        for (int j = 0; j < 3; ++j) QVERIFY(std::abs(e.forward(kN - 1, j) - e.backward(0, j)) < 1e-8 * e.forward(kN - 1, 0));
        const Matrix C0 = efaConcentrations(e, kN);
        QCOMPARE(C0(10, 0), 1.0);
        QCOMPARE(C0(0, 2), 0.0);
    }

    void simplismaPureVariables() {
        const Vec x = axis();
        const Matrix D = noisy(kinetics() * pureSpectra(), 0.002, 3);
        const SimplismaResult r = simplisma(D, 3, 5.0);
        QCOMPARE(int(r.pureVariables.size()), 3);
        // one pure variable at each selective band (A 520, B 1100, C 1650), in any order
        std::vector<bool> hit(3, false);
        const double bands[3] = {520, 1100, 1650};
        for (int v : r.pureVariables)
            for (int b = 0; b < 3; ++b)
                if (std::abs(x[size_t(v)] - bands[b]) < 25) hit[size_t(b)] = true;
        QVERIFY2(hit[0] && hit[1] && hit[2], qPrintable(QString("pure variables at %1, %2, %3")
                                                            .arg(x[size_t(r.pureVariables[0])])
                                                            .arg(x[size_t(r.pureVariables[1])])
                                                            .arg(x[size_t(r.pureVariables[2])])));
        QCOMPARE(r.C.rows(), Eigen::Index(kN));
        QCOMPARE(r.S.rows(), Eigen::Index(3));
        QVERIFY(std::abs(r.S.row(0).maxCoeff() - 1.0) < 1e-12);
        // the estimates already resemble the pure spectra
        const auto [perm, corr] = matchComponents(pureSpectra(), r.S);
        for (double c : corr) QVERIFY(c > 0.8);
        Q_UNUSED(perm);
    }

    void opaPureSpectraAndVariables() {
        const Matrix C = elution();
        const Matrix D = noisy(C * pureSpectra(), 0.002, 4);
        const OpaResult r = opa(D, 3);
        QCOMPARE(int(r.selected.size()), 3);
        QCOMPARE(r.estimate.rows(), Eigen::Index(3));
        // each selected spectrum is dominated by a different component
        std::vector<int> dominant;
        for (int s : r.selected) {
            Eigen::Index j = 0;
            C.row(s).maxCoeff(&j);
            QVERIFY(C(s, j) / C.row(s).sum() > 0.85);
            dominant.push_back(int(j));
        }
        std::sort(dominant.begin(), dominant.end());
        QCOMPARE(dominant, (std::vector<int>{0, 1, 2}));
        // on variables: selective bands
        const Matrix Dk = noisy(kinetics() * pureSpectra(), 0.002, 5);
        const OpaResult v = opa(Dk, 3, true);
        QCOMPARE(v.estimate.rows(), Eigen::Index(kN));
        const auto [perm, corr] = matchComponents(kinetics().transpose(), v.estimate.transpose());
        for (double c : corr) QVERIFY(c > 0.95);
        Q_UNUSED(perm);
    }

    void nnlsKkt() {
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        for (int trial = 0; trial < 50; ++trial) {
            const int m = 30, k = 1 + trial % 7;
            Matrix A(m, k);
            Vector b(m);
            for (Eigen::Index i = 0; i < A.size(); ++i) A.data()[i] = u(rng);
            for (int i = 0; i < m; ++i) b(i) = u(rng) - 0.3;
            const Vector x = nnls(A, b);
            const Vector w = A.transpose() * (b - A * x);   // gradient of -0.5||Ax-b||²
            QVERIFY(x.minCoeff() >= 0);
            for (int i = 0; i < k; ++i) {
                QVERIFY(w(i) < 1e-9);                       // no descent direction into the feasible set
                if (x(i) > 0) QVERIFY(std::abs(w(i)) < 1e-9);
            }
        }
        // exact recovery with zeros
        Matrix A(20, 4);
        for (Eigen::Index i = 0; i < A.size(); ++i) A.data()[i] = u(rng);
        const Vector xt = (Vector(4) << 1.5, 0.0, 2.0, 0.0).finished();
        const Vector x = nnls(A, A * xt);
        QVERIFY((x - xt).cwiseAbs().maxCoeff() < 1e-10);
    }

    void nnlsFitWithBackground() {
        const Vec x = axis();
        const Matrix S = pureSpectra();
        Vec y(static_cast<size_t>(kP));
        for (int j = 0; j < kP; ++j) y[size_t(j)] = 2.0 * S(0, j) + 0.5 * S(2, j) + 0.3 + 1e-4 * (x[size_t(j)] - 400);
        NnlsFit f = nnlsFit(S, x, y, 1);
        QVERIFY(std::abs(f.coefficients[0] - 2.0) < 1e-8);
        QVERIFY(std::abs(f.coefficients[1]) < 1e-8);
        QVERIFY(std::abs(f.coefficients[2] - 0.5) < 1e-8);
        QVERIFY(std::abs(f.background[0] - 0.3) < 1e-8);
        QVERIFY(f.residualNorm < 1e-8);
        QVERIFY(f.r2 > 0.999999);
        // without a background term the amounts are biased but still non-negative
        f = nnlsFit(S, x, y, -1);
        for (double c : f.coefficients) QVERIFY(c >= 0);
        QVERIFY(f.residualNorm > 0.1);
        QVERIFY(golden::throws<lr::Error>([&] { nnlsFit(S, x, Vec(5, 1.0)); }));
    }

    void mcrRecoversComponents() {
        const Matrix Ct = kinetics(), St = pureSpectra();
        const Matrix D = noisy(Ct * St, 0.005, 6);
        const SimplismaResult init = simplisma(D, 3, 5.0);
        McrOptions o;
        o.maxIterations = 200;
        int calls = 0;
        const McrResult r = mcrAls(D, {{}, init.S}, o, nullptr, [&](int, double) { ++calls; });
        QCOMPARE(calls, r.iterations);
        QCOMPARE(int(r.lofHistory.size()), r.iterations);
        QVERIFY2(r.lof < 5.0, qPrintable(QString("LOF %1 %").arg(r.lof)));
        QVERIFY(r.r2 > 99.5);
        QVERIFY(r.C.minCoeff() >= 0 && r.S.minCoeff() >= 0);
        const auto [ps, cs] = matchComponents(St, r.S);
        const auto [pc, cc] = matchComponents(Ct.transpose(), r.C.transpose());
        for (int j = 0; j < 3; ++j) {
            QVERIFY2(cs[size_t(j)] > 0.99, qPrintable(QString("spectrum %1: r = %2").arg(j).arg(cs[size_t(j)])));
            QVERIFY2(cc[size_t(j)] > 0.99, qPrintable(QString("profile %1: r = %2").arg(j).arg(cc[size_t(j)])));
        }
        QCOMPARE(ps, pc);                                    // same permutation for C and S
        for (int j = 0; j < 3; ++j) QVERIFY(std::abs(r.S.row(j).norm() - 1.0) < 1e-9);   // unit spectra
        // LOF close to the noise level (0.5 % of max ~ relative rms noise)
        const double noiseLof = 100.0 * (D - Ct * St).norm() / D.norm();
        QVERIFY(r.lof <= noiseLof * 1.05 + 0.05);
    }

    void mcrConstraints() {
        const Matrix Ct = kinetics(), St = pureSpectra();
        const Matrix D = noisy(Ct * St, 0.005, 8);
        const SimplismaResult init = simplisma(D, 3, 5.0);
        // closure: rows of C sum to the total
        McrOptions o;
        o.closure = true;
        o.closureTotal = 1.0;
        McrResult r = mcrAls(D, {{}, init.S}, o);
        for (int i = 0; i < kN; ++i) QVERIFY(std::abs(r.C.row(i).sum() - 1.0) < 1e-12);
        auto [pc, cc] = matchComponents(Ct.transpose(), r.C.transpose());
        for (double c : cc) QVERIFY(c > 0.99);
        // with closure the true concentrations themselves are recovered (scale fixed)
        for (int j = 0; j < 3; ++j) QVERIFY((r.C.col(pc[size_t(j)]) - Ct.col(j)).cwiseAbs().maxCoeff() < 0.05);
        // unimodality
        o = {};
        o.unimodalC = true;
        r = mcrAls(D, {{}, init.S}, o);
        for (int j = 0; j < 3; ++j) {
            Eigen::Index m = 0;
            r.C.col(j).maxCoeff(&m);
            for (Eigen::Index i = m + 1; i < kN; ++i) QVERIFY(r.C(i, j) <= r.C(i - 1, j) + 1e-12);
            for (Eigen::Index i = m - 1; i >= 0; --i) QVERIFY(r.C(i, j) <= r.C(i + 1, j) + 1e-12);
        }
        Vector bump = (Vector(6) << 0, 2, 1, 3, 1, 0).finished();
        makeUnimodal(bump);
        QCOMPARE(bump(1), 1.0);                              // clipped to its right neighbour on the way up
        // fixed (known) spectrum: start from the true spectrum of B, keep it
        o = {};
        o.fixedSpectra = {1};
        Matrix S0 = init.S;
        S0.row(1) = St.row(1);
        r = mcrAls(D, {{}, S0}, o);
        QVERIFY((r.S.row(1) - St.row(1)).cwiseAbs().maxCoeff() == 0.0);
        QVERIFY(r.lof < 5.0 && r.C.minCoeff() >= 0);
        // non-negativity off: unconstrained least squares still fits the data
        o = {};
        o.nonNegC = o.nonNegS = false;
        r = mcrAls(D, {{}, init.S}, o);
        QVERIFY(r.lof < 5.0);
        // from initial concentrations (EFA windows)
        const EfaResult e = efa(D, 3);
        r = mcrAls(D, {efaConcentrations(e, kN), {}}, McrOptions{});
        QVERIFY(r.lof < 5.0);
        // errors
        QVERIFY(golden::throws<lr::Error>([&] { mcrAls(D, {{}, Matrix::Ones(2, 5)}); }));
        McrOptions bad;
        bad.fixedSpectra = {5};
        QVERIFY(golden::throws<lr::Error>([&] { mcrAls(D, {{}, init.S}, bad); }));
    }

    void mcrStop() {
        const Matrix D = noisy(kinetics() * pureSpectra(), 0.005, 9);
        std::atomic<bool> stop{false};
        McrOptions o;
        o.tolerancePercent = 0;                   // never converges by itself
        o.maxIterations = 1000;
        const McrResult r = mcrAls(D, {{}, simplisma(D, 3).S}, o, &stop, [&](int it, double) {
            if (it == 5) stop = true;
        });
        QVERIFY(r.stopped);
        QCOMPARE(r.iterations, 5);
        QVERIFY(r.message.startsWith("stopped"));
    }

    void ambiguity() {
        const Vec x = axis();
        const int n = kN;
        // (a) selective in both C and S: unique solution, narrow band
        Matrix C1(n, 2), S1(2, kP);
        for (int i = 0; i < n; ++i) {
            C1(i, 0) = i < 25 ? gauss(i, 10, 4) : 0.0;
            C1(i, 1) = i > 15 ? gauss(i, 30, 4) : 0.0;
        }
        for (int j = 0; j < kP; ++j) {
            const double v = x[size_t(j)];
            S1(0, j) = lorentz(v, 700, 15) + 0.5 * gauss(v, 1000, 40);
            S1(1, j) = lorentz(v, 1400, 15) + 0.5 * gauss(v, 1050, 40);
        }
        // zero the far tails so that both factors have truly selective regions
        S1 = (S1.array() < 1e-3).select(0.0, S1);
        McrResult r = mcrAls(C1 * S1, {{}, S1}, McrOptions{});
        auto bands = ambiguityBands(r, McrOptions{});
        QCOMPARE(int(bands.size()), 2);
        for (const auto& b : bands) {
            QVERIFY(b.fMin <= b.fOptimal + 1e-12 && b.fOptimal <= b.fMax + 1e-12);
            QVERIFY2(b.fMax - b.fMin < 0.05, qPrintable(QString("selective: %1..%2").arg(b.fMin).arg(b.fMax)));
        }
        // (b) no selectivity anywhere: a wide band of feasible solutions
        Matrix C2(n, 2), S2(2, kP);
        for (int i = 0; i < n; ++i) {
            C2(i, 0) = 1.0 + 0.6 * std::sin(i * 0.2);
            C2(i, 1) = 1.0 + 0.6 * std::cos(i * 0.15);
        }
        for (int j = 0; j < kP; ++j) {
            const double v = x[size_t(j)];
            S2(0, j) = 0.3 + gauss(v, 900, 200);
            S2(1, j) = 0.3 + gauss(v, 1300, 200);
        }
        r = mcrAls(C2 * S2, {{}, S2}, McrOptions{});
        bands = ambiguityBands(r, McrOptions{});
        for (const auto& b : bands) {
            QVERIFY(b.fMin <= b.fOptimal + 1e-12 && b.fOptimal <= b.fMax + 1e-12);
            QVERIFY2(b.fMax - b.fMin > 0.2, qPrintable(QString("overlapped: %1..%2").arg(b.fMin).arg(b.fMax)));
            // the extreme solutions are feasible (within the 1 % tolerance)
            const Vector cmin = Eigen::Map<const Vector>(b.cMin.data(), Eigen::Index(b.cMin.size()));
            const Vector smax = Eigen::Map<const Vector>(b.sMax.data(), Eigen::Index(b.sMax.size()));
            QVERIFY(cmin.minCoeff() > -0.02 * cmin.cwiseAbs().maxCoeff());
            QVERIFY(smax.minCoeff() > -0.02 * smax.cwiseAbs().maxCoeff());
        }
        // brute force for k = 2: all T = [[1, a], [b, 1]] on a grid, hard constraints with the same 1 % tolerance
        {
            Matrix C0 = r.C, S0 = r.S;
            for (int j = 0; j < 2; ++j) {                   // balanced factors, as ambiguityBands does
                const double a = std::sqrt(S0.row(j).norm() / C0.col(j).norm());
                C0.col(j) *= a;
                S0.row(j) /= a;
            }
            const double total = (C0 * S0).norm();
            double gmin[2] = {1e9, 1e9}, gmax[2] = {-1e9, -1e9};
            for (int ia = -120; ia <= 120; ++ia)
                for (int ib = -120; ib <= 120; ++ib) {
                    Matrix T(2, 2);
                    T << 1, ia * 0.025, ib * 0.025, 1;
                    if (std::abs(T.determinant()) < 1e-6) continue;
                    const Matrix C = C0 * T, S = T.inverse() * S0;
                    bool ok = true;
                    for (int j = 0; j < 2 && ok; ++j)
                        ok = C.col(j).minCoeff() >= -0.01 * C.col(j).cwiseAbs().maxCoeff() &&
                             S.row(j).minCoeff() >= -0.01 * S.row(j).cwiseAbs().maxCoeff();
                    if (!ok) continue;
                    for (int j = 0; j < 2; ++j) {
                        const double f = C.col(j).norm() * S.row(j).norm() / total;
                        gmin[j] = std::min(gmin[j], f);
                        gmax[j] = std::max(gmax[j], f);
                    }
                }
            for (int j = 0; j < 2; ++j) {
                const auto& b = bands[size_t(j)];
                QVERIFY2(std::abs(b.fMin - gmin[j]) < 0.02 && std::abs(b.fMax - gmax[j]) < 0.02,
                         qPrintable(QString("component %1: optimiser %2..%3, grid %4..%5")
                                        .arg(j).arg(b.fMin).arg(b.fMax).arg(gmin[j]).arg(gmax[j])));
            }
        }
        // without non-negativity the band is unbounded
        McrOptions none;
        none.nonNegC = none.nonNegS = false;
        bands = ambiguityBands(r, none);
        QVERIFY(std::isinf(bands[0].fMax));
    }

    void matching() {
        const Matrix S = pureSpectra();
        Matrix F(3, kP);
        F.row(0) = 2.0 * S.row(2);
        F.row(1) = S.row(0);
        F.row(2) = 0.5 * S.row(1);
        const auto [perm, corr] = matchComponents(S, F);
        QCOMPARE(perm, (std::vector<int>{1, 2, 0}));
        for (double c : corr) QVERIFY(std::abs(c - 1.0) < 1e-12);
    }
};

QTEST_GUILESS_MAIN(TestChemometrics)
#include "test_chemometrics.moc"
