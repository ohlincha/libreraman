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
}

void AnalysisPageHost::addSpectrum(const lr::Spectrum& sp) {
    m_page->addSpectrum(sp);
    m_computed->setExperimental(fileio::fromSpectrum(sp, spectrumDisplayName(sp)));
}

}  // namespace lr::gui
