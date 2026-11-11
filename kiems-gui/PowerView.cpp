#include "PowerView.hpp"

#include "Document.hpp"
#include "QuantityEdit.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace kiems_gui {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kColorBarWidth = 90;
constexpr int kPointRole = Qt::UserRole;
constexpr int kLayerRole = Qt::UserRole + 1;

/// Interpolates `stops` (evenly spaced over [0, 1]) at t.
QColor ramp(const std::initializer_list<QColor>& stops, double t) {
    const std::vector<QColor> colors(stops);
    t = std::clamp(t, 0.0, 1.0) * static_cast<double>(colors.size() - 1);
    const std::size_t i = std::min(colors.size() - 2, static_cast<std::size_t>(t));
    const double f = t - static_cast<double>(i);
    const QColor& a = colors[i];
    const QColor& b = colors[i + 1];
    return QColor::fromRgbF(static_cast<float>(a.redF() + f * (b.redF() - a.redF())),
                            static_cast<float>(a.greenF() + f * (b.greenF() - a.greenF())),
                            static_cast<float>(a.blueF() + f * (b.blueF() - a.blueF())));
}

/// Viridis, for voltage: low is dark blue, high is yellow.
QColor viridis(double t) {
    return ramp({QColor(68, 1, 84), QColor(59, 82, 139), QColor(33, 145, 140), QColor(94, 201, 98), QColor(253, 231, 37)}, t);
}

/// Inferno, for current density: low is black, high is pale yellow.
QColor inferno(double t) {
    return ramp({QColor(0, 0, 4), QColor(87, 16, 110), QColor(188, 55, 84), QColor(249, 142, 9), QColor(252, 255, 164)}, t);
}

QString formatValue(double value, PowerCanvas::Quantity quantity) {
    if (std::isnan(value)) return QObject::tr("not connected");
    return quantity == PowerCanvas::Quantity::Voltage ? QString("%1 V").arg(value, 0, 'f', 4)
                                                      : QString("%1 A/mm²").arg(value, 0, 'g', 3);
}

QPointF padCentre(const kiems::dc::Terminal& terminal) {
    double xMin = std::numeric_limits<double>::infinity(), xMax = -xMin, yMin = xMin, yMax = -xMin;
    for (const Cu::Polygon& loop : terminal.pad) {
        for (const Cu::Position& p : loop) {
            xMin = std::min(xMin, p.x());
            xMax = std::max(xMax, p.x());
            yMin = std::min(yMin, p.y());
            yMax = std::max(yMax, p.y());
        }
    }
    return QPointF((xMin + xMax) / 2, (yMin + yMax) / 2);
}

QPushButton* smallButton(const QString& text, const QString& toolTip) {
    auto* button = new QPushButton(text);
    button->setToolTip(toolTip);
    button->setFixedWidth(28);
    return button;
}

QTableWidget* pinTable(const QStringList& headers) {
    auto* table = new QTableWidget(0, static_cast<int>(headers.size()));
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setMinimumHeight(110);
    table->setMaximumHeight(170);
    return table;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// PowerCanvas

PowerCanvas::PowerCanvas(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumSize(300, 240);
    setAutoFillBackground(false);
}

void PowerCanvas::setPreview(std::vector<QPainterPath> copper, std::vector<PadShape> pads) {
    const bool first = _previewCopper.empty();
    _previewCopper = std::move(copper);
    _pads = std::move(pads);
    if (first && !showingResult()) fit();
    _mapValid = false;
    update();
}

void PowerCanvas::setRoles(QHash<QString, Role> roles) {
    _roles = std::move(roles);
    update();
}

bool PowerCanvas::showingResult() const {
    return _run && _run->ok();
}

void PowerCanvas::setRun(std::shared_ptr<const PowerRun> run, bool refit) {
    _run = std::move(run);
    if (refit) _autoFit = true;
    if (_run && !_run->returnResult) _onReturn = false;
    if (_run && _layer >= static_cast<int>(result().layers.size())) _layer = 0;
    updateRange();
    if (refit) fit();
    _mapValid = false;
    update();
}

void PowerCanvas::setLayer(int layer, bool onReturn) {
    _layer = layer;
    _onReturn = onReturn && _run && _run->returnResult;
    updateRange();
    _mapValid = false;
    update();
}

const kiems::dc::Result& PowerCanvas::result() const {
    return _onReturn && _run->returnResult ? *_run->returnResult : _run->result;
}

const kiems::dc::Problem& PowerCanvas::problem() const {
    return _onReturn && _run->returnProblem ? *_run->returnProblem : _run->problem;
}

void PowerCanvas::setQuantity(Quantity quantity) {
    _quantity = quantity;
    updateRange();
    _mapValid = false;
    update();
}

void PowerCanvas::setLogScale(bool on) {
    _logScale = on;
    _mapValid = false;
    update();
}

void PowerCanvas::mark(std::optional<QPointF> point) {
    _mark = point;
    update();
}

QRectF PowerCanvas::mapArea() const {
    return QRectF(0, 0, std::max(10, width() - kColorBarWidth), height());
}

void PowerCanvas::fit() {
    double xMin = std::numeric_limits<double>::infinity(), xMax = -xMin, yMin = xMin, yMax = -xMin;
    if (!showingResult()) {
        // The board's copper for the rail, before it's run.
        QRectF bounds;
        for (const QPainterPath& path : _previewCopper) bounds = bounds.united(path.boundingRect());
        if (bounds.isEmpty()) return;
        const QRectF area = mapArea();
        _scale = 0.92 * std::min(area.width() / std::max(bounds.width(), 1e-3), area.height() / std::max(bounds.height(), 1e-3));
        _origin = QPointF(area.center().x() - bounds.center().x() * _scale, area.center().y() + bounds.center().y() * _scale);
        _mapValid = false;
        update();
        return;
    }
    // The rail and its return together, so switching between them keeps the view.
    std::vector<const kiems::dc::LayerResult*> layers;
    for (const auto& layer : _run->result.layers) layers.push_back(&layer);
    if (_run->returnResult) {
        for (const auto& layer : _run->returnResult->layers) layers.push_back(&layer);
    }
    for (const kiems::dc::LayerResult* layerPointer : layers) {
        const auto& layer = *layerPointer;
        for (const Cu::Position& p : layer.nodes) {
            xMin = std::min(xMin, p.x());
            xMax = std::max(xMax, p.x());
            yMin = std::min(yMin, p.y());
            yMax = std::max(yMax, p.y());
        }
    }
    if (!std::isfinite(xMin)) return;
    const QRectF area = mapArea();
    _scale = 0.92 * std::min(area.width() / std::max(xMax - xMin, 1e-3), area.height() / std::max(yMax - yMin, 1e-3));
    _origin = QPointF(area.center().x() - (xMin + xMax) / 2 * _scale, area.center().y() + (yMin + yMax) / 2 * _scale);
    _mapValid = false;
    update();
}

QPointF PowerCanvas::toScreen(double x, double y) const {
    return QPointF(_origin.x() + x * _scale, _origin.y() - y * _scale);
}

QPointF PowerCanvas::toBoard(const QPointF& screen) const {
    return QPointF((screen.x() - _origin.x()) / _scale, (_origin.y() - screen.y()) / _scale);
}

void PowerCanvas::updateRange() {
    _low = std::numeric_limits<double>::infinity();
    _high = -_low;
    if (!_run || !_run->ok()) return;
    for (const auto& layer : result().layers) {
        if (_quantity == Quantity::Voltage) {
            for (double v : layer.voltage) {
                if (std::isnan(v)) continue;
                _low = std::min(_low, v);
                _high = std::max(_high, v);
            }
        } else {
            _low = 0;
            _high = std::max(_high, layer.maxCurrentDensity);
        }
    }
    if (!(_high > _low)) _high = _low + 1e-9;
}

double PowerCanvas::valueOf(std::size_t triangle) const {
    const auto& layer = result().layers[static_cast<std::size_t>(_layer)];
    if (_quantity == Quantity::CurrentDensity) {
        return std::hypot(layer.currentDensity[triangle][0], layer.currentDensity[triangle][1]);
    }
    const auto& t = layer.triangles[triangle];
    return (layer.voltage[static_cast<std::size_t>(t[0])] + layer.voltage[static_cast<std::size_t>(t[1])] +
            layer.voltage[static_cast<std::size_t>(t[2])]) / 3;
}

QColor PowerCanvas::colorOf(double value) const {
    if (std::isnan(value)) return QColor(90, 90, 96);
    if (_quantity == Quantity::Voltage) return viridis((value - _low) / (_high - _low));
    if (_logScale) {
        // Three decades below the highest.
        const double floor = _high / 1000;
        return inferno(std::log10(std::max(value, floor) / floor) / 3);
    }
    return inferno(value / _high);
}

void PowerCanvas::renderMap() {
    const qreal ratio = devicePixelRatioF();
    _map = QImage((QSizeF(size()) * ratio).toSize(), QImage::Format_ARGB32_Premultiplied);
    _map.setDevicePixelRatio(ratio);
    _map.fill(QColor(28, 28, 31));
    _mapValid = true;
    if (!showingResult()) {
        // The rail's copper and pads from the board, in greys, until it's run.
        if (_layer >= static_cast<int>(_previewCopper.size())) return;
        QPainter painter(&_map);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setClipRect(mapArea());
        painter.setTransform(QTransform(_scale, 0, 0, -_scale, _origin.x(), _origin.y()));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(104, 104, 112));
        painter.drawPath(_previewCopper[static_cast<std::size_t>(_layer)]);
        painter.setBrush(QColor(150, 150, 160));
        for (const PadShape& pad : _pads) {
            if (pad.layer == _layer) painter.drawPath(pad.path);
        }
        return;
    }
    if (_layer >= static_cast<int>(result().layers.size())) return;
    QPainter painter(&_map);
    painter.setClipRect(mapArea());
    const auto& layer = result().layers[static_cast<std::size_t>(_layer)];
    for (std::size_t e = 0; e < layer.triangles.size(); ++e) {
        const auto& t = layer.triangles[e];
        const QPointF points[3] = {toScreen(layer.nodes[static_cast<std::size_t>(t[0])].x(), layer.nodes[static_cast<std::size_t>(t[0])].y()),
                                   toScreen(layer.nodes[static_cast<std::size_t>(t[1])].x(), layer.nodes[static_cast<std::size_t>(t[1])].y()),
                                   toScreen(layer.nodes[static_cast<std::size_t>(t[2])].x(), layer.nodes[static_cast<std::size_t>(t[2])].y())};
        const QColor color = colorOf(valueOf(e));
        // A hairline of the same colour closes the seams between neighbours.
        painter.setPen(QPen(color, 0));
        painter.setBrush(color);
        painter.drawPolygon(points, 3);
    }
}

void PowerCanvas::paintEvent(QPaintEvent*) {
    if (!_mapValid || _map.size() != (QSizeF(size()) * devicePixelRatioF()).toSize()) renderMap();
    QPainter painter(this);
    painter.drawImage(0, 0, _map);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!showingResult()) {
        // The pins chosen so far, and how to choose more.
        for (const PadShape& pad : _pads) {
            const auto role = _roles.value(pinKey(pad.footprint, pad.pin), Role::None);
            if (pad.layer != _layer || role == Role::None) continue;
            const QPointF at = toScreen(pad.path.boundingRect().center().x(), pad.path.boundingRect().center().y());
            painter.setPen(QPen(Qt::white, 1.5));
            painter.setBrush(role == Role::Source ? QColor(60, 200, 90) : QColor(255, 140, 30));
            painter.drawRect(QRectF(at - QPointF(5, 5), QSizeF(10, 10)));
        }
        painter.setPen(QColor(200, 200, 200));
        QString text;
        if (_run) text = tr("The analysis couldn't run:\n%1").arg(_run->error);
        else if (_previewCopper.empty()) text = tr("Run the analysis to see its voltage and current density here.");
        if (!text.isEmpty()) painter.drawText(mapArea(), Qt::AlignCenter | Qt::TextWordWrap, text);
        if (!_previewCopper.empty()) {
            painter.drawText(mapArea().adjusted(10, 0, -10, -8), Qt::AlignBottom | Qt::AlignLeft,
                             tr("Click a pad to make it a source or a load; Run to solve."));
        }
        return;
    }
    painter.setClipRect(mapArea());
    // Vias, sized by their current; then the pins: sources green, loads orange.
    double most = 0;
    for (const auto& barrel : result().barrels) most = std::max(most, barrel.current);
    for (std::size_t b = 0; b < result().barrels.size(); ++b) {
        const auto& barrel = result().barrels[b];
        if (!barrel.connected) continue;
        const QPointF at = toScreen(problem().barrels[b].x, problem().barrels[b].y);
        const double radius = 2.5 + (most > 0 ? 6 * std::sqrt(barrel.current / most) : 0);
        painter.setPen(QPen(QColor(255, 255, 255, 200), 1));
        painter.setBrush(barrel.current > 0.01 * most ? QColor(214, 170, 76, 220) : Qt::NoBrush);
        painter.drawEllipse(at, radius, radius);
    }
    for (const auto& terminal : problem().terminals) {
        if (terminal.layer != _layer) continue;
        const QPointF at = toScreen(padCentre(terminal).x(), padCentre(terminal).y());
        const bool source = terminal.kind == kiems::dc::Terminal::Kind::Source;
        painter.setPen(QPen(Qt::white, 1.5));
        painter.setBrush(source ? QColor(60, 200, 90) : QColor(255, 140, 30));
        painter.drawRect(QRectF(at - QPointF(5, 5), QSizeF(10, 10)));
        painter.setPen(Qt::white);
        // Beside the marker, on whichever side has room.
        const QString name = QString::fromStdString(terminal.name);
        const double textWidth = painter.fontMetrics().horizontalAdvance(name);
        const bool left = at.x() + 8 + textWidth > mapArea().right();
        // A dark outline keeps it readable over the palest colours.
        const QPointF textAt = at + QPointF(left ? -8 - textWidth : 8, -6);
        painter.setPen(QColor(0, 0, 0, 200));
        for (const QPointF& offset : {QPointF(-1, 0), QPointF(1, 0), QPointF(0, -1), QPointF(0, 1)}) {
            painter.drawText(textAt + offset, name);
        }
        painter.setPen(Qt::white);
        painter.drawText(textAt, name);
    }
    if (_mark) {
        const QPointF at = toScreen(_mark->x(), _mark->y());
        painter.setPen(QPen(QColor(255, 60, 60), 2.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(at, 14, 14);
        painter.drawLine(at + QPointF(-20, 0), at + QPointF(-9, 0));
        painter.drawLine(at + QPointF(9, 0), at + QPointF(20, 0));
    }
    painter.setClipping(false);

    // The colour bar.
    const QRectF bar(width() - kColorBarWidth + 12, 20, 16, std::max(40, height() - 40));
    for (int y = 0; y < static_cast<int>(bar.height()); ++y) {
        const double t = 1 - y / bar.height();
        QColor color;
        if (_quantity == Quantity::Voltage) color = viridis(t);
        else color = inferno(t);
        painter.setPen(color);
        painter.drawLine(QPointF(bar.left(), bar.top() + y), QPointF(bar.right(), bar.top() + y));
    }
    painter.setPen(QColor(200, 200, 200));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(bar);
    const auto label = [&](double t) {
        double value;
        if (_quantity == Quantity::Voltage) value = _low + t * (_high - _low);
        else if (_logScale) value = _high / 1000 * std::pow(10.0, 3 * t);
        else value = t * _high;
        const QString text = _quantity == Quantity::Voltage ? QString::number(value, 'f', 4) : QString::number(value, 'g', 3);
        painter.drawText(QPointF(bar.right() + 4, bar.bottom() - t * bar.height() + 4), text);
    };
    for (double t : {0.0, 0.5, 1.0}) label(t);
    painter.drawText(QRectF(bar.left() - 8, 2, kColorBarWidth, 16), Qt::AlignLeft,
                     _quantity == Quantity::Voltage ? tr("V") : tr("A/mm²"));
}

void PowerCanvas::resizeEvent(QResizeEvent*) {
    _mapValid = false;
    if (_autoFit) fit();
}

void PowerCanvas::wheelEvent(QWheelEvent* event) {
    const QPointF at = event->position();
    const QPointF board = toBoard(at);
    _scale *= std::pow(1.2, event->angleDelta().y() / 120.0);
    _autoFit = false;
    _origin = QPointF(at.x() - board.x() * _scale, at.y() + board.y() * _scale);
    _mapValid = false;
    update();
}

void PowerCanvas::mousePressEvent(QMouseEvent* event) {
    _lastMouse = event->position().toPoint();
    _pressedAt = _lastMouse;
}

void PowerCanvas::mouseReleaseEvent(QMouseEvent* event) {
    // A click rather than a drag, on one of the rail's pads on the layer shown.
    if (event->button() != Qt::LeftButton || (event->position().toPoint() - _pressedAt).manhattanLength() > 3) return;
    if (_onReturn && showingResult()) return;
    const QPointF at = toBoard(event->position());
    for (const PadShape& pad : _pads) {
        if (pad.layer == _layer && pad.path.contains(at)) {
            emit padClicked(pad.footprint, pad.pin, event->globalPosition().toPoint());
            return;
        }
    }
}

void PowerCanvas::mouseMoveEvent(QMouseEvent* event) {
    const QPoint position = event->position().toPoint();
    if (event->buttons() != Qt::NoButton) {
        _origin += position - _lastMouse;
        _lastMouse = position;
        _autoFit = false;
        _mapValid = false;
        update();
        return;
    }
    if (!_run || !_run->ok() || _layer >= static_cast<int>(result().layers.size())) return;
    // What's under the cursor.
    const QPointF p = toBoard(event->position());
    const auto& layer = result().layers[static_cast<std::size_t>(_layer)];
    for (std::size_t e = 0; e < layer.triangles.size(); ++e) {
        const auto& t = layer.triangles[e];
        const Cu::Position& a = layer.nodes[static_cast<std::size_t>(t[0])];
        const Cu::Position& b = layer.nodes[static_cast<std::size_t>(t[1])];
        const Cu::Position& c = layer.nodes[static_cast<std::size_t>(t[2])];
        const double det = (b.y() - c.y()) * (a.x() - c.x()) + (c.x() - b.x()) * (a.y() - c.y());
        if (det == 0) continue;
        const double l0 = ((b.y() - c.y()) * (p.x() - c.x()) + (c.x() - b.x()) * (p.y() - c.y())) / det;
        const double l1 = ((c.y() - a.y()) * (p.x() - c.x()) + (a.x() - c.x()) * (p.y() - c.y())) / det;
        const double l2 = 1 - l0 - l1;
        if (l0 < 0 || l1 < 0 || l2 < 0) continue;
        const double voltage = l0 * layer.voltage[static_cast<std::size_t>(t[0])] + l1 * layer.voltage[static_cast<std::size_t>(t[1])] +
                               l2 * layer.voltage[static_cast<std::size_t>(t[2])];
        const double density = std::hypot(layer.currentDensity[e][0], layer.currentDensity[e][1]);
        QToolTip::showText(event->globalPosition().toPoint(),
                           tr("(%1, %2) mm\n%3\n%4")
                               .arg(p.x(), 0, 'f', 2)
                               .arg(p.y(), 0, 'f', 2)
                               .arg(formatValue(voltage, Quantity::Voltage), formatValue(density, Quantity::CurrentDensity)),
                           this);
        return;
    }
    QToolTip::hideText();
}

void PowerCanvas::mouseDoubleClickEvent(QMouseEvent*) {
    _autoFit = true;
    fit();
}

// ---------------------------------------------------------------------------------------------
// PowerView

PowerView::PowerView(Document* document, QWidget* parent) : QWidget(parent), _document(document) {
    using Kind = QuantityEdit::Kind;
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* split = new QSplitter;
    outer->addWidget(split);

    // --- The map and the results
    auto* left = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(10, 6, 10, 6);
    _title = new QLabel;
    QFont titleFont = _title->font();
    titleFont.setBold(true);
    _title->setFont(titleFont);
    _status = new QLabel;
    _status->setEnabled(false);
    _layer = new QComboBox;
    _layer->setToolTip(tr("The copper layer the map shows."));
    _quantity = new QComboBox;
    _quantity->addItems({tr("Voltage"), tr("Current density")});
    _logScale = new QCheckBox(tr("Log scale"));
    _logScale->setChecked(true);
    _logScale->setToolTip(tr("Show current density over three decades, so the paths that carry little still show."));
    _runButton = new QPushButton(tr("Run"));
    _runButton->setToolTip(tr("Solve the voltage and current across the net's copper."));
    bar->addWidget(_title);
    bar->addWidget(_status, 1);
    bar->addWidget(_layer);
    bar->addWidget(_quantity);
    bar->addWidget(_logScale);
    bar->addWidget(_runButton);
    leftLayout->addLayout(bar);
    auto* vertical = new QSplitter(Qt::Vertical);
    _canvas = new PowerCanvas;
    vertical->addWidget(_canvas);
    _resultTabs = new QTabWidget;
    _pins = new QTreeWidget;
    _pins->setObjectName("pins");
    _pins->setHeaderLabels({tr("Pin"), tr("Role"), tr("Voltage"), tr("From the source"), tr("Ground"), tr("Across"),
                            tr("Current")});
    _pins->setRootIsDecorated(false);
    _vias = new QTreeWidget;
    _vias->setObjectName("vias");
    _vias->setHeaderLabels({tr("Via"), tr("Current"), tr("Resistance")});
    _vias->setRootIsDecorated(false);
    _problems = new QListWidget;
    _problems->setObjectName("problems");
    _resultTabs->addTab(_pins, tr("Pins"));
    _resultTabs->addTab(_vias, tr("Vias"));
    _resultTabs->addTab(_problems, tr("Problems"));
    vertical->addWidget(_resultTabs);
    vertical->setStretchFactor(0, 3);
    vertical->setStretchFactor(1, 1);
    leftLayout->addWidget(vertical, 1);
    split->addWidget(left);

    // --- The setup
    auto* setup = new QWidget;
    auto* setupLayout = new QVBoxLayout(setup);
    auto* rail = new QGroupBox(tr("Rail"));
    auto* railForm = new QFormLayout(rail);
    _name = new QLineEdit;
    _net = new QComboBox;
    _net->setEditable(true);
    _net->setInsertPolicy(QComboBox::NoInsert);
    _net->setToolTip(tr("The net whose copper carries the rail's current."));
    _returnNet = new QComboBox;
    _returnNet->setEditable(true);
    _returnNet->setInsertPolicy(QComboBox::NoInsert);
    _returnNet->setToolTip(tr("The net the current returns on, solved with the rail so each load's voltage is the "
                              "voltage across it. Empty to analyse the rail alone."));
    railForm->addRow(tr("Name:"), _name);
    railForm->addRow(tr("Net:"), _net);
    railForm->addRow(tr("Return net:"), _returnNet);
    setupLayout->addWidget(rail);

    auto* pins = new QGroupBox(tr("Pins"));
    auto* pinsLayout = new QVBoxLayout(pins);
    _sources = pinTable({tr("Part"), tr("Pin"), tr("Volts")});
    _sources->setToolTip(tr("Where the rail is supplied: a regulator's output pin, held at its voltage."));
    _loads = pinTable({tr("Part"), tr("Pin"), tr("Amps"), tr("Min V")});
    _loads->setToolTip(tr("What draws from the rail, and how much. Min V, if set, flags a pin left below it."));
    auto* addSource = smallButton("+", tr("Add a source pin."));
    auto* removeSource = smallButton(QString::fromUtf8("\xE2\x88\x92"), tr("Remove the selected source pin."));
    auto* addLoad = smallButton("+", tr("Add a load pin."));
    auto* removeLoad = smallButton(QString::fromUtf8("\xE2\x88\x92"), tr("Remove the selected load pin."));
    const auto withButtons = [](const QString& title, QWidget* table, QPushButton* add, QPushButton* remove) {
        auto* column = new QVBoxLayout;
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(title), 1);
        row->addWidget(add);
        row->addWidget(remove);
        column->addLayout(row);
        column->addWidget(table);
        return column;
    };
    pinsLayout->addLayout(withButtons(tr("Sources"), _sources, addSource, removeSource));
    pinsLayout->addLayout(withButtons(tr("Loads"), _loads, addLoad, removeLoad));
    auto* fillRow = new QHBoxLayout;
    _totalCurrent = new QuantityEdit(Kind::Number);
    _totalCurrent->setValue(1.0);
    _totalCurrent->setMinimum(0);
    _totalCurrent->setToolTip(tr("The current the loads share when they're filled in from KiCad's pin types."));
    auto* fill = new QPushButton(tr("Fill from Pin Types"));
    fill->setToolTip(tr("Make every power output pin on the net a source, at the voltage its name implies, and every "
                        "power input pin a load, sharing the total current."));
    fillRow->addWidget(new QLabel(tr("Total A:")));
    fillRow->addWidget(_totalCurrent);
    fillRow->addWidget(fill, 1);
    pinsLayout->addLayout(fillRow);
    setupLayout->addWidget(pins);

    auto* solver = new QGroupBox(tr("Solver"));
    auto* solverForm = new QFormLayout(solver);
    _temperature = new QuantityEdit(Kind::Number);
    _temperature->setToolTip(tr("The copper's temperature, in °C: its resistance rises 0.39 % per degree above 20."));
    _plating = new QuantityEdit(Kind::Length);
    _plating->setToolTip(tr("Via barrel plating; KiCad doesn't record it. IPC-6012 class 3 is 25 µm."));
    _maxElement = new QuantityEdit(Kind::Length);
    _maxElement->setToolTip(tr("The largest mesh element. Narrow copper gets small elements regardless; this "
                               "governs pours. 0.5 mm is within about 1 % of a fine mesh."));
    _loadPads = new QComboBox;
    _loadPads->addItems({tr("Tied together"), tr("Drawn across the pad")});
    _loadPads->setToolTip(tr("Whether a load's pad is one point, as its soldered lead ties it, or draws its current "
                             "evenly from the pad's copper, which then conducts like any other (a little more drop "
                             "where the rail runs through pads)."));
    solverForm->addRow(tr("Temperature (°C):"), _temperature);
    solverForm->addRow(tr("Via plating:"), _plating);
    solverForm->addRow(tr("Largest element:"), _maxElement);
    solverForm->addRow(tr("Load pads:"), _loadPads);
    setupLayout->addWidget(solver);

    auto* limits = new QGroupBox(tr("Limits"));
    auto* limitsForm = new QFormLayout(limits);
    _outerLimit = new QuantityEdit(Kind::Number);
    _innerLimit = new QuantityEdit(Kind::Number);
    _viaLimit = new QuantityEdit(Kind::Number);
    for (QuantityEdit* field : {_outerLimit, _innerLimit, _viaLimit}) {
        field->setAllowEmpty(true, tr("None"));
        field->setMinimum(0);
    }
    limitsForm->addRow(tr("Outer layers (A/mm²):"), _outerLimit);
    limitsForm->addRow(tr("Inner layers (A/mm²):"), _innerLimit);
    limitsForm->addRow(tr("Per via (A):"), _viaLimit);
    setupLayout->addWidget(limits);
    setupLayout->addStretch();
    auto* scroll = new QScrollArea;
    scroll->setWidget(setup);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(340);
    split->addWidget(scroll);
    split->setStretchFactor(0, 1);

    // --- Wiring
    connect(_runButton, &QPushButton::clicked, this, &PowerView::run);
    connect(_canvas, &PowerCanvas::padClicked, this, [this](const QString& footprint, const QString& pin, const QPoint& at) {
        const auto analysis = current();
        if (!analysis) return;
        const auto has = [&](const std::vector<kiems::dc::PinValue>& list) {
            return std::any_of(list.begin(), list.end(), [&](const kiems::dc::PinValue& p) {
                return p.footprint == footprint.toStdString() && p.pin == pin.toStdString();
            });
        };
        const PowerCanvas::Role role = has(analysis->sources) ? PowerCanvas::Role::Source
                                       : has(analysis->loads) ? PowerCanvas::Role::Load
                                                              : PowerCanvas::Role::None;
        QMenu menu(this);
        menu.addSection(tr("%1 pin %2").arg(footprint, pin));
        const auto add = [&](const QString& text, PowerCanvas::Role choice) {
            QAction* action = menu.addAction(text, this, [this, footprint, pin, choice] { setPinRole(footprint, pin, choice); });
            action->setCheckable(true);
            action->setChecked(role == choice);
        };
        add(tr("Source"), PowerCanvas::Role::Source);
        add(tr("Load"), PowerCanvas::Role::Load);
        add(tr("Not in the analysis"), PowerCanvas::Role::None);
        menu.exec(at);
    });
    connect(_layer, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (_updating || index < 0) return;
        const auto run = currentRun();
        const int railLayers = run && run->ok() ? static_cast<int>(run->result.layers.size()) : 0;
        _canvas->setLayer(index < railLayers ? index : index - railLayers, index >= railLayers);
    });
    connect(_quantity, &QComboBox::currentIndexChanged, this, [this](int index) {
        _canvas->setQuantity(index == 1 ? PowerCanvas::Quantity::CurrentDensity : PowerCanvas::Quantity::Voltage);
        _logScale->setEnabled(index == 1);
    });
    _logScale->setEnabled(false);
    connect(_logScale, &QCheckBox::toggled, _canvas, &PowerCanvas::setLogScale);
    const auto pointAt = [this](const QVariant& point, const QVariant& layer) {
        if (layer.isValid() && layer.toInt() >= 0) {
            _layer->setCurrentIndex(layer.toInt());
        }
        _canvas->mark(point.isValid() ? std::optional(point.toPointF()) : std::nullopt);
    };
    connect(_pins, &QTreeWidget::currentItemChanged, this, [pointAt](QTreeWidgetItem* item) {
        if (item) pointAt(item->data(0, kPointRole), item->data(0, kLayerRole));
    });
    connect(_vias, &QTreeWidget::currentItemChanged, this, [pointAt](QTreeWidgetItem* item) {
        if (item) pointAt(item->data(0, kPointRole), item->data(0, kLayerRole));
    });
    connect(_problems, &QListWidget::currentItemChanged, this, [pointAt](QListWidgetItem* item) {
        if (item) pointAt(item->data(kPointRole), item->data(kLayerRole));
    });

    connect(_name, &QLineEdit::editingFinished, this, [this] {
        const std::string name = _name->text().trimmed().toStdString();
        if (name.empty()) return;
        // Its last run follows it to its new name.
        if (const auto analysis = current(); analysis && analysis->name != name) {
            const auto run = _runs.take(QString::fromStdString(analysis->name));
            if (run) _runs.insert(QString::fromStdString(name), run);
        }
        edit([&](kiems::dc::AnalysisConfig& a) { a.name = name; });
    });
    const auto commitNet = [this] {
        const std::string net = _net->currentText().trimmed().toStdString();
        if (!net.empty()) edit([&](kiems::dc::AnalysisConfig& a) { a.net = net; });
    };
    connect(_net, &QComboBox::activated, this, commitNet);
    connect(_net->lineEdit(), &QLineEdit::editingFinished, this, commitNet);
    const auto commitReturn = [this] {
        if (_updating) return;
        const std::string net = _returnNet->currentText().trimmed().toStdString();
        edit([&](kiems::dc::AnalysisConfig& a) { a.referenceNet = net; });
    };
    connect(_returnNet, &QComboBox::activated, this, commitReturn);
    connect(_returnNet->lineEdit(), &QLineEdit::editingFinished, this, commitReturn);
    connect(_sources, &QTableWidget::cellChanged, this, [this] { if (!_updating) commitTables(); });
    connect(_loads, &QTableWidget::cellChanged, this, [this] { if (!_updating) commitTables(); });
    connect(addSource, &QPushButton::clicked, this, [this] {
        edit([](kiems::dc::AnalysisConfig& a) { a.sources.push_back({"", "", a.sources.empty() ? 0.0 : a.sources.back().value, std::nullopt}); });
    });
    connect(addLoad, &QPushButton::clicked, this, [this] {
        edit([](kiems::dc::AnalysisConfig& a) { a.loads.push_back({"", "", 0.1, std::nullopt}); });
    });
    connect(removeSource, &QPushButton::clicked, this, [this] {
        const int row = _sources->currentRow();
        edit([row](kiems::dc::AnalysisConfig& a) {
            if (row >= 0 && row < static_cast<int>(a.sources.size())) a.sources.erase(a.sources.begin() + row);
        });
    });
    connect(removeLoad, &QPushButton::clicked, this, [this] {
        const int row = _loads->currentRow();
        edit([row](kiems::dc::AnalysisConfig& a) {
            if (row >= 0 && row < static_cast<int>(a.loads.size())) a.loads.erase(a.loads.begin() + row);
        });
    });
    connect(fill, &QPushButton::clicked, this, &PowerView::fillFromPinTypes);
    connect(_temperature, &QuantityEdit::valueEdited, this, [this](std::optional<double> v) {
        if (v) edit([&](kiems::dc::AnalysisConfig& a) { a.options.temperatureC = *v; });
    });
    connect(_plating, &QuantityEdit::valueEdited, this, [this](std::optional<double> v) {
        if (v && *v > 0) edit([&](kiems::dc::AnalysisConfig& a) { a.options.platingMm = *v / 1000; });
    });
    connect(_maxElement, &QuantityEdit::valueEdited, this, [this](std::optional<double> v) {
        if (v && *v > 0) edit([&](kiems::dc::AnalysisConfig& a) { a.options.maxElementMm = *v / 1000; });
    });
    connect(_loadPads, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!_updating) edit([&](kiems::dc::AnalysisConfig& a) { a.options.equipotentialLoadPads = index == 0; });
    });
    connect(_outerLimit, &QuantityEdit::valueEdited, this, [this](std::optional<double> v) {
        edit([&](kiems::dc::AnalysisConfig& a) { a.limits.outerCurrentDensity = v; });
    });
    connect(_innerLimit, &QuantityEdit::valueEdited, this, [this](std::optional<double> v) {
        edit([&](kiems::dc::AnalysisConfig& a) { a.limits.innerCurrentDensity = v; });
    });
    connect(_viaLimit, &QuantityEdit::valueEdited, this, [this](std::optional<double> v) {
        edit([&](kiems::dc::AnalysisConfig& a) { a.limits.viaCurrent = v; });
    });
    connect(_document, &Document::changed, this, [this] {
        refreshSetup();
        updateStatus();
    });
    refreshSetup();
    refreshResults();
}

void PowerView::setBoard(std::shared_ptr<const BoardData> board) {
    _board = std::move(board);
    _previewNet.clear(); // rebuilt from the new board
    _updating = true;
    const QString text = _net->currentText();
    const QString returnText = _returnNet->currentText();
    _net->clear();
    _returnNet->clear();
    if (_board) {
        _net->addItems(powerNetCandidates(*_board));
        _returnNet->addItem(QString());
        _returnNet->addItems(powerNetCandidates(*_board));
    }
    _net->setCurrentText(text);
    _returnNet->setCurrentText(returnText);
    _updating = false;
    refreshPreview();
    if (!currentRun()) refreshResults();
    updateStatus();
}

void PowerView::refreshPreview() {
    const auto analysis = current();
    if (!_board || !analysis) return;
    // The pins' roles, every time; the copper only when the net changes.
    QHash<QString, PowerCanvas::Role> roles;
    for (const auto& pin : analysis->sources) {
        roles.insert(PowerCanvas::pinKey(QString::fromStdString(pin.footprint), QString::fromStdString(pin.pin)),
                     PowerCanvas::Role::Source);
    }
    for (const auto& pin : analysis->loads) {
        roles.insert(PowerCanvas::pinKey(QString::fromStdString(pin.footprint), QString::fromStdString(pin.pin)),
                     PowerCanvas::Role::Load);
    }
    _canvas->setRoles(roles);
    const QString net = QString::fromStdString(analysis->net);
    if (net == _previewNet) return;
    _previewNet = net;
    std::vector<QPainterPath> copper(static_cast<std::size_t>(_board->copperLayers.size()));
    for (QPainterPath& path : copper) path.setFillRule(Qt::WindingFill);
    std::vector<PowerCanvas::PadShape> pads;
    for (const libkicad::CopperPolygon& polygon : _board->geometry.copper) {
        if (QString::fromStdString(polygon.netName) != net) continue;
        const int layer = static_cast<int>(_board->copperLayers.indexOf(QString::fromStdString(polygon.copperLayerName)));
        if (layer < 0 || polygon.loop.pointsMm.size() < 3) continue;
        QPolygonF points;
        for (const auto& [x, y] : polygon.loop.pointsMm) points << QPointF(x, y);
        // Outlines counter-clockwise and holes clockwise, so winding fill cuts the holes.
        double area = 0;
        for (int i = 0; i < points.size(); ++i) {
            const QPointF& a = points[i];
            const QPointF& b = points[(i + 1) % points.size()];
            area += a.x() * b.y() - b.x() * a.y();
        }
        if ((area > 0) == polygon.loop.hole) std::reverse(points.begin(), points.end());
        points << points.first();
        copper[static_cast<std::size_t>(layer)].addPolygon(points);
        if (!polygon.footprintRef.empty() && !polygon.loop.hole) {
            QPainterPath pad;
            pad.addPolygon(points);
            pads.push_back({QString::fromStdString(polygon.footprintRef), QString::fromStdString(polygon.padNumber), layer, pad});
        }
    }
    _canvas->setPreview(std::move(copper), std::move(pads));
}

void PowerView::setPinRole(const QString& footprint, const QString& pin, PowerCanvas::Role role) {
    const std::string part = footprint.toStdString(), number = pin.toStdString();
    edit([&](kiems::dc::AnalysisConfig& a) {
        const auto same = [&](const kiems::dc::PinValue& p) { return p.footprint == part && p.pin == number; };
        // A pin keeps its value if it already had this role; a new one takes its list's last
        // value -- the rail's voltage, or a load's current.
        const auto source = std::find_if(a.sources.begin(), a.sources.end(), same);
        const auto load = std::find_if(a.loads.begin(), a.loads.end(), same);
        if ((role == PowerCanvas::Role::Source && source != a.sources.end()) ||
            (role == PowerCanvas::Role::Load && load != a.loads.end())) {
            return;
        }
        const double volts = !a.sources.empty() ? a.sources.back().value : kiems::dc::voltageFromNetName(a.net);
        const double amps = !a.loads.empty() ? a.loads.back().value : 0.1;
        std::erase_if(a.sources, same);
        std::erase_if(a.loads, same);
        if (role == PowerCanvas::Role::Source) a.sources.push_back({part, number, volts, std::nullopt});
        else if (role == PowerCanvas::Role::Load) a.loads.push_back({part, number, amps, std::nullopt});
    });
}

std::optional<kiems::dc::AnalysisConfig> PowerView::current() const {
    return powerAnalysis(*_document, _index);
}

std::shared_ptr<const PowerRun> PowerView::currentRun() const {
    const auto analysis = current();
    return analysis ? _runs.value(QString::fromStdString(analysis->name)) : nullptr;
}

void PowerView::showAnalysis(int index) {
    const bool changed = index != _index;
    _index = index;
    refreshSetup();
    if (changed) {
        _canvas->mark(std::nullopt);
        refreshResults();
    }
    updateStatus();
}

void PowerView::edit(const std::function<void(kiems::dc::AnalysisConfig&)>& change) {
    auto analysis = current();
    if (!analysis) return;
    change(*analysis);
    setPowerAnalysis(*_document, _index, *analysis);
}

void PowerView::commitTables() {
    const auto cell = [](QTableWidget* table, int row, int column) {
        const QTableWidgetItem* item = table->item(row, column);
        return item ? item->text().trimmed() : QString();
    };
    edit([&](kiems::dc::AnalysisConfig& a) {
        std::vector<kiems::dc::PinValue> sources, loads;
        for (int row = 0; row < _sources->rowCount(); ++row) {
            bool ok = false;
            const double volts = cell(_sources, row, 2).toDouble(&ok);
            sources.push_back({cell(_sources, row, 0).toStdString(), cell(_sources, row, 1).toStdString(),
                               ok ? volts : (row < static_cast<int>(a.sources.size()) ? a.sources[static_cast<std::size_t>(row)].value : 0.0),
                               std::nullopt});
        }
        for (int row = 0; row < _loads->rowCount(); ++row) {
            bool ok = false;
            const double amps = cell(_loads, row, 2).toDouble(&ok);
            bool minimumOk = false;
            const double minimum = cell(_loads, row, 3).toDouble(&minimumOk);
            loads.push_back({cell(_loads, row, 0).toStdString(), cell(_loads, row, 1).toStdString(),
                             ok ? amps : (row < static_cast<int>(a.loads.size()) ? a.loads[static_cast<std::size_t>(row)].value : 0.0),
                             minimumOk ? std::optional(minimum) : std::nullopt});
        }
        a.sources = std::move(sources);
        a.loads = std::move(loads);
    });
}

void PowerView::fillFromPinTypes() {
    const auto analysis = current();
    if (!analysis || !_board) return;
    const kiems::dc::AnalysisConfig filled =
        newPowerAnalysis(*_board, QString::fromStdString(analysis->net), _totalCurrent->value().value_or(1.0));
    edit([&](kiems::dc::AnalysisConfig& a) {
        a.sources = filled.sources;
        a.loads = filled.loads;
    });
}

void PowerView::refreshSetup() {
    refreshPreview();
    const auto analysis = current();
    setEnabled(_document->isOpen());
    _updating = true;
    _title->setText(analysis ? QString::fromStdString(analysis->name) : QString());
    if (analysis) {
        if (!_name->hasFocus()) _name->setText(QString::fromStdString(analysis->name));
        if (!_net->lineEdit()->hasFocus()) _net->setCurrentText(QString::fromStdString(analysis->net));
        if (!_returnNet->lineEdit()->hasFocus()) _returnNet->setCurrentText(QString::fromStdString(analysis->referenceNet));
        // Not while a cell is being typed into.
        const QWidget* focus = QApplication::focusWidget();
        const auto editing = [focus](QTableWidget* table) { return focus && (focus == table || table->isAncestorOf(focus)); };
        if (!editing(_sources)) {
            _sources->setRowCount(static_cast<int>(analysis->sources.size()));
            for (int row = 0; row < _sources->rowCount(); ++row) {
                const auto& pin = analysis->sources[static_cast<std::size_t>(row)];
                _sources->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(pin.footprint)));
                _sources->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(pin.pin)));
                _sources->setItem(row, 2, new QTableWidgetItem(QString::number(pin.value)));
            }
        }
        if (!editing(_loads)) {
            _loads->setRowCount(static_cast<int>(analysis->loads.size()));
            for (int row = 0; row < _loads->rowCount(); ++row) {
                const auto& pin = analysis->loads[static_cast<std::size_t>(row)];
                _loads->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(pin.footprint)));
                _loads->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(pin.pin)));
                _loads->setItem(row, 2, new QTableWidgetItem(QString::number(pin.value)));
                _loads->setItem(row, 3, new QTableWidgetItem(pin.minimumVoltage ? QString::number(*pin.minimumVoltage) : QString()));
            }
        }
        _temperature->refreshValue(analysis->options.temperatureC);
        _plating->refreshValue(analysis->options.platingMm * 1000);
        _maxElement->refreshValue(analysis->options.maxElementMm * 1000);
        _loadPads->setCurrentIndex(analysis->options.equipotentialLoadPads ? 0 : 1);
        _outerLimit->refreshValue(analysis->limits.outerCurrentDensity);
        _innerLimit->refreshValue(analysis->limits.innerCurrentDensity);
        _viaLimit->refreshValue(analysis->limits.viaCurrent);
    }
    _updating = false;
}

void PowerView::refreshResults() {
    const auto run = currentRun();
    _canvas->setRun(run, true);
    {
        const QSignalBlocker blocker(_layer);
        _layer->clear();
        if (run && run->ok()) {
            for (const auto& layer : run->result.layers) _layer->addItem(QString::fromStdString(layer.name));
            if (run->returnResult) {
                for (const auto& layer : run->returnResult->layers) {
                    _layer->addItem(QString::fromStdString(run->analysis.referenceNet + ": " + layer.name));
                }
            }
        } else if (_board) {
            _layer->addItems(_board->copperLayers); // the board's copper, before a run
        }
        _layer->setCurrentIndex(_layer->count() > 0 ? 0 : -1);
    }
    _canvas->setLayer(0, false);
    _pins->clear();
    _vias->clear();
    _problems->clear();
    if (!run) return;
    if (!run->ok()) {
        _problems->addItem(run->error);
        _resultTabs->setCurrentWidget(_problems);
        return;
    }
    double sourceVolts = 0;
    int sources = 0;
    for (const auto& terminal : run->result.terminals) {
        if (terminal.kind == kiems::dc::Terminal::Kind::Source && terminal.connected) {
            sourceVolts += terminal.voltage;
            ++sources;
        }
    }
    if (sources > 0) sourceVolts /= sources;
    const std::map<std::string, double> grounds =
        run->returnResult ? kiems::dc::partVoltages(*run->returnResult) : std::map<std::string, double>();
    const int railLayers = static_cast<int>(run->result.layers.size());
    for (std::size_t t = 0; t < run->result.terminals.size(); ++t) {
        const auto& terminal = run->result.terminals[t];
        const bool source = terminal.kind == kiems::dc::Terminal::Kind::Source;
        const auto ground = grounds.find(terminal.name.substr(0, terminal.name.find(" pin ")));
        const bool haveGround = terminal.connected && ground != grounds.end();
        auto* item = new QTreeWidgetItem(
            _pins, {QString::fromStdString(terminal.name), source ? tr("Source") : tr("Load"),
                    terminal.connected ? QString("%1 V").arg(terminal.voltage, 0, 'f', 4) : tr("not connected"),
                    terminal.connected && !source ? QString("%1 mV").arg((terminal.voltage - sourceVolts) * 1e3, 0, 'f', 2) : QString(),
                    haveGround ? QString("%1 mV").arg(ground->second * 1e3, 0, 'f', 2) : QString(),
                    haveGround ? QString("%1 V").arg(terminal.voltage - ground->second, 0, 'f', 4) : QString(),
                    QString("%1 A").arg(terminal.current, 0, 'g', 4)});
        if (t < run->problem.terminals.size()) {
            item->setData(0, kPointRole, padCentre(run->problem.terminals[t]));
            item->setData(0, kLayerRole, run->problem.terminals[t].layer);
        }
    }
    if (run->returnResult) {
        for (std::size_t t = 0; t < run->returnResult->terminals.size(); ++t) {
            const auto& terminal = run->returnResult->terminals[t];
            auto* item = new QTreeWidgetItem(
                _pins, {QString::fromStdString(terminal.name),
                        tr("Return (%1)").arg(QString::fromStdString(run->analysis.referenceNet)),
                        terminal.connected ? QString("%1 mV").arg(terminal.voltage * 1e3, 0, 'f', 3) : tr("not connected"),
                        QString(), QString(), QString(), QString("%1 A").arg(std::abs(terminal.current), 0, 'g', 4)});
            item->setData(0, kPointRole, padCentre(run->returnProblem->terminals[t]));
            item->setData(0, kLayerRole, railLayers + run->returnProblem->terminals[t].layer);
        }
    }
    std::vector<std::size_t> order(run->result.barrels.size());
    std::iota(order.begin(), order.end(), std::size_t(0));
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return run->result.barrels[a].current > run->result.barrels[b].current; });
    for (std::size_t b : order) {
        const auto& barrel = run->result.barrels[b];
        if (!barrel.connected) continue;
        auto* item = new QTreeWidgetItem(_vias, {QString::fromStdString(barrel.name), QString("%1 A").arg(barrel.current, 0, 'g', 4),
                                                 QString("%1 mΩ").arg(barrel.resistance * 1e3, 0, 'f', 3)});
        item->setData(0, kPointRole, QPointF(run->problem.barrels[b].x, run->problem.barrels[b].y));
        item->setData(0, kLayerRole, -1);
    }
    if (run->returnResult) {
        std::vector<std::size_t> returnOrder(run->returnResult->barrels.size());
        std::iota(returnOrder.begin(), returnOrder.end(), std::size_t(0));
        std::sort(returnOrder.begin(), returnOrder.end(), [&](std::size_t x, std::size_t y) {
            return run->returnResult->barrels[x].current > run->returnResult->barrels[y].current;
        });
        for (std::size_t b : returnOrder) {
            const auto& barrel = run->returnResult->barrels[b];
            if (!barrel.connected || barrel.current < 1e-6) continue;
            auto* item = new QTreeWidgetItem(
                _vias, {QString::fromStdString(run->analysis.referenceNet + ": " + barrel.name),
                        QString("%1 A").arg(barrel.current, 0, 'g', 4), QString("%1 m\u03A9").arg(barrel.resistance * 1e3, 0, 'f', 3)});
            item->setData(0, kPointRole, QPointF(run->returnProblem->barrels[b].x, run->returnProblem->barrels[b].y));
            item->setData(0, kLayerRole, -1);
        }
    }
    for (const auto& violation : run->violations) {
        auto* item = new QListWidgetItem(QString::fromStdString(violation.message), _problems);
        item->setForeground(QColor(0xd0, 0x3a, 0x2f));
        item->setData(kPointRole, QPointF(violation.x, violation.y));
        item->setData(kLayerRole, violation.layer < 0 ? -1 : violation.layer + (violation.onReturn ? railLayers : 0));
    }
    for (const auto& warning : run->result.warnings) _problems->addItem(QString::fromStdString(warning));
    for (const auto& note : run->notes) _problems->addItem(QString::fromStdString(note));
    if (run->returnResult) {
        for (const auto& warning : run->returnResult->warnings) {
            _problems->addItem(QString::fromStdString(run->analysis.referenceNet + ": " + warning));
        }
    }
    for (QTreeWidget* tree : {_pins, _vias}) {
        for (int c = 0; c < tree->columnCount(); ++c) tree->resizeColumnToContents(c);
    }
    _resultTabs->setTabText(2, _problems->count() > 0 ? tr("Problems (%1)").arg(_problems->count()) : tr("Problems"));
}

bool PowerView::canRun() const {
    return !_running && _board && current().has_value();
}

void PowerView::run() {
    const auto analysis = current();
    if (!analysis || !_board || _running) return;
    _running = true;
    updateStatus();
    auto* watcher = new QFutureWatcher<std::shared_ptr<const PowerRun>>(this);
    connect(watcher, &QFutureWatcher<std::shared_ptr<const PowerRun>>::finished, this, [this, watcher] {
        watcher->deleteLater();
        _running = false;
        const auto result = watcher->result();
        _runs.insert(QString::fromStdString(result->analysis.name), result);
        refreshResults();
        updateStatus();
        emit runFinished(QString::fromStdString(result->analysis.name), result->ok());
    });
    watcher->setFuture(QtConcurrent::run([board = _board, config = *analysis] { return runPowerAnalysis(*board, config); }));
}

void PowerView::updateStatus() {
    const auto analysis = current();
    const auto run = currentRun();
    _runButton->setEnabled(canRun());
    if (_running) {
        _status->setText(tr("Solving..."));
    } else if (!_board) {
        _status->setText(tr("Waiting for the board to load..."));
    } else if (!run) {
        _status->setText(tr("Not run yet"));
    } else if (!run->ok()) {
        _status->setText(tr("Failed"));
    } else {
        QString text = tr("%1 unknowns, %2 s, %3 mW lost in the copper")
                           .arg(run->result.unknowns + (run->returnResult ? run->returnResult->unknowns : 0))
                           .arg(run->seconds, 0, 'f', 2)
                           .arg((run->result.powerLoss + (run->returnResult ? run->returnResult->powerLoss : 0)) * 1e3, 0, 'f', 1);
        if (analysis && !(run->analysis == *analysis)) text = tr("Out of date: the settings changed since this run. ") + text;
        _status->setText(text);
    }
}

} // namespace kiems_gui
