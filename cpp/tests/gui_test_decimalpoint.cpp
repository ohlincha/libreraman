// Numbers in the GUI use '.' whatever the system locale; ',' typed into a number field becomes '.'.
#include <QApplication>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QtTest>

#include "gui/decimalpoint.h"

class GuiTestDecimalPoint : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QLocale::setDefault(QLocale(QLocale::Swedish, QLocale::Sweden));     // a ',' locale
        lr::gui::useDecimalPoint(*qApp);
    }
    void displayAndTyping() {
        QDoubleSpinBox b;
        b.setDecimals(2);
        b.setRange(0, 10000);
        b.setSuffix(" s");
        b.setValue(1.5);
        QCOMPARE(b.text(), QStringLiteral("1.50 s"));
        b.show();
        QVERIFY(QTest::qWaitForWindowExposed(&b));
        b.selectAll();
        QTest::keyClicks(&b, "2,25");                 // as typed with a Swedish keypad
        QCOMPARE(b.value(), 2.25);
        b.setValue(1234.5);
        QCOMPARE(b.text(), QStringLiteral("1234.50 s"));   // no thousands separator
    }
};

QTEST_MAIN(GuiTestDecimalPoint)
#include "gui_test_decimalpoint.moc"
