#pragma once
// A layout that places its items left to right and wraps onto further lines when the width runs out (the Qt
// "flow layout" example). Used for control rows that must fit laptop screens: its minimum width is that of the
// widest item, not the sum of all of them.
//
// groupRow(row) turns a QHBoxLayout that was filled with widgets into wrap-friendly groups: the widgets between
// spacings/stretches stay together (a label next to its control), each group becomes one flow item.
#include <QLayout>
#include <QList>

class QHBoxLayout;

namespace lr::gui {

class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int hSpacing = 12, int vSpacing = 4);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int count() const override { return int(m_items.size()); }
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return doLayout(QRect(0, 0, width, 0), true); }
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect& rect) override;

    // Moves the widgets of `row` (an unparented QHBoxLayout) into this layout as groups; spacings and
    // stretches separate groups. Returns the number of groups added. `row` is left empty.
    int addGroups(QHBoxLayout* row);

private:
    int doLayout(const QRect& rect, bool testOnly) const;
    QList<QLayoutItem*> m_items;
    int m_h, m_v;
};

}  // namespace lr::gui
