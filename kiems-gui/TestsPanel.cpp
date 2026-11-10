#include "TestsPanel.hpp"

#include "Document.hpp"
#include "EyeTestPresets.hpp"
#include "QuantityEdit.hpp"

#include <QAction>
#include <QActionGroup>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kiems_gui {

/// A key to the mask fields below it, as the macOS app draws one: an example eye -- traces that
/// cross the threshold at 0 and 1 UI and open smoothly between -- with a keep-out hexagon in its
/// opening and an outer limit beyond it, dimensioned with where X1, X2, Y1 and Y2 are measured.
/// The example is deliberately sloped so X1 and X2 stay distinct; the fields hold the real values.
class MaskShapeView : public QWidget {
public:
    using QWidget::QWidget;

    QSize sizeHint() const override { return {260, 190}; }
    QSize minimumSizeHint() const override { return {200, 190}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor text = palette().color(QPalette::WindowText);
        QColor guide = text;
        guide.setAlphaF(0.35);
        QColor construction = text;
        construction.setAlphaF(0.85);
        const QColor keepOut(0xff, 0x45, 0x3a);
        const QColor limit(0xff, 0x9f, 0x0a);
        const QColor trace(0x5a, 0x9b, 0xe6);

        QFont labelFont = font();
        labelFont.setPointSizeF(labelFont.pointSizeF() * 0.9);
        labelFont.setBold(true);
        QFont smallFont = font();
        smallFont.setPointSizeF(smallFont.pointSizeF() * 0.8);

        painter.setFont(labelFont);
        painter.setPen(text);
        painter.drawText(QPointF(0, 13), QObject::tr("Eye mask profile"));

        // The example: levels at +-L, a keep-out hexagon (x1, x2, y1) and an outer limit (y2).
        constexpr double x1 = 0.18, x2 = 0.32, y1 = 0.30, y2 = 0.74, level = 0.6;
        const double yExtent = y2 * 1.2;
        // A left gutter for the vertical dimensions; room above and below for the horizontal ones.
        const QRectF plot(58, 44, std::max(width() - 64.0, 1.0), height() - 44.0 - 40.0);
        auto point = [&](double x, double y) {
            return QPointF(plot.left() + (x + 0.1) / 1.2 * plot.width(), plot.center().y() - y / yExtent * plot.height() / 2);
        };
        auto pen = [](QColor color, double width, bool dashed = false) {
            QPen p(color, width);
            if (dashed) p.setDashPattern({3, 3});
            p.setCapStyle(Qt::FlatCap);
            return p;
        };

        // The outer limit: everything beyond +-Y2 is out of bounds.
        QColor limitFill = limit;
        limitFill.setAlphaF(0.22);
        for (const double sign : {1.0, -1.0}) {
            const double edge = point(0, sign * y2).y();
            const QRectF band = sign > 0 ? QRectF(QPointF(plot.left(), plot.top()), QPointF(plot.right(), edge))
                                         : QRectF(QPointF(plot.left(), edge), QPointF(plot.right(), plot.bottom()));
            painter.fillRect(band, limitFill);
            painter.setPen(pen(limit, 1.5));
            painter.drawLine(QPointF(plot.left(), edge), QPointF(plot.right(), edge));
        }

        // The eye: every 3-bit pattern, each transition a raised cosine centred on its crossing,
        // so the opening widens smoothly from each crossing to its full height.
        auto ramp = [](double u) {
            constexpr double halfWidth = 0.25;
            if (u <= -halfWidth) return 0.0;
            if (u >= halfWidth) return 1.0;
            return 0.5 - 0.5 * std::cos(std::numbers::pi * (u + halfWidth) / (2 * halfWidth));
        };
        QColor traceColor = trace;
        traceColor.setAlphaF(0.55);
        painter.setPen(pen(traceColor, 1.3));
        int variant = 0;
        for (int a = -1; a <= 1; a += 2) {
            for (int b = -1; b <= 1; b += 2) {
                for (int c = -1; c <= 1; c += 2) {
                    const double scale = level * (0.95 + 0.025 * (variant++ % 3));
                    QPainterPath path;
                    for (int i = 0; i <= 120; ++i) {
                        const double x = -0.1 + 1.2 * i / 120.0;
                        const double y = scale * (a + (b - a) * ramp(x) + (c - b) * ramp(x - 1));
                        if (i == 0) path.moveTo(point(x, y));
                        else path.lineTo(point(x, y));
                    }
                    painter.drawPath(path);
                }
            }
        }

        // The threshold and the two crossings.
        painter.setPen(pen(guide, 1, true));
        painter.drawLine(point(-0.1, 0), point(1.1, 0));
        for (const double x : {0.0, 1.0}) {
            painter.drawLine(QPointF(point(x, 0).x(), plot.top()), QPointF(point(x, 0).x(), plot.bottom()));
        }
        painter.setFont(smallFont);
        painter.setPen(guide);
        const QFontMetricsF small(smallFont);
        painter.drawText(QPointF(point(0, 0).x() - small.horizontalAdvance("0") / 2, plot.bottom() + 12), "0");
        painter.drawText(QPointF(point(1, 0).x() - small.horizontalAdvance("1 UI") / 2, plot.bottom() + 12), "1 UI");

        // The keep-out hexagon in the opening.
        QPainterPath hexagon;
        hexagon.moveTo(point(x1, 0));
        hexagon.lineTo(point(x2, y1));
        hexagon.lineTo(point(1 - x2, y1));
        hexagon.lineTo(point(1 - x1, 0));
        hexagon.lineTo(point(1 - x2, -y1));
        hexagon.lineTo(point(x2, -y1));
        hexagon.closeSubpath();
        QColor keepOutFill = keepOut;
        keepOutFill.setAlphaF(0.35);
        painter.setBrush(keepOutFill);
        painter.setPen(pen(keepOut, 1.5));
        painter.drawPath(hexagon);
        painter.setBrush(Qt::NoBrush);

        // Dimensions: a line with end ticks, a dashed projection to the feature, and a label.
        painter.setFont(labelFont);
        const QFontMetricsF labels(labelFont);
        auto horizontal = [&](double y, double fromX, double toX, const QColor& color, const QString& label,
                              bool labelAbove) {
            painter.setPen(pen(color, 1.5));
            painter.drawLine(QPointF(fromX, y), QPointF(toX, y));
            painter.drawLine(QPointF(fromX, y - 3), QPointF(fromX, y + 3));
            painter.drawLine(QPointF(toX, y - 3), QPointF(toX, y + 3));
            painter.setPen(text);
            const double labelY = labelAbove ? y - 5 : y + labels.ascent() + 3;
            painter.drawText(QPointF((fromX + toX - labels.horizontalAdvance(label)) / 2, labelY), label);
        };
        auto vertical = [&](double x, double fromY, double toY, const QColor& color, const QString& label) {
            painter.setPen(pen(color, 1.5));
            painter.drawLine(QPointF(x, fromY), QPointF(x, toY));
            painter.drawLine(QPointF(x - 3, fromY), QPointF(x + 3, fromY));
            painter.drawLine(QPointF(x - 3, toY), QPointF(x + 3, toY));
            painter.setPen(text);
            painter.drawText(QPointF(x - labels.horizontalAdvance(label) - 4, (fromY + toY) / 2 + labels.ascent() / 2 - 1),
                             label);
        };
        auto projection = [&](QPointF from, QPointF to, const QColor& color) {
            painter.setPen(pen(color, 1, true));
            painter.drawLine(from, to);
        };

        const QPointF crossing = point(0, 0);
        // X1: from the left crossing to where the keep-out meets the threshold (below the plot).
        const QPointF x1Point = point(x1, 0);
        const double x1Y = plot.bottom() + 20;
        projection(QPointF(x1Point.x(), x1Y), x1Point, keepOut);
        projection(QPointF(crossing.x(), plot.bottom()), QPointF(crossing.x(), x1Y), guide);
        horizontal(x1Y, crossing.x(), x1Point.x(), keepOut, "X1", false);
        // X2: from the same crossing to where the keep-out reaches full height (above the plot).
        const QPointF x2Corner = point(x2, y1);
        const double x2Y = plot.top() - 14;
        projection(QPointF(x2Corner.x(), x2Y), x2Corner, construction);
        projection(QPointF(crossing.x(), x2Y), QPointF(crossing.x(), plot.top()), guide);
        horizontal(x2Y, crossing.x(), x2Corner.x(), keepOut, "X2", true);
        // Y1: from the threshold to the keep-out's top edge.
        const double y1Y = point(0, y1).y();
        const double y1X = plot.left() - 12;
        projection(QPointF(y1X, y1Y), x2Corner, construction);
        vertical(y1X, crossing.y(), y1Y, keepOut, "Y1");
        // Y2: from the threshold to the outer limit.
        const double y2Y = point(0, y2).y();
        const double y2X = plot.left() - 34;
        projection(QPointF(y2X, y2Y), QPointF(plot.left(), y2Y), limit);
        projection(QPointF(y2X, crossing.y()), QPointF(plot.left(), crossing.y()), guide);
        vertical(y2X, crossing.y(), y2Y, limit, "Y2");
    }
};

namespace {

double number(const nlohmann::json& value, double fallback = 0) {
    return value.is_number() ? value.get<double>() : fallback;
}

bool same(double a, double b) {
    return std::fabs(a - b) <= 1e-9 * std::max({std::fabs(a), std::fabs(b), 1e-3});
}

} // namespace

TestsPanel::TestsPanel(Document* document, QWidget* parent) : QWidget(parent), _document(document) {
    using Kind = QuantityEdit::Kind;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* group = new QGroupBox(tr("Eye test"));
    auto* form = new QFormLayout(group);

    _standard = new QPushButton;
    auto* menu = new QMenu(_standard);
    auto* actions = new QActionGroup(menu);
    auto addStandard = [&](QMenu* into, const QString& title, const QString& standard) {
        QAction* action = into->addAction(title);
        action->setCheckable(true);
        action->setData(standard);
        actions->addAction(action);
        connect(action, &QAction::triggered, this, [this, standard] { pickStandard(standard); });
    };
    addStandard(menu, tr("Custom"), "custom");
    menu->addSeparator();
    for (const QString& family : EyeTestPreset::families()) {
        QMenu* submenu = menu->addMenu(family);
        for (const EyeTestPreset& preset : EyeTestPreset::all()) {
            if (preset.family != family) continue;
            if (preset.startsGroup) submenu->addSeparator();
            addStandard(submenu, preset.menuTitle, preset.standard);
        }
    }
    _standard->setMenu(menu);
    // Check the current standard's item each time the menu opens.
    connect(menu, &QMenu::aboutToShow, this, [this, actions] {
        const QString current = QString::fromStdString(
            _document->simulationValue(_simulation, "/eye_test/standard").get<std::string>());
        for (QAction* action : actions->actions()) action->setChecked(action->data().toString() == current);
    });

    _bitRate = new QuantityEdit(Kind::BitRate);
    _bitRate->setMinimum(1);
    _bitRate->setToolTip(tr("The serial data rate the eye's PRBS pattern is sent at."));
    _signalLevel = new QuantityEdit(Kind::Voltage);
    _signalLevel->setMinimum(1e-6);
    _signalLevel->setToolTip(tr("The +/- level the PRBS pattern is launched at -- differential for a pair -- as a "
                                "driver matched to the line would."));
    _shape = new MaskShapeView;
    _x1 = new QuantityEdit(Kind::UnitInterval);
    _x1->setToolTip(tr("Where the mask's keep-out region meets the threshold, from the eye's left crossing (and as far "
                       "from its right one)."));
    _x2 = new QuantityEdit(Kind::UnitInterval);
    _x2->setToolTip(tr("Where the keep-out region reaches its full height, from the eye's left crossing. Equal to X1 "
                       "for a rectangle; 0.5 UI for a diamond."));
    for (QuantityEdit* field : {_x1, _x2}) {
        field->setMinimum(0);
        field->setMaximum(0.5);
    }
    _y1 = new QuantityEdit(Kind::Voltage);
    _y1->setToolTip(tr("How far the keep-out region reaches above and below the threshold. Empty for no keep-out "
                       "region."));
    _y2 = new QuantityEdit(Kind::Voltage);
    _y2->setToolTip(tr("How far from the threshold the signal may go at all. Empty for no limit."));
    for (QuantityEdit* field : {_y1, _y2}) {
        field->setMinimum(0);
        field->setAllowEmpty(true, tr("None"));
    }

    form->addRow(tr("Eye mask:"), _standard);
    form->addRow(tr("Bit rate:"), _bitRate);
    form->addRow(tr("Signal level:"), _signalLevel);
    form->addRow(_shape);
    form->addRow(tr("Mask X1:"), _x1);
    form->addRow(tr("Mask X2:"), _x2);
    form->addRow(tr("Mask Y1:"), _y1);
    form->addRow(tr("Outer limit Y2:"), _y2);
    layout->addWidget(group);
    auto* note = new QLabel(tr("Every eye in this simulation's results is tested against this mask."));
    note->setWordWrap(true);
    note->setEnabled(false);
    layout->addWidget(note);
    layout->addStretch();

    connect(_bitRate, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (value) edit("/eye_bit_rate", *value);
    });
    connect(_signalLevel, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (value) edit("/eye_test/signal_level", *value);
    });
    connect(_x1, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (value) edit("/eye_test/mask_x1", *value);
    });
    connect(_x2, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (value) edit("/eye_test/mask_x2", *value);
    });
    // Zero means "none" for both heights.
    connect(_y1, &QuantityEdit::valueEdited, this,
            [this](std::optional<double> value) { edit("/eye_test/mask_y1", value.value_or(0)); });
    connect(_y2, &QuantityEdit::valueEdited, this,
            [this](std::optional<double> value) { edit("/eye_test/mask_y2", value.value_or(0)); });

    connect(_document, &Document::changed, this, &TestsPanel::refresh);
    refresh();
}

void TestsPanel::setSimulation(int simulation) {
    _simulation = simulation;
    refresh();
}

void TestsPanel::pickStandard(const QString& standard) {
    if (_simulation < 0) return;
    const EyeTestPreset* preset = EyeTestPreset::find(standard);
    _document->setSimulationValue(_simulation, "/eye_test/standard", standard.toStdString());
    if (!preset) return; // Custom keeps the numbers as they are.
    if (preset->bitRate) _document->setSimulationValue(_simulation, "/eye_bit_rate", *preset->bitRate);
    _document->setSimulationValue(_simulation, "/eye_test/signal_level", preset->signalLevel);
    _document->setSimulationValue(_simulation, "/eye_test/mask_x1", preset->x1);
    _document->setSimulationValue(_simulation, "/eye_test/mask_x2", preset->x2);
    _document->setSimulationValue(_simulation, "/eye_test/mask_y1", preset->y1);
    _document->setSimulationValue(_simulation, "/eye_test/mask_y2", preset->y2);
}

void TestsPanel::edit(const QString& pointer, double value) {
    if (_simulation < 0) return;
    _document->setSimulationValue(_simulation, pointer, value);
    const QString standard =
        QString::fromStdString(_document->simulationValue(_simulation, "/eye_test/standard").get<std::string>());
    const EyeTestPreset* preset = EyeTestPreset::find(standard);
    if (!preset) return;
    auto value_of = [&](const char* key) { return number(_document->simulationValue(_simulation, key)); };
    const bool matches = (!preset->bitRate || same(*preset->bitRate, value_of("/eye_bit_rate"))) &&
                         same(preset->signalLevel, value_of("/eye_test/signal_level")) &&
                         same(preset->x1, value_of("/eye_test/mask_x1")) &&
                         same(preset->x2, value_of("/eye_test/mask_x2")) &&
                         same(preset->y1, value_of("/eye_test/mask_y1")) &&
                         same(preset->y2, value_of("/eye_test/mask_y2"));
    if (!matches) _document->setSimulationValue(_simulation, "/eye_test/standard", "custom");
}

void TestsPanel::refresh() {
    if (_simulation >= _document->simulationCount()) _simulation = -1;
    const bool haveSimulation = _document->isOpen() && _simulation >= 0;
    setEnabled(haveSimulation);
    if (!haveSimulation) {
        _standard->setText(tr("Custom"));
        for (QuantityEdit* field : {_bitRate, _signalLevel, _x1, _x2, _y1, _y2}) field->setValue(std::nullopt);
        return;
    }
    auto value = [&](const char* key) { return number(_document->simulationValue(_simulation, key)); };
    _standard->setText(EyeTestPreset::titleFor(
        QString::fromStdString(_document->simulationValue(_simulation, "/eye_test/standard").get<std::string>())));
    const double bitRate = value("/eye_bit_rate");
    // No bit rate of its own means the eye is built at the top of the frequency range.
    _bitRate->refreshValue(bitRate > 0 ? bitRate : number(_document->documentValue("/frequency/stop")));
    _signalLevel->refreshValue(value("/eye_test/signal_level"));
    _x1->refreshValue(value("/eye_test/mask_x1"));
    _x2->refreshValue(value("/eye_test/mask_x2"));
    const double y1 = value("/eye_test/mask_y1");
    const double y2 = value("/eye_test/mask_y2");
    _y1->refreshValue(y1 > 0 ? std::optional(y1) : std::nullopt);
    _y2->refreshValue(y2 > 0 ? std::optional(y2) : std::nullopt);
}

} // namespace kiems_gui
