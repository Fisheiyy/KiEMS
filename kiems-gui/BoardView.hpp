// The 2D board view: copper per layer in the board's own layer colours, silkscreen and the board
// outline, from libkicad's geometry. Wheel zooms about the cursor, dragging pans, double-click
// fits the board. A click picks the topmost visible pad or net under the cursor.
#pragma once

#include "BoardModel.hpp"

#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QWidget>

#include <memory>
#include <vector>

namespace kiems_gui {

struct BoardPick {
    QString net;
    QString layer;
    QString footprint; // empty unless a pad was picked
    QString pad;
};

class BoardView : public QWidget {
    Q_OBJECT

public:
    explicit BoardView(QWidget* parent = nullptr);

    void setBoard(std::shared_ptr<const BoardData> board);
    /// Layers drawn, by name (copper layers, "F.Silkscreen", "B.Silkscreen", "Edge.Cuts").
    QStringList drawableLayers() const;
    QColor layerColor(const QString& layer) const;
    void setLayerVisible(const QString& layer, bool visible);
    bool isLayerVisible(const QString& layer) const;
    /// Nets drawn at full strength; when non-empty, every other net is dimmed.
    void setHighlightedNets(const QSet<QString>& nets);
    void setExcitation(const QString& footprint, const QString& pad);
    void setSelection(const BoardPick& pick);
    void fitToBoard();

signals:
    void picked(const BoardPick& pick);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Shape {
        QPainterPath path; // board millimetres, Y up; holes cut with the odd-even rule
        QRectF bounds;
        QString net;
        QString footprint;
        QString pad;
        bool zone = false;
    };
    struct Layer {
        QString name;
        QColor color;
        bool copper = false;
        bool visible = true;
        std::vector<Shape> shapes;
    };

    QTransform worldToScreen() const;
    Layer* findLayer(const QString& name);
    const Layer* findLayer(const QString& name) const;
    void pick(const QPointF& screenPoint);

    std::shared_ptr<const BoardData> _board;
    std::vector<Layer> _layers; // drawing order: bottom first
    QPainterPath _outline;
    QRectF _boardBounds;
    QSet<QString> _highlighted;
    BoardPick _selection;
    QString _excitationFootprint;
    QString _excitationPad;

    double _scale = 1.0;      // pixels per millimetre
    QPointF _center;          // board millimetres at the widget's centre
    bool _fitPending = true;
    QPointF _pressPosition;
    QPointF _lastDragPosition;
    bool _dragging = false;
};

} // namespace kiems_gui
