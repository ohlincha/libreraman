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
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <vector>

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

// Hands measured spectra to a page that is expensive to update in batches (fast series: the Data and Analysis
// areas would otherwise redo their work for every spectrum). While the page is visible a batch is delivered at
// most every `visibleMs` (the first spectrum after a quiet period on the next event-loop pass), and never more
// often than kDuty times the cost of the last update (delivery plus setExtraCost(), e.g. a recomputation the
// delivery triggers later), so a growing data set cannot occupy more than ~1/kDuty of the GUI thread; while
// the page is hidden the spectra wait and are delivered when it is shown.
class SpectrumBatcher : public QObject {
public:
    using Deliver = std::function<void(std::vector<lr::Spectrum>&&)>;
    SpectrumBatcher(QWidget* page, int visibleMs, Deliver deliver);
    void add(const lr::Spectrum& sp);
    void flush();                              // deliver what is pending now
    int pending() const { return int(m_pending.size()); }
    int deliveries() const { return m_deliveries; }
    void setExtraCost(std::function<qint64()> ms) { m_extraCost = std::move(ms); }
    static constexpr int kDuty = 5;

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    void schedule();
    QWidget* m_page;
    int m_visibleMs;
    Deliver m_deliver;
    std::vector<lr::Spectrum> m_pending;
    QTimer m_timer;
    QElapsedTimer m_since;                     // since the last delivery
    int m_deliveries = 0;
    qint64 m_lastCostMs = 0;
    std::function<qint64()> m_extraCost;
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
