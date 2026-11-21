#pragma once
// Library area (SPECIFICATION 5.5): browse/search/filter the entries of a library file, details and plot of
// an entry (source and licence always shown), add/edit/delete, import (spectrum files, RRUFF folder, band
// lists), open/create library files, and matching (full-spectrum HQI, peaks, NNLS mixtures) with an overlay
// of query and hit. Self-contained: the main window calls setLibraryPath() at start-up, addSpectrum() for
// "Add to library" and match() for "Match last spectrum".
#include <QWidget>
#include <memory>
#include <vector>

#include "core/library.h"
#include "core/match.h"
#include "core/spectrum.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QSplitter;
class QTabWidget;
class QTableWidget;
class QTimer;

namespace lr::gui {

class PlotWidget;

class LibraryPage : public QWidget {
    Q_OBJECT
public:
    enum class Method { Spectrum, Peaks, Mixture };

    explicit LibraryPage(QWidget* parent = nullptr);
    ~LibraryPage() override;

    // <AppDataLocation>/library.sqlite
    static QString defaultLibraryPath();
    // Opens (creating it if needed, unless readOnly) the library file. On failure the error is shown in the
    // page header, emitted as message() and false is returned (no dialog).
    bool setLibraryPath(const QString& path, bool readOnly = false);
    QString libraryPath() const;
    library::Library& library() { return *m_lib; }

    // "Add to library": asks for name, formula, CAS, notes ... (ask = true; dialog prefilled from the spectrum's
    // metadata) and stores the spectrum. Opens the default library if none is open. Returns the new id, or 0
    // if cancelled/failed.
    qint64 addSpectrum(const Spectrum& sp, bool ask = true);
    // "Match last spectrum": sets the query, runs the current method, shows the Match tab. Returns the hits
    // (empty for the mixture method; see mixtures()).
    std::vector<match::Hit> match(const Spectrum& sp);
    void setQuery(const Vec& x, const Vec& y, const QString& name);
    std::vector<match::Hit> runMatch();

    void setMethod(Method m);
    Method method() const;
    const std::vector<match::Hit>& hits() const { return m_hits; }
    const std::vector<match::Mixture>& mixtures() const { return m_mixtures; }

    // Browsing (also for tests)
    void setSearchText(const QString& text);
    int visibleEntries() const;
    void selectEntry(qint64 id);
    qint64 selectedEntry() const;
    void selectHit(int row);
    void showMatchTab();
    void showDetailsTab();
    PlotWidget* plot() const { return m_plot; }
    void refresh();                                  // re-reads the library (after external changes)

signals:
    void libraryChanged(const QString& path);
    void matched(const QString& bestName, double score);   // after match()/runMatch(), best hit (empty: none)
    void message(const QString& text);                     // for the status bar / log

private:
    void buildUi();
    QWidget* buildHeader();
    QWidget* buildBrowser();
    QWidget* buildDetails();
    QWidget* buildMatchPanel();
    void updateHeader(const QString& error = {});
    void updateFilters();
    void fillTable();
    library::Filter currentFilter(bool withText) const;
    void onSelectionChanged();
    void showEntry(qint64 id);
    void showNothing();
    void showHitOverlay(int row);
    void plotEntry(const library::Entry& e);
    void fillResults();
    bool ensureLibrary();
    bool requireWritable();
    void libraryModified();
    void ensureMatcher();
    match::Preprocess currentPreprocess() const;

    // actions
    void newLibrary();
    void openLibrary(bool readOnly);
    void addDemo();
    void importFiles();
    void importRruff();
    void importBandList();
    void exportToFiles();
    void exportToLibrary();
    void addEntry();
    void editEntry();
    void deleteEntries();
    void loadQueryFile();
    std::vector<qint64> selectedIds() const;
    void report(const QString& title, const QString& text, const QStringList& errors = {});

    std::unique_ptr<library::Library> m_lib;
    match::Matcher m_matcher;
    bool m_matcherDirty = true;
    std::vector<match::Hit> m_hits;
    std::vector<match::Mixture> m_mixtures;
    Method m_hitsMethod = Method::Spectrum;
    Vec m_qx, m_qy;
    QString m_qname;

    // header
    QLabel* m_libName = nullptr;
    QLabel* m_libInfo = nullptr;
    QPushButton* m_libMenu = nullptr;
    QPushButton* m_importMenu = nullptr;
    QPushButton* m_exportMenu = nullptr;
    // browser
    QLineEdit* m_search = nullptr;
    QComboBox* m_class = nullptr;
    QComboBox* m_source = nullptr;
    QComboBox* m_laser = nullptr;
    QTableWidget* m_table = nullptr;
    QPushButton* m_add = nullptr;
    QPushButton* m_addFile = nullptr;       // "From file…" (Import ▸ Spectrum files)
    QPushButton* m_edit = nullptr;
    QPushButton* m_delete = nullptr;
    QLabel* m_count = nullptr;
    QTimer* m_searchTimer = nullptr;
    bool m_columnsSized = false;
    // right side
    PlotWidget* m_plot = nullptr;
    QLabel* m_plotTitle = nullptr;
    QTabWidget* m_tabs = nullptr;
    QLabel* m_details = nullptr;
    // match panel
    QLabel* m_queryLabel = nullptr;
    QComboBox* m_method = nullptr;
    QComboBox* m_baseline = nullptr;
    QComboBox* m_deriv = nullptr;
    QDoubleSpinBox* m_tol = nullptr;
    QSpinBox* m_topN = nullptr;
    QCheckBox* m_useFilters = nullptr;
    QPushButton* m_matchButton = nullptr;
    QTableWidget* m_results = nullptr;
    QLabel* m_resultInfo = nullptr;
};

}  // namespace lr::gui
