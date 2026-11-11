// A simulation's Geometry page: what the pipeline will simulate, in 3D -- the board cut to the
// simulated area, layer by layer at its height in the stackup, its vias and stitching vias, its
// ports, the solder mask, and the FDTD mesh (see GeometryScene.hpp). Drag to orbit, right- or
// middle-drag to pan, scroll to zoom, double-click to frame the board again. A legend over the
// view's corner shows and hides each part. Until the geometry has been built, the page offers to
// build it (kiems -g), which takes seconds rather than a whole run.
#pragma once

#include "GeometryScene.hpp"

#include <QFutureWatcher>
#include <QHash>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QPoint>
#include <QWidget>

#include <memory>
#include <vector>

class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;

namespace kiems_gui {

class GeometryCanvas : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    explicit GeometryCanvas(QWidget* parent = nullptr);
    ~GeometryCanvas() override;

    void setScene(std::shared_ptr<const GeometryScene> scene, bool refit);
    void setGroupVisible(int group, bool visible);
    /// Frames the simulated board, seen from above and in front.
    void fit();

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct GpuGroup {
        QOpenGLBuffer triangles{QOpenGLBuffer::VertexBuffer};
        QOpenGLBuffer lines{QOpenGLBuffer::VertexBuffer};
        int triangleCount = 0;
        int lineCount = 0;
    };

    void upload();
    void releaseBuffers();
    QMatrix4x4 viewMatrix() const;
    QMatrix4x4 projectionMatrix() const;
    QVector3D eye() const;
    void draw(QOpenGLBuffer& buffer, int vertices, unsigned mode);

    std::shared_ptr<const GeometryScene> _scene;
    std::vector<bool> _visible;
    std::vector<GpuGroup> _gpu;
    bool _dirty = false;
    QOpenGLShaderProgram* _program = nullptr;

    QVector3D _target;
    float _distance = 50;
    float _yaw = 0;    // degrees about Z
    float _pitch = 55; // degrees above the board's plane
    float _radius = 10;
    QPoint _lastMouse;
};

class GeometryView : public QWidget {
    Q_OBJECT

public:
    explicit GeometryView(QWidget* parent = nullptr);

    /// Shows `simulation`'s geometry from `geometryPath` (its geometry.json), with what `context`
    /// adds. Loads in the background; the camera stays put when it's the simulation already shown.
    void showSimulation(const QString& simulation, const QString& geometryPath, const GeometryContext& context);
    /// Whether that simulation's geometry is being built right now.
    void setBuilding(bool building);

    GeometryCanvas* canvas() const { return _canvas; }
    /// The scene on show; null until one has loaded.
    std::shared_ptr<const GeometryScene> scene() const { return _scene; }

signals:
    /// The user asked for the geometry to be built (or built again).
    void buildRequested();
    /// A scene finished loading (for tests).
    void sceneLoaded();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Loaded {
        QString path;
        std::shared_ptr<const GeometryScene> scene;
        QString error;
    };

    void loaded(const Loaded& result);
    void populateLegend();
    void placeOverlays();
    void updateStatus();

    QLabel* _title;
    QLabel* _status;
    QPushButton* _build;
    QPushButton* _fit;
    QStackedWidget* _stack;
    QLabel* _message;
    QWidget* _canvasPage;
    GeometryCanvas* _canvas;
    QListWidget* _legend;
    QLabel* _gridSummary;

    QString _simulation;
    QString _path;
    bool _building = false;
    bool _refit = true;
    QString _error;
    std::shared_ptr<const GeometryScene> _scene;
    /// Each legend entry's visibility as last set, by name, so it survives a reload.
    QHash<QString, bool> _visibility;
    unsigned _generation = 0;
};

} // namespace kiems_gui
