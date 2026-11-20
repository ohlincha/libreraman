// Placeholder while the GUI is ported from libreramen/gui: splash, status strip and a plot with a
// synthetic spectrum. Options: --dark / --light, --screenshot <file.png> (save the window and quit).
#include "icon.h"
#include "plotwidget.h"
#include "splash.h"
#include "statusstrip.h"
#include "theme.h"

#include <QApplication>
#include <QMainWindow>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

using namespace lr::gui;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("LibreRamen"));
    app.setApplicationVersion(QStringLiteral(LIBRERAMEN_VERSION));
    app.setWindowIcon(appIcon());
    const QStringList args = app.arguments();
    theme::apply(app, args.contains(QStringLiteral("--dark"))    ? theme::Mode::Dark
                      : args.contains(QStringLiteral("--light")) ? theme::Mode::Light
                                                                 : theme::Mode::Auto);
    const int shot = int(args.indexOf(QStringLiteral("--screenshot")));

    Splash splash;
    splash.show();
    splash.message(QStringLiteral("Starting…"));
    splash.setProgress(0);

    QMainWindow win;
    win.setWindowTitle(QStringLiteral("LibreRamen " LIBRERAMEN_VERSION));
    auto* central = new QWidget;
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    auto* strip = new StatusStrip;
    StatusView st;
    st.connection = QStringLiteral("connected");
    st.simulated = true;
    st.armed = true;
    st.channel = 532;
    strip->update(st);
    v->addWidget(strip);

    auto* plot = new PlotWidget;                 // synthetic carbon tetrachloride spectrum
    plot->setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    plot->setLabel(Axis::Left, QStringLiteral("Intensity (counts)"));
    lr::Vec x, y, base;
    const double bands[][3] = {{218, 9000, 6}, {314, 12000, 7}, {459, 30000, 4}, {762, 5000, 6}, {791, 5200, 6}};
    for (int i = 0; i < 2048; ++i) {
        const double s = 120 + i * (3500.0 - 120) / 2047;
        const double b = 2500 + 1800 * std::exp(-std::pow((s - 1400) / 1300, 2));
        double val = b;
        for (const auto& k : bands)
            val += k[1] / (1 + std::pow((s - k[0]) / k[2], 2));
        x.push_back(s);
        y.push_back(val + 25 * std::sin(i * 1.7));
        base.push_back(b);
    }
    plot->addCurve(x, base, QPen(plot->plotPalette().baseline, 1.2, Qt::DashLine));
    plot->addCurve(x, y, QPen(plot->plotPalette().main, 1.6));
    plot->addPeakLabels()->setPeaks({218, 314, 459, 762, 791}, x, y);
    plot->setHomeXRange(150, 3500);
    v->addWidget(plot, 1);
    win.setCentralWidget(central);
    win.resize(1200, 760);

    double progress = 0;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        progress += 0.1;
        splash.setProgress(progress);
        splash.message(QStringLiteral("Initialising… %1 %").arg(int(progress * 100 + 0.5)));
        if (progress >= 1) {
            timer.stop();
            win.show();
            splash.finish(&win);
        }
    });
    timer.start(80);
    QObject::connect(&splash, &Splash::skipped, &app, [&] {
        timer.stop();
        win.show();
    });
    if (shot >= 0 && shot + 1 < args.size()) {
        const QString path = args.at(shot + 1);
        QTimer::singleShot(1500, &app, [&win, path] {
            win.grab().save(path);
            QApplication::quit();
        });
    }
    return app.exec();
}
