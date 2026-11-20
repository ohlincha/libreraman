#pragma once
// The Analysis area (AnalysisPage, a self-contained widget) as a side-bar Page of the main window.
// Measured spectra are added to its data set, so a running series can be analysed directly.
#include "page.h"

namespace lr::gui {

class AnalysisPage;

class AnalysisPageHost : public Page {
public:
    explicit AnalysisPageHost(const PageContext& ctx, QWidget* parent = nullptr);
    void addSpectrum(const lr::Spectrum& sp) override;
    AnalysisPage* analysisPage() const { return m_page; }

private:
    AnalysisPage* m_page;
};

}  // namespace lr::gui
