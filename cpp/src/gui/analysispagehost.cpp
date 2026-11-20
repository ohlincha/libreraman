#include "analysispagehost.h"

#include <QTabWidget>
#include <QVBoxLayout>

#include "analysispage.h"
#include "computedpanel.h"
#include "core/fileio.h"

namespace lr::gui {

AnalysisPageHost::AnalysisPageHost(const PageContext&, QWidget* parent)
    : Page(parent), m_page(new AnalysisPage), m_computed(new ComputedPanel) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    auto* tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);
    tabs->addTab(m_page, tr("Chemometrics"));
    tabs->addTab(m_computed, tr("Computed spectra"));
    v->addWidget(tabs);
    connect(m_page, &AnalysisPage::statusMessage, this, &Page::logMessage);
    m_batcher = new SpectrumBatcher(this, kVisibleMs, [this](std::vector<lr::Spectrum>&& b) {
        m_page->addSpectra(b);
        m_computed->setExperimental(fileio::fromSpectrum(b.back(), spectrumDisplayName(b.back())));
    });
    m_batcher->setExtraCost([this] { return m_page->lastRunMs(); });   // the method re-run after each batch
}

void AnalysisPageHost::addSpectrum(const lr::Spectrum& sp) { m_batcher->add(sp); }

}  // namespace lr::gui
