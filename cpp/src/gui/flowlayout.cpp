#include "flowlayout.h"

#include <QHBoxLayout>
#include <QWidget>

namespace lr::gui {

FlowLayout::FlowLayout(QWidget* parent, int hSpacing, int vSpacing) : QLayout(parent), m_h(hSpacing), m_v(vSpacing) {
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout() {
    while (QLayoutItem* item = takeAt(0)) delete item;
}

void FlowLayout::addItem(QLayoutItem* item) { m_items.append(item); }

QLayoutItem* FlowLayout::itemAt(int index) const {
    return index >= 0 && index < m_items.size() ? m_items.at(index) : nullptr;
}

QLayoutItem* FlowLayout::takeAt(int index) {
    return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
}

QSize FlowLayout::minimumSize() const {
    QSize s;
    for (const QLayoutItem* item : m_items)
        if (!item->isEmpty()) s = s.expandedTo(item->minimumSize());
    const QMargins m = contentsMargins();
    return s + QSize(m.left() + m.right(), m.top() + m.bottom());
}

QSize FlowLayout::sizeHint() const {   // everything on one line
    int w = 0, h = 0, n = 0;
    for (const QLayoutItem* item : m_items) {
        if (item->isEmpty()) continue;
        w += item->sizeHint().width() + (n++ ? m_h : 0);
        h = std::max(h, item->sizeHint().height());
    }
    const QMargins m = contentsMargins();
    return QSize(w + m.left() + m.right(), h + m.top() + m.bottom());
}

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const {
    const QMargins m = contentsMargins();
    const QRect r = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    int x = r.x(), y = r.y(), lineHeight = 0;
    // items of one line share its height (vertically centred)
    QList<QPair<QLayoutItem*, QPoint>> line;
    auto flush = [&] {
        if (!testOnly)
            for (const auto& [item, p] : line) {
                const QSize s = item->sizeHint();
                item->setGeometry(QRect(QPoint(p.x(), p.y() + (lineHeight - s.height()) / 2), s));
            }
        line.clear();
    };
    for (QLayoutItem* item : m_items) {
        if (item->isEmpty()) continue;
        QSize s = item->sizeHint();
        s.setWidth(std::min(s.width(), std::max(r.width(), item->minimumSize().width())));
        if (x > r.x() && x + s.width() > r.right() + 1) {   // wrap
            flush();
            x = r.x();
            y += lineHeight + m_v;
            lineHeight = 0;
        }
        line.append({item, QPoint(x, y)});
        x += s.width() + m_h;
        lineHeight = std::max(lineHeight, s.height());
    }
    flush();
    return y + lineHeight - rect.y() + m.bottom();
}

int FlowLayout::addGroups(QHBoxLayout* row) {
    int groups = 0;
    QHBoxLayout* current = nullptr;
    auto finish = [&] {
        if (!current) return;
        auto* w = new QWidget;
        current->setContentsMargins(0, 0, 0, 0);
        w->setLayout(current);
        addWidget(w);
        current = nullptr;
        ++groups;
    };
    while (QLayoutItem* item = row->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            if (!current) {
                current = new QHBoxLayout;
                current->setSpacing(6);
            }
            current->addWidget(w);
        } else if (QLayout* l = item->layout()) {
            finish();
            auto* holder = new QWidget;
            l->setParent(nullptr);
            l->setContentsMargins(0, 0, 0, 0);
            holder->setLayout(l);
            addWidget(holder);
            ++groups;
            continue;   // the layout now belongs to holder; the item is the layout itself
        } else {
            finish();   // spacing or stretch: a group boundary
        }
        delete item;
    }
    finish();
    return groups;
}

}  // namespace lr::gui
