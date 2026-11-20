// Build-time helper: makes the platform icon files from resources/icon.png (1024x1024), so that no
// generated binary icons have to be kept in the repository and no extra tools (Pillow, ImageMagick,
// iconutil) are needed.
//   mkicon ico  icon.png out.ico        Windows icon (PNG-compressed entries 16...256 px)
//   mkicon icns icon.png out.icns       macOS icon (PNG entries 16...1024 px)
//   mkicon png  icon.png outdir         outdir/<n>x<n>.png for the Linux hicolor theme
#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QString>
#include <QtEndian>

#include <cstdio>
#include <utility>
#include <vector>

namespace {

QByteArray pngOf(const QImage& src, int size) {
    QImage img = src.width() == size && src.height() == size
                     ? src
                     : src.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QByteArray data;
    QBuffer buf(&data);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return data;
}

void appendLE16(QByteArray& b, quint16 v) {
    v = qToLittleEndian(v);
    b.append(reinterpret_cast<const char*>(&v), 2);
}
void appendLE32(QByteArray& b, quint32 v) {
    v = qToLittleEndian(v);
    b.append(reinterpret_cast<const char*>(&v), 4);
}
void appendBE32(QByteArray& b, quint32 v) {
    v = qToBigEndian(v);
    b.append(reinterpret_cast<const char*>(&v), 4);
}

// ICONDIR + ICONDIRENTRY[n] + PNG images (supported since Windows Vista, also by rc.exe)
QByteArray makeIco(const QImage& src) {
    const std::vector<int> sizes = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    std::vector<QByteArray> images;
    for (int s : sizes) images.push_back(pngOf(src, s));
    QByteArray out;
    appendLE16(out, 0);                       // reserved
    appendLE16(out, 1);                       // type: icon
    appendLE16(out, quint16(sizes.size()));
    quint32 offset = 6 + 16 * quint32(sizes.size());
    for (size_t i = 0; i < sizes.size(); ++i) {
        out.append(char(sizes[i] >= 256 ? 0 : sizes[i]));   // width, height (0 = 256)
        out.append(char(sizes[i] >= 256 ? 0 : sizes[i]));
        out.append(char(0));                  // palette colours
        out.append(char(0));                  // reserved
        appendLE16(out, 1);                   // colour planes
        appendLE16(out, 32);                  // bits per pixel
        appendLE32(out, quint32(images[i].size()));
        appendLE32(out, offset);
        offset += quint32(images[i].size());
    }
    for (const QByteArray& img : images) out.append(img);
    return out;
}

// 'icns' + length, then (OSType, length incl. 8-byte header, PNG data) per entry
QByteArray makeIcns(const QImage& src) {
    const std::vector<std::pair<const char*, int>> entries = {
        {"icp4", 16},  {"icp5", 32},  {"ic11", 32},  {"icp6", 64},  {"ic12", 64},  {"ic07", 128},
        {"ic08", 256}, {"ic13", 256}, {"ic09", 512}, {"ic14", 512}, {"ic10", 1024}};
    QByteArray body;
    for (const auto& [type, size] : entries) {
        const QByteArray png = pngOf(src, size);
        body.append(type, 4);
        appendBE32(body, quint32(png.size() + 8));
        body.append(png);
    }
    QByteArray out("icns", 4);
    appendBE32(out, quint32(body.size() + 8));
    out.append(body);
    return out;
}

bool writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
        std::fprintf(stderr, "mkicon: cannot write %s\n", qPrintable(path));
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: mkicon ico|icns|png icon.png output\n");
        return 2;
    }
    const QString mode = QString::fromLocal8Bit(argv[1]), out = QString::fromLocal8Bit(argv[3]);
    QImage src(QString::fromLocal8Bit(argv[2]));
    if (src.isNull()) {
        std::fprintf(stderr, "mkicon: cannot read %s\n", argv[2]);
        return 1;
    }
    src = src.convertToFormat(QImage::Format_ARGB32);
    if (mode == "ico") return writeFile(out, makeIco(src)) ? 0 : 1;
    if (mode == "icns") return writeFile(out, makeIcns(src)) ? 0 : 1;
    if (mode == "png") {
        QDir().mkpath(out);
        for (int s : {16, 24, 32, 48, 64, 128, 256, 512})
            if (!writeFile(QDir(out).filePath(QStringLiteral("%1x%1.png").arg(s)), pngOf(src, s))) return 1;
        return 0;
    }
    std::fprintf(stderr, "mkicon: unknown mode %s\n", argv[1]);
    return 2;
}
