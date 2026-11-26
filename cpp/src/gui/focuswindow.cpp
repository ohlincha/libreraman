#include "focuswindow.h"

#include <QCloseEvent>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "plotwidget.h"

namespace lr::gui {

namespace {
QString big(double v) {   // 12 345 / 1.23e6
    if (!std::isfinite(v)) return QStringLiteral("—");
    if (std::abs(v) >= 1e6) return QString::number(v, 'g', 3);
    return QLocale::system().toString(v, 'f', 0);
}
const char* kBannerOff = "Focus mode switches the laser on and keeps it on. Press ARM on the instrument first. With "
                         "the lid-open accessory the laser can run with the lid open: wear laser goggles.";
}  // namespace

FocusWindow::FocusWindow(Controller* ctl, std::function<QString()> folder, std::function<int()> channel, QWidget* parent)
    : QWidget(parent, Qt::Window), m_ctl(ctl), m_folder(std::move(folder)), m_channel(std::move(channel)) {
    setWindowTitle(QStringLiteral("Focus – LibreRaman"));
    resize(1100, 760);

    m_banner = new QLabel(QString::fromUtf8(kBannerOff));
    m_banner->setWordWrap(true);
    m_banner->setStyleSheet(QStringLiteral("QLabel { background: #fff3cd; border-left: 4px solid #e0a800; padding: 6px; }"));

    m_exposure = new QDoubleSpinBox;
    m_exposure->setRange(0.02, 10);
    m_exposure->setDecimals(2);
    m_exposure->setSingleStep(0.05);
    m_exposure->setValue(0.2);
    m_exposure->setSuffix(QStringLiteral(" s"));
    m_exposure->setToolTip(QStringLiteral("Exposure of each spectrum: short for a quick response while turning the "
                                          "knob; longer for a weak sample. Takes effect at the next start."));
    m_follow = new QDoubleSpinBox;
    m_follow->setRange(0, 4000);
    m_follow->setDecimals(0);
    m_follow->setSuffix(QStringLiteral(" cm⁻¹"));
    m_follow->setSpecialValueText(QStringLiteral("strongest"));
    m_follow->setToolTip(QStringLiteral("The band followed: the strongest (0), or the one at this shift (±15 cm⁻¹)"));
    m_minutes = new QSpinBox;
    m_minutes->setRange(1, 60);
    m_minutes->setValue(5);
    m_minutes->setSuffix(QStringLiteral(" min"));
    m_minutes->setToolTip(QStringLiteral("The laser goes off after this long"));
    m_start = new QPushButton(QStringLiteral("◎  START FOCUS"));
    m_start->setMinimumHeight(38);
    m_keep = new QPushButton(QStringLiteral("Keep this spectrum"));
    m_keep->setEnabled(false);
    m_keep->setToolTip(QStringLiteral("Save the spectrum shown (CSV in the Measure folder)"));
    auto* resetBest = new QPushButton(QStringLiteral("Reset best"));

    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("Exposure"), m_exposure);
    form->addRow(QStringLiteral("Follow band"), m_follow);
    form->addRow(QStringLiteral("Laser off after"), m_minutes);

    auto mkValue = [] {
        auto* l = new QLabel(QStringLiteral("—"));
        QFont f = l->font();
        f.setPointSizeF(f.pointSizeF() * 2.4);
        f.setBold(true);
        l->setFont(f);
        return l;
    };
    m_bandValue = mkValue();
    m_totalValue = mkValue();
    m_bandBest = new QLabel;
    m_totalBest = new QLabel;
    m_bandBar = new QProgressBar;
    m_totalBar = new QProgressBar;
    for (QProgressBar* b : {m_bandBar, m_totalBar}) {
        b->setRange(0, 1000);
        b->setTextVisible(false);
        b->setMaximumHeight(14);
    }
    m_bandBar->setToolTip(QStringLiteral("Now, relative to the best so far"));
    m_totalBar->setToolTip(m_bandBar->toolTip());
    auto* grid = new QGridLayout;
    grid->addWidget(new QLabel(QStringLiteral("<b>Strongest band</b> (height above the background)")), 0, 0);
    grid->addWidget(new QLabel(QStringLiteral("<b>Total Raman signal</b> (200–3200 cm⁻¹)")), 0, 1);
    grid->addWidget(m_bandValue, 1, 0);
    grid->addWidget(m_totalValue, 1, 1);
    grid->addWidget(m_bandBar, 2, 0);
    grid->addWidget(m_totalBar, 2, 1);
    grid->addWidget(m_bandBest, 3, 0);
    grid->addWidget(m_totalBest, 3, 1);
    m_warn = new QLabel;
    m_warn->setStyleSheet(QStringLiteral("QLabel { color: #b3261e; font-weight: 600; }"));
    m_warn->setWordWrap(true);

    m_plot = new PlotWidget;
    m_plot->setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    m_plot->setLabel(Axis::Left, QStringLiteral("Counts"));
    m_curve = m_plot->addCurve(QPen(QColor(47, 127, 208), 1.2));
    m_trend = new PlotWidget;
    m_trend->setLabel(Axis::Bottom, QStringLiteral("Time (s)"));
    m_trend->setLabel(Axis::Left, QStringLiteral("% of best"));
    m_trendBand = m_trend->addCurve(QPen(QColor(47, 127, 208), 2), QStringLiteral("band"));
    m_trendTotal = m_trend->addCurve(QPen(QColor(224, 140, 30), 2), QStringLiteral("total"));
    m_trend->setMaximumHeight(200);
    m_trend->setAutoY(false);

    auto* left = new QVBoxLayout;
    left->addLayout(form);
    left->addWidget(m_start);
    auto* row = new QHBoxLayout;
    row->addWidget(m_keep);
    row->addWidget(resetBest);
    left->addLayout(row);
    auto* howTo = new QLabel(QStringLiteral(
        "<p><b>How to focus</b><br>1. Sample in place; ARM pressed.<br>2. START FOCUS.<br>3. Turn the distance "
        "knob slowly: the numbers rise to a maximum and fall again; go back to the best (the bars show how "
        "close you are).<br>4. Stop (or Esc). Then measure as usual.</p>"));
    howTo->setWordWrap(true);
    left->addWidget(howTo);
    left->addStretch(1);
    auto* leftBox = new QWidget;
    leftBox->setLayout(left);
    leftBox->setMaximumWidth(320);

    auto* right = new QVBoxLayout;
    right->addLayout(grid);
    right->addWidget(m_warn);
    right->addWidget(m_trend);
    right->addWidget(m_plot, 1);

    auto* body = new QHBoxLayout;
    body->addWidget(leftBox);
    body->addLayout(right, 1);
    auto* top = new QVBoxLayout(this);
    top->addWidget(m_banner);
    top->addLayout(body, 1);

    connect(m_start, &QPushButton::clicked, this, &FocusWindow::startStop);
    connect(m_keep, &QPushButton::clicked, this, &FocusWindow::keep);
    connect(resetBest, &QPushButton::clicked, this, &FocusWindow::resetBest);
    connect(m_follow, &QDoubleSpinBox::valueChanged, this, &FocusWindow::resetBest);
    if (m_ctl) {
        connect(m_ctl, &Controller::acquired, this, &FocusWindow::onSpectrum);
        connect(m_ctl, &Controller::status, this, &FocusWindow::onStatus);
        connect(m_ctl, &Controller::busyChanged, this, [this](bool busy) {
            if (busy || !m_running) return;
            m_running = false;
            m_start->setText(QStringLiteral("◎  START FOCUS"));
            onStatus(m_status);
        });
    }
}

void FocusWindow::startStop() {
    if (!m_ctl) return;
    if (m_running) {
        m_ctl->stop();
        return;
    }
    resetBest();
    m_t.clear(), m_band.clear(), m_total.clear();
    m_clock.start();
    if (!m_ctl->startFocus(m_exposure->value(), m_channel ? m_channel() : 0, m_minutes->value() * 60.0)) return;
    m_running = true;
    m_start->setText(QStringLiteral("■  STOP (laser off)"));
}

void FocusWindow::resetBest() {
    m_bestBand = m_bestTotal = 0;
}

void FocusWindow::onSpectrum(const lr::Spectrum& sp, const QString& tag) {
    if (tag != QLatin1String("focus")) return;
    m_last = sp;
    m_haveLast = true;
    m_keep->setEnabled(true);
    const focus::Signal s = focus::measure(sp, m_follow->value());
    m_bestBand = std::max(m_bestBand, s.band);
    m_bestTotal = std::max(m_bestTotal, s.total);
    m_bandValue->setText(QStringLiteral("%1  <span style='font-size:small;font-weight:normal'>at %2 cm⁻¹</span>")
                             .arg(big(s.band)).arg(s.bandShift, 0, 'f', 0));
    m_bandValue->setTextFormat(Qt::RichText);
    m_totalValue->setText(big(s.total));
    m_bandBest->setText(QStringLiteral("best %1").arg(big(m_bestBand)));
    m_totalBest->setText(QStringLiteral("best %1").arg(big(m_bestTotal)));
    m_bandBar->setValue(m_bestBand > 0 ? int(1000 * std::max(0.0, s.band) / m_bestBand) : 0);
    m_totalBar->setValue(m_bestTotal > 0 ? int(1000 * std::max(0.0, s.total) / m_bestTotal) : 0);
    m_warn->setText(s.saturated ? QStringLiteral("Saturated (%1 counts): shorten the exposure.").arg(big(s.rawMax)) : QString());
    m_curve->setData(sp.shift, sp.intensity);
    m_plot->autoRange();
    // trend of the last minute, in % of the best so far
    const double t = m_clock.elapsed() / 1000.0;
    m_t.push_back(t), m_band.push_back(s.band), m_total.push_back(s.total);
    while (!m_t.empty() && t - m_t.front() > 60) m_t.erase(m_t.begin()), m_band.erase(m_band.begin()), m_total.erase(m_total.begin());
    lr::Vec pb(m_band.size()), pt(m_total.size());
    for (std::size_t i = 0; i < pb.size(); ++i) {
        pb[i] = m_bestBand > 0 ? 100 * m_band[i] / m_bestBand : 0;
        pt[i] = m_bestTotal > 0 ? 100 * m_total[i] / m_bestTotal : 0;
    }
    m_trendBand->setData(m_t, pb);
    m_trendTotal->setData(m_t, pt);
    m_trend->setXRange(std::max(0.0, t - 60), std::max(10.0, t));   // the last minute, 0-105 % of the best
    m_trend->setYRange(0, 105);
}

void FocusWindow::onStatus(const StatusView& st) {
    m_status = st;
    if (m_running) {   // the lid-open accessory makes the lid read closed: assume it may be open
        m_banner->setText(st.compartmentOpen
                              ? QStringLiteral("⚠  LASER ON WITH THE LID OPEN — wear laser goggles; keep eyes and skin out of the beam.")
                              : QStringLiteral("⚠  LASER ON — the lid may be open (lid-open accessory): wear laser goggles; keep eyes "
                                               "and skin out of the beam. Stop or Esc switches it off."));
        m_banner->setStyleSheet(QStringLiteral("QLabel { background: #c62828; color: white; font-weight: 700; padding: 8px; }"));
    } else {
        m_banner->setText(QString::fromUtf8(kBannerOff) + (st.armed ? QString() : QStringLiteral("  (Not armed now.)")));
        m_banner->setStyleSheet(QStringLiteral("QLabel { background: #fff3cd; border-left: 4px solid #e0a800; padding: 6px; }"));
    }
}

void FocusWindow::keep() {
    if (!m_haveLast) return;
    const QString dir = m_folder ? m_folder() : QDir::homePath();
    QDir().mkpath(dir);
    lr::Spectrum sp = m_last;
    sp.setMeta(QStringLiteral("sample"), QStringLiteral("focus"));
    const QString path = Controller::savePath(dir, QStringLiteral("focus"), sp.t);
    try {
        sp.saveCsv(path);
        m_warn->setText(QStringLiteral("Saved %1").arg(QDir::toNativeSeparators(path)));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, windowTitle(), QString::fromUtf8(e.what()));
    }
}

void FocusWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape && m_running) {
        m_ctl->stop();
        return;
    }
    QWidget::keyPressEvent(e);
}

void FocusWindow::closeEvent(QCloseEvent* e) {
    if (m_running && m_ctl) m_ctl->stop();   // the laser off when the window goes
    e->accept();
}

}  // namespace lr::gui
