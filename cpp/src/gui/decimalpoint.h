#pragma once
// Numbers in the GUI always use '.' as decimal separator (whatever the system locale, e.g. Swedish ','),
// like the files LibreRaman writes. On keyboards whose numeric keypad gives ',' (European layouts), a ','
// typed into a number field is turned into '.', so typing works naturally everywhere.
class QApplication;

namespace lr::gui {

// Call once, before any widget is created.
void useDecimalPoint(QApplication& app);

}  // namespace lr::gui
