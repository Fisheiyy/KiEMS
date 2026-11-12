#include "Sidebar.hpp"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>

#include <cmath>
#include <numbers>

namespace kiems_gui {

namespace {

constexpr int kMarkWidth = 22; // the mark's column at the row's right

} // namespace

QSize SidebarDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    if (index.data(kSidebarSeparatorRole).toBool()) return {option.rect.width(), 9};
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(std::max(size.height(), 24));
    return size;
}

void SidebarDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    if (index.data(kSidebarSeparatorRole).toBool()) {
        painter->save();
        QColor line = option.palette.color(QPalette::Mid);
        line.setAlphaF(0.7);
        painter->setPen(QPen(line, 1));
        const double y = option.rect.center().y() + 0.5;
        painter->drawLine(QPointF(option.rect.left() + 4, y), QPointF(option.rect.right() - 4, y));
        painter->restore();
        return;
    }
    QStyleOptionViewItem item = option;
    initStyleOption(&item, index);
    const auto status = static_cast<SidebarStatus>(index.data(kSidebarStatusRole).toInt());
    const bool marked = status != SidebarStatus::None;
    // Elided short of the mark, so the name never runs under it.
    if (marked) {
        const int iconWidth = item.icon.isNull() ? 0 : item.decorationSize.width() + 6;
        const int available = item.rect.width() - iconWidth - kMarkWidth - 8;
        item.text = item.fontMetrics.elidedText(item.text, Qt::ElideRight, std::max(10, available));
    }
    const QWidget* widget = option.widget;
    QStyle* style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);
    if (!marked) return;
    const QRectF mark(option.rect.right() - kMarkWidth + 4, option.rect.center().y() - 6.5, 13, 13);
    // On a selected row the marks that are drawn in the accent colour would vanish into it.
    const bool selected = option.state & QStyle::State_Selected;
    paintStatus(painter, mark, status, index.data(kSidebarProgressRole).isValid() ? index.data(kSidebarProgressRole).toDouble() : -1,
                selected ? QColor(Qt::white) : option.palette.color(QPalette::Highlight), selected);
}

void SidebarDelegate::paintStatus(QPainter* painter, const QRectF& rect, SidebarStatus status, double progress,
                                  const QColor& accent, bool onSelection) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRectF dot = rect.adjusted(1.5, 1.5, -1.5, -1.5);
    static const QColor blue(0x0a, 0x84, 0xff), green(0x30, 0xd1, 0x58), red(0xff, 0x45, 0x3a), yellow(0xff, 0xcc, 0x00);
    // The slight shadow the macOS marks have, lifting them off the row.
    const auto shadow = [&](const QPainterPath& shape) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 90));
        painter->drawPath(shape.translated(0, 0.8));
    };
    switch (status) {
    case SidebarStatus::None:
        break;
    case SidebarStatus::NotRun:
        painter->setPen(QPen(onSelection ? QColor(Qt::white) : blue, 1.5));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(dot);
        break;
    case SidebarStatus::Done: {
        QPainterPath circle;
        circle.addEllipse(dot);
        shadow(circle);
        painter->setBrush(green);
        painter->drawPath(circle);
        break;
    }
    case SidebarStatus::TestsFailed: {
        QPolygonF octagon;
        for (int k = 0; k < 8; ++k) {
            const double a = (k + 0.5) * std::numbers::pi / 4;
            octagon << QPointF(rect.center().x() + 6 * std::cos(a), rect.center().y() + 6 * std::sin(a));
        }
        QPainterPath shape;
        shape.addPolygon(octagon);
        shadow(shape);
        painter->setBrush(red);
        painter->drawPath(shape);
        painter->setBrush(Qt::white);
        painter->drawRect(QRectF(rect.center().x() - 3, rect.center().y() - 0.8, 6, 1.6));
        break;
    }
    case SidebarStatus::Failed: {
        QPainterPath triangle;
        triangle.moveTo(rect.center().x(), rect.top() + 0.5);
        triangle.lineTo(rect.right() - 0.5, rect.bottom() - 1);
        triangle.lineTo(rect.left() + 0.5, rect.bottom() - 1);
        triangle.closeSubpath();
        shadow(triangle);
        painter->setBrush(yellow);
        painter->drawPath(triangle);
        painter->setBrush(QColor(40, 40, 40));
        painter->drawRoundedRect(QRectF(rect.center().x() - 0.75, rect.top() + 4, 1.5, 4.5), 0.7, 0.7);
        painter->drawEllipse(QPointF(rect.center().x(), rect.bottom() - 3.2), 0.85, 0.85);
        break;
    }
    case SidebarStatus::Running:
    case SidebarStatus::Queued: {
        // A ring that fills like a pie, as the macOS list's progress indicator does.
        painter->setPen(QPen(accent, 1.3));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(dot);
        if (status == SidebarStatus::Running) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(accent);
            const QRectF pie = dot.adjusted(1.8, 1.8, -1.8, -1.8);
            if (progress >= 0) {
                painter->drawPie(pie, 90 * 16, -static_cast<int>(std::lround(360 * 16 * std::min(progress, 1.0))));
            } else {
                painter->drawPie(pie, 90 * 16, -90 * 16); // busy, with no measure yet
            }
        }
        break;
    }
    }
    painter->restore();
}

} // namespace kiems_gui
