#include "SimulationSetPanel.hpp"

#include "Document.hpp"
#include "SimulationSet.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCollator>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

namespace kiems_gui {

namespace {

QString qstr(const std::string& text) {
    return QString::fromStdString(text);
}

QString minus() {
    return QString::fromUtf8("\xE2\x88\x92");
}

QPushButton* smallButton(const QString& text, const QString& toolTip) {
    auto* button = new QPushButton(text);
    button->setToolTip(toolTip);
    button->setFixedWidth(28);
    return button;
}

/// A scroll over an unfocused drop-down scrolls the panel rather than changing the setting.
class IgnoreUnfocusedWheel : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Wheel && !static_cast<QWidget*>(watched)->hasFocus()) {
            event->ignore();
            return true;
        }
        return false;
    }
};

/// A list with its add/remove buttons under it, optionally after a field that says what to add.
QWidget* listWithButtons(QListWidget* list, QWidget* before, std::initializer_list<QPushButton*> buttons) {
    auto* box = new QWidget;
    auto* column = new QVBoxLayout(box);
    column->setContentsMargins(0, 0, 0, 0);
    column->addWidget(list);
    auto* row = new QHBoxLayout;
    if (before) row->addWidget(before, 1);
    else row->addStretch();
    for (QPushButton* button : buttons) row->addWidget(button);
    column->addLayout(row);
    return box;
}

void setPatternColor(QLineEdit* field, bool valid) {
    QPalette palette = field->palette();
    palette.setColor(QPalette::Text, valid ? QApplication::palette(field).color(QPalette::Text) : QColor(0xd0, 0x3a, 0x2f));
    field->setPalette(palette);
}

} // namespace

SimulationSetPanel::SimulationSetPanel(Document* document, QWidget* parent) : QWidget(parent), _document(document) {
    using Kind = QuantityEdit::Kind;
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* group = new QGroupBox(tr("Simulation Set"));
    outer->addWidget(group);
    _form = new QFormLayout(group);

    _excitationIC = footprintCombo();
    _excitationIC->setObjectName("excitationIC");
    _excitationIC->setToolTip(tr("The IC whose pin on each selected net is driven, one simulation per net."));
    _selectionMode = new QComboBox;
    _selectionMode->setObjectName("selectionMode");
    _selectionMode->addItems({tr("Regular Expression"), tr("List")});
    _selectionMode->setToolTip(tr("Select the nets to simulate by a pattern their whole name matches, or name them."));
    _pattern = new QLineEdit;
    _pattern->setObjectName("pattern");
    _pattern->setPlaceholderText(tr("e.g. DDR_ADDR(\\d+)"));
    _pattern->setToolTip(tr("A regular expression the whole net name must match. Its groups can be used in the "
                            "additional net patterns."));
    _netNames = new QListWidget;
    _netNames->setMaximumHeight(110);
    _netPicker = new QComboBox;
    _netPicker->setEditable(true);
    _netPicker->setInsertPolicy(QComboBox::NoInsert);
    _addNet = smallButton("+", tr("Add the net chosen here."));
    _removeNet = smallButton(minus(), tr("Remove the selected net."));
    _netNamesBox = listWithButtons(_netNames, _netPicker, {_addNet, _removeNet});
    _additionalPatterns = new QListWidget;
    _additionalPatterns->setMaximumHeight(90);
    _additionalPatterns->setToolTip(tr("Further nets each simulation includes, as patterns where $1, $2... stand for the "
                                       "net name pattern's groups: for DDR_DQ(\\d+), DDR_DQS$1 adds the matching "
                                       "strobe. Double-click a pattern to edit it."));
    _addPattern = smallButton("+", tr("Add a pattern."));
    _removePattern = smallButton(minus(), tr("Remove the selected pattern."));
    _additionalBox = listWithButtons(_additionalPatterns, nullptr, {_addPattern, _removePattern});

    _phase = new QuantityEdit(Kind::Angle);
    _amplitude = new QuantityEdit(Kind::Voltage);
    _driveImpedance = new QuantityEdit(Kind::Resistance);
    _driveImpedance->setMinimum(0);
    _probed = new QCheckBox(tr("Probed"));
    _probed->setToolTip(tr("Report the driven pin's own port in the results."));
    _probeNetImpedance = new QCheckBox(tr("Probe net impedance"));
    _absorbing = new QCheckBox(tr("Absorbing"));
    _loadImpedance = new QuantityEdit(Kind::Resistance);
    _loadImpedance->setMinimum(0);
    _loadCapacitance = new QuantityEdit(Kind::Capacitance);
    _loadCapacitance->setMinimum(0);
    _loadCapacitance->setAllowEmpty(true, tr("None"));

    _probesLayout = new QVBoxLayout;
    _probesLayout->setContentsMargins(0, 0, 0, 0);
    _addProbe = new QPushButton(tr("Add Probe"));
    _addProbe->setObjectName("addProbe");
    _addProbe->setToolTip(tr("Probe another IC's pins on each simulation's nets."));
    _summary = new QLabel;
    _summary->setObjectName("summary");
    _summary->setWordWrap(true);
    _summary->setEnabled(false); // the palette's muted text

    _form->addRow(tr("Excitation IC:"), _excitationIC);
    _form->addRow(tr("Net selection:"), _selectionMode);
    _form->addRow(tr("Net name pattern:"), _pattern);
    _form->addRow(tr("Nets to simulate:"), _netNamesBox);
    _form->addRow(tr("Additional net patterns:"), _additionalBox);
    _form->addRow(tr("Phase:"), _phase);
    _form->addRow(tr("Amplitude:"), _amplitude);
    _form->addRow(tr("Drive impedance:"), _driveImpedance);
    _form->addRow(QString(), _probed);
    _form->addRow(QString(), _probeNetImpedance);
    _form->addRow(QString(), _absorbing);
    _form->addRow(tr("Load impedance:"), _loadImpedance);
    _form->addRow(tr("Load capacitance:"), _loadCapacitance);
    _form->addRow(_probesLayout);
    auto* addRow = new QHBoxLayout;
    addRow->addWidget(_addProbe);
    addRow->addStretch();
    _form->addRow(addRow);
    _form->addRow(_summary);

    auto* wheelGuard = new IgnoreUnfocusedWheel(this);
    for (QComboBox* combo : {_selectionMode, _netPicker}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(8);
        combo->setFocusPolicy(Qt::StrongFocus);
        combo->installEventFilter(wheelGuard);
    }

    // Chosen from the list, or typed and confirmed.
    const auto commitExcitationIC = [this] {
        if (_updating) return;
        const std::string value = _excitationIC->currentText().trimmed().toStdString();
        edit([&](kiems::SimulationSetConfig& set) { set.excitationFootprint = value; });
    };
    connect(_excitationIC, &QComboBox::activated, this, commitExcitationIC);
    connect(_excitationIC->lineEdit(), &QLineEdit::editingFinished, this, commitExcitationIC);
    connect(_selectionMode, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (_updating) return;
        edit([&](kiems::SimulationSetConfig& set) {
            set.netSelectionMode =
                index == 1 ? kiems::SimulationSetNetSelectionMode::List : kiems::SimulationSetNetSelectionMode::RegularExpression;
        });
    });
    connect(_pattern, &QLineEdit::textChanged, this, [this](const QString& text) {
        setPatternColor(_pattern, text.isEmpty() || setNetPattern(text).isValid());
    });
    connect(_pattern, &QLineEdit::editingFinished, this, [this] {
        if (_updating) return;
        const std::string value = _pattern->text().toStdString();
        edit([&](kiems::SimulationSetConfig& set) { set.netNamePattern = value; });
    });
    connect(_addNet, &QPushButton::clicked, this, [this] {
        const std::string net = _netPicker->currentText().trimmed().toStdString();
        if (net.empty()) return;
        edit([&](kiems::SimulationSetConfig& set) {
            if (std::find(set.netNames.begin(), set.netNames.end(), net) == set.netNames.end()) set.netNames.push_back(net);
        });
    });
    connect(_removeNet, &QPushButton::clicked, this, [this] {
        const int row = _netNames->currentRow();
        if (row < 0) return;
        edit([&](kiems::SimulationSetConfig& set) {
            if (row < static_cast<int>(set.netNames.size())) set.netNames.erase(set.netNames.begin() + row);
        });
    });
    connect(_additionalPatterns, &QListWidget::itemChanged, this, [this] {
        if (!_updating) commitAdditionalPatterns();
    });
    connect(_addPattern, &QPushButton::clicked, this, [this] {
        // A new, empty row to type into; it's stored once it has text.
        auto* item = new QListWidgetItem(QString(), _additionalPatterns);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        _additionalPatterns->setCurrentItem(item);
        _additionalPatterns->editItem(item);
    });
    connect(_removePattern, &QPushButton::clicked, this, [this] {
        delete _additionalPatterns->currentItem();
        commitAdditionalPatterns();
    });
    connect(_phase, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (value) edit([&](kiems::SimulationSetConfig& set) { set.phaseDegrees = *value; });
    });
    connect(_amplitude, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (value) edit([&](kiems::SimulationSetConfig& set) { set.amplitude = *value; });
    });
    connect(_driveImpedance, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (value) edit([&](kiems::SimulationSetConfig& set) { set.driveImpedance = std::max(0.0, *value); });
    });
    connect(_probed, &QCheckBox::toggled, this, [this](bool on) {
        if (!_updating) edit([&](kiems::SimulationSetConfig& set) { set.probed = on; });
    });
    connect(_probeNetImpedance, &QCheckBox::toggled, this, [this](bool on) {
        if (!_updating) edit([&](kiems::SimulationSetConfig& set) { set.probeNetImpedance = on; });
    });
    connect(_absorbing, &QCheckBox::toggled, this, [this](bool on) {
        if (!_updating) edit([&](kiems::SimulationSetConfig& set) { set.absorbing = on; });
    });
    connect(_loadImpedance, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (value) edit([&](kiems::SimulationSetConfig& set) { set.loadImpedance = std::max(0.0, *value); });
    });
    connect(_loadCapacitance, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        edit([&](kiems::SimulationSetConfig& set) {
            set.loadCapacitance = value && *value > 0 ? value : std::nullopt;
        });
    });
    connect(_addProbe, &QPushButton::clicked, this, [this] {
        edit([](kiems::SimulationSetConfig& set) { set.probes.emplace_back(); });
    });
    connect(_document, &Document::changed, this, &SimulationSetPanel::refresh);

    refresh();
}

QComboBox* SimulationSetPanel::footprintCombo() {
    auto* combo = new QComboBox;
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(8);
    combo->setFocusPolicy(Qt::StrongFocus);
    combo->installEventFilter(new IgnoreUnfocusedWheel(combo));
    combo->addItems(_references);
    return combo;
}

void SimulationSetPanel::setBoard(std::shared_ptr<const BoardData> board) {
    _board = std::move(board);
    _references.clear();
    if (_board) {
        for (const libkicad::FootprintInfo& info : _board->footprints) _references << qstr(info.reference);
        QCollator collator;
        collator.setNumericMode(true);
        std::sort(_references.begin(), _references.end(),
                  [&collator](const QString& a, const QString& b) { return collator.compare(a, b) < 0; });
    }
    populateFootprints();
    refresh();
}

void SimulationSetPanel::populateFootprints() {
    _updating = true;
    for (QComboBox* combo : {_excitationIC}) {
        const QString text = combo->currentText();
        combo->clear();
        combo->addItems(_references);
        combo->setCurrentText(text);
    }
    for (const ProbeControls& probe : _probes) {
        const QString text = probe.footprint->currentText();
        probe.footprint->clear();
        probe.footprint->addItems(_references);
        probe.footprint->setCurrentText(text);
    }
    _netPicker->clear();
    if (_board) _netPicker->addItems(_board->nets);
    _updating = false;
}

void SimulationSetPanel::setSimulation(int simulation) {
    _simulation = simulation;
    refresh();
}

void SimulationSetPanel::edit(const std::function<void(kiems::SimulationSetConfig&)>& change) {
    auto set = simulationSet(*_document, _simulation);
    if (!set) return;
    change(*set);
    setSimulationSet(*_document, _simulation, *set);
}

void SimulationSetPanel::editProbe(int index, const std::function<void(kiems::SimulationSetProbe&)>& change) {
    edit([&](kiems::SimulationSetConfig& set) {
        if (index >= 0 && index < static_cast<int>(set.probes.size())) change(set.probes[static_cast<std::size_t>(index)]);
    });
}

void SimulationSetPanel::commitAdditionalPatterns() {
    std::vector<std::string> patterns;
    for (int i = 0; i < _additionalPatterns->count(); ++i) {
        const QString text = _additionalPatterns->item(i)->text().trimmed();
        if (!text.isEmpty()) patterns.push_back(text.toStdString());
    }
    edit([&](kiems::SimulationSetConfig& set) { set.additionalNetPatterns = patterns; });
}

void SimulationSetPanel::rebuildProbes(int count) {
    using Kind = QuantityEdit::Kind;
    while (static_cast<int>(_probes.size()) > count) {
        delete _probes.back().group;
        _probes.pop_back();
    }
    while (static_cast<int>(_probes.size()) < count) {
        const int index = static_cast<int>(_probes.size());
        ProbeControls probe;
        probe.group = new QGroupBox(tr("Probe %1").arg(index + 1));
        probe.form = new QFormLayout(probe.group);
        probe.footprint = footprintCombo();
        probe.footprint->setToolTip(tr("Every pin of this IC on a simulation's nets is probed."));
        probe.absorbing = new QCheckBox(tr("Absorbing"));
        probe.absorbing->setToolTip(tr("Terminate the probed pins in a load rather than leaving them open."));
        probe.loadImpedance = new QuantityEdit(Kind::Resistance);
        probe.loadImpedance->setMinimum(0);
        probe.loadCapacitance = new QuantityEdit(Kind::Capacitance);
        probe.loadCapacitance->setMinimum(0);
        probe.loadCapacitance->setAllowEmpty(true, tr("None"));
        auto* remove = smallButton(minus(), tr("Remove this probe."));
        auto* footprintRow = new QHBoxLayout;
        footprintRow->addWidget(probe.footprint, 1);
        footprintRow->addWidget(remove);
        probe.form->addRow(tr("IC:"), footprintRow);
        probe.form->addRow(QString(), probe.absorbing);
        probe.form->addRow(tr("Load impedance:"), probe.loadImpedance);
        probe.form->addRow(tr("Load capacitance:"), probe.loadCapacitance);
        _probesLayout->addWidget(probe.group);

        const auto commitFootprint = [this, index, combo = probe.footprint] {
            if (_updating) return;
            const std::string value = combo->currentText().trimmed().toStdString();
            editProbe(index, [&](kiems::SimulationSetProbe& p) { p.footprint = value; });
        };
        connect(probe.footprint, &QComboBox::activated, this, commitFootprint);
        connect(probe.footprint->lineEdit(), &QLineEdit::editingFinished, this, commitFootprint);
        connect(probe.absorbing, &QCheckBox::toggled, this, [this, index](bool on) {
            if (!_updating) editProbe(index, [on](kiems::SimulationSetProbe& p) { p.absorbing = on; });
        });
        connect(probe.loadImpedance, &QuantityEdit::valueEdited, this, [this, index](std::optional<double> value) {
            if (value) editProbe(index, [&](kiems::SimulationSetProbe& p) { p.loadImpedance = std::max(0.0, *value); });
        });
        connect(probe.loadCapacitance, &QuantityEdit::valueEdited, this, [this, index](std::optional<double> value) {
            editProbe(index, [&](kiems::SimulationSetProbe& p) { p.loadCapacitance = value && *value > 0 ? value : std::nullopt; });
        });
        connect(remove, &QPushButton::clicked, this, [this, index] {
            edit([index](kiems::SimulationSetConfig& set) {
                if (index < static_cast<int>(set.probes.size())) set.probes.erase(set.probes.begin() + index);
            });
        });
        _probes.push_back(probe);
    }
}

void SimulationSetPanel::refresh() {
    const auto set = simulationSet(*_document, _simulation);
    setVisible(set.has_value());
    if (!set) {
        rebuildProbes(0);
        return;
    }
    _updating = true;
    if (!_excitationIC->lineEdit()->hasFocus()) _excitationIC->setCurrentText(qstr(set->excitationFootprint));
    const bool byList = set->netSelectionMode == kiems::SimulationSetNetSelectionMode::List;
    _selectionMode->setCurrentIndex(byList ? 1 : 0);
    if (!_pattern->hasFocus()) _pattern->setText(qstr(set->netNamePattern));
    _form->setRowVisible(_pattern, !byList);
    _form->setRowVisible(_netNamesBox, byList);
    _form->setRowVisible(_additionalBox, !byList);

    const int netRow = _netNames->currentRow();
    _netNames->clear();
    for (const std::string& net : set->netNames) _netNames->addItem(qstr(net));
    _netNames->setCurrentRow(std::min(netRow, _netNames->count() - 1));
    // Not while a pattern is being typed into it.
    const QWidget* focus = QApplication::focusWidget();
    if (!focus || (focus != _additionalPatterns && !_additionalPatterns->isAncestorOf(focus))) {
        const int patternRow = _additionalPatterns->currentRow();
        _additionalPatterns->clear();
        for (const std::string& pattern : set->additionalNetPatterns) {
            auto* item = new QListWidgetItem(qstr(pattern), _additionalPatterns);
            item->setFlags(item->flags() | Qt::ItemIsEditable);
        }
        _additionalPatterns->setCurrentRow(std::min(patternRow, _additionalPatterns->count() - 1));
    }

    _phase->refreshValue(set->phaseDegrees);
    _amplitude->refreshValue(set->amplitude);
    _driveImpedance->refreshValue(set->driveImpedance);
    _probed->setChecked(set->probed);
    _probeNetImpedance->setChecked(set->probeNetImpedance);
    _absorbing->setChecked(set->absorbing);
    _loadImpedance->refreshValue(set->loadImpedance);
    _loadCapacitance->refreshValue(set->loadCapacitance);
    _form->setRowVisible(_loadImpedance, set->absorbing);
    _form->setRowVisible(_loadCapacitance, set->absorbing);

    rebuildProbes(static_cast<int>(set->probes.size()));
    for (std::size_t i = 0; i < set->probes.size(); ++i) {
        const kiems::SimulationSetProbe& probe = set->probes[i];
        const ProbeControls& controls = _probes[i];
        if (!controls.footprint->lineEdit()->hasFocus()) controls.footprint->setCurrentText(qstr(probe.footprint));
        controls.absorbing->setChecked(probe.absorbing);
        controls.loadImpedance->refreshValue(probe.loadImpedance);
        controls.loadCapacitance->refreshValue(probe.loadCapacitance);
        controls.form->setRowVisible(controls.loadImpedance, probe.absorbing);
        controls.form->setRowVisible(controls.loadCapacitance, probe.absorbing);
    }
    _summary->setText(describeSimulationSet(*set, _board.get()).join('\n'));
    _updating = false;
}

} // namespace kiems_gui
