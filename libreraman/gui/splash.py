"""Splash screen: a bowl of ramen whose steam rises as a Raman spectrum."""

import math

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QLinearGradient, QPainter, QPainterPath, QPen, QPixmap
from PySide6.QtWidgets import QSplashScreen

from .. import __version__


def logo_pixmap(w=560, h=340):
    pm = QPixmap(w, h)
    pm.fill(Qt.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.Antialiasing)
    g = QLinearGradient(0, 0, 0, h)
    g.setColorAt(0, QColor("#1d2433"))
    g.setColorAt(1, QColor("#10141d"))
    p.setBrush(g)
    p.setPen(Qt.NoPen)
    p.drawRoundedRect(QRectF(0, 0, w, h), 18, 18)

    cx, top = 140, 120
    # steam = a Raman spectrum (water/ethanol-like bands)
    path = QPainterPath()
    bands = [(0.18, 0.35, 0.03), (0.33, 0.55, 0.02), (0.47, 0.95, 0.018), (0.62, 0.45, 0.025), (0.8, 0.7, 0.03)]
    for i in range(121):
        x = i / 120
        y = 0.05 + sum(a * math.exp(-((x - c) / s) ** 2) for c, a, s in bands)
        pt = QPointF(cx - 75 + x * 150, top - 8 - y * 85)
        path.moveTo(pt) if i == 0 else path.lineTo(pt)
    p.setPen(QPen(QColor("#ff8f5a"), 3))
    p.setBrush(Qt.NoBrush)
    p.drawPath(path)
    # bowl
    bowl = QPainterPath()
    bowl.moveTo(cx - 90, top + 10)
    bowl.cubicTo(cx - 85, top + 95, cx + 85, top + 95, cx + 90, top + 10)
    bowl.closeSubpath()
    p.setPen(Qt.NoPen)
    p.setBrush(QColor("#d6493b"))
    p.drawPath(bowl)
    p.setBrush(QColor("#f2c94c"))
    p.drawRoundedRect(QRectF(cx - 90, top + 2, 180, 14), 7, 7)
    p.setPen(QPen(QColor("#ffffff"), 2))
    for k in range(4):                                   # noodles
        x0 = cx - 60 + k * 30
        p.drawArc(QRectF(x0, top - 2, 26, 14), 0, 180 * 16)
    p.setPen(QPen(QColor("#c9b28a"), 4))                 # chopsticks
    p.drawLine(QPointF(cx + 40, top - 70), QPointF(cx + 110, top + 30))
    p.drawLine(QPointF(cx + 55, top - 75), QPointF(cx + 118, top + 22))

    p.setPen(QColor("#ffffff"))
    f = QFont()
    f.setPointSize(30)
    f.setBold(True)
    p.setFont(f)
    p.drawText(QRectF(255, 70, 300, 50), Qt.AlignLeft, "LibreRaman")
    f.setPointSize(11)
    f.setBold(False)
    p.setFont(f)
    p.setPen(QColor("#b8c2d6"))
    p.drawText(QRectF(258, 122, 300, 60), Qt.AlignLeft | Qt.TextWordWrap,
               "Open-source software for Raman\nspectrometers that deserve a second life")
    p.setPen(QColor("#7d879b"))
    f.setPointSize(9)
    p.setFont(f)
    p.drawText(QRectF(258, 180, 300, 20), Qt.AlignLeft, f"Version {__version__} · MIT licence")
    p.end()
    return pm


class Splash(QSplashScreen):
    def __init__(self):
        super().__init__(logo_pixmap())

    def message(self, text):
        self.showMessage("  " + text, Qt.AlignBottom | Qt.AlignLeft, QColor("#d7deea"))
