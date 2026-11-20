#include "statusstrip.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace lr::gui {

using theme::Tone;

// ============================================================================================ Tile
Tile::Tile(const QString& title, QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("tile"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 6, 12, 6);
    lay->setSpacing(0);
    const QString caption = QStringLiteral("color: rgba(255,255,255,0.85); font-size: 9pt;");
    m_head = new QLabel(title, this);
    m_head->setStyleSheet(caption);
    m_main = new QLabel(QStringLiteral("—"), this);
    m_main->setFont(theme::tileValueFont());
    m_sub = new QLabel(this);
    m_sub->setStyleSheet(caption);
    for (QLabel* l : {m_head, m_main, m_sub}) {
        l->setTextInteractionFlags(Qt::NoTextInteraction);
        lay->addWidget(l);
    }
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    set(Tone::Grey, QStringLiteral("—"));
}

void Tile::set(Tone tone, const QString& main, const QString& sub, bool alarm) {
    m_tone = tone;
    m_alarm = alarm;
    // alarm: the one state that must be impossible to miss (whole tile filled)
    m_main->setStyleSheet(QStringLiteral("color: %1;").arg(alarm ? QStringLiteral("white")
                                                                 : theme::toneColor(tone).name()));
    m_main->setText(main);
    m_sub->setText(sub);
    QFrame::update();
}

void Tile::paintEvent(QPaintEvent*) {
    // Python: "#tile { background: …; border-radius: 6px; border-left: 6px solid accent }", drawn by hand
    // so that the accent bar follows the rounded corners
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), 6, 6);
    const QColor accent = theme::toneColor(m_tone);
    p.fillPath(path, m_alarm ? accent : theme::tileBackground());
    if (!m_alarm) {
        p.setClipPath(path);
        p.fillRect(QRectF(0, 0, 6, height()), accent);
    }
}

QString Tile::title() const { return m_head->text(); }
QString Tile::mainText() const { return m_main->text(); }
QString Tile::subText() const { return m_sub->text(); }

// ============================================================================================ StatusStrip
StatusStrip::StatusStrip(QWidget* parent) : QWidget(parent) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 4);
    m_conn = new Tile(QStringLiteral("INSTRUMENT"), this);
    m_laser = new Tile(QStringLiteral("LASER"), this);
    m_comp = new Tile(QStringLiteral("COMPARTMENT"), this);
    m_chan = new Tile(QStringLiteral("CHANNEL"), this);
    lay->addWidget(m_conn, 3);
    lay->addWidget(m_laser, 3);
    lay->addWidget(m_comp, 2);
    lay->addWidget(m_chan, 1);
    m_stop = new QPushButton(QStringLiteral("■  STOP"), this);
    m_stop->setMinimumSize(120, 48);
    m_stop->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_stop->setStyleSheet(theme::stopButtonStyle());
    m_stop->setToolTip(QStringLiteral("Stop the measurement and switch the laser off"));
    connect(m_stop, &QPushButton::clicked, this, &StatusStrip::stopClicked);
    lay->addWidget(m_stop);
    update(StatusView{});
}

void StatusStrip::update(const StatusView& st) {
    m_status = st;
    const QString& c = st.connection;
    QString name;
    if (!st.model.isEmpty() || !st.serial.isEmpty())
        name = QStringLiteral("%1 · S/N %2")
                   .arg(st.model.isEmpty() ? QStringLiteral("Cora 5000") : st.model,
                        st.serial.isEmpty() ? QStringLiteral("?") : st.serial);
    if (st.simulated && c == QLatin1String("connected"))
        m_conn->set(Tone::Blue, QStringLiteral("● SIMULATOR"), QStringLiteral("spectra recorded on a real Cora 5000"));
    else if (c == QLatin1String("connected"))
        m_conn->set(Tone::Green, QStringLiteral("● CONNECTED"), name);
    else if (c == QLatin1String("initialising"))
        m_conn->set(Tone::Yellow, QStringLiteral("◐ INITIALISING…"),
                    QStringLiteral("switching on spectrometers and cooling"));
    else if (c == QLatin1String("lost"))
        m_conn->set(Tone::Red, QStringLiteral("✖ CONNECTION LOST"), QStringLiteral("check the USB cable"), true);
    else if (c == QLatin1String("not found"))
        m_conn->set(Tone::Grey, QStringLiteral("○ NOT FOUND"), QStringLiteral("no instrument"));
    else
        m_conn->set(Tone::Grey, QStringLiteral("○ SEARCHING…"), QString());
    if (c != QLatin1String("connected")) {
        for (Tile* t : {m_laser, m_comp, m_chan})
            t->set(Tone::Grey, QStringLiteral("—"));
        return;
    }
    const QString ch = st.channel ? QString::number(st.channel) : QStringLiteral("—");
    if (st.laserOn)
        m_laser->set(Tone::Red, QStringLiteral("✹ LASER ON"), QStringLiteral("%1 nm emitting").arg(ch), true);
    else if (st.armed)
        m_laser->set(Tone::Orange, QStringLiteral("▲ ARMED"), QStringLiteral("laser can be switched on"));
    else
        m_laser->set(Tone::Grey, QStringLiteral("○ NOT ARMED"), QStringLiteral("press ARM on the instrument"));
    if (st.compartmentOpen)
        m_comp->set(Tone::Yellow, QStringLiteral("▢ OPEN"), QStringLiteral("laser disabled"));
    else
        m_comp->set(Tone::Green, QStringLiteral("■ CLOSED"));
    m_chan->set(Tone::Green, QStringLiteral("%1 nm").arg(ch));
}

// ============================================================================================ ElidedLabel
ElidedLabel::ElidedLabel(const QString& text, QWidget* parent) : QLabel(parent) {
    setMinimumWidth(40);
    setFullText(text);
}

void ElidedLabel::setFullText(const QString& text) {
    m_full = text;
    setToolTip(text);
    refresh();
}

void ElidedLabel::setElideMode(Qt::TextElideMode m) {
    m_mode = m;
    refresh();
}

QSize ElidedLabel::minimumSizeHint() const {
    const QSize s = QLabel::minimumSizeHint();
    return {40, s.height()};        // never forces the layout wider
}

void ElidedLabel::resizeEvent(QResizeEvent* e) {
    QLabel::resizeEvent(e);
    refresh();
}

void ElidedLabel::refresh() {
    QLabel::setText(fontMetrics().elidedText(m_full, m_mode, std::max(20, width() - 2)));
}

}  // namespace lr::gui
