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

namespace kiems_gui {

/// A sketch of the mask over one unit interval: the keep-out hexagon on the threshold, the outer
/// limit, and the nominal signal levels for scale.
class MaskShapeView : public QWidget {
public:
    using QWidget::QWidget;

    void setMask(double x1, double x2, double y1, double y2, double signalLevel) {
        _x1 = x1;
        _x2 = x2;
        _y1 = y1;
        _y2 = y2;
        _level = signalLevel;
        update();
    }

    QSize sizeHint() const override { return {240, 96}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area = QRectF(rect()).adjusted(8, 6, -8, -6);
        painter.fillRect(rect(), palette().color(QPalette::Base));
        const double extent = std::max({_y2, _level * 1.25, _y1 * 1.6, 1e-6});
        auto point = [&](double x, double y) {
            return QPointF(area.left() + x * area.width(), area.center().y() - y / extent * area.height() / 2);
        };
        const QColor muted = palette().color(QPalette::Disabled, QPalette::Text);

        // Nominal transitions and the threshold.
        painter.setPen(QPen(muted, 1, Qt::DashLine));
        painter.drawLine(point(0, extent), point(0, -extent));
        painter.drawLine(point(1, extent), point(1, -extent));
        painter.setPen(QPen(muted, 1));
        painter.drawLine(point(0, 0), point(1, 0));
        // The levels the signal is launched at.
        if (_level > 0) {
            painter.setPen(QPen(QColor(0x5a, 0x9b, 0xe6), 1.2));
            painter.drawLine(point(0, _level), point(1, _level));
            painter.drawLine(point(0, -_level), point(1, -_level));
        }
        // The outer limit: everything beyond it is out of bounds.
        if (_y2 > 0) {
            const QColor limit(0xe0, 0x6c, 0x5a, 90);
            painter.fillRect(QRectF(point(0, extent), point(1, _y2)), limit);
            painter.fillRect(QRectF(point(0, -_y2), point(1, -extent)), limit);
        }
        // The keep-out region.
        if (_y1 > 0 && _x1 < 0.5) {
            QPainterPath path;
            path.moveTo(point(_x1, 0));
            path.lineTo(point(_x2, _y1));
            path.lineTo(point(1 - _x2, _y1));
            path.lineTo(point(1 - _x1, 0));
            path.lineTo(point(1 - _x2, -_y1));
            path.lineTo(point(_x2, -_y1));
            path.closeSubpath();
            painter.setPen(QPen(QColor(0xe0, 0x6c, 0x5a), 1.2));
            painter.setBrush(QColor(0xe0, 0x6c, 0x5a, 110));
            painter.drawPath(path);
        }
    }

private:
    double _x1 = 0, _x2 = 0, _y1 = 0, _y2 = 0, _level = 0;
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
        _shape->setMask(0, 0, 0, 0, 0);
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
    _shape->setMask(value("/eye_test/mask_x1"), value("/eye_test/mask_x2"), y1, y2, value("/eye_test/signal_level"));
}

} // namespace kiems_gui
