#include "Icons.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace kiems_gui::icons {

namespace {

void draw(QPainter& p, Glyph glyph, const QColor& color) {
    const QPen line(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(line);
    p.setBrush(Qt::NoBrush);
    switch (glyph) {
    case Glyph::Play: {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        const QPointF tri[] = {{4.5, 2.5}, {13.5, 8}, {4.5, 13.5}};
        p.drawPolygon(tri, 3);
        break;
    }
    case Glyph::PlayAll: {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        const QPointF a[] = {{1.5, 3}, {8.5, 8}, {1.5, 13}};
        const QPointF b[] = {{7.5, 3}, {14.5, 8}, {7.5, 13}};
        p.drawPolygon(a, 3);
        p.drawPolygon(b, 3);
        break;
    }
    case Glyph::Stop:
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(3.5, 3.5, 9, 9), 1.5, 1.5);
        break;
    case Glyph::Folder: {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        QPainterPath path;
        path.moveTo(1.5, 4);
        path.lineTo(6, 4);
        path.lineTo(7.5, 5.5);
        path.lineTo(14.5, 5.5);
        path.lineTo(14.5, 13);
        path.lineTo(1.5, 13);
        path.closeSubpath();
        p.drawPath(path);
        break;
    }
    case Glyph::Sliders:
        for (int k = 0; k < 3; ++k) {
            const double y = 4 + 4 * k;
            p.drawLine(QPointF(2, y), QPointF(14, y));
            const double x = k == 1 ? 10 : (k == 0 ? 5 : 7);
            p.setBrush(color);
            p.drawEllipse(QPointF(x, y), 1.7, 1.7);
            p.setBrush(Qt::NoBrush);
        }
        break;
    case Glyph::Board:
        p.drawRoundedRect(QRectF(2, 3, 12, 10), 1.5, 1.5);
        p.drawLine(QPointF(5, 6), QPointF(9, 6));
        p.drawLine(QPointF(9, 6), QPointF(11, 9));
        p.drawEllipse(QPointF(11, 10), 1, 1);
        break;
    case Glyph::Cube: {
        const QPointF top[] = {{8, 2}, {14, 5}, {8, 8}, {2, 5}};
        p.drawPolygon(top, 4);
        p.drawLine(QPointF(2, 5), QPointF(2, 11));
        p.drawLine(QPointF(14, 5), QPointF(14, 11));
        p.drawLine(QPointF(8, 8), QPointF(8, 14));
        p.drawLine(QPointF(2, 11), QPointF(8, 14));
        p.drawLine(QPointF(14, 11), QPointF(8, 14));
        break;
    }
    case Glyph::Chart:
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(2, 8, 3, 6), 0.8, 0.8);
        p.drawRoundedRect(QRectF(6.5, 3, 3, 11), 0.8, 0.8);
        p.drawRoundedRect(QRectF(11, 6, 3, 8), 0.8, 0.8);
        break;
    case Glyph::Waveform:
        for (int k = 0; k < 6; ++k) {
            static const double heights[] = {3, 7, 11, 6, 9, 4};
            const double x = 2.5 + 2.2 * k, h = heights[k];
            p.drawLine(QPointF(x, 8 - h / 2), QPointF(x, 8 + h / 2));
        }
        break;
    case Glyph::Trace: {
        QPainterPath path;
        path.moveTo(1.5, 12);
        path.lineTo(5, 12);
        path.lineTo(9, 5);
        path.lineTo(14.5, 5);
        p.drawPath(path);
        p.setBrush(color);
        p.drawEllipse(QPointF(1.8, 12), 1.4, 1.4);
        p.drawEllipse(QPointF(14.2, 5), 1.4, 1.4);
        break;
    }
    case Glyph::Stack:
        p.drawRoundedRect(QRectF(2, 7, 9, 7), 1.2, 1.2);
        p.drawLine(QPointF(4.5, 4.5), QPointF(13.5, 4.5));
        p.drawLine(QPointF(13.5, 4.5), QPointF(13.5, 11.5));
        p.drawLine(QPointF(7, 2), QPointF(15, 2));
        break;
    case Glyph::Bolt: {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        const QPointF bolt[] = {{9.5, 1}, {3.5, 9}, {7.5, 9}, {6, 15}, {12.5, 6.5}, {8.5, 6.5}, {10.5, 1}};
        p.drawPolygon(bolt, 7);
        break;
    }
    case Glyph::Engine:
        p.drawRoundedRect(QRectF(4, 4, 8, 8), 1.2, 1.2);
        for (int k = 0; k < 3; ++k) {
            const double c = 5.5 + 2.5 * k;
            p.drawLine(QPointF(c, 1.5), QPointF(c, 4));
            p.drawLine(QPointF(c, 12), QPointF(c, 14.5));
            p.drawLine(QPointF(1.5, c), QPointF(4, c));
            p.drawLine(QPointF(12, c), QPointF(14.5, c));
        }
        break;
    }
}

} // namespace

QIcon icon(Glyph glyph, const QColor& color, int size) {
    QIcon result;
    for (int scale : {1, 2}) {
        QPixmap pixmap(size * scale, size * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 16.0, size / 16.0);
        draw(painter, glyph, color);
        painter.end();
        result.addPixmap(pixmap);
        // Greyed when disabled, as a toolbar's icons should be.
        QPixmap disabled(pixmap.size());
        disabled.setDevicePixelRatio(scale);
        disabled.fill(Qt::transparent);
        QPainter dim(&disabled);
        dim.setOpacity(0.35);
        dim.drawPixmap(0, 0, pixmap);
        dim.end();
        result.addPixmap(disabled, QIcon::Disabled);
    }
    return result;
}

} // namespace kiems_gui::icons
