#pragma once
// The Analysis area (AnalysisPage, a self-contained widget) as a side-bar Page of the main window.
// Two tabs: chemometrics (AnalysisPage) and computed spectra (ComputedPanel). Measured spectra are added to
// the chemometrics data set (a running series can be analysed directly) and become the computed panel's
// experimental spectrum.
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

private:
    AnalysisPage* m_page;
    ComputedPanel* m_computed;
};

}  // namespace lr::gui
