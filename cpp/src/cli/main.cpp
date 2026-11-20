// Placeholder: the command line is ported from libreramen/__main__.py.
#include <QCoreApplication>
#include <cstdio>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    std::puts("LibreRamen CLI " LIBRERAMEN_VERSION);
    return 0;
}
