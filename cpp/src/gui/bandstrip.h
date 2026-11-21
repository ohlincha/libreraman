#pragma once
// Band-assignment strip: typical ranges of functional groups / characteristic bands drawn as labelled bars
// at the top of the plot area (sharp bands as a faint line through the plot). Organic and inorganic
// categories can be shown separately or together.
#include "../core/bandassign.h"
#include "plotwidget.h"

#include <vector>

namespace lr::gui {

class BandStripItem : public PlotItem {
    Q_OBJECT
public:
    explicit BandStripItem(PlotWidget* plot);
    void setCategories(bool organic, bool inorganic);
    bool organic() const { return m_organic; }
    bool inorganic() const { return m_inorganic; }
    // Description of the band(s) under a widget position (empty: none), for tooltips.
    QString toolTipAt(const QPointF& widgetPos) const;

protected:
    void paint(QPainter& p, const PlotTransform& t) override;

private:
    bool m_organic = true, m_inorganic = false;
    struct Hit {
        QRectF rect;
        const bands::Band* band;
    };
    mutable std::vector<Hit> m_hits;   // drawn bars (painter coordinates = widget coordinates)
};

}  // namespace lr::gui
