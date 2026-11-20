#pragma once
// The Data area (DataPage, a self-contained widget) as a side-bar Page of the main window. Measured spectra
// reach the DataPage in batches (SpectrumBatcher: at most one redraw per second while visible, none while
// hidden), so a fast series does not redraw all ticked spectra for every new one.
#include "page.h"

namespace lr::gui {

class DataPage;

class DataPageHost : public Page {
public:
    explicit DataPageHost(const PageContext& ctx, QWidget* parent = nullptr);
    void addSpectrum(const lr::Spectrum& sp) override;
    DataPage* dataPage() const { return m_page; }
    SpectrumBatcher* batcher() const { return m_batcher; }
    static constexpr int kVisibleMs = 1000;

private:
    DataPage* m_page;
    SpectrumBatcher* m_batcher;
};

}  // namespace lr::gui
