#include "TraceView.hpp"

#include "QuantityEdit.hpp"

#include <QChart>
#include <QChartView>
#include <QCheckBox>
#include <QComboBox>
#include <QFutureWatcher>
#include <QGraphicsLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QLegend>
#include <QLegendMarker>
#include <QLineSeries>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QToolTip>
#include <QTreeWidget>
#include <QValueAxis>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace kiems_gui {

namespace {

using kiems::trace::kNaN;

constexpr int kColorBarWidth = 76;
constexpr double kBand = 0.10; // the +/- band round the target impedance that counts as matched

QColor mix(const QColor& a, const QColor& b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

/// Blue below, pale on, red above: t in [-1, 1].
QColor diverging(double t) {
    static const QColor low(59, 110, 230), mid(225, 225, 215), high(220, 50, 40);
    return t < 0 ? mix(mid, low, -t) : mix(mid, high, t);
}

/// Dark to bright, for how much a point rings: t in [0, 1].
QColor heat(double t) {
    static const QColor stops[] = {QColor(40, 30, 80), QColor(150, 40, 120), QColor(235, 90, 50), QColor(250, 220, 90)};
    t = std::clamp(t, 0.0, 1.0) * 3;
    const int i = std::min(2, static_cast<int>(t));
    return mix(stops[i], stops[i + 1], t - i);
}

/// The chart colours the Results view uses on screen.
const QColor kChartBackground(0x1e, 0x1f, 0x22), kChartGrid(0x3a, 0x3d, 0x42), kChartText(0xd6, 0xd8, 0xdc);

QChartView* makeChart(const QString& xTitle, const QString& yTitle) {
    auto* chart = new QChart;
    chart->setBackgroundBrush(kChartBackground);
    chart->setBackgroundRoundness(0);
    chart->setMargins(QMargins(6, 6, 6, 6));
    chart->layout()->setContentsMargins(0, 0, 0, 0);
    chart->legend()->setLabelColor(kChartText);
    chart->legend()->setAlignment(Qt::AlignTop);
    for (const auto& [title, alignment] : {std::pair(xTitle, Qt::AlignBottom), std::pair(yTitle, Qt::AlignLeft)}) {
        auto* axis = new QValueAxis;
        axis->setTitleText(title);
        axis->setLabelFormat("%g");
        axis->setLabelsColor(kChartText);
        axis->setTitleBrush(kChartText);
        axis->setGridLineColor(kChartGrid);
        axis->setLinePenColor(kChartGrid);
        chart->addAxis(axis, alignment);
    }
    auto* view = new QChartView(chart);
    view->setRenderHint(QPainter::Antialiasing);
    view->setMinimumHeight(180);
    return view;
}

QLineSeries* addLine(QChartView* view, const QString& name, const QList<QPointF>& points, const QPen& pen) {
    auto* series = new QLineSeries;
    series->setName(name);
    series->replace(points);
    series->setPen(pen);
    view->chart()->addSeries(series);
    for (QAbstractAxis* axis : view->chart()->axes()) series->attachAxis(axis);
    if (name.isEmpty()) {
        for (QLegendMarker* marker : view->chart()->legend()->markers(series)) marker->setVisible(false);
    }
    return series;
}

void clearChart(QChartView* view) { view->chart()->removeAllSeries(); }

void setRange(QChartView* view, Qt::Orientation orientation, double low, double high) {
    if (!(high > low)) high = low + 1;
    for (QAbstractAxis* axis : view->chart()->axes(orientation)) {
        axis->setRange(low, high);
        if (auto* values = qobject_cast<QValueAxis*>(axis)) values->applyNiceNumbers();
    }
}

const QColor kPalette[] = {QColor(0xff, 0x8c, 0x2a), QColor(0x8f, 0xd1, 0x4f), QColor(0xe0, 0x5a, 0x7b),
                           QColor(0x5a, 0xb4, 0xff), QColor(0xf2, 0xd0, 0x4b), QColor(0xb4, 0x8c, 0xff)};

/// The driven-net probes, nearest the driver first.
std::vector<int> drivenProbes(const TraceAnalysis& analysis) {
    std::vector<int> order;
    for (std::size_t i = 0; i < analysis.probes.size(); ++i) {
        const TraceProbe& p = analysis.probes[i];
        if (std::isfinite(p.distanceMm) && p.referenced && !p.step.empty()) order.push_back(static_cast<int>(i));
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return analysis.probes[static_cast<std::size_t>(a)].distanceMm < analysis.probes[static_cast<std::size_t>(b)].distanceMm;
    });
    return order;
}

/// The ringing at `distanceMm`, between the driven-net probes either side.
double ringingAt(const TraceAnalysis& analysis, const std::vector<int>& order, double distanceMm) {
    double beforeD = kNaN, beforeV = kNaN, afterD = kNaN, afterV = kNaN;
    for (int i : order) {
        const TraceProbe& p = analysis.probes[static_cast<std::size_t>(i)];
        if (!std::isfinite(p.ringing.deviation)) continue;
        if (p.distanceMm <= distanceMm) {
            beforeD = p.distanceMm;
            beforeV = p.ringing.deviation;
        } else {
            afterD = p.distanceMm;
            afterV = p.ringing.deviation;
            break;
        }
    }
    // Not across a gap in the probes (where they had no plane under them): grey there.
    const auto near = [&](double d) { return std::isfinite(d) && std::abs(d - distanceMm) <= 0.5; };
    if (std::isnan(beforeV)) return near(afterD) ? afterV : kNaN;
    if (std::isnan(afterV)) return near(beforeD) ? beforeV : kNaN;
    if (afterD - beforeD > 1.0 && !near(beforeD) && !near(afterD)) return kNaN;
    const double t = afterD > beforeD ? (distanceMm - beforeD) / (afterD - beforeD) : 0;
    return beforeV + t * (afterV - beforeV);
}

/// The other port on the driven net that a step reaches last: where the trace is meant to end.
const std::pair<QString, std::vector<double>>* farEnd(const TraceAnalysis& analysis) {
    const std::pair<QString, std::vector<double>>* best = nullptr;
    double latest = -1;
    for (const auto& port : analysis.portSteps) {
        const auto r = kiems::trace::measureRinging(analysis.times, port.second, analysis.riseTime);
        if (std::isfinite(r.arrival) && r.arrival > latest) {
            latest = r.arrival;
            best = &port;
        }
    }
    return best;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// TraceCanvas

TraceCanvas::TraceCanvas(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumSize(300, 220);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void TraceCanvas::setBackdrop(std::shared_ptr<const PowerCanvas::Backdrop> backdrop) {
    _backdrop = std::move(backdrop);
    if (_autoFit) fit();
    update();
}

void TraceCanvas::setShowBoard(bool on) {
    _showBoard = on;
    update();
}

void TraceCanvas::setAnalysis(std::shared_ptr<const TraceAnalysis> analysis, bool refit) {
    _analysis = std::move(analysis);
    _ringingTop = 0.05;
    if (_analysis) {
        for (const TraceProbe& p : _analysis->probes) {
            if (std::isfinite(p.distanceMm) && std::isfinite(p.ringing.deviation)) _ringingTop = std::max(_ringingTop, p.ringing.deviation);
        }
        _ringingTop = std::min(_ringingTop, 1.0);
    }
    if (refit) {
        _autoFit = true;
        fit();
    }
    update();
}

void TraceCanvas::setQuantity(Quantity quantity) {
    _quantity = quantity;
    update();
}

void TraceCanvas::setSelection(std::optional<double> distanceMm) {
    _selection = distanceMm;
    update();
}

void TraceCanvas::setMessage(const QString& message) {
    _message = message;
    update();
}

QRectF TraceCanvas::mapArea() const { return QRectF(0, 0, std::max(10, width() - kColorBarWidth), height()); }

QPointF TraceCanvas::toScreen(double x, double y) const { return {_origin.x() + x * _scale, _origin.y() - y * _scale}; }

QPointF TraceCanvas::toBoard(const QPointF& screen) const {
    return {(screen.x() - _origin.x()) / _scale, (_origin.y() - screen.y()) / _scale};
}

void TraceCanvas::fit() {
    // The simulated nets' copper, with room round it; the board's outline if there's nothing else.
    QRectF bounds;
    if (_analysis) {
        for (const TraceTrack& t : _analysis->tracks) {
            bounds = bounds.united(QRectF(QPointF(t.track.x0, t.track.y0), QPointF(t.track.x1, t.track.y1)).normalized());
        }
    }
    if (bounds.isNull() && _backdrop) bounds = _backdrop->outline.boundingRect();
    if (bounds.isNull()) return;
    bounds.adjust(-2, -2, 2, 2);
    const QRectF area = mapArea();
    _scale = 0.92 * std::min(area.width() / std::max(bounds.width(), 1e-3), area.height() / std::max(bounds.height(), 1e-3));
    _origin = QPointF(area.center().x() - bounds.center().x() * _scale, area.center().y() + bounds.center().y() * _scale);
    update();
}

double TraceCanvas::valueAt(double distanceMm) const {
    if (!_analysis || !std::isfinite(distanceMm)) return kNaN;
    if (_quantity == Quantity::Impedance) return _analysis->impedanceAt(distanceMm);
    return ringingAt(*_analysis, drivenProbes(*_analysis), distanceMm);
}

QColor TraceCanvas::colorOf(double value) const {
    if (!std::isfinite(value)) return QColor(120, 120, 128);
    if (_quantity == Quantity::Impedance) {
        const double target = _analysis ? _analysis->targetImpedance : 50;
        // Full colour 30 % off the target.
        return diverging((value - target) / target / 0.3);
    }
    return heat(value / _ringingTop);
}

TraceCanvas::Hit TraceCanvas::hitTest(const QPointF& screen) const {
    Hit hit;
    if (!_analysis) return hit;
    const QPointF p = toBoard(screen);
    for (const TraceTrack& t : _analysis->tracks) {
        if (!t.driven) continue;
        const double dx = t.track.x1 - t.track.x0, dy = t.track.y1 - t.track.y0;
        const double length2 = dx * dx + dy * dy;
        const double along = length2 > 0 ? std::clamp(((p.x() - t.track.x0) * dx + (p.y() - t.track.y0) * dy) / length2, 0.0, 1.0) : 0;
        const QPointF at(t.track.x0 + along * dx, t.track.y0 + along * dy);
        const double pixels = std::hypot(at.x() - p.x(), at.y() - p.y()) * _scale;
        if (pixels >= hit.pixels) continue;
        const double length = std::sqrt(length2);
        const double viaStart = t.startDistance + along * length, viaEnd = t.endDistance + (1 - along) * length;
        hit.distance = std::isnan(viaStart) ? viaEnd : std::isnan(viaEnd) ? viaStart : std::min(viaStart, viaEnd);
        hit.at = at;
        hit.pixels = pixels;
    }
    if (std::isfinite(hit.distance)) {
        double best = 1e9;
        for (std::size_t i = 0; i < _analysis->probes.size(); ++i) {
            const TraceProbe& probe = _analysis->probes[i];
            if (!std::isfinite(probe.distanceMm)) continue;
            const double d = std::abs(probe.distanceMm - hit.distance);
            if (d < best) {
                best = d;
                hit.probe = static_cast<int>(i);
            }
        }
    }
    return hit;
}

void TraceCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(28, 28, 31));
    painter.setRenderHint(QPainter::Antialiasing);
    const QTransform boardToScreen(_scale, 0, 0, -_scale, _origin.x(), _origin.y());
    painter.setClipRect(mapArea());
    if (_showBoard && _backdrop) {
        painter.save();
        painter.setTransform(boardToScreen);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(38, 50, 42));
        painter.drawPath(_backdrop->outline);
        for (std::size_t l = 0; l < _backdrop->copper.size(); ++l) {
            painter.setBrush(l == 0 ? QColor(78, 82, 90, 200) : QColor(70, 74, 82, 110));
            painter.drawPath(_backdrop->copper[l]);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(130, 140, 130), 0));
        painter.drawPath(_backdrop->outline);
        painter.restore();
    }
    if (!_analysis || !_analysis->ok()) {
        painter.setClipping(false);
        painter.setPen(QColor(200, 200, 200));
        const QString text = _analysis ? _analysis->error : _message;
        painter.drawText(mapArea().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap, text);
        return;
    }
    // The other simulated nets in grey; the driven one coloured piece by piece.
    const auto penWidth = [&](const TraceTrack& t) { return std::max(3.0, t.track.width * _scale); };
    for (const TraceTrack& t : _analysis->tracks) {
        if (t.driven) continue;
        painter.setPen(QPen(QColor(150, 150, 160, 170), penWidth(t), Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(toScreen(t.track.x0, t.track.y0), toScreen(t.track.x1, t.track.y1));
    }
    for (const TraceTrack& t : _analysis->tracks) {
        if (!t.driven) continue;
        const double length = std::hypot(t.track.x1 - t.track.x0, t.track.y1 - t.track.y0);
        const int pieces = std::clamp(static_cast<int>(length * _scale / 3), 1, 400);
        for (int k = 0; k < pieces; ++k) {
            const double a = static_cast<double>(k) / pieces, b = static_cast<double>(k + 1) / pieces, m = 0.5 * (a + b);
            const double viaStart = t.startDistance + m * length, viaEnd = t.endDistance + (1 - m) * length;
            const double distance = std::isnan(viaStart) ? viaEnd : std::isnan(viaEnd) ? viaStart : std::min(viaStart, viaEnd);
            painter.setPen(QPen(colorOf(valueAt(distance)), penWidth(t), Qt::SolidLine, k == 0 || k == pieces - 1 ? Qt::RoundCap : Qt::SquareCap));
            painter.drawLine(toScreen(t.track.x0 + a * (t.track.x1 - t.track.x0), t.track.y0 + a * (t.track.y1 - t.track.y0)),
                             toScreen(t.track.x0 + b * (t.track.x1 - t.track.x0), t.track.y0 + b * (t.track.y1 - t.track.y0)));
        }
    }
    // The selected point: where along the driven net it is.
    if (_selection) {
        for (const TraceTrack& t : _analysis->tracks) {
            if (!t.driven || std::isnan(t.startDistance + t.endDistance)) continue;
            const double length = std::hypot(t.track.x1 - t.track.x0, t.track.y1 - t.track.y0);
            const double lo = std::min(t.startDistance, t.endDistance), hi = std::max(t.startDistance, t.endDistance);
            if (*_selection < lo - 1e-6 || *_selection > hi + 1e-6 || hi - lo < length * 0.5) continue;
            const double along = t.endDistance > t.startDistance ? (*_selection - t.startDistance) / length
                                                                : 1 - (*_selection - t.endDistance) / length;
            const QPointF at = toScreen(t.track.x0 + along * (t.track.x1 - t.track.x0), t.track.y0 + along * (t.track.y1 - t.track.y0));
            painter.setPen(QPen(QColor(255, 255, 255), 2.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(at, 9, 9);
            break;
        }
    }
    painter.setClipping(false);

    // The colour bar.
    const QRectF bar(width() - kColorBarWidth + 12, 24, 16, std::max(40, height() - 48));
    for (int y = 0; y < static_cast<int>(bar.height()); ++y) {
        const double t = 1 - y / bar.height();
        painter.setPen(_quantity == Quantity::Impedance ? diverging(2 * t - 1) : heat(t));
        painter.drawLine(QPointF(bar.left(), bar.top() + y), QPointF(bar.right(), bar.top() + y));
    }
    painter.setPen(QColor(200, 200, 200));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(bar);
    const double target = _analysis->targetImpedance;
    for (double t : {0.0, 0.5, 1.0}) {
        const QString text = _quantity == Quantity::Impedance ? QString::number(target * (1 + 0.3 * (2 * t - 1)), 'f', 0)
                                                              : QString::number(100 * _ringingTop * t, 'f', 0) + "%";
        painter.drawText(QPointF(bar.right() + 4, bar.bottom() - t * bar.height() + 4), text);
    }
    painter.drawText(QRectF(bar.left() - 8, 4, kColorBarWidth, 16), Qt::AlignLeft,
                     _quantity == Quantity::Impedance ? tr("Ω") : tr("Ringing"));
}

void TraceCanvas::resizeEvent(QResizeEvent*) {
    if (_autoFit) fit();
}

void TraceCanvas::wheelEvent(QWheelEvent* event) {
    const QPointF at = event->position();
    const QPointF board = toBoard(at);
    _scale *= std::pow(1.2, event->angleDelta().y() / 120.0);
    _autoFit = false;
    _origin = QPointF(at.x() - board.x() * _scale, at.y() + board.y() * _scale);
    update();
}

void TraceCanvas::mousePressEvent(QMouseEvent* event) {
    _lastMouse = event->position().toPoint();
    _pressedAt = _lastMouse;
}

void TraceCanvas::mouseMoveEvent(QMouseEvent* event) {
    const QPoint position = event->position().toPoint();
    if (event->buttons() != Qt::NoButton) {
        _origin += position - _lastMouse;
        _lastMouse = position;
        _autoFit = false;
        update();
        return;
    }
    const Hit hit = hitTest(event->position());
    if (!_analysis || !std::isfinite(hit.distance) || hit.pixels > 8) {
        QToolTip::hideText();
        return;
    }
    QString text = tr("%1 mm from %2").arg(hit.distance, 0, 'f', 2).arg(_analysis->driver.name);
    const double z = _analysis->impedanceAt(hit.distance);
    if (std::isfinite(z)) text += tr("\nImpedance %1 Ω (%2%3 %)").arg(z, 0, 'f', 1).arg(z >= _analysis->targetImpedance ? "+" : "")
                                     .arg(100 * (z - _analysis->targetImpedance) / _analysis->targetImpedance, 0, 'f', 1);
    const double ring = ringingAt(*_analysis, drivenProbes(*_analysis), hit.distance);
    if (std::isfinite(ring)) text += tr("\nRinging %1 % of the step").arg(100 * ring, 0, 'f', 1);
    QToolTip::showText(event->globalPosition().toPoint(), text, this);
}

void TraceCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || (event->position().toPoint() - _pressedAt).manhattanLength() > 3) return;
    const Hit hit = hitTest(event->position());
    if (std::isfinite(hit.distance) && hit.pixels <= 10) emit picked(hit.distance, hit.probe);
}

void TraceCanvas::mouseDoubleClickEvent(QMouseEvent*) {
    _autoFit = true;
    fit();
}

// ---------------------------------------------------------------------------------------------
// SpaceTimeView

SpaceTimeView::SpaceTimeView(QWidget* parent) : QWidget(parent) { setMinimumHeight(160); }

QRectF SpaceTimeView::plotArea() const { return QRectF(56, 22, std::max(10, width() - 70), std::max(10, height() - 46)); }

void SpaceTimeView::setAnalysis(std::shared_ptr<const TraceAnalysis> analysis) {
    _analysis = std::move(analysis);
    _rows.clear();
    _image = QImage();
    if (!_analysis || !_analysis->ok()) {
        update();
        return;
    }
    _rows = drivenProbes(*_analysis);
    if (_rows.empty()) {
        update();
        return;
    }
    // Each row is a probe; each column a time, up to the window. Coloured by the response less
    // where it settles, as a fraction of the step, on a scale set by the largest ringing once the
    // step has arrived: the step's front and every reflection show as lines.
    std::size_t columns = 0;
    while (columns < _analysis->times.size() && _analysis->times[columns] <= _analysis->windowEnd) ++columns;
    _scale = 0.02;
    for (int row : _rows) {
        const TraceProbe& probe = _analysis->probes[static_cast<std::size_t>(row)];
        if (std::isfinite(probe.ringing.deviation)) _scale = std::max(_scale, probe.ringing.deviation);
    }
    _image = QImage(static_cast<int>(std::max<std::size_t>(columns, 1)), static_cast<int>(_rows.size()), QImage::Format_RGB32);
    for (std::size_t r = 0; r < _rows.size(); ++r) {
        const TraceProbe& probe = _analysis->probes[static_cast<std::size_t>(_rows[r])];
        const double final = std::isfinite(probe.ringing.finalValue) ? probe.ringing.finalValue : 1;
        for (std::size_t c = 0; c < columns && c < probe.step.size(); ++c) {
            const double off = (probe.step[c] - final) / std::max(std::abs(final), 1e-9);
            _image.setPixelColor(static_cast<int>(c), static_cast<int>(r), diverging(std::clamp(off / _scale, -1.0, 1.0)));
        }
    }
    update();
}

void SpaceTimeView::setSelection(std::optional<double> distanceMm) {
    _selection = distanceMm;
    update();
}

void SpaceTimeView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kChartBackground);
    painter.setPen(kChartText);
    if (_image.isNull()) {
        painter.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap,
                         tr("Run the simulation again to record the voltage along the trace: this run has no trace probes."));
        return;
    }
    const QRectF area = plotArea();
    painter.drawImage(area, _image);
    painter.setPen(kChartGrid);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(area);
    // Distance runs down, by probe; time across.
    const auto rowDistance = [&](std::size_t r) { return _analysis->probes[static_cast<std::size_t>(_rows[r])].distanceMm; };
    painter.setPen(kChartText);
    const double first = rowDistance(0), last = rowDistance(_rows.size() - 1);
    painter.drawText(QRectF(0, area.top() - 6, 52, 14), Qt::AlignRight, QString("%1 mm").arg(first, 0, 'f', 1));
    painter.drawText(QRectF(0, area.bottom() - 8, 52, 14), Qt::AlignRight, QString("%1 mm").arg(last, 0, 'f', 1));
    const double start = _analysis->times.front() * 1e9, end = _analysis->windowEnd * 1e9;
    for (int k = 0; k <= 4; ++k) {
        const double x = area.left() + area.width() * k / 4;
        painter.drawText(QRectF(x - 30, area.bottom() + 4, 60, 14), Qt::AlignHCenter,
                         QString("%1 ns").arg(start + (end - start) * k / 4, 0, 'g', 3));
    }
    painter.drawText(QRectF(area.left(), 4, area.width(), 14), Qt::AlignRight,
                     tr("Blue below where it settles, red above; full colour at ±%1 % of the step")
                         .arg(100 * _scale, 0, 'g', 2));
    if (_selection) {
        // The row nearest the selection.
        std::size_t best = 0;
        for (std::size_t r = 0; r < _rows.size(); ++r) {
            if (std::abs(rowDistance(r) - *_selection) < std::abs(rowDistance(best) - *_selection)) best = r;
        }
        const double y = area.top() + (best + 0.5) * area.height() / static_cast<double>(_rows.size());
        painter.setPen(QPen(Qt::white, 1, Qt::DashLine));
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }
}

void SpaceTimeView::mouseReleaseEvent(QMouseEvent* event) {
    const QRectF area = plotArea();
    if (_rows.empty() || !area.contains(event->position())) return;
    const auto r = static_cast<std::size_t>(std::clamp((event->position().y() - area.top()) / area.height() * _rows.size(), 0.0,
                                                       static_cast<double>(_rows.size() - 1)));
    const TraceProbe& probe = _analysis->probes[static_cast<std::size_t>(_rows[r])];
    emit picked(probe.distanceMm, _rows[r]);
}

// ---------------------------------------------------------------------------------------------
// TraceView

TraceView::TraceView(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(10, 6, 10, 6);
    _heading = new QLabel;
    QFont bold = _heading->font();
    bold.setBold(true);
    _heading->setFont(bold);
    _status = new QLabel;
    _status->setEnabled(false);
    _status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _driver = new QComboBox;
    _driver->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    _driver->setToolTip(tr("Which run to look at: the port driving the step."));
    _quantity = new QComboBox;
    _quantity->addItems({tr("Impedance"), tr("Ringing")});
    _quantity->setToolTip(tr("Colour the driven trace by the impedance a step meets along it, or by how much it rings "
                             "once the step has passed."));
    _riseTime = new QuantityEdit(QuantityEdit::Kind::Time);
    _riseTime->setToolTip(tr("The step's 10-90 % rise time. A faster edge separates features closer together, down to "
                             "what the simulation's frequency sweep can carry; a real driver's edge shows what it "
                             "will see."));
    _riseTime->setMinimumWidth(90);
    _otherNets = new QCheckBox(tr("Board"));
    _otherNets->setChecked(true);
    _otherNets->setToolTip(tr("Draw the board under the trace: its outline and copper."));
    _runButton = new QPushButton(tr("Run"));
    _runButton->setToolTip(tr("Run the simulation, recording the voltage along its traces."));
    _runButton->hide();
    bar->addWidget(_heading);
    bar->addSpacing(8);
    bar->addWidget(_status, 1);
    bar->addWidget(new QLabel(tr("Driven by:")));
    bar->addWidget(_driver);
    bar->addWidget(_quantity);
    bar->addWidget(new QLabel(tr("Rise time:")));
    bar->addWidget(_riseTime);
    bar->addWidget(_otherNets);
    bar->addWidget(_runButton);
    layout->addLayout(bar);

    auto* split = new QSplitter(Qt::Horizontal);
    auto* vertical = new QSplitter(Qt::Vertical);
    _canvas = new TraceCanvas;
    vertical->addWidget(_canvas);
    _tabs = new QTabWidget;
    _tabs->setObjectName("traceTabs");
    _tabs->setUsesScrollButtons(false);
    _impedanceChart = makeChart(tr("Distance from the driver [mm]"), tr("Impedance [Ω]"));
    _stepChart = makeChart(tr("Time [ns]"), tr("Voltage, per volt of step"));
    _ringingChart = makeChart(tr("Distance from the driver [mm]"), tr("Of the step [%]"));
    _spaceTime = new SpaceTimeView;
    _tabs->addTab(_impedanceChart, tr("Impedance"));
    _tabs->addTab(_stepChart, tr("Step Response"));
    _tabs->addTab(_ringingChart, tr("Ringing"));
    _tabs->addTab(_spaceTime, tr("Reflections"));
    _spaceTime->setToolTip(tr("Distance along the trace (down) against time (across), coloured by how far the voltage "
                              "is from where it settles: blue below, red above. The step's front is the edge where "
                              "blue ends; each reflection is a line leaving the spot that made it."));
    vertical->addWidget(_tabs);
    vertical->setStretchFactor(0, 3);
    vertical->setStretchFactor(1, 2);
    vertical->setSizes({500, 340});
    split->addWidget(vertical);
    _findings = new QTreeWidget;
    _findings->setObjectName("findings");
    _findings->setHeaderLabels({tr("Finding"), tr("Where")});
    _findings->setRootIsDecorated(false);
    _findings->setWordWrap(true);
    _findings->setMinimumWidth(300);
    _findings->header()->setStretchLastSection(false);
    _findings->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    _findings->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    split->addWidget(_findings);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);
    split->setSizes({1000, 360});
    layout->addWidget(split, 1);

    connect(_quantity, &QComboBox::currentIndexChanged, this, [this](int index) {
        _canvas->setQuantity(index == 0 ? TraceCanvas::Quantity::Impedance : TraceCanvas::Quantity::Ringing);
    });
    connect(_otherNets, &QCheckBox::toggled, _canvas, &TraceCanvas::setShowBoard);
    connect(_driver, &QComboBox::activated, this, [this] { analyse(); });
    connect(_riseTime, &QuantityEdit::valueEdited, this, [this](std::optional<double> v) {
        if (v && *v > 0) analyse();
    });
    connect(_runButton, &QPushButton::clicked, this, &TraceView::runRequested);
    connect(_canvas, &TraceCanvas::picked, this, &TraceView::select);
    connect(_spaceTime, &SpaceTimeView::picked, this, &TraceView::select);
    connect(_findings, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        const QVariant distance = item->data(0, Qt::UserRole);
        if (distance.isValid()) select(distance.toDouble(), item->data(0, Qt::UserRole + 1).toInt());
    });
    _canvas->setMessage(tr("Choose a simulation's Trace page to see its trace."));
}

void TraceView::setBoard(std::shared_ptr<const BoardData> board) {
    _board = std::move(board);
    _backdrop = _board ? PowerCanvas::makeBackdrop(*_board) : nullptr;
    _canvas->setBackdrop(_backdrop);
    if (!_simulationDirectory.isEmpty() && (!_analysis || !_analysis->ok())) analyse();
}

void TraceView::showSimulation(const QString& title, const QString& simulationDirectory, double fMax, double pulseSeconds) {
    const bool same = simulationDirectory == _simulationDirectory;
    _title = title;
    _simulationDirectory = simulationDirectory;
    _fMax = fMax;
    _pulseSeconds = pulseSeconds;
    _heading->setText(title);
    const int previous = _driver->currentData().toInt();
    _driver->clear();
    for (const TracePort& port : tracePorts(simulationDirectory)) {
        if (port.excited) _driver->addItem(port.name, port.index);
    }
    if (same && _driver->findData(previous) >= 0) _driver->setCurrentIndex(_driver->findData(previous));
    if (!same || !_riseTime->value()) _riseTime->refreshValue(kiems::trace::minimumRiseTime(fMax));
    _riseTime->setMinimum(kiems::trace::minimumRiseTime(fMax));
    analyse();
}

void TraceView::analyse() {
    const int generation = ++_generation;
    _runButton->setVisible(false);
    if (_driver->count() == 0) {
        _analysis.reset();
        _canvas->setAnalysis(nullptr, true);
        _canvas->setMessage(tr("%1 hasn't been run yet. Run it to see the impedance and ringing along its traces.").arg(_title));
        _status->clear();
        _runButton->setVisible(true);
        showAnalysis();
        return;
    }
    if (!_board) {
        _canvas->setMessage(tr("Waiting for the board to load..."));
        return;
    }
    _status->setText(tr("Analysing..."));
    const double rise = _riseTime->value().value_or(kiems::trace::minimumRiseTime(_fMax));
    auto* watcher = new QFutureWatcher<std::shared_ptr<const TraceAnalysis>>(this);
    connect(watcher, &QFutureWatcher<std::shared_ptr<const TraceAnalysis>>::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != _generation) return; // superseded
        const bool refit = !_analysis || !_analysis->ok() || _analysis->driver.index != watcher->result()->driver.index;
        _analysis = watcher->result();
        _canvas->setAnalysis(_analysis, refit);
        showAnalysis();
        emit analysed(_analysis->ok());
    });
    watcher->setFuture(QtConcurrent::run([board = _board, directory = _simulationDirectory, port = _driver->currentData().toInt(),
                                          fMax = _fMax, pulse = _pulseSeconds, rise] {
        return analyseTrace(*board, directory, port, fMax, rise, pulse);
    }));
}

void TraceView::showAnalysis() {
    _spaceTime->setAnalysis(_analysis);
    _findings->clear();
    _selection.reset();
    _selectedProbe = -1;
    if (!_analysis || !_analysis->ok()) {
        if (_analysis) _status->setText(_analysis->error);
        updateCharts();
        return;
    }
    const TraceAnalysis& a = *_analysis;
    const bool haveProbes = !drivenProbes(a).empty();
    QString status = tr("%1 mm of %2 from %3; target %4 Ω")
                         .arg(a.netLength, 0, 'f', 1)
                         .arg(a.driver.net, a.driver.name)
                         .arg(a.targetImpedance, 0, 'f', 0);
    if (a.delayMeasured) status += tr("; the step travels %1 mm/ns along it").arg(a.velocity * 1e-9, 0, 'f', 0);
    else status += tr("; no trace probes in this run, so distances assume a typical microstrip speed and there's no ringing");
    if (a.unreferencedProbes > 0) {
        status += tr("; %n probe(s) left out, with no plane under them", nullptr, a.unreferencedProbes);
    }
    if (a.profileReachMm < a.netLength) {
        status += tr("; the impedance reaches %1 mm of it").arg(a.profileReachMm, 0, 'f', 1);
    }
    _status->setText(status);
    _status->setToolTip(status);
    _runButton->setVisible(!haveProbes);
    _runButton->setText(tr("Run Again"));

    // Findings: each stretch more than 10 % off the target, then the worst ringing, then each port.
    const auto add = [&](const QString& what, const QString& where, double distance, int probe, const QColor& color) {
        auto* item = new QTreeWidgetItem(_findings, {what, where});
        if (std::isfinite(distance)) {
            item->setData(0, Qt::UserRole, distance);
            item->setData(0, Qt::UserRole + 1, probe);
        }
        if (color.isValid()) item->setForeground(0, color);
        return item;
    };
    // First what limits the run itself: everything below rests on it.
    const QColor warning(240, 110, 90), caution(250, 200, 90);
    const bool cutShort = a.pulseSeconds > 0 && a.recordSeconds < a.pulseSeconds;
    if (cutShort) {
        // Enough steps for the pulse, and as long again for it to travel and ring down.
        const double needed = a.timestepSeconds > 0 ? std::ceil(2 * a.pulseSeconds / a.timestepSeconds / 10000) * 10000 : 0;
        auto* item = add(tr("This run stopped at %1 ns, before its excitation had played: the pulse for a sweep to %2 MHz "
                            "lasts %3 ns, so these results are mostly noise. Raise Max steps to about %4, or the "
                            "sweep's top frequency (a few GHz suits a trace's profile).")
                             .arg(a.recordSeconds * 1e9, 0, 'g', 3)
                             .arg(a.fMax / 1e6, 0, 'g', 4)
                             .arg(a.pulseSeconds * 1e9, 0, 'g', 3)
                             .arg(QLocale().toString(needed, 'f', 0)),
                         QString(), kNaN, -1, warning);
        item->setToolTip(0, item->text(0));
    }
    if (!cutShort && a.profileReachMm < a.netLength) {
        auto* item = add(tr("The impedance is known only to %1 mm of %2: in the run's %3 ns a step gets no farther and back. "
                            "Raise Max steps to see the rest.")
                             .arg(a.profileReachMm, 0, 'f', 1)
                             .arg(a.netLength, 0, 'f', 1)
                             .arg(a.recordSeconds * 1e9, 0, 'g', 3),
                         tr("%1 mm on").arg(a.profileReachMm, 0, 'f', 1), a.profileReachMm, -1, caution);
        item->setToolTip(0, item->text(0));
    }
    // The step's edge, there and back, smears the profile over about this much trace.
    if (a.velocity > 0) {
        const double blur = 0.5 * a.riseTime * a.velocity;
        if (blur > 0.25 * a.netLength) {
            auto* item = add(tr("The sharpest step a sweep to %1 MHz can carry rises in %2 ns, which blurs the impedance "
                                "over about %3 mm of trace: raise the sweep's top frequency for a finer profile.")
                                 .arg(a.fMax / 1e6, 0, 'g', 4)
                                 .arg(a.riseTime * 1e9, 0, 'g', 2)
                                 .arg(blur, 0, 'f', 0),
                             QString(), kNaN, -1, caution);
            item->setToolTip(0, item->text(0));
        }
    }
    // A run that stopped before its pulse had played measured noise: say only that.
    if (cutShort) {
        _findings->resizeColumnToContents(1);
        updateCharts();
        return;
    }
    const double target = a.targetImpedance;
    bool anyMismatch = false;
    for (std::size_t i = 0; i < a.tdrDistance.size();) {
        const auto off = [&](std::size_t k) { return std::abs(a.tdrImpedance[k] - target) / target > kBand; };
        if (!(a.tdrDistance[i] <= a.netLength) || !off(i)) {
            ++i;
            continue;
        }
        std::size_t worst = i, j = i;
        while (j < a.tdrDistance.size() && a.tdrDistance[j] <= a.netLength && off(j) &&
               (a.tdrImpedance[j] > target) == (a.tdrImpedance[i] > target)) {
            if (std::abs(a.tdrImpedance[j] - target) > std::abs(a.tdrImpedance[worst] - target)) worst = j;
            ++j;
        }
        const double z = a.tdrImpedance[worst];
        add(tr("%1 Ω, %2%3 % %4 the target").arg(z, 0, 'f', 1).arg(z > target ? "+" : "")
                .arg(100 * (z - target) / target, 0, 'f', 0).arg(z > target ? tr("above") : tr("below")),
            tr("%1-%2 mm").arg(a.tdrDistance[i], 0, 'f', 1).arg(a.tdrDistance[j - 1], 0, 'f', 1), a.tdrDistance[worst], -1,
            z > target ? QColor(240, 110, 90) : QColor(110, 160, 255));
        anyMismatch = true;
        i = j;
    }
    if (!anyMismatch && !a.tdrDistance.empty() && a.profileReachMm >= a.netLength) {
        add(tr("Within %1 % of %2 Ω all along").arg(100 * kBand, 0, 'f', 0).arg(target, 0, 'f', 0), QString(), kNaN, -1, QColor());
    }
    std::vector<int> ringing = drivenProbes(a);
    std::sort(ringing.begin(), ringing.end(), [&](int x, int y) {
        const double dx = a.probes[static_cast<std::size_t>(x)].ringing.deviation, dy = a.probes[static_cast<std::size_t>(y)].ringing.deviation;
        return (std::isfinite(dx) ? dx : -1) > (std::isfinite(dy) ? dy : -1);
    });
    if (!ringing.empty()) {
        const TraceProbe& worst = a.probes[static_cast<std::size_t>(ringing.front())];
        if (std::isfinite(worst.ringing.deviation)) {
            add(tr("Rings most: %1 % of the step after it passes, settling in %2 ns")
                    .arg(100 * worst.ringing.deviation, 0, 'f', 0)
                    .arg(worst.ringing.settling * 1e9, 0, 'f', 2),
                tr("%1 mm").arg(worst.distanceMm, 0, 'f', 1), worst.distanceMm, ringing.front(), QColor(250, 200, 90));
        }
    }
    for (const auto& [name, step] : a.portSteps) {
        const auto r = kiems::trace::measureRinging(a.times, step, a.riseTime);
        if (!std::isfinite(r.finalValue)) continue;
        add(tr("%1: overshoot %2 %, ringback %3 %, settles in %4 ns")
                .arg(name)
                .arg(100 * r.overshoot, 0, 'f', 0)
                .arg(100 * r.ringback, 0, 'f', 0)
                .arg(r.settling * 1e9, 0, 'f', 2),
            QString(), kNaN, -1, QColor());
    }
    _findings->resizeColumnToContents(1);
    updateCharts();
}

void TraceView::select(double distanceMm, int probe) {
    _selection = distanceMm;
    _selectedProbe = probe;
    _canvas->setSelection(distanceMm);
    _spaceTime->setSelection(distanceMm);
    updateCharts();
}

void TraceView::updateCharts() {
    for (QChartView* chart : {_impedanceChart, _stepChart, _ringingChart}) clearChart(chart);
    if (!_analysis || !_analysis->ok()) return;
    const TraceAnalysis& a = *_analysis;
    const double end = std::max(1.0, a.netLength * 1.1);

    // Impedance against distance, with the target and its band.
    QList<QPointF> profile;
    double low = a.targetImpedance * (1 - 2 * kBand), high = a.targetImpedance * (1 + 2 * kBand);
    for (std::size_t i = 0; i < a.tdrDistance.size(); ++i) {
        if (!(a.tdrDistance[i] <= end) || !std::isfinite(a.tdrImpedance[i])) continue;
        profile.append({a.tdrDistance[i], a.tdrImpedance[i]});
        low = std::min(low, a.tdrImpedance[i]);
        high = std::max(high, a.tdrImpedance[i]);
    }
    addLine(_impedanceChart, tr("Impedance"), profile, QPen(kPalette[0], 2));
    addLine(_impedanceChart, tr("Target"), {{0, a.targetImpedance}, {end, a.targetImpedance}}, QPen(kPalette[1], 1.2));
    for (double side : {-1.0, 1.0}) {
        const double z = a.targetImpedance * (1 + side * kBand);
        addLine(_impedanceChart, side < 0 ? tr("±%1 %").arg(100 * kBand, 0, 'f', 0) : QString(), {{0, z}, {end, z}},
                QPen(kPalette[1], 1, Qt::DashLine));
    }
    if (a.netLength > 0) {
        addLine(_impedanceChart, QString(), {{a.netLength, low}, {a.netLength, high}}, QPen(kChartGrid.lighter(150), 1, Qt::DotLine));
    }
    if (_selection) addLine(_impedanceChart, QString(), {{*_selection, low}, {*_selection, high}}, QPen(Qt::white, 1, Qt::DashLine));
    setRange(_impedanceChart, Qt::Horizontal, 0, end);
    setRange(_impedanceChart, Qt::Vertical, low - 2, high + 2);

    // Step responses: the point picked, the driver and the other ports.
    const auto points = [&](const std::vector<double>& values) {
        QList<QPointF> list;
        for (std::size_t i = 0; i < values.size() && i < a.times.size(); ++i) {
            if (std::isfinite(values[i])) list.append({a.times[i] * 1e9, values[i]});
        }
        return list;
    };
    double vLow = 0, vHigh = 1;
    const auto track = [&](const std::vector<double>& values) {
        for (double v : values) {
            if (std::isfinite(v)) {
                vLow = std::min(vLow, v);
                vHigh = std::max(vHigh, v);
            }
        }
    };
    addLine(_stepChart, tr("%1 (driver)").arg(a.driver.name), points(a.driverStep), QPen(kPalette[3], 1.5));
    track(a.driverStep);
    int color = 4;
    for (const auto& [name, step] : a.portSteps) {
        addLine(_stepChart, name, points(step), QPen(kPalette[color++ % 6], 1.5));
        track(step);
    }
    if (_selectedProbe >= 0 && static_cast<std::size_t>(_selectedProbe) < a.probes.size()) {
        const TraceProbe& probe = a.probes[static_cast<std::size_t>(_selectedProbe)];
        addLine(_stepChart, tr("At %1 mm").arg(probe.distanceMm, 0, 'f', 1), points(probe.step), QPen(kPalette[0], 2.2));
        track(probe.step);
    }
    setRange(_stepChart, Qt::Horizontal, a.times.empty() ? 0 : a.times.front() * 1e9, a.windowEnd * 1e9);
    setRange(_stepChart, Qt::Vertical, vLow - 0.05, vHigh + 0.05);

    // Ringing along the trace.
    QList<QPointF> deviation, overshoot;
    double top = 10;
    for (int i : drivenProbes(a)) {
        const TraceProbe& p = a.probes[static_cast<std::size_t>(i)];
        if (std::isfinite(p.ringing.deviation)) {
            deviation.append({p.distanceMm, 100 * p.ringing.deviation});
            top = std::max(top, 100 * p.ringing.deviation);
        }
        if (std::isfinite(p.ringing.overshoot)) {
            overshoot.append({p.distanceMm, 100 * p.ringing.overshoot});
            top = std::max(top, 100 * p.ringing.overshoot);
        }
    }
    addLine(_ringingChart, tr("Most off once the step has passed"), deviation, QPen(kPalette[4], 2));
    addLine(_ringingChart, tr("Overshoot"), overshoot, QPen(kPalette[2], 1.5));
    if (_selection) addLine(_ringingChart, QString(), {{*_selection, 0}, {*_selection, top}}, QPen(Qt::white, 1, Qt::DashLine));
    setRange(_ringingChart, Qt::Horizontal, 0, end);
    setRange(_ringingChart, Qt::Vertical, 0, top * 1.1);
}

} // namespace kiems_gui
