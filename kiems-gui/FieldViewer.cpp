#include "FieldViewer.hpp"

#include "QuantityEdit.hpp"

#include "FieldFrameSeriesReader.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

namespace kiems_gui {

namespace {

enum class Quantity { MagnitudeE, Ex, Ey, Ez, MagnitudeH, Hx, Hy, Hz, Energy };

bool isSigned(Quantity quantity) {
    return quantity != Quantity::MagnitudeE && quantity != Quantity::MagnitudeH && quantity != Quantity::Energy;
}

QString unitOf(Quantity quantity) {
    switch (quantity) {
    case Quantity::MagnitudeE:
    case Quantity::Ex:
    case Quantity::Ey:
    case Quantity::Ez: return "V/m";
    case Quantity::MagnitudeH:
    case Quantity::Hx:
    case Quantity::Hy:
    case Quantity::Hz: return "A/m";
    case Quantity::Energy: return QString::fromUtf8("J/m\xC2\xB3");
    }
    return {};
}

std::filesystem::path fsPath(const QString& path) {
    return std::filesystem::path(path.toStdU16String());
}

/// The magma-like map magnitudes are drawn in, dark at the bottom of the range.
QRgb magnitudeColor(double t) {
    static const QColor stops[] = {QColor(0x10, 0x0b, 0x2d), QColor(0x4f, 0x12, 0x7b), QColor(0xb6, 0x36, 0x79),
                                   QColor(0xfb, 0x87, 0x61), QColor(0xfc, 0xfd, 0xbf)};
    t = std::clamp(t, 0.0, 1.0) * 4.0;
    const int i = std::min(static_cast<int>(t), 3);
    const double f = t - i;
    const QColor& a = stops[i];
    const QColor& b = stops[i + 1];
    return qRgb(static_cast<int>(a.red() + (b.red() - a.red()) * f), static_cast<int>(a.green() + (b.green() - a.green()) * f),
                static_cast<int>(a.blue() + (b.blue() - a.blue()) * f));
}

/// Signed values: blue for negative, red for positive, both fading to near-black at zero.
QRgb signedColor(double t, bool negative) {
    t = std::clamp(t, 0.0, 1.0);
    const QColor zero(0x16, 0x18, 0x1c);
    const QColor end = negative ? QColor(0x6a, 0xb4, 0xff) : QColor(0xff, 0x6a, 0x5a);
    return qRgb(static_cast<int>(zero.red() + (end.red() - zero.red()) * t),
                static_cast<int>(zero.green() + (end.green() - zero.green()) * t),
                static_cast<int>(zero.blue() + (end.blue() - zero.blue()) * t));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// FieldCanvas: the slice, fitted to the view with pan and zoom, and its colour scale.

class FieldCanvas : public QWidget {
public:
    using QWidget::QWidget;

    /// `world` is the image's extent in millimetres, x right and y up.
    void setImage(const QImage& image, const QRectF& world, const QString& top, const QString& bottom, bool diverging) {
        const bool newExtent = world != _world;
        _image = image;
        _world = world;
        _top = top;
        _bottom = bottom;
        _diverging = diverging;
        if (newExtent) {
            _zoom = 1.0;
            _pan = {};
        }
        update();
    }

    void clearImage() {
        _image = {};
        update();
    }

    /// Board outlines in the same millimetre frame as setImage()'s `world`.
    void setOverlay(const QPainterPath& outline, const QPainterPath& copper) {
        _outline = outline;
        _copper = copper;
        update();
    }

    void setOverlayVisible(bool visible) {
        _overlayVisible = visible;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::Base));
        if (_image.isNull() || _world.isEmpty()) return;
        const QRectF target = imageRect();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(target, _image);
        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        painter.drawRect(target);

        if (_overlayVisible && (!_outline.isEmpty() || !_copper.isEmpty())) {
            // World millimetres (y up) onto the image's rectangle.
            QTransform toView;
            toView.translate(target.left(), target.bottom());
            toView.scale(target.width() / _world.width(), -target.height() / _world.height());
            toView.translate(-_world.left(), -_world.top());
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setClipRect(target);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(255, 255, 255, 70), 1));
            painter.drawPath(toView.map(_copper));
            painter.setPen(QPen(QColor(255, 255, 255, 170), 1.5));
            painter.drawPath(toView.map(_outline));
            painter.restore();
        }

        // Scale bar: 1, 2 or 5 times a power of ten, about a fifth of the view wide.
        const double millimetresPerPixel = _world.width() / target.width();
        const double wanted = millimetresPerPixel * width() / 5;
        const double power = std::pow(10.0, std::floor(std::log10(wanted)));
        const double length = wanted / power >= 5 ? 5 * power : wanted / power >= 2 ? 2 * power : power;
        const double pixels = length / millimetresPerPixel;
        const QPointF start(16, height() - 18);
        painter.setPen(QPen(palette().color(QPalette::WindowText), 2));
        painter.drawLine(start, start + QPointF(pixels, 0));
        painter.drawText(start + QPointF(0, -6), QuantityEdit::format(QuantityEdit::Kind::Length, length * 1000));

        // Colour scale.
        const QRectF bar(width() - 26, 16, 12, height() - 60);
        for (int i = 0; i < static_cast<int>(bar.height()); ++i) {
            const double t = 1.0 - i / bar.height();
            const QRgb color = _diverging ? (t >= 0.5 ? signedColor((t - 0.5) * 2, false) : signedColor((0.5 - t) * 2, true))
                                          : magnitudeColor(t);
            painter.fillRect(QRectF(bar.left(), bar.top() + i, bar.width(), 1), QColor(color));
        }
        painter.setPen(palette().color(QPalette::WindowText));
        const QFontMetrics metrics(font());
        painter.drawText(QPointF(bar.right() - metrics.horizontalAdvance(_top), bar.top() - 4 + metrics.ascent()) -
                             QPointF(16, 0),
                         _top);
        painter.drawText(QPointF(bar.right() - metrics.horizontalAdvance(_bottom) - 16, bar.bottom()), _bottom);
    }

    void wheelEvent(QWheelEvent* event) override {
        const double factor = std::pow(1.0015, event->angleDelta().y());
        const QPointF cursor = event->position();
        const QRectF before = imageRect();
        _zoom = std::clamp(_zoom * factor, 0.2, 200.0);
        const QRectF after = imageRect();
        // Keep the point under the cursor where it is.
        const QPointF relative((cursor.x() - before.left()) / before.width(), (cursor.y() - before.top()) / before.height());
        _pan += cursor - (after.topLeft() + QPointF(relative.x() * after.width(), relative.y() * after.height()));
        update();
    }

    void mousePressEvent(QMouseEvent* event) override { _dragFrom = event->position(); }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!(event->buttons() & Qt::LeftButton)) return;
        _pan += event->position() - _dragFrom;
        _dragFrom = event->position();
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override {
        _zoom = 1.0;
        _pan = {};
        update();
    }

private:
    QRectF imageRect() const {
        const QRectF area = QRectF(rect()).adjusted(12, 12, -96, -36);
        const double scale = std::min(area.width() / _world.width(), area.height() / _world.height()) * _zoom;
        const QSizeF size(_world.width() * scale, _world.height() * scale);
        return QRectF(area.center() - QPointF(size.width() / 2, size.height() / 2) + _pan, size);
    }

    QImage _image;
    QRectF _world;
    QPainterPath _outline;
    QPainterPath _copper;
    bool _overlayVisible = true;
    QString _top, _bottom;
    bool _diverging = false;
    double _zoom = 1.0;
    QPointF _pan;
    QPointF _dragFrom;
};

// ---------------------------------------------------------------------------------------------
// FieldViewer

FieldViewer::FieldViewer(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    _stack = new QStackedWidget;
    outer->addWidget(_stack);

    // Nothing stored yet.
    auto* empty = new QWidget;
    auto* emptyLayout = new QVBoxLayout(empty);
    emptyLayout->addStretch();
    _emptyLabel = new QLabel;
    _emptyLabel->setAlignment(Qt::AlignCenter);
    _emptyLabel->setWordWrap(true);
    _capture = new QPushButton(tr("Run and Store Fields"));
    _capture->setToolTip(tr("Run this simulation again, storing its field frames for playback here."));
    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    buttonRow->addWidget(_capture);
    buttonRow->addStretch();
    emptyLayout->addWidget(_emptyLabel);
    emptyLayout->addLayout(buttonRow);
    emptyLayout->addStretch();
    _stack->addWidget(empty);

    // The viewer.
    auto* viewer = new QWidget;
    auto* layout = new QVBoxLayout(viewer);
    _title = new QLabel;
    QFont titleFont = _title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.2);
    _title->setFont(titleFont);
    auto* controls = new QHBoxLayout;
    _port = new QComboBox;
    _port->setToolTip(tr("Which excited port's run to show: each port is driven in a run of its own."));
    _quantity = new QComboBox;
    _quantity->addItem(tr("|E|"), static_cast<int>(Quantity::MagnitudeE));
    _quantity->addItem("Ex", static_cast<int>(Quantity::Ex));
    _quantity->addItem("Ey", static_cast<int>(Quantity::Ey));
    _quantity->addItem("Ez", static_cast<int>(Quantity::Ez));
    _quantity->addItem(tr("|H|"), static_cast<int>(Quantity::MagnitudeH));
    _quantity->addItem("Hx", static_cast<int>(Quantity::Hx));
    _quantity->addItem("Hy", static_cast<int>(Quantity::Hy));
    _quantity->addItem("Hz", static_cast<int>(Quantity::Hz));
    _quantity->addItem(tr("Energy density"), static_cast<int>(Quantity::Energy));
    _height = new QSlider(Qt::Horizontal);
    _height->setToolTip(tr("The height of the slice through the board."));
    _heightLabel = new QLabel;
    _heightLabel->setMinimumWidth(150);
    _range = new QSpinBox;
    _range->setRange(10, 160);
    _range->setValue(60);
    _range->setSingleStep(10);
    _range->setSuffix(tr(" dB"));
    _range->setToolTip(tr("How far below the series' peak the colour scale reaches."));
    controls->addWidget(new QLabel(tr("Excited:")));
    controls->addWidget(_port, 1);
    controls->addSpacing(8);
    controls->addWidget(_quantity);
    controls->addSpacing(8);
    controls->addWidget(new QLabel(tr("Height:")));
    controls->addWidget(_height, 1);
    controls->addWidget(_heightLabel);
    controls->addWidget(new QLabel(tr("Range:")));
    controls->addWidget(_range);
    _showBoard = new QCheckBox(tr("Board"));
    _showBoard->setChecked(true);
    _showBoard->setToolTip(tr("Draw the board's outline and top copper over the fields."));
    controls->addWidget(_showBoard);
    _canvas = new FieldCanvas;
    _canvas->setMinimumSize(300, 200);
    auto* timeline = new QHBoxLayout;
    _play = new QPushButton(tr("Play"));
    _play->setCheckable(true);
    _time = new QSlider(Qt::Horizontal);
    _timeLabel = new QLabel;
    _timeLabel->setMinimumWidth(230);
    timeline->addWidget(_play);
    timeline->addWidget(_time, 1);
    timeline->addWidget(_timeLabel);
    layout->addWidget(_title);
    layout->addLayout(controls);
    layout->addWidget(_canvas, 1);
    layout->addLayout(timeline);
    _stack->addWidget(viewer);

    _playTimer = new QTimer(this);
    _playTimer->setInterval(120);

    connect(_capture, &QPushButton::clicked, this, &FieldViewer::captureRequested);
    connect(_showBoard, &QCheckBox::toggled, _canvas, [this](bool on) { _canvas->setOverlayVisible(on); });
    connect(_port, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0) openSeries(_port->itemData(index).toInt());
    });
    auto redraw = [this] {
        ++_generation;
        requestFrame();
    };
    connect(_quantity, &QComboBox::currentIndexChanged, this, redraw);
    connect(_range, &QSpinBox::valueChanged, this, redraw);
    connect(_height, &QSlider::valueChanged, this, [this, redraw] {
        populateHeights();
        redraw();
    });
    connect(_time, &QSlider::valueChanged, this, [this] {
        updateTimeLabel();
        requestFrame();
    });
    connect(_play, &QPushButton::toggled, this, [this](bool on) {
        _play->setText(on ? tr("Pause") : tr("Play"));
        if (on) _playTimer->start();
        else _playTimer->stop();
    });
    connect(_playTimer, &QTimer::timeout, this, [this] {
        // Advance once the frame on show has been drawn, so playback never queues reads.
        if (_reading || _time->maximum() <= 0) return;
        _time->setValue(_time->value() >= _time->maximum() ? 0 : _time->value() + 1);
    });
}

FieldViewer::~FieldViewer() = default;

void FieldViewer::setBoard(std::shared_ptr<const BoardData> board) {
    QPainterPath outline, copper;
    if (board) {
        // The simulation's frame is the board's, shifted so the outline's lower-left corner is at
        // the origin (libkiems' port resolution does the same).
        double xMin = std::numeric_limits<double>::infinity(), yMin = xMin;
        for (const libkicad::PolygonLoop& loop : board->geometry.outline) {
            for (const auto& [x, y] : loop.pointsMm) {
                xMin = std::min(xMin, x);
                yMin = std::min(yMin, y);
            }
        }
        if (std::isfinite(xMin)) {
            auto add = [&](QPainterPath& path, const libkicad::PolygonLoop& loop) {
                if (loop.pointsMm.size() < 2) return;
                QPolygonF polygon;
                for (const auto& [x, y] : loop.pointsMm) polygon << QPointF(x - xMin, y - yMin);
                polygon << polygon.front();
                path.addPolygon(polygon);
            };
            for (const libkicad::PolygonLoop& loop : board->geometry.outline) add(outline, loop);
            const std::string top = board->copperLayers.isEmpty() ? "F.Cu" : board->copperLayers.front().toStdString();
            for (const libkicad::CopperPolygon& polygon : board->geometry.copper) {
                // Tracks and pads; pours would only cover the picture.
                if (polygon.copperLayerName == top && !polygon.zone) add(copper, polygon.loop);
            }
        }
    }
    _canvas->setOverlay(outline, copper);
}

std::vector<FieldSeriesFile> FieldViewer::findSeries(const QString& simulationDirectory) {
    std::vector<FieldSeriesFile> series;
    const QDir directory(simulationDirectory);
    QFile manifest(directory.filePath("field_frames_current.txt"));
    if (manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&manifest);
        in.setEncoding(QStringConverter::Utf8);
        while (!in.atEnd()) {
            // "port path"; the path may contain spaces, and is relative to the simulation directory
            // unless the frames were stored elsewhere.
            const QString line = in.readLine().trimmed();
            const int space = line.indexOf(' ');
            bool ok = false;
            const int port = line.left(space).toInt(&ok);
            if (space < 0 || !ok) continue;
            const QString path = line.mid(space + 1);
            const QString absolute = QDir::isAbsolutePath(path) ? path : directory.filePath(path);
            if (QFileInfo::exists(absolute)) series.push_back({port, QDir::cleanPath(absolute)});
        }
    }
    if (!series.empty()) return series;
    // No manifest (an interrupted run): the newest file per port in the package.
    static const QRegularExpression name(QStringLiteral(R"(^(?:field_frames_)?port_(\d+)_[01]\.h5$)"));
    std::map<int, QFileInfo> newest;
    for (const QString& folder : {directory.filePath("field_frames.kemsf"), simulationDirectory}) {
        for (const QFileInfo& file : QDir(folder).entryInfoList({"*.h5"}, QDir::Files)) {
            const auto match = name.match(file.fileName());
            if (!match.hasMatch()) continue;
            const int port = match.captured(1).toInt();
            if (!newest.contains(port) || newest[port].lastModified() < file.lastModified()) newest[port] = file;
        }
    }
    for (const auto& [port, file] : newest) series.push_back({port, file.absoluteFilePath()});
    return series;
}

void FieldViewer::showSimulation(const QString& title, const QString& simulationDirectory,
                                 const std::map<int, QString>& portNames) {
    close();
    _simulationDirectory = simulationDirectory;
    _portNames = portNames;
    _series = findSeries(simulationDirectory);
    _title->setText(tr("%1: fields").arg(title));
    if (_series.empty()) {
        _emptyLabel->setText(tr("No field frames are stored for %1.\n\nRunning it with fields stored keeps a frame "
                                "every thousand timesteps of every excited port's run, to play back here.")
                                 .arg(title));
        _stack->setCurrentIndex(0);
        return;
    }
    _stack->setCurrentIndex(1);
    {
        const QSignalBlocker blocker(_port);
        _port->clear();
        for (const FieldSeriesFile& file : _series) {
            const auto name = _portNames.find(file.port);
            _port->addItem(name != _portNames.end() ? tr("Port %1: %2").arg(file.port).arg(name->second)
                                                    : tr("Port %1").arg(file.port),
                           file.port);
        }
    }
    openSeries(_series.front().port);
}

void FieldViewer::close() {
    _play->setChecked(false);
    ++_generation;
    _reader.reset();
    _shownFrame = -1;
    _canvas->clearImage();
}

void FieldViewer::openSeries(int port) {
    ++_generation;
    _reader.reset();
    _canvas->clearImage();
    const auto file = std::find_if(_series.begin(), _series.end(), [&](const FieldSeriesFile& f) { return f.port == port; });
    if (file == _series.end()) return;
    auto opened = copper::FieldFrameSeriesReader::open(fsPath(file->path));
    if (!opened) {
        _timeLabel->setText(tr("Couldn't open the frames: %1").arg(QString::fromStdString(opened.error())));
        return;
    }
    _reader = std::make_shared<copper::FieldFrameSeriesReader>(std::move(*opened));
    const auto& header = _reader->header();
    const bool energyOnly = header.storage.channels == copper::FieldChannels::Energy;
    // An energy-only series has only energy to show.
    for (int i = 0; i < _quantity->count(); ++i) {
        const bool available = !energyOnly || static_cast<Quantity>(_quantity->itemData(i).toInt()) == Quantity::Energy;
        _quantity->setItemData(i, available ? QVariant() : QVariant(0), Qt::UserRole - 1);
    }
    if (energyOnly) _quantity->setCurrentIndex(_quantity->findData(static_cast<int>(Quantity::Energy)));

    // Heights: the slice starts just inside the board under its top copper, where a trace's fields
    // are strongest.
    const auto& lines = energyOnly ? _reader->previewHeader().lineZ : header.lineZ;
    {
        const QSignalBlocker blocker(_height);
        _height->setRange(0, static_cast<int>(lines.size()) - 1);
        int start = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i] < header.boardZMax) start = static_cast<int>(i);
        }
        _height->setValue(start);
    }
    {
        const QSignalBlocker blocker(_time);
        _time->setRange(0, std::max(0, static_cast<int>(_reader->frameCount()) - 1));
        // The most telling frame is rarely the first (nothing has arrived yet): start a third of
        // the way in, while the pulse is crossing the board.
        _time->setValue(static_cast<int>(_reader->frameCount() / 3));
    }
    populateHeights();
    updateTimeLabel();
    requestFrame();
}

void FieldViewer::populateHeights() {
    if (!_reader) return;
    const auto& header = _reader->header();
    const bool energyOnly = header.storage.channels == copper::FieldChannels::Energy;
    const auto& lines = energyOnly ? _reader->previewHeader().lineZ : header.lineZ;
    const int index = _height->value();
    if (index < 0 || index >= static_cast<int>(lines.size())) return;
    const double z = lines[static_cast<std::size_t>(index)];
    const QString where = z > header.boardZMax ? tr("above the board")
                        : z < header.boardZMin ? tr("below the board")
                                               : tr("in the board");
    _heightLabel->setText(tr("%1 (%2)").arg(QuantityEdit::format(QuantityEdit::Kind::Length, z * 1e6), where));
}

void FieldViewer::updateTimeLabel() {
    if (!_reader || _reader->frameCount() == 0) {
        _timeLabel->clear();
        return;
    }
    const auto frame = static_cast<std::uint32_t>(_time->value());
    _timeLabel->setText(tr("Frame %1 of %2, %3 (step %4)")
                            .arg(frame + 1)
                            .arg(_reader->frameCount())
                            .arg(QuantityEdit::format(QuantityEdit::Kind::Time, _reader->timeSeconds(frame)))
                            .arg(_reader->timestep(frame)));
}

void FieldViewer::requestFrame() {
    if (!_reader || _reader->frameCount() == 0) return;
    if (_reading) {
        _readAgain = true;
        return;
    }
    _reading = true;
    _readAgain = false;
    const int generation = _generation;
    const int frame = _time->value();
    const auto quantity = static_cast<Quantity>(_quantity->currentData().toInt());
    const int z = _height->value();
    auto reader = _reader;
    QPointer<FieldViewer> self(this);
    auto* watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, watcher, &QObject::deleteLater);
    watcher->setFuture(QtConcurrent::run([=] {
        auto values = std::make_shared<std::vector<float>>();
        QString error;
        const auto& header = reader->header();
        const auto index = static_cast<std::uint32_t>(frame);
        if (header.storage.channels == copper::FieldChannels::Energy) {
            std::vector<float> energy, ex, ey, ez, hx, hy, hz;
            if (auto read = reader->readPreviewFrame(index, energy, ex, ey, ez, hx, hy, hz); !read) {
                error = QString::fromStdString(read.error());
            } else {
                const auto& preview = reader->previewHeader();
                const std::size_t slab = static_cast<std::size_t>(preview.nx) * preview.ny;
                values->assign(energy.begin() + static_cast<std::ptrdiff_t>(slab * z),
                               energy.begin() + static_cast<std::ptrdiff_t>(slab * (z + 1)));
            }
        } else {
            std::vector<float> ex, ey, ez, hx, hy, hz;
            if (auto read = reader->readRegion(index, 0, 0, static_cast<std::uint32_t>(z), header.nx, header.ny, 1, ex, ey,
                                               ez, hx, hy, hz);
                !read) {
                error = QString::fromStdString(read.error());
            } else {
                constexpr double epsilon0 = 8.8541878128e-12, mu0 = 1.25663706212e-6;
                values->resize(ex.size());
                for (std::size_t i = 0; i < ex.size(); ++i) {
                    const double e2 = double(ex[i]) * ex[i] + double(ey[i]) * ey[i] + double(ez[i]) * ez[i];
                    const double h2 = double(hx[i]) * hx[i] + double(hy[i]) * hy[i] + double(hz[i]) * hz[i];
                    double v = 0;
                    switch (quantity) {
                    case Quantity::MagnitudeE: v = std::sqrt(e2); break;
                    case Quantity::Ex: v = ex[i]; break;
                    case Quantity::Ey: v = ey[i]; break;
                    case Quantity::Ez: v = ez[i]; break;
                    case Quantity::MagnitudeH: v = std::sqrt(h2); break;
                    case Quantity::Hx: v = hx[i]; break;
                    case Quantity::Hy: v = hy[i]; break;
                    case Quantity::Hz: v = hz[i]; break;
                    case Quantity::Energy: v = 0.5 * (epsilon0 * e2 + mu0 * h2); break;
                    }
                    (*values)[i] = static_cast<float>(v);
                }
            }
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(), [self, generation, frame, values, error] {
                if (self) self->frameRead(generation, frame, values, error);
            },
            Qt::QueuedConnection);
    }));
}

void FieldViewer::frameRead(int generation, int frame, std::shared_ptr<std::vector<float>> values, QString error) {
    _reading = false;
    const bool stale = generation != _generation || frame != _time->value();
    if (stale || _readAgain) {
        if (_reader) requestFrame();
        if (stale) return;
    }
    if (!_reader) return;
    if (!error.isEmpty()) {
        _timeLabel->setText(tr("Couldn't read frame %1: %2").arg(frame + 1).arg(error));
        return;
    }
    const auto& header = _reader->header();
    const bool energyOnly = header.storage.channels == copper::FieldChannels::Energy;
    const auto& grid = energyOnly ? _reader->previewHeader() : header;
    const auto nx = static_cast<std::size_t>(grid.nx), ny = static_cast<std::size_t>(grid.ny);
    if (values->size() < nx * ny || nx < 2 || ny < 2) return;
    const auto quantity = static_cast<Quantity>(_quantity->currentData().toInt());

    // The colour scale's top: the whole series' peak, so playback keeps one scale.
    double reference = 0;
    auto peak = [&](copper::FieldComponent component) {
        const auto range = _reader->componentRange(component);
        return range ? std::max(std::fabs(range->minimum), std::fabs(range->maximum)) : 0.0f;
    };
    using C = copper::FieldComponent;
    switch (quantity) {
    case Quantity::MagnitudeE: reference = std::hypot(peak(C::Ex), peak(C::Ey), peak(C::Ez)); break;
    case Quantity::Ex: reference = peak(C::Ex); break;
    case Quantity::Ey: reference = peak(C::Ey); break;
    case Quantity::Ez: reference = peak(C::Ez); break;
    case Quantity::MagnitudeH: reference = std::hypot(peak(C::Hx), peak(C::Hy), peak(C::Hz)); break;
    case Quantity::Hx: reference = peak(C::Hx); break;
    case Quantity::Hy: reference = peak(C::Hy); break;
    case Quantity::Hz: reference = peak(C::Hz); break;
    case Quantity::Energy:
        for (std::uint32_t i = 0; i < _reader->frameCount(); ++i) reference = std::max<double>(reference, _reader->maxEnergy(i));
        break;
    }
    if (!(reference > 0)) reference = 1;
    const double rangeDb = _range->value();
    // Field amplitudes in 20 log10, energy (already a square) in 10 log10.
    const double decibelsPerDecade = quantity == Quantity::Energy ? 10 : 20;

    // Resample onto an even grid, so the graded mesh's cells keep their real sizes.
    const auto& lineX = grid.lineX;
    const auto& lineY = grid.lineY;
    const double x0 = lineX.front(), x1 = lineX.back(), y0 = lineY.front(), y1 = lineY.back();
    const double aspect = (y1 - y0) / (x1 - x0);
    const int width = static_cast<int>(std::clamp<std::size_t>(nx * 2, 400, 1400));
    const int height = std::max(1, static_cast<int>(width * aspect));
    auto nearest = [](const std::vector<double>& lines, double value) {
        const auto upper = std::lower_bound(lines.begin(), lines.end(), value);
        if (upper == lines.begin()) return std::size_t(0);
        if (upper == lines.end()) return lines.size() - 1;
        const auto lower = upper - 1;
        return static_cast<std::size_t>((value - *lower < *upper - value ? lower : upper) - lines.begin());
    };
    std::vector<std::size_t> columns(static_cast<std::size_t>(width)), rows(static_cast<std::size_t>(height));
    for (int i = 0; i < width; ++i) columns[static_cast<std::size_t>(i)] = nearest(lineX, x0 + (x1 - x0) * (i + 0.5) / width);
    for (int j = 0; j < height; ++j) rows[static_cast<std::size_t>(j)] = nearest(lineY, y1 - (y1 - y0) * (j + 0.5) / height);
    const auto& domain = grid.domainXYClass;
    const bool masked = !energyOnly && domain.size() == nx * ny;

    QImage image(width, height, QImage::Format_ARGB32);
    const bool diverging = isSigned(quantity);
    for (int j = 0; j < height; ++j) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(j));
        const std::size_t row = rows[static_cast<std::size_t>(j)] * nx;
        for (int i = 0; i < width; ++i) {
            const std::size_t cell = row + columns[static_cast<std::size_t>(i)];
            if (masked && domain[cell] == 0) {
                line[i] = qRgba(0, 0, 0, 0);
                continue;
            }
            const double v = (*values)[cell];
            const double magnitude = std::fabs(v);
            const double t = magnitude > 0
                ? (decibelsPerDecade * std::log10(magnitude / reference) + rangeDb) / rangeDb
                : 0.0;
            line[i] = diverging ? signedColor(t, v < 0) : magnitudeColor(t);
        }
    }
    const QString unit = unitOf(quantity);
    const QString top = (diverging ? QString::fromUtf8("\xC2\xB1") : QString()) +
                        QString::number(reference, 'g', 3) + " " + unit;
    const QString bottom = (diverging ? QString::fromUtf8("\xC2\xB1") : QString()) + tr("%1 dB").arg(-rangeDb);
    _canvas->setImage(image, QRectF(x0 * 1e3, y0 * 1e3, (x1 - x0) * 1e3, (y1 - y0) * 1e3), top, bottom, diverging);
    _shownFrame = frame;
}

} // namespace kiems_gui
