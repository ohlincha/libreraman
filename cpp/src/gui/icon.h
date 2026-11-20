#pragma once
// Program icon: a ramen bowl whose steam is a Raman spectrum, drawn at any size (Python gui/icon.py).
#include <QIcon>
#include <QImage>

namespace lr::gui {

QImage drawIcon(int size = 1024);   // transparent background, rounded dark square
QIcon appIcon();                    // 16…512 px drawn, plus :/gui/icon.png (installers' 1024 px PNG)

}  // namespace lr::gui
