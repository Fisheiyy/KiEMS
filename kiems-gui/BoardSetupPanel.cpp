#include "BoardSetupPanel.hpp"

#include "Document.hpp"
#include "QuantityEdit.hpp"
#include "SimulationEditor.hpp"

#include "kiems/port_resolution.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

namespace kiems_gui {

namespace {

constexpr int kKindRole = Qt::UserRole;     // "net" or "net_class"
constexpr int kNameRole = Qt::UserRole + 1;

std::string utf8(const QString& text) {
    return text.toUtf8().toStdString();
}

QString qstr(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

/// Adds `value` to `list` or takes every copy of it out.
void setMember(std::vector<std::string>& list, const std::string& value, bool member) {
    if (member) {
        if (!contains(list, value)) list.push_back(value);
    } else {
        std::erase(list, value);
    }
}

QLabel* heading() {
    auto* label = new QLabel;
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

QLabel* note(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setEnabled(false); // the palette's muted text
    return label;
}

QPushButton* smallButton(const QString& text, const QString& toolTip) {
    auto* button = new QPushButton(text);
    button->setToolTip(toolTip);
    button->setFixedWidth(28);
    return button;
}

QString formatOhms(double ohms) {
    return QuantityEdit::format(QuantityEdit::Kind::Resistance, ohms);
}

} // namespace

BoardSetupPanel::BoardSetupPanel(Document* document, QWidget* parent) : QWidget(parent), _document(document) {
    using Kind = QuantityEdit::Kind;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(note(tr("Defaults for the whole board. New simulations start from them, and a net takes "
                              "them when it's included; simulations that already exist keep their own settings.")));

    // --- Board-wide
    _boardGroup = new QGroupBox(tr("Board"));
    auto* boardForm = new QFormLayout(_boardGroup);
    _ground = new QComboBox;
    _ground->setObjectName("boardGround");
    _ground->setEditable(true);
    _ground->setInsertPolicy(QComboBox::NoInsert);
    _ground->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _ground->setMinimumContentsLength(8);
    _ground->setToolTip(tr("The ground net, or net class, new simulations start with. Empty guesses it from the "
                           "net names."));
    _edgeNets = new QListWidget;
    _edgeNets->setObjectName("boardEdgeNets");
    _edgeNets->setMaximumHeight(90);
    _edgeNets->setToolTip(tr("Nets new simulations terminate in their impedance where the simulated area cuts "
                             "them, rather than leaving an open end."));
    _edgeNetPicker = new QComboBox;
    _edgeNetPicker->setEditable(true);
    _edgeNetPicker->setInsertPolicy(QComboBox::NoInsert);
    _edgeNetPicker->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _edgeNetPicker->setMinimumContentsLength(8);
    _addEdgeNet = smallButton("+", tr("Add the chosen net."));
    _removeEdgeNet = smallButton(QString::fromUtf8("\xE2\x88\x92"), tr("Remove the selected net."));
    auto* edgeColumn = new QVBoxLayout;
    edgeColumn->setContentsMargins(0, 0, 0, 0);
    edgeColumn->addWidget(_edgeNets);
    auto* edgeRow = new QHBoxLayout;
    edgeRow->addWidget(_edgeNetPicker, 1);
    edgeRow->addWidget(_addEdgeNet);
    edgeRow->addWidget(_removeEdgeNet);
    edgeColumn->addLayout(edgeRow);

    _signalImpedance = new QuantityEdit(Kind::Resistance);
    _signalImpedance->setMinimum(0);
    _signalImpedance->setToolTip(tr("The impedance a signal net's ports are terminated in, unless its net class, the "
                                    "net or the pin sets one."));
    _powerImpedance = new QuantityEdit(Kind::Resistance);
    _powerImpedance->setMinimum(0);
    _powerImpedance->setToolTip(tr("The impedance a power net's ports are terminated in -- nets named like rails "
                                   "(+3V3, VCC, VBUS, GND...)."));
    _classImpedances = new QWidget;
    _classImpedanceForm = new QFormLayout(_classImpedances);
    _classImpedanceForm->setContentsMargins(0, 0, 0, 0);
    _bitRate = new QuantityEdit(Kind::BitRate);
    _bitRate->setAllowEmpty(true, tr("Automatic"));
    _bitRate->setMinimum(0);
    _bitRate->setToolTip(tr("The bit rate new simulations' eyes are drawn at. Automatic uses the top of the "
                            "frequency range."));
    boardForm->addRow(tr("Ground net:"), _ground);
    // Label above, list full width: beside a long label the list and its buttons outgrow the panel.
    boardForm->addRow(new QLabel(tr("Edge-terminated nets:")));
    boardForm->addRow(edgeColumn);
    boardForm->addRow(tr("Signal nets:"), _signalImpedance);
    boardForm->addRow(tr("Power nets:"), _powerImpedance);
    boardForm->addRow(note(tr("Net class impedances (empty uses the signal nets'):")));
    boardForm->addRow(_classImpedances);
    boardForm->addRow(tr("Digital bit rate:"), _bitRate);
    layout->addWidget(_boardGroup);

    // --- Picked pin
    _pinGroup = new QGroupBox(tr("Pin"));
    auto* pinForm = new QFormLayout(_pinGroup);
    _pinLabel = heading();
    _pinImpedance = new QuantityEdit(Kind::Resistance);
    _pinImpedance->setObjectName("boardPinImpedance");
    _pinImpedance->setAllowEmpty(true);
    _pinImpedance->setMinimum(0);
    _pinImpedance->setToolTip(tr("The impedance of what's connected to this pin. Empty uses the net's."));
    _pinAbsorbing = new QCheckBox(tr("Absorbing"));
    _pinAbsorbing->setObjectName("boardPinAbsorbing");
    _pinAbsorbing->setToolTip(tr("Terminate this pin in its impedance, as the input it connects to would. Unless "
                                 "set here, inputs on nets with more than one pad absorb and other pins don't."));
    _noise = new QCheckBox(tr("Noise generating"));
    _noise->setObjectName("boardPinNoise");
    _noise->setToolTip(tr("Drive this pin with a narrowband aggressor in every simulation that simulates its net, "
                          "for the eyes' crosstalk."));
    _noiseControls = new QWidget;
    auto* noiseForm = new QFormLayout(_noiseControls);
    noiseForm->setContentsMargins(16, 0, 0, 0);
    _noiseFrequency = new QuantityEdit(Kind::Frequency);
    _noiseFrequency->setMinimum(1);
    _noiseAmplitude = new QuantityEdit(Kind::Voltage);
    noiseForm->addRow(tr("Frequency:"), _noiseFrequency);
    noiseForm->addRow(tr("Amplitude:"), _noiseAmplitude);
    pinForm->addRow(_pinLabel);
    pinForm->addRow(tr("Impedance:"), _pinImpedance);
    pinForm->addRow(_pinAbsorbing);
    pinForm->addRow(_noise);
    pinForm->addRow(_noiseControls);
    layout->addWidget(_pinGroup);

    // --- Picked passive part
    _componentGroup = new QGroupBox(tr("Component"));
    auto* componentForm = new QFormLayout(_componentGroup);
    _componentLabel = heading();
    _componentIncluded = new QCheckBox(tr("Included in new simulations"));
    _componentIncluded->setObjectName("boardComponentIncluded");
    _componentIncluded->setToolTip(tr("New simulations model this resistor, inductor or capacitor between its pads."));
    componentForm->addRow(_componentLabel);
    componentForm->addRow(_componentIncluded);
    layout->addWidget(_componentGroup);

    // --- Picked net and its class
    _netGroup = new QGroupBox(tr("Net"));
    auto* netForm = new QFormLayout(_netGroup);
    _netLabel = heading();
    _netIncluded = new QCheckBox(tr("Included in new simulations"));
    _netIncluded->setObjectName("boardNetIncluded");
    _netIncluded->setToolTip(tr("New simulations start with this net's copper in their geometry."));
    _netTerminated = new QCheckBox(tr("Edge terminated"));
    _netTerminated->setObjectName("boardNetTerminated");
    _netTerminated->setToolTip(tr("New simulations terminate this net where the simulated area cuts it."));
    _netImpedance = new QuantityEdit(Kind::Resistance);
    _netImpedance->setObjectName("boardNetImpedance");
    _netImpedance->setAllowEmpty(true);
    _netImpedance->setMinimum(0);
    _netImpedance->setToolTip(tr("This net's own impedance. Empty uses its net class's, or the signal or power "
                                 "nets'."));
    netForm->addRow(_netLabel);
    netForm->addRow(_netIncluded);
    netForm->addRow(_netTerminated);
    netForm->addRow(tr("Impedance:"), _netImpedance);
    layout->addWidget(_netGroup);

    _netClassGroup = new QGroupBox(tr("Net class"));
    auto* netClassForm = new QFormLayout(_netClassGroup);
    _netClassLabel = heading();
    _netClassIncluded = new QCheckBox(tr("Included in new simulations"));
    _netClassIncluded->setObjectName("boardNetClassIncluded");
    _netClassIncluded->setToolTip(tr("New simulations start with every net of this class in their geometry."));
    _netClassImpedance = new QuantityEdit(Kind::Resistance);
    _netClassImpedance->setAllowEmpty(true);
    _netClassImpedance->setMinimum(0);
    _netClassImpedance->setToolTip(tr("The impedance of this class's nets. Empty uses the signal or power nets'."));
    netClassForm->addRow(_netClassLabel);
    netClassForm->addRow(_netClassIncluded);
    netClassForm->addRow(tr("Impedance:"), _netClassImpedance);
    layout->addWidget(_netClassGroup);
    layout->addWidget(note(tr("Click a pad or a trace on the board, with this tab open, to set up that pin, net or "
                              "component.")));

    // --- Board-wide edits
    connect(_ground, &QComboBox::activated, this, [this] { commitGround(); });
    connect(_ground->lineEdit(), &QLineEdit::editingFinished, this, [this] { commitGround(); });
    connect(_addEdgeNet, &QPushButton::clicked, this, [this] {
        const QString net = _edgeNetPicker->currentText().trimmed();
        if (net.isEmpty()) return;
        edit([&](kiems::BoardSetupConfig& setup) { setMember(setup.edgeTerminatedNets(), utf8(net), true); });
    });
    connect(_removeEdgeNet, &QPushButton::clicked, this, [this] {
        if (!_edgeNets->currentItem()) return;
        const std::string net = utf8(_edgeNets->currentItem()->text());
        edit([&](kiems::BoardSetupConfig& setup) { setMember(setup.edgeTerminatedNets(), net, false); });
    });
    connect(_signalImpedance, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (value) edit([&](kiems::BoardSetupConfig& setup) { setup.setSignalImpedance(*value); });
    });
    connect(_powerImpedance, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (value) edit([&](kiems::BoardSetupConfig& setup) { setup.setPowerImpedance(*value); });
    });
    connect(_bitRate, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        edit([&](kiems::BoardSetupConfig& setup) { setup.setBitRate(value.value_or(0.0)); });
    });

    // --- Pin edits
    connect(_pinImpedance, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        edit([&](kiems::BoardSetupConfig& setup) {
            setup.pinSetup(utf8(_pick.footprint), utf8(_pick.pad)).impedance = value;
        });
    });
    connect(_pinAbsorbing, &QCheckBox::toggled, this, [this](bool on) {
        if (_updating) return;
        edit([&](kiems::BoardSetupConfig& setup) {
            // Matching the default rule leaves the pin to it, so a later change of pin type still counts.
            setup.pinSetup(utf8(_pick.footprint), utf8(_pick.pad)).absorbing =
                on == pinAbsorbsByDefault(setup) ? std::nullopt : std::optional(on);
        });
    });
    connect(_noise, &QCheckBox::toggled, this, [this](bool on) {
        if (_updating) return;
        edit([&](kiems::BoardSetupConfig& setup) {
            kiems::BoardPinSetup& pin = setup.pinSetup(utf8(_pick.footprint), utf8(_pick.pad));
            if (on && !pin.noiseGenerating) pin.noiseFrequency = defaultNoiseFrequency();
            pin.noiseGenerating = on;
        });
    });
    connect(_noiseFrequency, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (!value) return;
        edit([&](kiems::BoardSetupConfig& setup) {
            setup.pinSetup(utf8(_pick.footprint), utf8(_pick.pad)).noiseFrequency = *value;
        });
    });
    connect(_noiseAmplitude, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (!value) return;
        edit([&](kiems::BoardSetupConfig& setup) {
            setup.pinSetup(utf8(_pick.footprint), utf8(_pick.pad)).noiseAmplitude = *value;
        });
    });

    // --- Component, net and net class edits
    connect(_componentIncluded, &QCheckBox::toggled, this, [this](bool on) {
        if (_updating) return;
        edit([&](kiems::BoardSetupConfig& setup) { setMember(setup.includedComponents(), utf8(_pick.footprint), on); });
    });
    connect(_netIncluded, &QCheckBox::toggled, this, [this](bool on) {
        if (_updating) return;
        edit([&](kiems::BoardSetupConfig& setup) {
            setMember(setup.includedNets(), utf8(_pick.net), on);
            // A net new simulations leave out has nothing to terminate.
            if (!on) setMember(setup.edgeTerminatedNets(), utf8(_pick.net), false);
        });
    });
    connect(_netTerminated, &QCheckBox::toggled, this, [this](bool on) {
        if (_updating) return;
        edit([&](kiems::BoardSetupConfig& setup) { setMember(setup.edgeTerminatedNets(), utf8(_pick.net), on); });
    });
    connect(_netImpedance, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        edit([&](kiems::BoardSetupConfig& setup) { setup.setNetImpedance(utf8(_pick.net), value); });
    });
    connect(_netClassIncluded, &QCheckBox::toggled, this, [this](bool on) {
        if (_updating) return;
        const QString netClass = netClassOf(_pick.net);
        if (netClass.isEmpty()) return;
        edit([&](kiems::BoardSetupConfig& setup) { setMember(setup.includedNetClasses(), utf8(netClass), on); });
    });
    connect(_netClassImpedance, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        const QString netClass = netClassOf(_pick.net);
        if (netClass.isEmpty()) return;
        edit([&](kiems::BoardSetupConfig& setup) { setup.setNetClassImpedance(utf8(netClass), value); });
    });

    connect(_document, &Document::changed, this, &BoardSetupPanel::refresh);
    refresh();
}

kiems::BoardSetupConfig BoardSetupPanel::setup() const {
    return SimulationEditor::boardSetup(*_document);
}

void BoardSetupPanel::edit(const std::function<void(kiems::BoardSetupConfig&)>& change) {
    if (_updating || !_document->isOpen()) return;
    kiems::BoardSetupConfig setup = this->setup();
    change(setup);
    SimulationEditor::setBoardSetup(*_document, setup);
}

void BoardSetupPanel::setBoard(std::shared_ptr<const BoardData> board) {
    _board = std::move(board);
    populateBoardLists();
    refresh();
}

void BoardSetupPanel::setPick(const BoardPick& pick) {
    _pick = pick;
    refresh();
}

QString BoardSetupPanel::netClassOf(const QString& net) const {
    if (!_board || net.isEmpty()) return {};
    const auto it = _board->netClasses.find(utf8(net));
    return it == _board->netClasses.end() ? QString() : qstr(it->second);
}

QStringList BoardSetupPanel::netClasses() const {
    std::set<QString> classes;
    if (_board) {
        for (const auto& [net, netClass] : _board->netClasses) classes.insert(qstr(netClass));
    }
    return QStringList(classes.begin(), classes.end());
}

bool BoardSetupPanel::pinPicked() const {
    return !_pick.footprint.isEmpty() && !SimulationEditor::isPassive(_pick.footprint);
}

bool BoardSetupPanel::componentPicked() const {
    return !_pick.footprint.isEmpty() && SimulationEditor::isPassive(_pick.footprint);
}

bool BoardSetupPanel::pinAbsorbsByDefault(const kiems::BoardSetupConfig& setup) const {
    // The resolver's rule (SimulationEditor::absorbsByDefault), against Board Setup's ground.
    if (!_board || _pick.net.isEmpty()) return false;
    const std::string net = utf8(_pick.net);
    std::string pinType;
    int padsOnNet = 0;
    for (const libkicad::FootprintInfo& info : _board->footprints) {
        for (const libkicad::FootprintPin& pin : info.pins) {
            if (pin.netName != net) continue;
            ++padsOnNet;
            if (qstr(info.reference) == _pick.footprint && qstr(pin.number) == _pick.pad) pinType = pin.pinType;
        }
    }
    bool ground = false;
    if (setup.groundName().empty()) {
        ground = _pick.net.section('/', -1).contains("GND", Qt::CaseInsensitive);
    } else if (setup.groundKind() == kiems::GroundSelectorKind::NetClass) {
        ground = utf8(netClassOf(_pick.net)) == setup.groundName();
    } else {
        ground = net == setup.groundName();
    }
    return padsOnNet > 1 && kiems::pinTypeAbsorbsByDefault(pinType, ground);
}

double BoardSetupPanel::defaultNoiseFrequency() const {
    // 100 MHz, kept inside the board's frequency range (the macOS app's rule).
    const nlohmann::json start = _document->documentValue("/frequency/start");
    const nlohmann::json stop = _document->documentValue("/frequency/stop");
    double frequency = 100e6;
    if (stop.is_number()) frequency = std::min(frequency, stop.get<double>());
    if (start.is_number()) frequency = std::max(frequency, start.get<double>());
    return frequency;
}

void BoardSetupPanel::populateBoardLists() {
    _updating = true;
    _ground->clear();
    _edgeNetPicker->clear();
    _ground->addItem(QString());
    if (_board) {
        for (const QString& net : _board->nets) {
            _ground->addItem(net);
            _ground->setItemData(_ground->count() - 1, "net", kKindRole);
            _ground->setItemData(_ground->count() - 1, net, kNameRole);
        }
        for (const QString& netClass : netClasses()) {
            _ground->addItem(tr("Class %1").arg(netClass));
            _ground->setItemData(_ground->count() - 1, "net_class", kKindRole);
            _ground->setItemData(_ground->count() - 1, netClass, kNameRole);
        }
        _edgeNetPicker->addItems(_board->nets);
    }

    // One impedance field per net class on the board.
    while (_classImpedanceForm->rowCount() > 0) _classImpedanceForm->removeRow(0);
    _classImpedanceFields.clear();
    for (const QString& netClass : netClasses()) {
        auto* field = new QuantityEdit(QuantityEdit::Kind::Resistance);
        field->setAllowEmpty(true);
        field->setMinimum(0);
        field->setToolTip(tr("The impedance of the %1 class's nets. Empty uses the signal or power nets'.").arg(netClass));
        connect(field, &QuantityEdit::valueEdited, this, [this, netClass](std::optional<double> value) {
            edit([&](kiems::BoardSetupConfig& setup) { setup.setNetClassImpedance(utf8(netClass), value); });
        });
        _classImpedanceForm->addRow(netClass + ":", field);
        _classImpedanceFields.emplace_back(netClass, field);
    }
    if (_classImpedanceFields.empty()) _classImpedanceForm->addRow(note(tr("The board has no net classes.")));
    _updating = false;
}

void BoardSetupPanel::commitGround() {
    if (_updating) return;
    const QString text = _ground->currentText().trimmed();
    if (text.isEmpty()) {
        edit([](kiems::BoardSetupConfig& setup) {
            setup.setGroundKind(kiems::GroundSelectorKind::Net);
            setup.setGroundName({});
        });
        return;
    }
    // Only a real net or net class; anything else is put back by refresh().
    const int index = _ground->findText(text, Qt::MatchFixedString);
    if (index <= 0) {
        refresh();
        return;
    }
    const bool netClass = _ground->itemData(index, kKindRole).toString() == "net_class";
    const std::string name = utf8(_ground->itemData(index, kNameRole).toString());
    edit([&](kiems::BoardSetupConfig& setup) {
        setup.setGroundKind(netClass ? kiems::GroundSelectorKind::NetClass : kiems::GroundSelectorKind::Net);
        setup.setGroundName(name);
    });
}

void BoardSetupPanel::refresh() {
    _updating = true;
    const bool open = _document->isOpen();
    const kiems::BoardSetupConfig setup = open ? this->setup() : kiems::BoardSetupConfig();
    for (QWidget* group : std::initializer_list<QWidget*>{_boardGroup, _pinGroup, _componentGroup, _netGroup,
                                                          _netClassGroup}) {
        group->setEnabled(open);
    }

    // --- Board-wide
    QString groundText;
    if (!setup.groundName().empty()) {
        groundText = setup.groundKind() == kiems::GroundSelectorKind::NetClass
                         ? tr("Class %1").arg(qstr(setup.groundName()))
                         : qstr(setup.groundName());
    }
    if (!_ground->hasFocus()) _ground->setCurrentText(groundText);
    _edgeNets->clear();
    for (const std::string& net : setup.edgeTerminatedNets()) {
        if (!net.empty()) _edgeNets->addItem(qstr(net));
    }
    _signalImpedance->refreshValue(open ? std::optional(setup.signalImpedance()) : std::nullopt);
    _powerImpedance->refreshValue(open ? std::optional(setup.powerImpedance()) : std::nullopt);
    for (const auto& [netClass, field] : _classImpedanceFields) {
        field->refreshValue(setup.netClassImpedance(utf8(netClass)));
        field->setPlaceholderText(formatOhms(setup.signalImpedance()));
    }
    _bitRate->refreshValue(setup.bitRate() > 0 ? std::optional(setup.bitRate()) : std::nullopt);

    // --- Picked pin
    _pinGroup->setVisible(pinPicked());
    const std::string net = utf8(_pick.net);
    const QString netClass = netClassOf(_pick.net);
    const bool power = SimulationEditor::isPowerNet(_pick.net);
    if (pinPicked()) {
        QString text = tr("%1 pin %2").arg(_pick.footprint, _pick.pad);
        if (_board) {
            if (const libkicad::PadPosition* pad = _board->pad(_pick.footprint, _pick.pad); pad && !pad->pinType.empty()) {
                text += " (" + qstr(pad->pinType).replace('_', ' ') + ")";
            }
        }
        _pinLabel->setText(text);
        const kiems::BoardPinSetup* pin = setup.pin(utf8(_pick.footprint), utf8(_pick.pad));
        _pinImpedance->refreshValue(pin ? pin->impedance : std::nullopt);
        _pinImpedance->setPlaceholderText(
            _pick.net.isEmpty() ? QString()
                                : formatOhms(setup.effectiveNetImpedance(net, utf8(netClass), power)) + tr(" (net)"));
        _pinAbsorbing->setEnabled(open && !_pick.net.isEmpty());
        _pinAbsorbing->setChecked(pin && pin->absorbing ? *pin->absorbing : pinAbsorbsByDefault(setup));
        const bool noise = pin && pin->noiseGenerating;
        _noise->setChecked(noise);
        _noiseControls->setVisible(noise);
        _noiseFrequency->refreshValue(pin ? pin->noiseFrequency : defaultNoiseFrequency());
        _noiseAmplitude->refreshValue(pin ? pin->noiseAmplitude : 1.0);
    }

    // --- Picked passive part
    _componentGroup->setVisible(componentPicked());
    if (componentPicked()) {
        QString text = _pick.footprint;
        if (_board) {
            for (const libkicad::FootprintInfo& info : _board->footprints) {
                if (qstr(info.reference) == _pick.footprint && !info.value.empty()) text += " (" + qstr(info.value) + ")";
            }
        }
        _componentLabel->setText(text);
        _componentIncluded->setChecked(contains(setup.includedComponents(), utf8(_pick.footprint)));
    }

    // --- Picked net and its class
    _netGroup->setVisible(!_pick.net.isEmpty());
    if (!_pick.net.isEmpty()) {
        _netLabel->setText(_pick.net);
        const bool included = contains(setup.includedNets(), net);
        _netIncluded->setChecked(included);
        _netTerminated->setChecked(contains(setup.edgeTerminatedNets(), net));
        _netTerminated->setEnabled(open && included);
        _netImpedance->refreshValue(setup.netImpedance(net));
        const double inherited = setup.netClassImpedance(utf8(netClass))
                                     .value_or(power ? setup.powerImpedance() : setup.signalImpedance());
        _netImpedance->setPlaceholderText(formatOhms(inherited));
    }
    _netClassGroup->setVisible(!netClass.isEmpty());
    if (!netClass.isEmpty()) {
        _netClassLabel->setText(netClass);
        _netClassIncluded->setChecked(contains(setup.includedNetClasses(), utf8(netClass)));
        _netClassImpedance->refreshValue(setup.netClassImpedance(utf8(netClass)));
        _netClassImpedance->setPlaceholderText(formatOhms(setup.signalImpedance()));
    }
    _updating = false;
}

} // namespace kiems_gui
