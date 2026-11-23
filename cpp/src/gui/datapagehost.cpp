#include "datapagehost.h"

#include <QMessageBox>
#include <QMetaMethod>
#include <QVBoxLayout>

#include "datapage.h"

namespace lr::gui {

DataPageHost::DataPageHost(const PageContext& ctx, QWidget* parent) : Page(parent), m_page(new DataPage(this)) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(m_page);
    if (ctx.processing) m_page->setProcessingSettings(ctx.processing);
    m_batcher = new SpectrumBatcher(this, kVisibleMs, [this](std::vector<lr::Spectrum>&& b) { m_page->addSpectra(b); });
    // The page reports problems through message(); route them like the rest of the GUI (dialogs off in tests).
    m_page->setMessagesEnabled(false);
    QWidget* window = ctx.window;
    connect(m_page, &DataPage::message, this, [this, window](const QString& title, const QString& text) {
        emit logMessage(title + ": " + text);
        if (dialogsEnabled()) QMessageBox::warning(window ? window : this, title, text);
    });
    connect(m_page, &DataPage::addToLibraryRequested, this, [this, window](const fileio::DataSpectrum& d) {
        QString err;
        if (!routeToLibrary(window ? window : this->window(), toSpectrum(d), &err)) {
            emit logMessage(tr("Add to library: %1").arg(err));
            m_page->showNote(tr("“%1” not added to the library: %2").arg(d.name, err));
        }
    });
    connect(m_page, &DataPage::matchRequested, this, [this, window](const fileio::DataSpectrum& d) {
        QString err;
        if (!routeToLibrary(window ? window : this->window(), toSpectrum(d), &err, "matchInLibrary")) {
            emit logMessage(tr("Match: %1").arg(err));
            m_page->showNote(tr("“%1” not matched: %2").arg(d.name, err));
        }
    });
}

lr::Spectrum DataPageHost::toSpectrum(const fileio::DataSpectrum& d) {
    lr::Spectrum sp;
    sp.shift = d.x;
    sp.intensity = d.y;
    if (d.signal.size() == d.x.size()) sp.signal = d.signal;
    if (d.background.size() == d.x.size()) sp.background = d.background;
    sp.meta = d.meta;
    if (sp.metaValue(QStringLiteral("sample")).isEmpty()) sp.setMeta(QStringLiteral("sample"), d.name);
    const double laser = d.metaValue(QStringLiteral("laser_nm")).toDouble();
    if (laser > 900) sp.channel = 1064;
    bool ok = false;
    const double exposure = d.metaValue(QStringLiteral("exposure_s")).toDouble(&ok);
    if (ok && exposure > 0) sp.exposure_s = exposure;
    const int averages = d.metaValue(QStringLiteral("averages")).toInt(&ok);
    if (ok && averages > 0) sp.averages = averages;
    const QDateTime t = QDateTime::fromString(d.metaValue(QStringLiteral("time")), Qt::ISODate);
    if (t.isValid()) sp.t = t;
    return sp;
}

bool DataPageHost::routeToLibrary(QObject* window, const lr::Spectrum& sp, QString* error, const char* method) {
    // The main window's `bool addToLibrary(const lr::Spectrum&)` (a slot or Q_INVOKABLE) passes it to the
    // Library page. It is looked up by name so that the Data page needs no reference to the window or the
    // Library page.
    if (!window) {
        if (error) *error = tr("no main window");
        return false;
    }
    const QMetaObject* mo = window->metaObject();
    for (int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        if (m.name() != method || m.parameterCount() != 1) continue;
        bool added = false;
        const bool invoked = m.returnType() == QMetaType::Bool
                                 ? m.invoke(window, Qt::DirectConnection, Q_RETURN_ARG(bool, added),
                                            Q_ARG(lr::Spectrum, sp))
                                 : (added = m.invoke(window, Qt::DirectConnection, Q_ARG(lr::Spectrum, sp)));
        if (!invoked) {
            if (error) *error = tr("the main window could not take the spectrum");
            return false;
        }
        if (!added && error)
            *error = qstrcmp(method, "addToLibrary") == 0 ? tr("not added (no writable library, or cancelled)")
                                                          : tr("no library to match against");
        return added;
    }
    if (error) *error = tr("the Library page is not available");
    return false;
}

void DataPageHost::addSpectrum(const lr::Spectrum& sp) { m_batcher->add(sp); }

}  // namespace lr::gui
