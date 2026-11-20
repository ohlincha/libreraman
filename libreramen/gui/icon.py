"""Program icon: a ramen bowl whose steam is a Raman spectrum, drawn at any size."""

import math
import os

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QIcon, QLinearGradient, QPainter, QPainterPath, QPen, QPixmap

ICON_PNG = os.path.join(os.path.dirname(os.path.dirname(__file__)), "data", "icon.png")


def draw_icon(size=1024):
    s = size / 1024.0
    pm = QPixmap(size, size)
    pm.fill(Qt.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.Antialiasing)
    # background: dark rounded square (macOS-style margins)
    m = (100 if size >= 64 else 24) * s          # small sizes: use the space for the motif
    g = QLinearGradient(0, m, 0, size - m)
    g.setColorAt(0, QColor("#26304a"))
    g.setColorAt(1, QColor("#121722"))
    p.setPen(QPen(QColor(255, 255, 255, 60), max(1.0, 8 * s)))   # faint edge: visible on dark backgrounds
    p.setBrush(g)
    p.drawRoundedRect(QRectF(m, m, size - 2 * m, size - 2 * m), 185 * s, 185 * s)
    # steam = Raman spectrum (few, tall bands so it reads at small sizes)
    bands = [(0.16, 0.42, 0.035), (0.36, 0.62, 0.03), (0.54, 1.0, 0.03), (0.72, 0.55, 0.035), (0.88, 0.3, 0.035)]
    path = QPainterPath()
    x0, x1, base, amp = 270 * s, 754 * s, 560 * s, 330 * s
    for i in range(241):
        t = i / 240
        y = 0.03 + sum(a * math.exp(-((t - c) / w) ** 2) for c, a, w in bands)
        pt = QPointF(x0 + t * (x1 - x0), base - y * amp)
        path.moveTo(pt) if i == 0 else path.lineTo(pt)
    pen = QPen(QColor("#ff9a5c"), max(1.5, 34 * s))
    pen.setCapStyle(Qt.RoundCap)
    pen.setJoinStyle(Qt.RoundJoin)
    p.setPen(pen)
    p.setBrush(Qt.NoBrush)
    p.drawPath(path)
    # bowl
    bowl = QPainterPath()
    bowl.moveTo(225 * s, 600 * s)
    bowl.cubicTo(235 * s, 860 * s, 789 * s, 860 * s, 799 * s, 600 * s)
    bowl.closeSubpath()
    p.setPen(Qt.NoPen)
    p.setBrush(QColor("#e04a3c"))
    p.drawPath(bowl)
    p.setBrush(QColor("#f2c94c"))                               # rim / broth
    p.drawRoundedRect(QRectF(205 * s, 570 * s, 614 * s, 60 * s), 30 * s, 30 * s)
    if size >= 48:                                               # details only where they are visible
        p.setPen(QPen(QColor("#ffffff"), 14 * s))
        for k in range(4):
            p.drawArc(QRectF((300 + k * 110) * s, 560 * s, 90 * s, 50 * s), 0, 180 * 16)
        p.setPen(QPen(QColor("#b03428"), 16 * s))
        p.drawLine(QPointF(330 * s, 740 * s), QPointF(694 * s, 740 * s))
    p.end()
    return pm


def app_icon():
    icon = QIcon()
    for sz in (16, 24, 32, 48, 64, 128, 256, 512):
        icon.addPixmap(draw_icon(sz))
    return icon


if __name__ == "__main__":       # writes data/icon.png (used by the installers)
    from PySide6.QtWidgets import QApplication
    QApplication([])
    draw_icon(1024).save(ICON_PNG)
    print("wrote", ICON_PNG)
