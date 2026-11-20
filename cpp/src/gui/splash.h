#pragma once
// Splash screen: a bowl of ramen whose steam rises as a Raman spectrum (Python gui/splash.py), with a
// progress message and an optional progress bar. A click hides it (skipped()).
#include <QImage>
#include <QSplashScreen>
#include <QString>

namespace lr::gui {

constexpr int kLogoWidth = 560, kLogoHeight = 340;

// The splash artwork (logical size kLogoWidth × kLogoHeight, image size × dpr).
QImage drawLogo(qreal dpr = 1.0, const QString& version = QStringLiteral(LIBRERAMEN_VERSION));

class Splash : public QSplashScreen {
    Q_OBJECT
public:
    explicit Splash(const QString& version = QStringLiteral(LIBRERAMEN_VERSION));
    void message(const QString& text);      // bottom-left status line (Python Splash.message)
    void setProgress(double fraction);      // 0..1 shows the bar; < 0 hides it (default)
    double progress() const { return m_progress; }
    QString text() const { return m_text; }
    QImage renderImage() const;             // what is shown (for screenshots/tests)

signals:
    void skipped();                         // clicked away by the user

protected:
    void drawContents(QPainter* p) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    void paintOverlay(QPainter& p, const QSizeF& size) const;
    QString m_version;
    QString m_text;
    double m_progress = -1;
};

}  // namespace lr::gui
