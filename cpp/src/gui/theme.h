#pragma once
// Central colours, fonts and style sheets of the GUI (Python: constants and inline style sheets in
// gui/app.py). Calm palette: dark slate status tiles with an accent bar; only alarms (LASER ON,
// connection lost) fill a tile red. Teal MEASURE, indigo START SERIES, red STOP.
#include <QColor>
#include <QFont>
#include <QString>

class QApplication;

namespace lr::gui {

struct PlotPalette;

namespace theme {

enum class Mode { Auto, Light, Dark };          // Auto follows the desktop colour scheme
enum class Tone { Grey, Green, Yellow, Red, Orange, Blue };   // status tile accents (Python COLORS)
enum class Action { Measure, Series, Danger };  // big action buttons (Python ACCENT)

// Sets style/palette (Fusion + dark palette in dark mode), the base style sheet and the default plot
// palette. In Auto mode it re-applies itself when the desktop switches between light and dark.
void apply(QApplication& app, Mode mode = Mode::Auto);
Mode mode();                    // as requested by apply()
bool isDark();                  // effective scheme

QColor toneColor(Tone t);
QColor tileBackground();        // #27303b (same in light and dark)
QColor tileText();              // tile titles/subtitles
QColor actionColor(Action a);
QColor actionPressedColor(Action a);
QColor disabledButton();        // background of disabled big buttons
QColor mutedText();             // secondary labels (#666 light)
QColor warningText();           // "interval too short", "SATURATED" (#d43c3c)
QColor warningBackground();     // input field with an invalid value (#fbe3e3 light)

QFont tileValueFont();          // 13 pt bold
QFont tileCaptionFont();        // 9 pt

QString bigButtonStyle(Action a);   // ▶ MEASURE / ⟳ START SERIES (15 pt bold, rounded)
QString stopButtonStyle();          // ■ STOP in the status strip (14 pt bold, red)
QString groupBoxStyle();            // Python box(): bold title
QString sideBarButtonStyle();       // area buttons at the left (checked: accent bar)
QString baseStyleSheet();           // applied to the application (tooltips wrap etc.)

PlotPalette plotPalette();          // light or dark, matching the current scheme

}  // namespace theme
}  // namespace lr::gui
