// Placeholder: the GUI is ported from libreramen/gui.
#include <QApplication>
#include <QLabel>
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QLabel l("LibreRamen " LIBRERAMEN_VERSION);
    l.show();
    return 0;
}
