#include "datapagehost.h"

#include <QMessageBox>
#include <QVBoxLayout>

#include "datapage.h"

namespace lr::gui {

DataPageHost::DataPageHost(const PageContext& ctx, QWidget* parent) : Page(parent), m_page(new DataPage(this)) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(m_page);
    if (ctx.processing) m_page->setProcessingSettings(ctx.processing);
    // The page reports problems through message(); route them like the rest of the GUI (dialogs off in tests).
    m_page->setMessagesEnabled(false);
    QWidget* window = ctx.window;
    connect(m_page, &DataPage::message, this, [this, window](const QString& title, const QString& text) {
        emit logMessage(title + ": " + text);
        if (dialogsEnabled()) QMessageBox::warning(window ? window : this, title, text);
    });
}

void DataPageHost::addSpectrum(const lr::Spectrum& sp) { m_page->addSpectrum(sp); }

}  // namespace lr::gui
