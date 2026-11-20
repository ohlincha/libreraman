#include "librarypagehost.h"

#include <QVBoxLayout>

#include "librarypage.h"

namespace lr::gui {

LibraryPageHost::LibraryPageHost(const PageContext&, QWidget* parent) : Page(parent), m_page(new LibraryPage(this)) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(m_page);
    connect(m_page, &LibraryPage::message, this, &Page::logMessage);
}

void LibraryPageHost::addSpectrum(const lr::Spectrum& sp) {
    m_page->setQuery(sp.shift, sp.intensity, spectrumDisplayName(sp));
}

void LibraryPageHost::activated() {
    if (!m_opened && m_page->libraryPath().isEmpty()) {     // open the default library on first use
        m_page->setLibraryPath(LibraryPage::defaultLibraryPath());
        m_opened = true;
    }
}

}  // namespace lr::gui
