#pragma once
// The Data area (DataPage, a self-contained widget) as a side-bar Page of the main window. Measured spectra
// reach the DataPage in batches (SpectrumBatcher: at most one redraw per second while visible, none while
// hidden), so a fast series does not redraw all ticked spectra for every new one.
//
// "Add to library…" on the Data page (DataPage::addToLibraryRequested) is routed to the main window: its
// method `bool addToLibrary(const lr::Spectrum&)` (slot or Q_INVOKABLE, which hands it to the Library page)
// is invoked through the meta-object system, so neither page depends on the other or on the window class.
// If there is no such method or it returns false, the reason goes to the log (logMessage).
#include "page.h"

#include "core/fileio.h"

namespace lr::gui {

class DataPage;

class DataPageHost : public Page {
    Q_OBJECT
public:
    explicit DataPageHost(const PageContext& ctx, QWidget* parent = nullptr);
    void addSpectrum(const lr::Spectrum& sp) override;
    DataPage* dataPage() const { return m_page; }
    SpectrumBatcher* batcher() const { return m_batcher; }
    static constexpr int kVisibleMs = 1000;
    // A Data list entry as an acquisition spectrum (shift, intensity, raw frames if present, metadata;
    // "sample" defaults to the entry name).
    static lr::Spectrum toSpectrum(const fileio::DataSpectrum& d);
    // Invokes window->addToLibrary(sp) by name; false with the reason if it is missing, fails or declines.
    static bool routeToLibrary(QObject* window, const lr::Spectrum& sp, QString* error = nullptr,
                               const char* method = "addToLibrary");   // or "matchInLibrary" (Match)

private:
    DataPage* m_page;
    SpectrumBatcher* m_batcher;
};

}  // namespace lr::gui
