#include "core/PixelBuffer.h"

#include <QApplication>
#include <QBuffer>
#include <QImage>
#include <QPainter>
#include <QTextLayout>
#include <QTextOption>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    require(QByteArray(qVersion()) == "6.12.1", "runtime Qt must match the pinned headers");
    const std::array<QRgb, 12> pixels{
        qRgba(1, 2, 3, 0), qRgba(240, 120, 60, 1), qRgba(7, 11, 19, 127),
        qRgba(17, 23, 31, 128), qRgba(41, 43, 47, 254), qRgba(53, 59, 61, 255),
        qRgba(255, 0, 128, 0), qRgba(2, 5, 8, 2), qRgba(13, 21, 34, 192),
        qRgba(0, 0, 0, 255), qRgba(255, 255, 255, 255), qRgba(3, 2, 1, 0)
    };
    QImage source(3, 4, QImage::Format_ARGB32);
    require(!source.isNull(), "allocate straight-alpha source");
    for (int y = 0; y < source.height(); ++y)
        std::memcpy(source.scanLine(y), pixels.data() + y * source.width(), source.width() * sizeof(QRgb));

    // A painter round-trip through premultiplied alpha destroys hidden RGB at
    // alpha=0. The export contract copies original rows after painting the
    // separate note area; this check freezes the required byte-preserving seam.
    QImage composed(160, 80, QImage::Format_ARGB32);
    composed.fill(Qt::white);
    {
        QPainter painter(&composed);
        require(painter.isActive(), "real Qt raster engine is available");
        painter.fillRect(QRect(4, 0, 156, 80), QColor(248, 249, 250));
    }
    for (int y = 0; y < source.height(); ++y)
        std::memcpy(composed.scanLine(y), source.constScanLine(y), source.width() * sizeof(QRgb));

    QByteArray encoded;
    QBuffer buffer(&encoded);
    require(buffer.open(QIODevice::WriteOnly) && composed.save(&buffer, "PNG"), "encode a real PNG");
    buffer.close();
    const QImage decoded = QImage::fromData(encoded, "PNG").convertToFormat(QImage::Format_ARGB32);
    require(decoded.size() == composed.size(), "PNG dimensions survive encoding");
    for (int y = 0; y < source.height(); ++y)
        require(std::memcmp(decoded.constScanLine(y), source.constScanLine(y), source.width() * sizeof(QRgb)) == 0,
            "all source channels, including hidden RGB and alpha, survive PNG export");

    const QString text = QString::fromUtf8("完整备注：中文、多行与 emoji 😀\nhttps://example.invalid/") + QString(2048, QChar('x'));
    QFont font;
    font.setPixelSize(14);
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    qsizetype consumed = 0;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(120);
        require(line.textLength() > 0, "layout must make forward progress");
        consumed += line.textLength();
    }
    layout.endLayout();
    require(consumed == text.size(), "full text including long unbroken strings must be laid out");
    std::cout << "Qt " << qVersion() << ": real PNG pixels and complete text layout passed\n";
}
