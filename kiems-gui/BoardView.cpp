#include "BoardView.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace kiems_gui {

namespace {

QColor parseColor(const std::string& hex, const QColor& fallback) {
    // libkicad's colours are "#RRGGBBAA"; QColor reads "#AARRGGBB".
    if (hex.size() == 9 && hex[0] == '#') {
        return QColor(QString::fromStdString("#" + hex.substr(7, 2) + hex.substr(1, 6)));
    }
    return fallback;
}

QPainterPath loopPath(const libkicad::PolygonLoop& loop) {
    QPainterPath path;
    if (loop.pointsMm.empty()) return path;
    path.moveTo(loop.pointsMm.front().first, loop.pointsMm.front().second);
    for (std::size_t i = 1; i < loop.pointsMm.size(); ++i) path.lineTo(loop.pointsMm[i].first, loop.pointsMm[i].second);
    path.closeSubpath();
    return path;
}

const QColor kBackground(0x16, 0x18, 0x1c);
const QColor kSubstrate(0x1f, 0x33, 0x27);

} // namespace

BoardView::BoardView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(false);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 200);
    setAutoFillBackground(false);
}

void BoardView::setBoard(std::shared_ptr<const BoardData> board) {
    _board = std::move(board);
    _layers.clear();
    _outline = QPainterPath();
    _boardBounds = QRectF();
    _selection = {};
    if (!_board) {
        update();
        return;
    }
    const auto& geometry = _board->geometry;
    _outline.setFillRule(Qt::OddEvenFill);
    for (const auto& loop : geometry.outline) _outline.addPath(loopPath(loop));
    _boardBounds = _outline.boundingRect();

    auto color = [&](const std::string& name, const QColor& fallback) {
        const auto it = _board->layerColors.find(name);
        return parseColor(it == _board->layerColors.end() ? std::string() : it->second, fallback);
    };

    // Bottom silkscreen, copper from the bottom up, then top silkscreen.
    const QStringList copper = _board->copperLayers;
    std::string backSilk = "B.Silkscreen", frontSilk = "F.Silkscreen";
    for (const auto& layer : _board->layers) {
        if (layer.name == "B.SilkS") backSilk = layer.name;
        if (layer.name == "F.SilkS") frontSilk = layer.name;
    }
    auto addSilk = [&](const std::string& name, const std::vector<libkicad::SilkscreenPolygon>& polygons) {
        Layer layer{QString::fromStdString(name), color(name, QColor(0xf2, 0xed, 0xa1)), false, true, {}};
        for (const auto& polygon : polygons) {
            if (polygon.loop.hole && !layer.shapes.empty()) {
                layer.shapes.back().path.addPath(loopPath(polygon.loop));
                continue;
            }
            Shape shape;
            shape.path = loopPath(polygon.loop);
            shape.path.setFillRule(Qt::OddEvenFill);
            shape.footprint = QString::fromStdString(polygon.footprintRef);
            layer.shapes.push_back(std::move(shape));
        }
        for (auto& shape : layer.shapes) shape.bounds = shape.path.boundingRect();
        _layers.push_back(std::move(layer));
    };
    addSilk(backSilk, geometry.backSilkscreen);
    for (auto it = copper.crbegin(); it != copper.crend(); ++it) {
        _layers.push_back(Layer{*it, color(it->toStdString(), QColor(0xc8, 0x34, 0x34)), true, true, {}});
    }
    for (const auto& polygon : geometry.copper) {
        Layer* layer = findLayer(QString::fromStdString(polygon.copperLayerName));
        if (!layer) continue;
        // Hole loops follow the outline they cut (see libkicad::CopperPolygon).
        if (polygon.loop.hole && !layer->shapes.empty()) {
            layer->shapes.back().path.addPath(loopPath(polygon.loop));
            continue;
        }
        Shape shape;
        shape.path = loopPath(polygon.loop);
        shape.path.setFillRule(Qt::OddEvenFill);
        shape.net = QString::fromStdString(polygon.netName);
        shape.footprint = QString::fromStdString(polygon.footprintRef);
        shape.pad = QString::fromStdString(polygon.padNumber);
        shape.zone = polygon.zone;
        layer->shapes.push_back(std::move(shape));
    }
    for (auto& layer : _layers) {
        for (auto& shape : layer.shapes) shape.bounds = shape.path.boundingRect();
    }
    addSilk(frontSilk, geometry.frontSilkscreen);
    _layers.push_back(Layer{"Edge.Cuts", color("Edge.Cuts", QColor(0xd0, 0xd2, 0xcd)), false, true, {}});

    // Start like KiCad's own default view: the outer copper and silkscreen.
    for (auto& layer : _layers) {
        layer.visible = !layer.copper || layer.name == copper.value(0) || layer.name == copper.value(copper.size() - 1);
    }
    _fitPending = true;
    fitToBoard();
}

QStringList BoardView::drawableLayers() const {
    QStringList names;
    for (auto it = _layers.crbegin(); it != _layers.crend(); ++it) names << it->name; // top first
    return names;
}

QColor BoardView::layerColor(const QString& name) const {
    const Layer* layer = findLayer(name);
    return layer ? layer->color : QColor();
}

void BoardView::setLayerVisible(const QString& name, bool visible) {
    if (Layer* layer = findLayer(name)) {
        layer->visible = visible;
        update();
    }
}

bool BoardView::isLayerVisible(const QString& name) const {
    const Layer* layer = findLayer(name);
    return layer && layer->visible;
}

void BoardView::setHighlightedNets(const QSet<QString>& nets) {
    _highlighted = nets;
    update();
}

void BoardView::setExcitation(const QString& footprint, const QString& pad) {
    _excitationFootprint = footprint;
    _excitationPad = pad;
    update();
}

void BoardView::setSelection(const BoardPick& pick) {
    _selection = pick;
    update();
}

BoardView::Layer* BoardView::findLayer(const QString& name) {
    for (auto& layer : _layers) {
        if (layer.name == name) return &layer;
    }
    return nullptr;
}

const BoardView::Layer* BoardView::findLayer(const QString& name) const {
    for (const auto& layer : _layers) {
        if (layer.name == name) return &layer;
    }
    return nullptr;
}

void BoardView::fitToBoard() {
    if (_boardBounds.isEmpty() || width() <= 0 || height() <= 0) return;
    _fitPending = false;
    const double margin = 24.0;
    _scale = std::min((width() - 2 * margin) / _boardBounds.width(), (height() - 2 * margin) / _boardBounds.height());
    _center = _boardBounds.center();
    update();
}

QTransform BoardView::worldToScreen() const {
    // Board coordinates are Y-up millimetres; the screen is Y-down pixels.
    QTransform transform;
    transform.translate(width() / 2.0, height() / 2.0);
    transform.scale(_scale, -_scale);
    transform.translate(-_center.x(), -_center.y());
    return transform;
}

void BoardView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kBackground);
    if (!_board) {
        painter.setPen(QColor(0x88, 0x8c, 0x94));
        painter.drawText(rect(), Qt::AlignCenter, tr("No board loaded"));
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setTransform(worldToScreen());
    painter.setPen(Qt::NoPen);
    painter.setBrush(kSubstrate);
    painter.drawPath(_outline);

    const QRectF visible = worldToScreen().inverted().mapRect(QRectF(rect()));
    const bool dimOthers = !_highlighted.isEmpty();
    for (const Layer& layer : _layers) {
        if (!layer.visible) continue;
        if (layer.name == "Edge.Cuts") {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(layer.color, 0)); // cosmetic: one pixel at any zoom
            painter.drawPath(_outline);
            painter.setPen(Qt::NoPen);
            continue;
        }
        QColor normal = layer.color;
        QColor zoneColor = layer.color;
        zoneColor.setAlphaF(layer.color.alphaF() * 0.55);
        QColor dimmed = layer.color;
        dimmed.setAlphaF(layer.color.alphaF() * 0.12);
        for (const Shape& shape : layer.shapes) {
            if (!shape.bounds.intersects(visible)) continue;
            const bool lit = !layer.copper || !dimOthers || _highlighted.contains(shape.net);
            painter.setBrush(!lit ? dimmed : shape.zone ? zoneColor : normal);
            painter.drawPath(shape.path);
        }
    }

    // The selected net or pad, and the simulation's excitation, outlined on top.
    auto outlineShapes = [&](auto&& matches, const QColor& color, double width) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(color, width / _scale));
        for (const Layer& layer : _layers) {
            if (!layer.visible || !layer.copper) continue;
            for (const Shape& shape : layer.shapes) {
                if (matches(shape) && shape.bounds.intersects(visible)) painter.drawPath(shape.path);
            }
        }
    };
    if (!_selection.footprint.isEmpty()) {
        outlineShapes([&](const Shape& s) { return s.footprint == _selection.footprint && s.pad == _selection.pad; },
                      QColor(255, 255, 255), 2.0);
    } else if (!_selection.net.isEmpty()) {
        outlineShapes([&](const Shape& s) { return s.net == _selection.net; }, QColor(255, 255, 255, 200), 1.5);
    }
    if (!_excitationFootprint.isEmpty()) {
        outlineShapes([&](const Shape& s) { return s.footprint == _excitationFootprint && s.pad == _excitationPad; },
                      QColor(0x4c, 0xd9, 0x64), 2.5);
    }
}

void BoardView::resizeEvent(QResizeEvent*) {
    if (_fitPending) fitToBoard();
}

void BoardView::wheelEvent(QWheelEvent* event) {
    if (!_board) return;
    const QPointF cursor = event->position();
    const QPointF before = worldToScreen().inverted().map(cursor);
    const double factor = std::pow(1.0015, event->angleDelta().y());
    _scale = std::clamp(_scale * factor, 0.05, 20000.0);
    const QPointF after = worldToScreen().inverted().map(cursor);
    _center += before - after; // Keep the point under the cursor fixed.
    update();
}

void BoardView::mousePressEvent(QMouseEvent* event) {
    _pressPosition = event->position();
    _lastDragPosition = event->position();
    _dragging = false;
}

void BoardView::mouseMoveEvent(QMouseEvent* event) {
    if (!(event->buttons() & (Qt::LeftButton | Qt::MiddleButton | Qt::RightButton))) return;
    if (!_dragging && (event->position() - _pressPosition).manhattanLength() > 4) _dragging = true;
    if (_dragging) {
        const QPointF delta = event->position() - _lastDragPosition;
        _center += QPointF(-delta.x() / _scale, delta.y() / _scale);
        _lastDragPosition = event->position();
        update();
    }
}

void BoardView::mouseReleaseEvent(QMouseEvent* event) {
    if (!_dragging && event->button() == Qt::LeftButton) pick(event->position());
    _dragging = false;
}

void BoardView::mouseDoubleClickEvent(QMouseEvent*) {
    fitToBoard();
}

void BoardView::pick(const QPointF& screenPoint) {
    if (!_board) return;
    const QPointF point = worldToScreen().inverted().map(screenPoint);
    // A pad on any visible layer first, topmost first: pads are the small targets ports need,
    // while a track or zone over one can be picked anywhere else along it.
    for (auto layer = _layers.crbegin(); layer != _layers.crend(); ++layer) {
        if (!layer->visible || !layer->copper) continue;
        for (const Shape& shape : layer->shapes) {
            if (shape.pad.isEmpty() || !shape.bounds.contains(point) || !shape.path.contains(point)) continue;
            _selection = BoardPick{shape.net, layer->name, shape.footprint, shape.pad};
            update();
            emit picked(_selection);
            return;
        }
    }
    // Otherwise the topmost visible copper, tracks winning over the zones beneath them.
    for (auto layer = _layers.crbegin(); layer != _layers.crend(); ++layer) {
        if (!layer->visible || !layer->copper) continue;
        const Shape* hit = nullptr;
        for (const Shape& shape : layer->shapes) {
            if (!shape.bounds.contains(point) || !shape.path.contains(point)) continue;
            if (!hit || (!shape.pad.isEmpty() && hit->pad.isEmpty()) || (hit->zone && !shape.zone)) hit = &shape;
        }
        if (hit) {
            _selection = BoardPick{hit->net, layer->name, hit->footprint, hit->pad};
            update();
            emit picked(_selection);
            return;
        }
    }
    _selection = {};
    update();
    emit picked(_selection);
}

} // namespace kiems_gui
