#include "SelectionPanel.hpp"

#include "Document.hpp"
#include "QuantityEdit.hpp"
#include "SimulationEditor.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

namespace kiems_gui {

namespace {

QLabel* heading() {
    auto* label = new QLabel;
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

} // namespace

SelectionPanel::SelectionPanel(Document* document, QWidget* parent) : QWidget(parent), _document(document) {
    using Kind = QuantityEdit::Kind;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    _nothing = new QLabel(tr("Click a pad or a trace on the board to set up how this simulation uses it."));
    _nothing->setWordWrap(true);
    _nothing->setEnabled(false);
    layout->addWidget(_nothing);

    // --- Pin
    _pinGroup = new QGroupBox(tr("Pin"));
    auto* pinForm = new QFormLayout(_pinGroup);
    _pinLabel = heading();
    _excited = new QCheckBox(tr("Excited"));
    _excited->setToolTip(tr("Drive a signal into the board at this pin. Every main excitation is its own run; "
                            "adversarial ones add their noise to the eyes."));
    _excitation = new QWidget;
    _excitationForm = new QFormLayout(_excitation);
    _excitationForm->setContentsMargins(16, 0, 0, 0);
    _excitationType = new QComboBox;
    _excitationType->addItems({tr("Main"), tr("Adversarial")});
    _excitationType->setToolTip(tr("Main: the broadband pulse the results are measured with. Adversarial: a "
                                   "narrowband aggressor whose coupled noise is added to the eyes."));
    _startTime = new QuantityEdit(Kind::Time);
    _startTime->setMinimum(0);
    _phase = new QuantityEdit(Kind::Angle);
    _amplitude = new QuantityEdit(Kind::Number);
    _amplitude->setToolTip(tr("The drive's amplitude relative to the main excitation (negative inverts it)."));
    _frequency = new QuantityEdit(Kind::Frequency);
    _frequency->setMinimum(0);
    _durationMode = new QComboBox;
    _durationMode->addItems({tr("Continuous"), tr("Limited")});
    _duration = new QuantityEdit(Kind::Time);
    _duration->setMinimum(0);
    _excitationForm->addRow(tr("Type:"), _excitationType);
    _excitationForm->addRow(tr("Start time:"), _startTime);
    _excitationForm->addRow(tr("Phase:"), _phase);
    _excitationForm->addRow(tr("Relative amplitude:"), _amplitude);
    _excitationForm->addRow(tr("Frequency:"), _frequency);
    _excitationForm->addRow(tr("Runs:"), _durationMode);
    _excitationForm->addRow(tr("Duration:"), _duration);
    _probed = new QCheckBox(tr("Probed"));
    _probed->setToolTip(tr("Measure the signal arriving at this pin: it becomes a port in the results."));
    _absorbing = new QCheckBox(tr("Absorbing"));
    _absorbing->setToolTip(tr("Terminate this pin in its impedance, as the input it connects to would, instead of "
                              "leaving it an open, reflecting end."));
    _pinImpedance = new QuantityEdit(Kind::Resistance);
    _pinImpedance->setAllowEmpty(true);
    _pinImpedance->setMinimum(0);
    _pinImpedance->setToolTip(tr("The impedance of what's connected here (the port's termination). Empty uses the "
                                 "net's."));
    _loadCapacitance = new QuantityEdit(Kind::Capacitance);
    _loadCapacitance->setAllowEmpty(true, tr("None"));
    _loadCapacitance->setMinimum(0);
    _loadCapacitance->setToolTip(tr("The input capacitance of what's connected here, modelled as a capacitor from the "
                                    "pad to its reference plane."));
    pinForm->addRow(_pinLabel);
    pinForm->addRow(_excited);
    pinForm->addRow(_excitation);
    pinForm->addRow(_probed);
    pinForm->addRow(_absorbing);
    pinForm->addRow(tr("Impedance:"), _pinImpedance);
    pinForm->addRow(tr("Load capacitance:"), _loadCapacitance);
    layout->addWidget(_pinGroup);

    // --- Component
    _componentGroup = new QGroupBox(tr("Component"));
    auto* componentForm = new QFormLayout(_componentGroup);
    _componentLabel = heading();
    _componentIncluded = new QCheckBox(tr("Included in simulation"));
    _componentIncluded->setToolTip(tr("Model this resistor, inductor or capacitor as a lumped component between its "
                                      "pads."));
    _componentHull = new QCheckBox(tr("Contributes to hull"));
    _componentHull->setToolTip(tr("Grow the simulated area to take in this component's pads."));
    _componentPadding = new QuantityEdit(Kind::Length);
    _componentPadding->setMinimum(0);
    componentForm->addRow(_componentLabel);
    componentForm->addRow(_componentIncluded);
    componentForm->addRow(_componentHull);
    componentForm->addRow(tr("Hull padding:"), _componentPadding);
    layout->addWidget(_componentGroup);

    // --- Net
    _netGroup = new QGroupBox(tr("Net"));
    auto* netForm = new QFormLayout(_netGroup);
    _netLabel = heading();
    _netClassLabel = new QLabel;
    _netIncluded = new QCheckBox(tr("Included in simulation"));
    _netIncluded->setToolTip(tr("Put this net's copper in the simulated geometry, for what it couples to."));
    _netSimulated = new QCheckBox(tr("Contributes to hull"));
    _netSimulated->setToolTip(tr("Make this a simulated net: the simulated area grows to take in its copper, and its "
                                 "pins can be excited and probed."));
    _netPadding = new QuantityEdit(Kind::Length);
    _netPadding->setMinimum(0);
    _netPadding->setToolTip(tr("How far beyond this net's copper the simulated area reaches."));
    _netImpedance = new QuantityEdit(Kind::Resistance);
    _netImpedance->setMinimum(0);
    _netImpedance->setToolTip(tr("The impedance this net's ports are terminated in, unless a pin sets its own."));
    _impedanceProbed = new QCheckBox(tr("Impedance probed"));
    _impedanceProbed->setToolTip(tr("Measure the impedance along this net's straight trace runs."));
    netForm->addRow(_netLabel);
    netForm->addRow(tr("Net class:"), _netClassLabel);
    netForm->addRow(_netIncluded);
    netForm->addRow(_netSimulated);
    netForm->addRow(tr("Hull padding:"), _netPadding);
    netForm->addRow(tr("Impedance:"), _netImpedance);
    netForm->addRow(_impedanceProbed);
    layout->addWidget(_netGroup);

    // --- Pin edits
    connect(_excited, &QCheckBox::toggled, this, [this](bool on) {
        if (!_updating) editor().setPinExcited(_pick.footprint, _pick.pad, on);
    });
    connect(_probed, &QCheckBox::toggled, this, [this](bool on) {
        if (!_updating) editor().setPinProbed(_pick.footprint, _pick.pad, on);
    });
    connect(_absorbing, &QCheckBox::toggled, this, [this](bool on) {
        if (!_updating) editor().setPinAbsorbing(_pick.footprint, _pick.pad, on);
    });
    connect(_pinImpedance, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        editor().setPinImpedance(_pick.footprint, _pick.pad, value);
    });
    connect(_loadCapacitance, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        editor().setPinLoadCapacitance(_pick.footprint, _pick.pad, value && *value > 0 ? value : std::nullopt);
    });
    connect(_excitationType, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!_updating && index >= 0) editor().setExcitationMain(_pick.footprint, _pick.pad, index == 0);
    });
    connect(_durationMode, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (_updating || index < 0) return;
        editor().updateExcitation(_pick.footprint, _pick.pad, [index](kiems::ExcitationConfig& excitation) {
            excitation.setDurationMode(index == 0 ? kiems::ExcitationDurationMode::Continuous
                                                  : kiems::ExcitationDurationMode::Limited);
        });
    });
    auto excitationField = [this](QuantityEdit* field, std::function<void(kiems::ExcitationConfig&, double)> set) {
        connect(field, &QuantityEdit::valueEdited, this, [this, set](std::optional<double> value) {
            if (!value) return;
            editor().updateExcitation(_pick.footprint, _pick.pad,
                                      [&](kiems::ExcitationConfig& excitation) { set(excitation, *value); });
        });
    };
    excitationField(_startTime, [](kiems::ExcitationConfig& e, double v) { e.setStartTime(v); });
    excitationField(_phase, [](kiems::ExcitationConfig& e, double v) { e.setPhaseDegrees(v); });
    excitationField(_amplitude, [](kiems::ExcitationConfig& e, double v) { e.setAmplitude(v); });
    excitationField(_frequency, [](kiems::ExcitationConfig& e, double v) { e.setFrequency(v); });
    excitationField(_duration, [](kiems::ExcitationConfig& e, double v) { e.setDuration(v); });

    // --- Component edits
    connect(_componentIncluded, &QCheckBox::toggled, this, [this](bool on) {
        if (!_updating) editor().setComponentIncluded(_pick.footprint, on);
    });
    connect(_componentHull, &QCheckBox::toggled, this, [this](bool on) {
        if (!_updating) editor().setComponentContributesToHull(_pick.footprint, on);
    });
    connect(_componentPadding, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (value) editor().setComponentHullPadding(_pick.footprint, *value);
    });

    // --- Net edits
    connect(_netIncluded, &QCheckBox::toggled, this, [this](bool on) {
        if (!_updating) editor().setNetIncluded(_pick.net, on);
    });
    connect(_netSimulated, &QCheckBox::toggled, this, [this](bool on) {
        if (!_updating) editor().setNetSimulated(_pick.net, on);
    });
    connect(_netPadding, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (value) editor().setHullPadding(_pick.net, *value);
    });
    connect(_netImpedance, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (value) editor().setNetImpedance(_pick.net, *value);
    });
    connect(_impedanceProbed, &QCheckBox::toggled, this, [this](bool on) {
        if (!_updating) editor().setImpedanceProbed(_pick.net, on);
    });

    connect(_document, &Document::changed, this, &SelectionPanel::refresh);
    refresh();
}

SelectionPanel::~SelectionPanel() = default;

SimulationEditor SelectionPanel::editor() const {
    return SimulationEditor(*_document, _board.get(), _simulation, defaultPlane());
}

std::optional<int> SelectionPanel::defaultPlane() const {
    if (!_board || _board->copperLayers.size() < 2) return std::nullopt;
    if (_board->copperLayers.size() > 2) return 1;
    return _pick.layer == _board->copperLayers.value(0) ? 1 : 0;
}

bool SelectionPanel::pinPicked() const {
    return !_pick.footprint.isEmpty() && !SimulationEditor::isPassive(_pick.footprint);
}

bool SelectionPanel::componentPicked() const {
    // A passive part's pads stand for the part, as on macOS: it's modelled whole or not at all.
    return !_pick.footprint.isEmpty() && SimulationEditor::isPassive(_pick.footprint);
}

void SelectionPanel::setBoard(std::shared_ptr<const BoardData> board) {
    _board = std::move(board);
    refresh();
}

void SelectionPanel::setSimulation(int simulation) {
    _simulation = simulation;
    refresh();
}

void SelectionPanel::setPick(const BoardPick& pick) {
    _pick = pick;
    refresh();
}

void SelectionPanel::refresh() {
    _updating = true;
    const SimulationEditor edit = editor();
    const bool haveSimulation = _document->isOpen() && edit.isValid();
    const bool havePick = !_pick.net.isEmpty() || !_pick.footprint.isEmpty();
    _nothing->setVisible(!havePick);
    _nothing->setText(haveSimulation ? tr("Click a pad or a trace on the board to set up how this simulation uses it.")
                                     : tr("Select a simulation, then click a pad or a trace on the board."));

    // --- Pin
    _pinGroup->setVisible(pinPicked());
    if (pinPicked()) {
        QString pinText = tr("%1 pin %2").arg(_pick.footprint, _pick.pad);
        if (_board) {
            if (const libkicad::PadPosition* pad = _board->pad(_pick.footprint, _pick.pad); pad && !pad->pinType.empty()) {
                pinText += " (" + QString::fromStdString(pad->pinType).replace('_', ' ') + ")";
            }
        }
        _pinLabel->setText(pinText);
        const bool editable = haveSimulation && !_pick.net.isEmpty();
        for (QWidget* control : std::initializer_list<QWidget*>{_excited, _probed, _absorbing, _pinImpedance,
                                                                _loadCapacitance, _excitation}) {
            control->setEnabled(editable);
        }
        const PinState state = haveSimulation ? edit.pinState(_pick.footprint, _pick.pad) : PinState{};
        _excited->setChecked(state.excited);
        _probed->setChecked(state.probed);
        _absorbing->setChecked(state.absorbing);
        // An excited pin is always terminated, whatever its own setting says.
        _absorbing->setEnabled(editable && !state.excited);
        _pinImpedance->refreshValue(state.impedance);
        _pinImpedance->setPlaceholderText(QuantityEdit::format(QuantityEdit::Kind::Resistance, state.netImpedance) +
                                          tr(" (net)"));
        _loadCapacitance->refreshValue(state.loadCapacitance);

        const auto excitation = haveSimulation ? edit.excitation(_pick.footprint, _pick.pad) : std::nullopt;
        _excitation->setVisible(excitation.has_value());
        if (excitation) {
            const bool main = excitation->isMain();
            const bool continuous = excitation->durationMode() == kiems::ExcitationDurationMode::Continuous;
            _excitationType->setCurrentIndex(main ? 0 : 1);
            _startTime->refreshValue(excitation->startTime());
            _phase->refreshValue(excitation->phaseDegrees());
            _amplitude->refreshValue(excitation->amplitude().value_or(1.0));
            _frequency->refreshValue(excitation->frequency().value_or(
                _document->documentValue("/frequency/start").get<double>()));
            _durationMode->setCurrentIndex(continuous ? 0 : 1);
            _duration->refreshValue(excitation->duration());
            // A main excitation is the sweep's broadband pulse: no frequency of its own, and its
            // duration is its pulse window. An adversarial one runs continuously or for a while.
            _excitationForm->setRowVisible(_frequency, !main);
            _excitationForm->setRowVisible(_durationMode, !main);
            _excitationForm->setRowVisible(_duration, main || !continuous);
        }
    }

    // --- Component
    _componentGroup->setVisible(componentPicked());
    if (componentPicked()) {
        QString text = _pick.footprint;
        if (_board) {
            for (const libkicad::FootprintInfo& info : _board->footprints) {
                if (QString::fromStdString(info.reference) == _pick.footprint && !info.value.empty()) {
                    text += " (" + QString::fromStdString(info.value) + ")";
                }
            }
        }
        _componentLabel->setText(text);
        const bool included = haveSimulation && edit.componentIncluded(_pick.footprint);
        const bool contributes = included && edit.componentContributesToHull(_pick.footprint);
        _componentIncluded->setEnabled(haveSimulation);
        _componentIncluded->setChecked(included);
        _componentHull->setEnabled(included);
        _componentHull->setChecked(contributes);
        _componentPadding->setEnabled(contributes);
        _componentPadding->refreshValue(haveSimulation ? std::optional(edit.componentHullPadding(_pick.footprint))
                                                       : std::nullopt);
    }

    // --- Net
    _netGroup->setVisible(!_pick.net.isEmpty());
    if (!_pick.net.isEmpty()) {
        _netLabel->setText(_pick.net);
        QString netClass;
        if (_board) {
            const auto it = _board->netClasses.find(_pick.net.toStdString());
            if (it != _board->netClasses.end()) netClass = QString::fromStdString(it->second);
        }
        _netClassLabel->setText(netClass.isEmpty() ? tr("--") : netClass);
        const NetLevel level = haveSimulation ? edit.netLevel(_pick.net) : NetLevel::None;
        _netIncluded->setEnabled(haveSimulation);
        _netIncluded->setChecked(level != NetLevel::None);
        _netSimulated->setEnabled(level != NetLevel::None);
        _netSimulated->setChecked(level == NetLevel::Simulated);
        _netPadding->setEnabled(level == NetLevel::Simulated);
        _netPadding->refreshValue(haveSimulation ? std::optional(edit.hullPadding(_pick.net)) : std::nullopt);
        _netImpedance->setEnabled(level != NetLevel::None);
        _netImpedance->refreshValue(haveSimulation ? std::optional(edit.netImpedance(_pick.net)) : std::nullopt);
        _impedanceProbed->setEnabled(level != NetLevel::None);
        _impedanceProbed->setChecked(haveSimulation && edit.impedanceProbed(_pick.net));
    }
    _updating = false;
}

} // namespace kiems_gui
