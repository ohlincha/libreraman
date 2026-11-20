#include "theme.h"

#include "plotwidget.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace lr::gui::theme {

namespace {

Mode g_mode = Mode::Auto;
bool g_dark = false;
bool g_connected = false;
QString g_nativeStyle;

bool systemDark() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme cs = QGuiApplication::styleHints()->colorScheme();
    if (cs != Qt::ColorScheme::Unknown)
        return cs == Qt::ColorScheme::Dark;
#endif
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

QPalette darkPalette() {
    QPalette p;
    const QColor window("#1c222c"), base("#151a22"), text("#e3e7ee"), button("#262e3a"), mid("#3a4352");
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor("#1d242f"));
    p.setColor(QPalette::ToolTipBase, QColor("#2a3240"));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor("#7d8798"));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor("#ff6b5e"));
    p.setColor(QPalette::Light, QColor("#3d4757"));
    p.setColor(QPalette::Midlight, QColor("#323a48"));
    p.setColor(QPalette::Mid, mid);
    p.setColor(QPalette::Dark, QColor("#10141a"));
    p.setColor(QPalette::Shadow, QColor("#05070a"));
    p.setColor(QPalette::Highlight, QColor("#2f8f84"));
    p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    p.setColor(QPalette::Link, QColor("#6fb6e8"));
    p.setColor(QPalette::LinkVisited, QColor("#a58bd6"));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, QColor("#6b7484"));
    p.setColor(QPalette::Disabled, QPalette::Button, QColor("#20262f"));
    return p;
}

QPalette lightPalette() {
    QPalette p;
    p.setColor(QPalette::Window, QColor("#eff1f4"));
    p.setColor(QPalette::WindowText, QColor("#1b1f24"));
    p.setColor(QPalette::Base, QColor("#ffffff"));
    p.setColor(QPalette::AlternateBase, QColor("#f5f6f8"));
    p.setColor(QPalette::ToolTipBase, QColor("#ffffff"));
    p.setColor(QPalette::ToolTipText, QColor("#1b1f24"));
    p.setColor(QPalette::Text, QColor("#1b1f24"));
    p.setColor(QPalette::Button, QColor("#f7f8fa"));
    p.setColor(QPalette::ButtonText, QColor("#1b1f24"));
    p.setColor(QPalette::Highlight, QColor("#1f6f68"));
    p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    return p;
}

void applyNow(QApplication& app) {
    g_dark = g_mode == Mode::Dark || (g_mode == Mode::Auto && systemDark());
    PlotPalette::setDefaultPalette(g_dark ? PlotPalette::dark() : PlotPalette::light());
    if (g_dark) {
        app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        app.setPalette(darkPalette());
    } else if (g_mode == Mode::Light) {
        app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        app.setPalette(lightPalette());
    } else {                                        // light desktop: native look, as the Python app
        if (!g_nativeStyle.isEmpty() && app.style()->name() != g_nativeStyle)
            app.setStyle(QStyleFactory::create(g_nativeStyle));
        app.setPalette(app.style()->standardPalette());
    }
    app.setStyleSheet(baseStyleSheet());
}

}  // namespace

void apply(QApplication& app, Mode m) {
    if (g_nativeStyle.isEmpty() && app.style())
        g_nativeStyle = app.style()->name();
    g_mode = m;
    applyNow(app);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (!g_connected) {
        g_connected = true;
        QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &app, [&app] {
            if (g_mode == Mode::Auto)
                applyNow(app);
        });
    }
#endif
}

Mode mode() { return g_mode; }
bool isDark() { return g_dark; }

QColor toneColor(Tone t) {
    switch (t) {
    case Tone::Grey: return QColor("#7b8594");
    case Tone::Green: return QColor("#2f9e8f");
    case Tone::Yellow: return QColor("#c9922b");
    case Tone::Red: return QColor("#c0392b");
    case Tone::Orange: return QColor("#d4892b");
    case Tone::Blue: return QColor("#7f93b8");
    }
    return QColor("#7b8594");
}

QColor tileBackground() { return QColor("#27303b"); }
QColor tileText() { return QColor(255, 255, 255, 217); }

QColor actionColor(Action a) {
    switch (a) {
    case Action::Measure: return QColor("#1f6f68");
    case Action::Series: return QColor("#34477a");
    case Action::Danger: return QColor("#b42318");
    }
    return QColor("#1f6f68");
}

QColor actionPressedColor(Action a) {
    switch (a) {
    case Action::Measure: return QColor("#15524d");
    case Action::Series: return QColor("#25335a");
    case Action::Danger: return QColor("#7f1a15");
    }
    return QColor("#15524d");
}

QColor disabledButton() { return g_dark ? QColor("#3a4250") : QColor("#b9bfca"); }
QColor mutedText() { return g_dark ? QColor("#8f99aa") : QColor("#666666"); }
QColor warningText() { return g_dark ? QColor("#ff7a6e") : QColor("#d43c3c"); }
QColor warningBackground() { return g_dark ? QColor("#4a2326") : QColor("#fbe3e3"); }

QFont tileValueFont() {
    QFont f = QApplication::font();
    f.setPointSize(13);
    f.setBold(true);
    return f;
}

QFont tileCaptionFont() {
    QFont f = QApplication::font();
    f.setPointSize(9);
    return f;
}

QString bigButtonStyle(Action a) {
    return QStringLiteral("QPushButton { background: %1; color: white; font-size: 15pt; font-weight: bold;"
                          " border: none; border-radius: 8px; padding: 6px; }"
                          " QPushButton:hover { background: %3; }"
                          " QPushButton:disabled { background: %4; color: %5; }"
                          " QPushButton:pressed { background: %2; }")
        .arg(actionColor(a).name(), actionPressedColor(a).name(), actionColor(a).lighter(112).name(),
             disabledButton().name(), g_dark ? QStringLiteral("#7d8798") : QStringLiteral("#eef0f4"));
}

QString stopButtonStyle() {
    return QStringLiteral("QPushButton { background: #b42318; color: white; font-weight: bold; font-size: 14pt;"
                          " border: none; border-radius: 6px; padding: 4px 14px; }"
                          " QPushButton:hover { background: #c62a1d; }"
                          " QPushButton:pressed { background: #7f1a15; }");
}

QString groupBoxStyle() {
    return QStringLiteral("QGroupBox { font-weight: bold; margin-top: 10px; }"
                          " QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 3px; }");
}

QString sideBarButtonStyle() {
    const QString text = g_dark ? QStringLiteral("#c3cad6") : QStringLiteral("#3a3f47");
    const QString textOn = g_dark ? QStringLiteral("#ffffff") : QStringLiteral("#1b1f24");
    const QString hover = g_dark ? QStringLiteral("rgba(255,255,255,0.06)") : QStringLiteral("rgba(0,0,0,0.05)");
    const QString on = g_dark ? QStringLiteral("rgba(47,158,143,0.18)") : QStringLiteral("rgba(31,111,104,0.10)");
    return QStringLiteral("QToolButton { border: none; border-left: 4px solid transparent; padding: 8px 6px;"
                          " text-align: left; color: %1; font-size: 11pt; }"
                          " QToolButton:hover { background: %2; }"
                          " QToolButton:checked { border-left: 4px solid %3; background: %4;"
                          " font-weight: bold; color: %5; }")
        .arg(text, hover, g_dark ? QStringLiteral("#2f9e8f") : actionColor(Action::Measure).name(), on, textOn);
}

QString baseStyleSheet() {
    return QStringLiteral("QToolTip { max-width: 340px; padding: 4px; }");     // long tooltips wrap
}

PlotPalette plotPalette() { return g_dark ? PlotPalette::dark() : PlotPalette::light(); }

}  // namespace lr::gui::theme
