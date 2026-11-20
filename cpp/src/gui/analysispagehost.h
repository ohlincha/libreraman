#pragma once
// The Analysis area (AnalysisPage, a self-contained widget) as a side-bar Page of the main window.
// Two tabs: chemometrics (AnalysisPage) and computed spectra (ComputedPanel). Measured spectra are added to
// the chemometrics data set (a running series can be analysed directly) and become the computed panel's
// experimental spectrum.
//
// Measured spectra arrive in batches (SpectrumBatcher): while the area is visible at most every kVisibleMs
// (each batch rebuilds the data matrix and re-runs the visible method), while it is hidden not at all until
// it is shown; the computed panel gets only the newest spectrum of a batch.
#include "page.h"

namespace lr::gui {

class AnalysisPage;
class ComputedPanel;

class AnalysisPageHost : public Page {
public:
    explicit AnalysisPageHost(const PageContext& ctx, QWidget* parent = nullptr);
    void addSpectrum(const lr::Spectrum& sp) override;
    AnalysisPage* analysisPage() const { return m_page; }
    ComputedPanel* computedPanel() const { return m_computed; }
    SpectrumBatcher* batcher() const { return m_batcher; }
    static constexpr int kVisibleMs = 2000;

private:
    AnalysisPage* m_page;
    ComputedPanel* m_computed;
    SpectrumBatcher* m_batcher;
};

}  // namespace lr::gui
