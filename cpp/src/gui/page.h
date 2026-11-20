#pragma once
// The main areas of the window (side bar: Measure, Data, Library, Analysis, Calibration) are Pages.
//
// Plugging in a page: implement `class XyzPage : public lr::gui::Page`, then add one line to
// builtinPageFactories() in pages.cpp:
//     {QStringLiteral("Data"), [](const PageContext& c) { return new DataPage(c); }},
// (or call registerPage("Data", factory) before the MainWindow is built, e.g. in a test). Areas without a
// factory show a PlaceholderPage (Python gui/app.py Placeholder).
//
// The MainWindow passes every spectrum the controller delivers (single measurements and series) to
// addSpectrum() of every page (Python MainWindow.add_to_data for the Data page; the Measure page draws it).
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>

#include "core/processing.h"
#include "core/spectrum.h"

namespace lr::gui {

class Controller;

struct PageContext {
    Controller* controller = nullptr;
    // Current processing settings of the Measure page (Python: DataPage(..., self.measure.proc_settings)).
    std::function<lr::processing::Settings()> processing;
    QWidget* window = nullptr;                 // the main window (parent for dialogs)
};

class Page : public QWidget {
    Q_OBJECT
public:
    explicit Page(QWidget* parent = nullptr) : QWidget(parent) {}
    virtual void setController(Controller* c) { m_controller = c; }
    Controller* controller() const { return m_controller; }
    // A spectrum from the instrument (Python add_to_data); default: ignored.
    virtual void addSpectrum(const lr::Spectrum& /*sp*/) {}
    // The page became the visible area.
    virtual void activated() {}

signals:
    void logMessage(const QString& msg);       // shown in the log view at the bottom of the window

private:
    Controller* m_controller = nullptr;
};

// Placeholder for an area that is not implemented yet.
class PlaceholderPage : public Page {
    Q_OBJECT
public:
    PlaceholderPage(const QString& title, const QString& text, QWidget* parent = nullptr);
    QString title() const { return m_title; }

private:
    QString m_title;
};

using PageFactory = std::function<Page*(const PageContext&)>;

// Side bar areas in order (the Measure page is built by the MainWindow itself).
const QStringList& pageAreas();
// Replaces the factory of an area (call before the MainWindow is constructed).
void registerPage(const QString& area, PageFactory factory);
// Registered or built-in factory; PlaceholderPage if there is none.
Page* createPage(const QString& area, const PageContext& ctx);

// Modal questions and warnings of the GUI (MainWindow, pages). Off in tests and the self-test: questions
// then take their default answer and warnings only go to the log.
bool dialogsEnabled();
void setDialogsEnabled(bool on);

// Name of a measured spectrum in the Data list (Python add_to_data): "<sample> HH:MM:SS [#index]".
QString spectrumDisplayName(const lr::Spectrum& sp);

}  // namespace lr::gui
