#pragma once
// Computed Raman spectra (specification 5.6; engine: core/computed): a self-contained panel for the
// Analysis page.
//
// Load one or more quantum-chemistry output files (Gaussian, ORCA, NWChem, CP2K, Quantum ESPRESSO,
// vasp_raman.py, Phonopy-Spectroscopy, CASTEP; or every supported file under a folder, e.g. an ECCE
// project). Every frequency section is one structure in the list (ticked = drawn). For the current
// structure: frequency scaling (single factor or two regions, manual or fitted to the experimental
// spectrum), stick spectrum, assignment table against the experimental peaks; for all: laser wavelength
// and temperature (Raman activities → cross-sections), broadening (Lorentzian/Gaussian/pseudo-Voigt,
// FWHM), comparison with the experimental spectrum (correlation, each with its fitted scaling).
// Plot layouts: stacked (experimental above the computed spectra), overlay, mirror (computed downwards).
// Export: broadened spectrum (CSV, JCAMP-DX), assignment table (CSV), plot (PNG, SVG).
#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

#include "core/computed.h"
#include "core/fileio.h"
#include "core/peaks.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTableWidget;

namespace lr::gui {

class PlotWidget;

class ComputedPanel : public QWidget {
    Q_OBJECT
public:
    enum class Layout { Stacked = 0, Overlay = 1, Mirror = 2 };

    explicit ComputedPanel(QWidget* parent = nullptr);
    ~ComputedPanel() override;

    // ---- structures
    // Adds every frequency section of the file; on failure returns false (message box unless messages
    // are disabled, and the message() signal) and sets *error.
    bool loadFile(const QString& path, QString* error = nullptr);
    QStringList loadFiles(const QStringList& paths);        // returns the errors
    int loadFolder(const QString& dir);                     // ECCE/project folder (recursive); number added
    void addCalculation(const computed::Calculation& c, bool checked = true);
    int count() const { return int(m_entries.size()); }
    const computed::Calculation& calculation(int row) const { return m_entries.at(std::size_t(row)).calc; }
    int currentRow() const;
    void setCurrentRow(int row);
    bool isChecked(int row) const;
    void setChecked(int row, bool on);
    void removeRow(int row);
    void clear();

    // ---- experimental spectrum
    void setExperimental(const fileio::DataSpectrum& sp);
    void clearExperimental();
    bool hasExperimental() const { return !m_exp.x.empty(); }
    const std::vector<peaks::Peak>& experimentalPeaks() const { return m_expPeaks; }
    void setBaselineCorrection(bool on);                    // of the experimental spectrum (default on)

    // ---- settings
    computed::Options options() const;                      // with the scaling of the current structure
    void setLaser(double nm);
    void setTemperature(double kelvin);
    void setQuantity(computed::Quantity q);
    computed::Scaling scaling(int row) const;
    void setScaling(int row, const computed::Scaling& s);
    computed::Broadening broadening() const;
    void setBroadening(const computed::Broadening& b);
    // Two-region scaling for fits and comparisons (the current structure's factors are kept until refitted).
    void setTwoRegion(bool on, double split = 2000.0);
    bool twoRegion() const;
    void setPlotLayout(Layout l);
    Layout plotLayout() const;
    void setSticksVisible(bool on);

    // ---- analysis
    // Fit the scaling of the current structure to the experimental peaks (two regions if ticked).
    bool fitScaling(QString* error = nullptr);
    // Correlation of every structure with the experimental spectrum, each with its fitted scaling (single
    // or two regions as set; fitted factors are applied). Best first; also shown in the list.
    std::vector<computed::StructureScore> compareStructures(QString* error = nullptr);
    std::vector<computed::AssignmentRow> assignment() const;   // current structure vs experimental peaks
    Vec grid() const;                                       // experimental x range (1 cm⁻¹) or bands ± 150

    // ---- export
    bool exportSpectrum(const QString& path, QString* error = nullptr);    // .jdx/.dx: JCAMP-DX, else CSV
    bool exportAssignment(const QString& path, QString* error = nullptr);  // CSV
    bool exportPlot(const QString& path);                                  // .png or .svg

    // ---- widgets (tests, preview)
    PlotWidget* plot() const { return m_plot; }
    QTableWidget* assignmentTable() const { return m_table; }
    QListWidget* list() const { return m_list; }
    QString infoText() const;
    void setMessagesEnabled(bool on) { m_messages = on; }

public slots:
    void openFilesDialog();
    void openFolderDialog();
    void exportDialog(const QString& kind);                 // "csv", "jdx", "table", "png", "svg"
    void redraw();

signals:
    void calculationsChanged();
    void message(const QString& title, const QString& text);

private:
    struct Entry {
        computed::Calculation calc;
        computed::Scaling scaling;
        double score = computed::kNaN;                      // last comparison
    };
    void build();
    void onCurrentChanged();
    void onScalingEdited();
    void updateItemText(int row);
    void updateTable();
    void updateInfo();
    void processExperimental();
    void warn(const QString& title, const QString& text);

    std::vector<Entry> m_entries;
    fileio::DataSpectrum m_exp;                             // as given
    Vec m_expX, m_expY;                                     // processed (baseline, max = 1)
    std::vector<peaks::Peak> m_expPeaks;
    bool m_expBaseline = true;
    bool m_messages = true;
    bool m_collecting = false;                              // loadFiles(): errors reported together
    bool m_updating = false;
    QString m_fitNote;

    QListWidget* m_list = nullptr;
    QLabel* m_info = nullptr;
    QDoubleSpinBox* m_laser = nullptr;
    QDoubleSpinBox* m_temp = nullptr;
    QComboBox* m_quantity = nullptr;
    QDoubleSpinBox* m_factor = nullptr;
    QCheckBox* m_two = nullptr;
    QDoubleSpinBox* m_factorHigh = nullptr;
    QDoubleSpinBox* m_split = nullptr;
    QComboBox* m_matching = nullptr;
    QDoubleSpinBox* m_tolerance = nullptr;
    QLabel* m_fitLabel = nullptr;
    QComboBox* m_shape = nullptr;
    QDoubleSpinBox* m_fwhm = nullptr;
    QDoubleSpinBox* m_eta = nullptr;
    QComboBox* m_layout = nullptr;
    QCheckBox* m_sticks = nullptr;
    QCheckBox* m_baseline = nullptr;
    PlotWidget* m_plot = nullptr;
    QTableWidget* m_table = nullptr;
    QLabel* m_tableNote = nullptr;
};

}  // namespace lr::gui
