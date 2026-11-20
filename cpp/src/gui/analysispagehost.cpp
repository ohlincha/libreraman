#include "analysispagehost.h"

#include <QVBoxLayout>

#include "analysispage.h"

namespace lr::gui {

AnalysisPageHost::AnalysisPageHost(const PageContext&, QWidget* parent)
    : Page(parent), m_page(new AnalysisPage(this)) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(m_page);
    connect(m_page, &AnalysisPage::statusMessage, this, &Page::logMessage);
}

void AnalysisPageHost::addSpectrum(const lr::Spectrum& sp) { m_page->addSpectrum(sp); }

}  // namespace lr::gui
