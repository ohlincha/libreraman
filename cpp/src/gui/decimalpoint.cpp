#include "decimalpoint.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QKeyEvent>
#include <QLocale>

namespace lr::gui {

namespace {

class CommaToPoint : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (ev->type() != QEvent::KeyPress) return false;
        auto* k = static_cast<QKeyEvent*>(ev);
        if (k->text() != QStringLiteral(",")) return false;
        // spin boxes, or the line edit inside one
        QObject* o = obj;
        if (!qobject_cast<QAbstractSpinBox*>(o) && o->parent()) o = o->parent();
        if (!qobject_cast<QAbstractSpinBox*>(o)) return false;
        QKeyEvent dot(QEvent::KeyPress, Qt::Key_Period, k->modifiers(), QStringLiteral("."), k->isAutoRepeat(),
                      ushort(k->count()));
        QCoreApplication::sendEvent(obj, &dot);
        return true;
    }
};

}  // namespace

void useDecimalPoint(QApplication& app) {
    // English (UK) number format: '.' decimal point; no thousands separators in fields
    QLocale loc(QLocale::English, QLocale::UnitedKingdom);
    loc.setNumberOptions(QLocale::OmitGroupSeparator);
    QLocale::setDefault(loc);
    app.installEventFilter(new CommaToPoint(&app));
}

}  // namespace lr::gui
