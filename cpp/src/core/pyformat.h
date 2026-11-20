#pragma once
// Python-compatible text formatting for output that must match the Python reference byte for byte:
//   str(float) / repr(float), f"{x:.Nf}", f"{x:g}", repr(str), str(bool), str(list of int).
// printf-style rounding (exact binary value, ties to even) is what Python does; QString::number(x, 'f', n)
// rounds ties away from zero (12.25 -> "12.3", Python "12.2"), so fixed() uses snprintf.
#include <QByteArray>
#include <QLocale>
#include <QString>
#include <QStringList>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <vector>

namespace lr::py {

namespace detail {
inline QString cfmt(const char* fmt, double v, int prec) {
    char buf[400];
    std::snprintf(buf, sizeof buf, fmt, prec, v);
    QString s = QString::fromLatin1(buf);
    const char* dp = std::localeconv()->decimal_point;          // QCoreApplication may set LC_ALL from the env
    if (dp && dp[0] && !(dp[0] == '.' && !dp[1])) s.replace(QString::fromLatin1(dp), QStringLiteral("."));
    return s;
}
}  // namespace detail

// f"{v:.{prec}f}"
inline QString fixed(double v, int prec) {
    if (std::isnan(v)) return QStringLiteral("nan");
    if (std::isinf(v)) return v > 0 ? QStringLiteral("inf") : QStringLiteral("-inf");
    return detail::cfmt("%.*f", v, prec);
}

// f"{v:g}"
inline QString g(double v) {
    if (std::isnan(v)) return QStringLiteral("nan");
    if (std::isinf(v)) return v > 0 ? QStringLiteral("inf") : QStringLiteral("-inf");
    return detail::cfmt("%.*g", v, 6);
}

// str(v) for a Python float: shortest round-trip digits; exponent form below 1e-4 and from 1e16.
inline QString str(double v) {
    if (std::isnan(v)) return QStringLiteral("nan");
    if (std::isinf(v)) return v > 0 ? QStringLiteral("inf") : QStringLiteral("-inf");
    if (v == 0) return std::signbit(v) ? QStringLiteral("-0.0") : QStringLiteral("0.0");
    // Shortest digits in scientific form, e.g. "-1.2345e+02" (C locale, locale-independent).
    const QString e = QString::number(v, 'e', QLocale::FloatingPointShortest);
    const int epos = e.indexOf('e');
    QString mant = e.left(epos);
    const int exp = e.mid(epos + 1).toInt();
    const bool neg = mant.startsWith('-');
    if (neg) mant.remove(0, 1);
    const QString digits = QString(mant).remove('.');
    QString out;
    if (exp >= -4 && exp < 16) {
        if (exp >= 0) {
            QString ip = digits.left(exp + 1);
            while (ip.size() < exp + 1) ip += '0';
            QString fp = digits.mid(exp + 1);
            out = ip + "." + (fp.isEmpty() ? QStringLiteral("0") : fp);
        } else {
            out = "0." + QString(-exp - 1, '0') + digits;
        }
    } else {
        out = digits.left(1);
        if (digits.size() > 1) out += "." + digits.mid(1);
        out += QString("e%1%2").arg(exp < 0 ? '-' : '+').arg(std::abs(exp), 2, 10, QChar('0'));
    }
    return neg ? "-" + out : out;
}

inline QString str(bool b) { return b ? QStringLiteral("True") : QStringLiteral("False"); }

// str(list of int): "[532, 1064]"
inline QString str(const std::vector<int>& v) {
    QStringList p;
    for (int x : v) p << QString::number(x);
    return "[" + p.join(", ") + "]";
}

// bytes.decode("ascii", "replace")
inline QString asciiReplace(const QByteArray& b) {
    QString s;
    s.reserve(b.size());
    for (char c : b) s += (static_cast<unsigned char>(c) < 0x80) ? QChar(c) : QChar(0xFFFD);
    return s;
}

// repr(str): '...' (or "..." if the text contains ' but no ").
inline QString repr(const QString& s) {
    const QChar q = (s.contains('\'') && !s.contains('"')) ? QChar('"') : QChar('\'');
    QString o = q;
    for (QChar c : s) {
        const ushort u = c.unicode();
        if (c == q || c == '\\') o += QString("\\") + c;
        else if (u == '\n') o += "\\n";
        else if (u == '\r') o += "\\r";
        else if (u == '\t') o += "\\t";
        else if (u < 0x20 || u == 0x7F) o += QString("\\x%1").arg(u, 2, 16, QChar('0'));
        else o += c;
    }
    return o + q;
}

}  // namespace lr::py
