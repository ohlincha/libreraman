#pragma once
// The Library area (LibraryPage, a self-contained widget) as a side-bar Page of the main window.
// Every measured spectrum becomes the page's match query ("match last spectrum"); nothing is added to the
// library automatically (that is the user's "Add to library").
#include "page.h"

namespace lr::gui {

class LibraryPage;

class LibraryPageHost : public Page {
public:
    explicit LibraryPageHost(const PageContext& ctx, QWidget* parent = nullptr);
    void addSpectrum(const lr::Spectrum& sp) override;
    void activated() override;
    LibraryPage* libraryPage() const { return m_page; }

private:
    LibraryPage* m_page;
    bool m_opened = false;
};

}  // namespace lr::gui
