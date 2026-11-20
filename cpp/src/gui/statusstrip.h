#pragma once
// Status strip at the top of the main window (Python gui/app.py: Tile, StatusStrip) and ElidedLabel.
#include "theme.h"

#include <QFrame>
#include <QLabel>
#include <QString>
#include <QWidget>

class QPushButton;

namespace lr::gui {

// What the strip shows; filled by the controller from the instrument status (GUI-side, no core types).
struct StatusView {
    QString connection;         // "connected", "initialising", "lost", "not found", other: searching
    bool simulated = false;
    bool armed = false;
    bool compartmentOpen = false;
    bool laserOn = false;
    int channel = 0;            // 532 / 1064 nm; 0 unknown
    QString model, serial;      // empty: not known yet
};

// Dark slate tile with a coloured accent bar at the left; alarm: the whole tile filled.
class Tile : public QFrame {
    Q_OBJECT
public:
    explicit Tile(const QString& title, QWidget* parent = nullptr);
    void set(theme::Tone tone, const QString& main, const QString& sub = {}, bool alarm = false);
    QString title() const;
    QString mainText() const;
    QString subText() const;
    theme::Tone tone() const { return m_tone; }
    bool isAlarm() const { return m_alarm; }

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QLabel* m_head;
    QLabel* m_main;
    QLabel* m_sub;
    theme::Tone m_tone = theme::Tone::Grey;
    bool m_alarm = false;
};

class StatusStrip : public QWidget {
    Q_OBJECT
public:
    explicit StatusStrip(QWidget* parent = nullptr);
    using QWidget::update;
    void update(const StatusView& st);      // Python update_status
    const StatusView& status() const { return m_status; }
    Tile* instrumentTile() const { return m_conn; }
    Tile* laserTile() const { return m_laser; }
    Tile* compartmentTile() const { return m_comp; }
    Tile* channelTile() const { return m_chan; }
    QPushButton* stopButton() const { return m_stop; }

signals:
    void stopClicked();

private:
    Tile *m_conn, *m_laser, *m_comp, *m_chan;
    QPushButton* m_stop;
    StatusView m_status;
};

// Shows a long text (e.g. a path) shortened to fit the current width (at the front by default); the
// full text is the tooltip.
class ElidedLabel : public QLabel {
    Q_OBJECT
public:
    explicit ElidedLabel(const QString& text = {}, QWidget* parent = nullptr);
    void setFullText(const QString& text);
    QString fullText() const { return m_full; }
    void setElideMode(Qt::TextElideMode m);
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void refresh();
    QString m_full;
    Qt::TextElideMode m_mode = Qt::ElideLeft;
};

}  // namespace lr::gui
