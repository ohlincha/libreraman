#pragma once
// QPainter-based 2D plot widget (replaces pyqtgraph of the Python version; no Qt Charts/QCustomPlot/Qwt).
//
// A PlotWidget owns PlotItems (curves, error bars, text, peak labels, lines, regions, heat maps), draws axes with
// nice ticks, and implements the spectroscopy navigation of the Python SpectrumViewBox:
//   WheelMode::Spectrum (default, as Python): wheel = change the top of the y axis (the bottom/baseline
//     stays put); Ctrl+wheel or wheel over the x axis = zoom x around the cursor; wheel over the y axis
//     = y top as well.
//   WheelMode::ZoomX: wheel = zoom x around the cursor; Ctrl/Shift+wheel = change the top of the y axis.
//   Left drag = pan along x (and y if setPanY(true)); Shift+drag = rubber-band zoom; double-click =
//   full view (home x range or data range, automatic y); horizontal scrolling pans x.
// Automatic y scaling (autoY, default on) fits the y axis to the data visible in the current x range with
// headroom above the tallest point for peak labels (Python SpectrumViewBox.rescale_y).
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPen>
#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace lr {

using Vec = std::vector<double>;     // same alias as core/spectrum.h

namespace gui {

class PlotWidget;

// --------------------------------------------------------------------------------------------- palette
struct PlotPalette {
    QColor background;          // widget background around the plot area
    QColor plotArea;            // inside the axes
    QColor axis;                // axis lines and tick marks
    QColor text;                // tick labels and axis titles
    QColor grid;                // grid lines (includes alpha)
    QColor label;               // peak labels, text items
    QColor main;                // live/main spectrum
    QColor baseline;            // baseline estimate (dashed)
    QColor previous;            // faded previous spectra (alpha is set per curve)
    QColor reference;           // zero/reference lines, fit model
    QColor region;              // shaded regions (includes alpha)
    QColor accent;              // draggable lines, rubber band
    QColor legendBackground;    // legend and hover readout boxes (includes alpha)
    std::vector<QColor> series; // Data area overlay colours (Python data.COLORS)

    QColor seriesColor(int k) const;           // cycles through series
    bool isDark() const;

    static PlotPalette light();     // the Python app's plot: white background, navy (#1f4e79) spectra
    static PlotPalette dark();      // navy-slate background for a dark desktop
    static const PlotPalette& defaultPalette();          // used by new widgets (theme::apply sets it)
    static void setDefaultPalette(const PlotPalette& p);
};

// --------------------------------------------------------------------------------------------- helpers
struct Range {
    double lo = 0, hi = 1;
    double span() const { return hi - lo; }
    bool operator==(const Range& o) const { return lo == o.lo && hi == o.hi; }
};

struct Bounds {                 // running min/max; invalid until something was added
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    bool valid() const { return lo <= hi; }
    void add(double v);
    void add(const Bounds& b);
};

struct Ticks {
    std::vector<double> major;
    std::vector<double> minor;  // between the majors (not including them)
    double step = 0;            // major spacing
    int decimals = 0;           // decimals needed to label the majors
};

namespace plot {
double niceStep(double span, int maxTicks);                     // 1, 2, 2.5, 5 × 10^k >= span/maxTicks
Ticks niceTicks(double lo, double hi, int maxTicks);
QString formatTick(double v, const Ticks& t);                   // "1500", "0.25", "−5", "2×10⁶"
QString superscript(int n);                                     // -1 -> "⁻¹"
// Python rescale_y: y range [lo, hi + headroom*(hi-lo or 1)], then pyqtgraph padding on both sides.
Range autoYRange(double lo, double hi, double headroom, double padding = 0.02);
// Python peak-label staggering: a label closer than minGap to the previous one goes one level up
// (level resets to 0 otherwise). Positions are taken in the given order; callers sort them.
// The drawn height is level % 3.
std::vector<int> staggerLevels(const std::vector<double>& positions, double minGap);
// Per-pixel-column decimation of the samples i0..i1-1 (x monotonic): for every column of the x range
// [x0, x1] (mapped onto `columns` columns) keeps the first, minimum, maximum and last sample, in index
// order. Non-finite y values are skipped. Returns data coordinates.
std::vector<QPointF> decimateMinMax(const double* x, const double* y, std::size_t i0, std::size_t i1,
                                    double x0, double x1, int columns);
}  // namespace plot

// Maps data coordinates to painter coordinates of the plot area.
struct PlotTransform {
    QRectF area;                        // plot area (painter coordinates)
    Range x, y;                         // visible data range (lo < hi)
    bool invertX = false;               // x increases to the left
    double px(double vx) const;
    double py(double vy) const;
    QPointF map(double vx, double vy) const { return {px(vx), py(vy)}; }
    QPointF map(const QPointF& d) const { return map(d.x(), d.y()); }
    double dataX(double pxv) const;
    double dataY(double pyv) const;
    QPointF unmap(const QPointF& p) const { return {dataX(p.x()), dataY(p.y())}; }
};

// --------------------------------------------------------------------------------------------- items
// Base of everything drawn in the plot area. Items are owned by their PlotWidget (QObject parent).
class PlotItem : public QObject {
    Q_OBJECT
public:
    explicit PlotItem(PlotWidget* plot);    // registers with the plot (normally via PlotWidget::add...)
    ~PlotItem() override;
    PlotWidget* plot() const;
    void setVisible(bool on);
    bool isVisible() const { return m_visible; }
    void setZValue(double z);               // higher = drawn later (on top)
    double zValue() const { return m_z; }
    void setName(const QString& name);      // legend entry (curves) if non-empty
    QString name() const { return m_name; }
    void setAutoScale(bool on);             // take part in automatic ranges (default true)
    bool autoScale() const { return m_autoScale; }

    // Data extent used by auto range; yBounds only over [x0, x1]. Invalid Bounds: none.
    virtual Bounds xBounds() const { return {}; }
    virtual Bounds yBounds(double x0, double x1) const;
    virtual bool yBoundsExact() const { return false; }   // true: no headroom/padding (heat maps)

protected:
    void changed(bool boundsChanged = false);  // request a repaint (and an auto-range update)

    virtual void paint(QPainter& p, const PlotTransform& t) = 0;
    virtual bool hasLegendEntry() const { return false; }
    virtual void paintLegendSymbol(QPainter&, const QRectF&) const {}
    // Interaction for draggable items: hitTest returns a part id (>= 0) under the pixel position or -1.
    virtual int hitTest(const QPointF&, const PlotTransform&) const { return -1; }
    virtual Qt::CursorShape cursorFor(int /*part*/) const { return Qt::ArrowCursor; }
    virtual void dragStart(int /*part*/, const QPointF& /*data*/) {}
    virtual void dragTo(int /*part*/, const QPointF& /*data*/) {}
    virtual void dragEnd(int /*part*/) {}

private:
    PlotWidget* m_plot = nullptr;
    bool m_visible = true;
    bool m_autoScale = true;
    double m_z = 0;
    QString m_name;
    friend class PlotWidget;
};

enum class Marker { None, Circle, Square, Diamond, Triangle, Cross, Plus };

// Line series and/or scatter markers, optionally filled down to a level.
class Curve : public PlotItem {
    Q_OBJECT
public:
    void setData(Vec x, Vec y);             // sizes must match (the shorter length is used)
    void clearData();
    const Vec& xData() const { return m_x; }
    const Vec& yData() const { return m_y; }
    std::size_t size() const { return std::min(m_x.size(), m_y.size()); }

    void setPen(const QPen& pen);           // width in pixels (cosmetic)
    QPen pen() const { return m_pen; }
    void setColor(const QColor& c);
    void setWidth(double w);
    void setStyle(Qt::PenStyle s);
    void setOpacity(double o);              // 0..1, multiplies line, fill and markers
    double opacity() const { return m_opacity; }
    void setLineVisible(bool on);           // false: markers only (scatter plot)
    bool lineVisible() const { return m_line; }
    void setMarker(Marker m, double size = 6, const QBrush& brush = {}, const QPen& pen = QPen(Qt::NoPen));
    Marker marker() const { return m_marker; }
    void setFill(const QBrush& brush, double level = 0);
    void clearFill();
    void setDecimation(bool on);            // min/max per pixel column when zoomed out (default on)

    Bounds xBounds() const override;
    Bounds yBounds(double x0, double x1) const override;

public:
    using PlotItem::PlotItem;

protected:
    void paint(QPainter& p, const PlotTransform& t) override;
    bool hasLegendEntry() const override { return true; }
    void paintLegendSymbol(QPainter& p, const QRectF& r) const override;

private:
    std::pair<std::size_t, std::size_t> visibleIndices(double x0, double x1) const;
    Vec m_x, m_y;
    int m_order = 0;                        // +1 ascending x, -1 descending, 0 unordered
    QPen m_pen;
    double m_opacity = 1;
    bool m_line = true;
    Marker m_marker = Marker::None;
    double m_markerSize = 6;
    QBrush m_markerBrush;
    QPen m_markerPen = QPen(Qt::NoPen);
    bool m_fill = false;
    QBrush m_fillBrush;
    double m_fillLevel = 0;
    bool m_decimate = true;
    friend class PlotWidget;
};

// Vertical error bars y ± err at points x (with caps); NaN or non-positive errors are skipped. Usually
// added next to a Curve with the same data; takes part in the automatic y range (y ± err).
class ErrorBars : public PlotItem {
    Q_OBJECT
public:
    void setData(Vec x, Vec y, Vec err);    // sizes must match (the shortest length is used)
    std::size_t size() const { return std::min({m_x.size(), m_y.size(), m_err.size()}); }
    void setPen(const QPen& pen);           // default: palette.main
    void setCapWidth(double px);            // full cap width in pixels (default 6; 0: no caps)

    Bounds xBounds() const override;
    Bounds yBounds(double x0, double x1) const override;

public:
    explicit ErrorBars(PlotWidget* plot) : PlotItem(plot) {}

protected:
    void paint(QPainter& p, const PlotTransform& t) override;

private:
    Vec m_x, m_y, m_err;
    QPen m_pen;
    bool m_hasPen = false;
    double m_cap = 6;
};

// Text anchored to a data point. anchor is the point of the text box placed at (x, y), as a fraction of
// its size: (0,0) top-left, (0.5,1) centred above; values outside 0..1 move it further (pyqtgraph).
class TextItem : public PlotItem {
    Q_OBJECT
public:
    void setText(const QString& text);
    QString text() const { return m_text; }
    void setPos(double x, double y);
    QPointF pos() const { return m_pos; }
    void setAnchor(const QPointF& anchor);
    QPointF anchor() const { return m_anchor; }
    void setColor(const QColor& c);         // invalid: palette.label
    void setFont(const QFont& f);
    void setBackground(const QBrush& b);    // box behind the text (default none)
    QRectF boundingRect(const PlotTransform& t, const QFont& base) const;   // painter coordinates

public:
    explicit TextItem(PlotWidget* plot) : PlotItem(plot) { setZValue(10); }   // z 10

protected:
    void paint(QPainter& p, const PlotTransform& t) override;

private:
    QString m_text;
    QPointF m_pos, m_anchor{0.5, 1.0};
    QColor m_color;
    QFont m_font;
    bool m_hasFont = false;
    QBrush m_bg;
};

// Peak labels drawn above their peaks, staggered (as in the Python Measure page) so that labels closer
// than minGapFraction of the visible x span stack up to three levels. Re-staggered on every zoom.
struct PeakLabel {
    double x = 0, y = 0;
    QString text;
};

class PeakLabels : public PlotItem {
    Q_OBJECT
public:
    void setLabels(std::vector<PeakLabel> labels);
    void setPeaks(const Vec& positions, const Vec& x, const Vec& y, int decimals = 0);  // y at nearest x
    const std::vector<PeakLabel>& labels() const { return m_labels; }
    void setMinGapFraction(double f);       // default 0.035 (Python)
    void setColor(const QColor& c);         // invalid: palette.label
    void setFont(const QFont& f);
    std::vector<int> levels(double xSpan) const;   // Python level per label (in label order), before % 3
    // Text rectangles (painter coordinates) of the labels as drawn; null rect for invalid points.
    std::vector<QRectF> layoutLabels(const PlotTransform& t, const QFontMetricsF& fm) const;

public:
    explicit PeakLabels(PlotWidget* plot) : PlotItem(plot) { setZValue(10); }   // z 10

protected:
    void paint(QPainter& p, const PlotTransform& t) override;

private:
    std::vector<PeakLabel> m_labels;        // sorted by x
    double m_gap = 0.035;
    QColor m_color;
    QFont m_font;
    bool m_hasFont = false;
};

// Infinite vertical or horizontal line, optionally draggable.
class InfiniteLine : public PlotItem {
    Q_OBJECT
public:
    enum Orientation { Vertical, Horizontal };
    Orientation orientation() const { return m_orient; }
    void setValue(double v);                // clamped to the bounds
    double value() const { return m_value; }
    void setPen(const QPen& pen);           // default: palette.accent (movable) or palette.reference
    void setMovable(bool on);
    bool isMovable() const { return m_movable; }
    void setBounds(double lo, double hi);   // allowed values while dragging
    void setLabel(const QString& text);     // drawn at the top (vertical) or right end (horizontal)

signals:
    void moved(double value);               // while dragging and on setValue
    void moveFinished(double value);        // mouse released

public:
    InfiniteLine(PlotWidget* plot, Orientation o);

protected:
    void paint(QPainter& p, const PlotTransform& t) override;
    int hitTest(const QPointF& pos, const PlotTransform& t) const override;
    Qt::CursorShape cursorFor(int part) const override;
    void dragTo(int part, const QPointF& data) override;
    void dragEnd(int part) override;

private:
    Orientation m_orient;
    double m_value = 0;
    QPen m_pen;
    bool m_hasPen = false;
    bool m_movable = false;
    double m_lo = -std::numeric_limits<double>::infinity();
    double m_hi = std::numeric_limits<double>::infinity();
    QString m_label;
    friend class PlotWidget;
};

// Shaded x region [lo, hi] (e.g. fit window, integration range) with draggable edges; dragging inside
// moves the whole region.
class RegionItem : public PlotItem {
    Q_OBJECT
public:
    void setRegion(double lo, double hi);
    Range region() const { return {m_lo, m_hi}; }
    void setBrush(const QBrush& b);         // default: palette.region
    void setEdgePen(const QPen& pen);       // default: palette.accent
    void setMovable(bool on);               // default true
    bool isMovable() const { return m_movable; }

signals:
    void regionChanged(double lo, double hi);
    void regionChangeFinished(double lo, double hi);

public:
    explicit RegionItem(PlotWidget* plot) : PlotItem(plot) { setZValue(-10); }   // z -10

protected:
    void paint(QPainter& p, const PlotTransform& t) override;
    int hitTest(const QPointF& pos, const PlotTransform& t) const override;
    Qt::CursorShape cursorFor(int part) const override;
    void dragStart(int part, const QPointF& data) override;
    void dragTo(int part, const QPointF& data) override;
    void dragEnd(int part) override;

private:
    double m_lo = 0, m_hi = 1;
    QBrush m_brush;
    bool m_hasBrush = false;
    QPen m_pen;
    bool m_hasPen = false;
    bool m_movable = true;
    double m_grabX = 0, m_grabLo = 0, m_grabHi = 0;
};

enum class ColorMap { Viridis, Inferno, Grey };
QColor colorMapColor(ColorMap map, double t);     // t in 0..1

// 2D array drawn as an image over a data rectangle (Data area heat map: one row per spectrum).
class HeatMapItem : public PlotItem {
    Q_OBJECT
public:
    // values row-major (rows × cols); row 0 is drawn at the bottom (y0). NaN: transparent.
    void setData(int rows, int cols, Vec values);
    int rows() const { return m_rows; }
    int cols() const { return m_cols; }
    void setExtent(double x0, double x1, double y0, double y1);   // data rectangle covered (x0 -> col 0)
    void setColorMap(ColorMap m);
    ColorMap colorMap() const { return m_map; }
    void setLevels(double lo, double hi);   // fixed colour scale
    void setAutoLevels();                   // min..max of the data (default)
    Range levels() const { return m_levels; }
    void setColorBarVisible(bool on);       // colour bar at the right of the plot
    bool colorBarVisible() const { return m_colorBar; }
    void setColorBarLabel(const QString& s);
    QString colorBarLabel() const { return m_colorBarLabel; }
    const QImage& image() const;            // colour-mapped image (top row = highest y)

    Bounds xBounds() const override;
    Bounds yBounds(double x0, double x1) const override;
    bool yBoundsExact() const override { return true; }

public:
    explicit HeatMapItem(PlotWidget* plot) : PlotItem(plot) { setZValue(-20); }   // z -20

protected:
    void paint(QPainter& p, const PlotTransform& t) override;

private:
    void rebuild() const;
    int m_rows = 0, m_cols = 0;
    Vec m_values;
    double m_x0 = 0, m_x1 = 1, m_y0 = 0, m_y1 = 1;
    ColorMap m_map = ColorMap::Viridis;
    bool m_autoLevels = true;
    Range m_levels;
    bool m_colorBar = false;
    QString m_colorBarLabel;
    mutable QImage m_image;
    mutable bool m_dirty = true;
};

// --------------------------------------------------------------------------------------------- widget
enum class Axis { Bottom, Left };
enum class WheelMode { Spectrum, ZoomX };

class PlotWidget : public QWidget {
    Q_OBJECT
public:
    explicit PlotWidget(QWidget* parent = nullptr);
    ~PlotWidget() override;

    // ---- items (owned by the widget)
    Curve* addCurve(const QPen& pen = QPen(), const QString& name = {});
    Curve* addCurve(Vec x, Vec y, const QPen& pen = QPen(), const QString& name = {});
    Curve* addScatter(Vec x, Vec y, const QColor& color, double size = 6, Marker m = Marker::Circle);
    ErrorBars* addErrorBars(Vec x, Vec y, Vec err, const QPen& pen = QPen());
    TextItem* addText(const QString& text, double x, double y, const QPointF& anchor = {0.5, 1.0});
    PeakLabels* addPeakLabels();
    InfiniteLine* addVLine(double x, const QPen& pen = QPen(), bool movable = false);
    InfiniteLine* addHLine(double y, const QPen& pen = QPen(), bool movable = false);
    RegionItem* addRegion(double lo, double hi);
    HeatMapItem* addHeatMap();
    template <class T, class... A> T* addItem(A&&... args) {   // custom PlotItem subclasses
        return new T(this, std::forward<A>(args)...);
    }
    void removeItem(PlotItem* item);        // deletes it
    void clear();                           // removes all items
    std::vector<PlotItem*> items() const { return m_items; }

    // ---- appearance
    void setPlotPalette(const PlotPalette& p);      // overrides the default (theme) palette
    const PlotPalette& plotPalette() const { return m_pal; }
    void setLabel(Axis axis, const QString& text);  // e.g. "Raman shift (cm⁻¹)"
    QString label(Axis axis) const;
    void setGrid(bool x, bool y);
    void setXInverted(bool on);
    bool xInverted() const { return m_invertX; }
    void setLegendVisible(bool on);
    void setHoverReadout(bool on);                  // "x, y" of the cursor in the plot corner
    void setReadoutFormatter(std::function<QString(const QPointF&)> f);
    static QString navigationHelp(WheelMode mode = WheelMode::Spectrum);   // tooltip text

    // ---- view
    void setXRange(double lo, double hi);           // turns automatic x off; rescales y if autoY
    void setYRange(double lo, double hi);           // call setAutoY(false) first to keep it
    Range xRange() const;
    Range yRange() const;
    void setHomeXRange(double lo, double hi);       // restored by double-click (Python home_x)
    void clearHomeXRange();
    void setAutoX(bool on);                         // x follows the data range (until zoom/pan)
    bool autoX() const { return m_autoX; }
    void setAutoY(bool on);                         // y follows the visible data (Python auto_y)
    bool autoY() const { return m_autoY; }
    void setHeadroom(double fraction);              // above the tallest point; 0.18 Measure, 0.08 Data
    double headroom() const { return m_headroom; }
    void rescaleY();                                // fit y now (only if autoY)
    void autoRange();                               // full view, as a double-click
    void zoomX(double factor, double centerX);      // factor < 1 zooms in around centerX
    void scaleYTop(double factor);                  // y bottom stays; turns autoY off
    void panPixels(const QPointF& delta);           // as dragging by delta pixels
    void setWheelMode(WheelMode m) { m_wheelMode = m; }
    // x range allowed for zooming/panning: data extent united with the home range (empty: no limit)
    Range xLimit() const;
    WheelMode wheelMode() const { return m_wheelMode; }
    void setPanY(bool on) { m_panY = on; }          // drag also pans y (default off, as Python)

    PlotTransform transform() const;                // for the current widget geometry
    QRectF plotArea() const { return transform().area; }
    QPointF mapToData(const QPointF& widgetPos) const;
    QPointF mapFromData(const QPointF& dataPos) const;

    // ---- output
    void render(QPainter& p, const QRectF& target);         // whole plot without interactive overlays
    QImage renderImage(QSize logicalSize = {}, qreal scale = 1.0);   // image size = logicalSize·scale
    bool exportPng(const QString& path, QSize logicalSize = {}, qreal scale = 2.0);
    bool exportSvg(const QString& path, QSize size = {}, const QString& title = {});
    QByteArray toSvg(QSize size = {}, const QString& title = {});

    QSize sizeHint() const override { return {640, 400}; }
    QSize minimumSizeHint() const override { return {160, 100}; }

signals:
    void xRangeChanged(double lo, double hi);
    void yRangeChanged(double lo, double hi);
    void clicked(const QPointF& data, Qt::MouseButton button, Qt::KeyboardModifiers mods);
    void rightClicked(const QPointF& data, const QPoint& globalPos);   // context menu (Python on_right_click)
    void hovered(const QPointF& data);
    void hoverLeft();
    void autoRangeRequested();      // after double-click/autoRange (Python on_auto: redraw)

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void leaveEvent(QEvent*) override;
    void changeEvent(QEvent*) override;

private:
    struct Layout;
    Layout layout(const QRectF& r, const QFont& font, QPaintDevice* dev) const;
    void registerItem(PlotItem* it);
    void unregisterItem(PlotItem* it);
    void itemChanged(bool boundsChanged);
    void ensureView() const;
    void applyX(double lo, double hi);
    void applyY(double lo, double hi);
    Bounds dataXBounds() const;
    std::vector<PlotItem*> sortedItems() const;
    HeatMapItem* colorBarItem() const;
    void drawLegend(QPainter& p, const QRectF& area);
    void drawColorBar(QPainter& p, const Layout& L, HeatMapItem* hm);
    QString readoutText(const QPointF& d) const;
    friend class PlotItem;

    PlotPalette m_pal;
    bool m_customPalette = false;
    std::vector<PlotItem*> m_items;
    QString m_xLabel, m_yLabel;
    bool m_gridX = true, m_gridY = true;
    bool m_invertX = false;
    bool m_legend = false;
    bool m_readout = true;
    std::function<QString(const QPointF&)> m_readoutFmt;
    WheelMode m_wheelMode = WheelMode::ZoomX;          // wheel: x zoom (y follows); Ctrl: y
    bool m_panY = false;

    mutable Range m_x{0, 1}, m_y{0, 1};
    mutable bool m_autoX = true, m_autoY = true;
    mutable bool m_viewDirty = true;
    bool m_hasHome = false;
    Range m_home;
    double m_headroom = 0.18;

    // interaction state
    enum class Drag { None, Pan, Rubber, Item };
    Drag m_drag = Drag::None;
    Qt::MouseButton m_pressButton = Qt::NoButton;
    QPointF m_pressPos, m_lastPos;
    bool m_moved = false;
    PlotItem* m_dragItem = nullptr;
    int m_dragPart = -1;
    QPointF m_hoverPos;
    bool m_hover = false;
};

}  // namespace gui
}  // namespace lr
