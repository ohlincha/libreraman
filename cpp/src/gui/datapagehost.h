#pragma once
// The Data area (DataPage, a self-contained widget) as a side-bar Page of the main window.
#include "page.h"

namespace lr::gui {

class DataPage;

class DataPageHost : public Page {
public:
    explicit DataPageHost(const PageContext& ctx, QWidget* parent = nullptr);
    void addSpectrum(const lr::Spectrum& sp) override;
    DataPage* dataPage() const { return m_page; }

private:
    DataPage* m_page;
};

}  // namespace lr::gui
