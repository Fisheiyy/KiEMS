#include "GeometryView.hpp"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace kiems_gui {

namespace {

const char* kVertexShader = R"(
attribute highp vec3 position;
attribute lowp vec4 color;
uniform highp mat4 transform;
varying lowp vec4 fragmentColor;
void main() {
    gl_Position = transform * vec4(position, 1.0);
    fragmentColor = color;
}
)";

const char* kFragmentShader = R"(
varying lowp vec4 fragmentColor;
void main() {
    gl_FragColor = fragmentColor;
}
)";

QIcon swatch(const QColor& color) {
    QPixmap pixmap(12, 12);
    pixmap.fill(QColor(color.red(), color.green(), color.blue()));
    return QIcon(pixmap);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// GeometryCanvas

GeometryCanvas::GeometryCanvas(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumSize(200, 200);
    setFocusPolicy(Qt::ClickFocus);
    QSurfaceFormat surface = format();
    surface.setDepthBufferSize(24);
    surface.setSamples(4);
    setFormat(surface);
}

GeometryCanvas::~GeometryCanvas() {
    if (!context()) return;
    makeCurrent();
    releaseBuffers();
    delete _program;
    doneCurrent();
}

void GeometryCanvas::setScene(std::shared_ptr<const GeometryScene> scene, bool refit) {
    _scene = std::move(scene);
    _visible.clear();
    if (_scene) {
        for (const SceneGroup& group : _scene->groups) _visible.push_back(group.visibleByDefault);
    }
    _dirty = true;
    if (refit) fit();
    update();
}

void GeometryCanvas::setGroupVisible(int group, bool visible) {
    if (group < 0 || group >= static_cast<int>(_visible.size())) return;
    _visible[static_cast<std::size_t>(group)] = visible;
    update();
}

void GeometryCanvas::fit() {
    if (!_scene) return;
    _target = (_scene->boundsMin + _scene->boundsMax) / 2;
    _radius = std::max(0.5f, (_scene->boundsMax - _scene->boundsMin).length() / 2);
    _distance = _radius / std::sin(qDegreesToRadians(15.0f)) * 1.1f;
    _yaw = 0;
    _pitch = 55;
    update();
}

void GeometryCanvas::initializeGL() {
    initializeOpenGLFunctions();
    _program = new QOpenGLShaderProgram;
    _program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    _program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    _program->bindAttributeLocation("position", 0);
    _program->bindAttributeLocation("color", 1);
    if (!_program->link()) qWarning("Geometry view shaders: %s", qPrintable(_program->log()));
    _dirty = true;
}

void GeometryCanvas::resizeGL(int, int) {}

void GeometryCanvas::releaseBuffers() {
    for (GpuGroup& group : _gpu) {
        group.triangles.destroy();
        group.lines.destroy();
    }
    _gpu.clear();
}

void GeometryCanvas::upload() {
    releaseBuffers();
    _dirty = false;
    if (!_scene) return;
    _gpu.resize(_scene->groups.size());
    for (std::size_t i = 0; i < _scene->groups.size(); ++i) {
        const SceneGroup& group = _scene->groups[i];
        GpuGroup& gpu = _gpu[i];
        const auto fill = [](QOpenGLBuffer& buffer, const std::vector<SceneVertex>& vertices) {
            if (vertices.empty()) return;
            buffer.create();
            buffer.bind();
            buffer.allocate(vertices.data(), static_cast<int>(vertices.size() * sizeof(SceneVertex)));
            buffer.release();
        };
        fill(gpu.triangles, group.triangles);
        fill(gpu.lines, group.lines);
        gpu.triangleCount = static_cast<int>(group.triangles.size());
        gpu.lineCount = static_cast<int>(group.lines.size());
    }
}

QVector3D GeometryCanvas::eye() const {
    const float yaw = qDegreesToRadians(_yaw), pitch = qDegreesToRadians(_pitch);
    const QVector3D direction(std::cos(pitch) * std::sin(yaw), -std::cos(pitch) * std::cos(yaw), std::sin(pitch));
    return _target + direction * _distance;
}

QMatrix4x4 GeometryCanvas::viewMatrix() const {
    QMatrix4x4 view;
    view.lookAt(eye(), _target, QVector3D(0, 0, 1));
    return view;
}

QMatrix4x4 GeometryCanvas::projectionMatrix() const {
    QMatrix4x4 projection;
    const float aspect = height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0f;
    const float nearPlane = std::max(0.01f, _distance - 4 * _radius);
    projection.perspective(30.0f, aspect, std::min(nearPlane, _distance * 0.05f), _distance + 8 * _radius);
    return projection;
}

void GeometryCanvas::draw(QOpenGLBuffer& buffer, int vertices, unsigned mode) {
    if (vertices == 0 || !buffer.isCreated()) return;
    buffer.bind();
    _program->enableAttributeArray(0);
    _program->enableAttributeArray(1);
    _program->setAttributeBuffer(0, GL_FLOAT, offsetof(SceneVertex, x), 3, sizeof(SceneVertex));
    _program->setAttributeBuffer(1, GL_FLOAT, offsetof(SceneVertex, r), 4, sizeof(SceneVertex));
    glDrawArrays(mode, 0, vertices);
    buffer.release();
}

void GeometryCanvas::paintGL() {
    if (_dirty) upload();
    glClearColor(0.11f, 0.11f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!_scene || !_program || !_program->isLinked()) return;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    _program->bind();
    _program->setUniformValue("transform", projectionMatrix() * viewMatrix());

    // Solid copper first, pushed back a little so the lines drawn on its surfaces stay visible.
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 1.0f);
    for (std::size_t i = 0; i < _gpu.size(); ++i) {
        if (_visible[i] && !_scene->groups[i].translucent) draw(_gpu[i].triangles, _gpu[i].triangleCount, GL_TRIANGLES);
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (std::size_t i = 0; i < _gpu.size(); ++i) {
        if (_visible[i]) draw(_gpu[i].lines, _gpu[i].lineCount, GL_LINES);
    }
    // Then what's see-through, without hiding what's behind it.
    glDepthMask(GL_FALSE);
    for (std::size_t i = 0; i < _gpu.size(); ++i) {
        if (_visible[i] && _scene->groups[i].translucent) draw(_gpu[i].triangles, _gpu[i].triangleCount, GL_TRIANGLES);
    }
    glDepthMask(GL_TRUE);
    _program->release();
}

void GeometryCanvas::mousePressEvent(QMouseEvent* event) {
    _lastMouse = event->position().toPoint();
}

void GeometryCanvas::mouseMoveEvent(QMouseEvent* event) {
    const QPoint position = event->position().toPoint();
    const QPoint delta = position - _lastMouse;
    _lastMouse = position;
    if (event->buttons() & Qt::LeftButton) {
        _yaw -= delta.x() * 0.4f;
        _pitch = std::clamp(_pitch + delta.y() * 0.4f, -89.0f, 89.0f);
    } else if (event->buttons() & (Qt::RightButton | Qt::MiddleButton)) {
        // Moves the target with the cursor, in the plane facing the viewer.
        const QVector3D forward = (_target - eye()).normalized();
        const QVector3D right = QVector3D::crossProduct(forward, QVector3D(0, 0, 1)).normalized();
        const QVector3D up = QVector3D::crossProduct(right, forward);
        const float perPixel = 2 * _distance * std::tan(qDegreesToRadians(15.0f)) / std::max(1, height());
        _target += (-right * delta.x() + up * delta.y()) * perPixel;
    } else {
        return;
    }
    update();
}

void GeometryCanvas::mouseDoubleClickEvent(QMouseEvent*) {
    fit();
}

void GeometryCanvas::wheelEvent(QWheelEvent* event) {
    const float steps = event->angleDelta().y() / 120.0f;
    _distance = std::clamp(_distance * std::pow(0.85f, steps), _radius * 0.02f, _radius * 200.0f);
    update();
}

// ---------------------------------------------------------------------------------------------
// GeometryView

GeometryView::GeometryView(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* bar = new QWidget;
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(10, 6, 10, 6);
    _title = new QLabel;
    QFont titleFont = _title->font();
    titleFont.setBold(true);
    _title->setFont(titleFont);
    _status = new QLabel;
    _status->setEnabled(false);
    _build = new QPushButton(tr("Build Geometry"));
    _build->setToolTip(tr("Cut the board to this simulation and mesh it, as a run would, without running it."));
    // Wide enough for either label, so the bar doesn't shift when it changes.
    _build->setText(tr("Rebuild Geometry"));
    _build->setMinimumWidth(_build->sizeHint().width());
    _fit = new QPushButton(tr("Fit"));
    _fit->setToolTip(tr("Frame the board again (or double-click it)."));
    barLayout->addWidget(_title);
    barLayout->addWidget(_status, 1);
    barLayout->addWidget(_build);
    barLayout->addWidget(_fit);
    layout->addWidget(bar);

    _stack = new QStackedWidget;
    _message = new QLabel;
    _message->setAlignment(Qt::AlignCenter);
    _message->setWordWrap(true);
    _canvasPage = new QWidget;
    auto* canvasLayout = new QVBoxLayout(_canvasPage);
    canvasLayout->setContentsMargins(0, 0, 0, 0);
    _canvas = new GeometryCanvas;
    canvasLayout->addWidget(_canvas);
    // The legend and mesh size float over the view's corners, as the board page's layer list does.
    _legend = new QListWidget(_canvasPage);
    _legend->setFrameShape(QFrame::NoFrame);
    _legend->setSelectionMode(QAbstractItemView::NoSelection);
    _legend->setFocusPolicy(Qt::NoFocus);
    _legend->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    {
        // Through the palette rather than a style sheet, which would also strip the checkboxes' frames.
        QPalette dark = _legend->palette();
        dark.setColor(QPalette::Base, QColor(28, 28, 31, 210));
        dark.setColor(QPalette::Text, QColor(232, 232, 232));
        _legend->setPalette(dark);
    }
    _gridSummary = new QLabel(_canvasPage);
    _gridSummary->setStyleSheet("QLabel { background: rgba(28, 28, 31, 200); color: #e8e8e8; padding: 3px 6px; "
                                "border-radius: 4px; }");
    _stack->addWidget(_message);
    _stack->addWidget(_canvasPage);
    layout->addWidget(_stack, 1);

    connect(_build, &QPushButton::clicked, this, &GeometryView::buildRequested);
    connect(_fit, &QPushButton::clicked, _canvas, &GeometryCanvas::fit);
    connect(_legend, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        const bool visible = item->checkState() == Qt::Checked;
        _visibility.insert(item->text(), visible);
        _canvas->setGroupVisible(_legend->row(item), visible);
    });
    updateStatus();
}

void GeometryView::showSimulation(const QString& simulation, const QString& geometryPath,
                                  const GeometryContext& context) {
    _refit = _refit || simulation != _simulation;
    _simulation = simulation;
    _path = geometryPath;
    _title->setText(simulation);
    const unsigned generation = ++_generation;
    if (!QFileInfo::exists(geometryPath)) {
        _scene.reset();
        _error.clear();
        updateStatus();
        return;
    }
    _status->setText(tr("Loading..."));
    auto* watcher = new QFutureWatcher<Loaded>(this);
    connect(watcher, &QFutureWatcher<Loaded>::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != _generation) return; // a later simulation was asked for meanwhile
        loaded(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run([geometryPath, context] {
        Loaded result;
        result.path = geometryPath;
        const SimulationGeometry geometry = SimulationGeometry::load(geometryPath);
        if (!geometry.isValid()) {
            result.error = geometry.error;
            return result;
        }
        result.scene = std::make_shared<GeometryScene>(buildGeometryScene(geometry, context));
        return result;
    }));
}

void GeometryView::loaded(const Loaded& result) {
    _error = result.error;
    _scene = result.scene;
    if (_scene) {
        _canvas->setScene(_scene, _refit);
        _refit = false;
        populateLegend();
    }
    updateStatus();
    emit sceneLoaded();
}

void GeometryView::setBuilding(bool building) {
    _building = building;
    updateStatus();
}

void GeometryView::populateLegend() {
    const QSignalBlocker blocker(_legend);
    _legend->clear();
    for (int i = 0; i < static_cast<int>(_scene->groups.size()); ++i) {
        const SceneGroup& group = _scene->groups[static_cast<std::size_t>(i)];
        auto* item = new QListWidgetItem(swatch(group.swatch), group.name, _legend);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        const bool visible = _visibility.value(group.name, group.visibleByDefault);
        item->setCheckState(visible ? Qt::Checked : Qt::Unchecked);
        _canvas->setGroupVisible(i, visible);
    }
    _gridSummary->setText(_scene->gridSummary);
    _gridSummary->setVisible(!_scene->gridSummary.isEmpty());
    placeOverlays();
}

void GeometryView::placeOverlays() {
    const int rows = _legend->count();
    const int rowHeight = rows > 0 ? _legend->sizeHintForRow(0) : 0;
    _legend->setFixedSize(_legend->sizeHintForColumn(0) + 16,
                          std::min(rows * rowHeight + 6, std::max(rowHeight * 3, _canvasPage->height() - 60)));
    _legend->move(8, 8);
    _gridSummary->adjustSize();
    _gridSummary->move(8, _canvasPage->height() - _gridSummary->height() - 8);
    _legend->raise();
    _gridSummary->raise();
}

void GeometryView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    placeOverlays();
}

void GeometryView::updateStatus() {
    const bool haveScene = static_cast<bool>(_scene);
    _build->setText(haveScene ? tr("Rebuild Geometry") : tr("Build Geometry"));
    _build->setEnabled(!_building && !_simulation.isEmpty());
    _fit->setEnabled(haveScene);
    if (_building) {
        _status->setText(tr("Building the geometry..."));
    } else if (!_error.isEmpty()) {
        _status->setText(_error);
    } else if (haveScene) {
        _status->setText(tr("As the simulation will see it"));
    } else {
        _status->setText(QString());
    }
    if (haveScene) {
        _stack->setCurrentWidget(_canvasPage);
    } else {
        _message->setText(_building ? tr("Building the geometry...")
                          : !_error.isEmpty()
                              ? _error
                              : tr("This simulation's geometry hasn't been built yet.\n\nBuild Geometry cuts the board "
                                   "to the simulated area and meshes it, as a run would, without running it."));
        _stack->setCurrentWidget(_message);
    }
}

} // namespace kiems_gui
