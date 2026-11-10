#include "Inspector.hpp"

#include "QuantityEdit.hpp"
#include "SelectionPanel.hpp"
#include "SettingsPanel.hpp"
#include "SimulationEditor.hpp"
#include "TestsPanel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace kiems_gui {

namespace {

constexpr int kFootprintRole = Qt::UserRole;
constexpr int kPinRole = Qt::UserRole + 1;
constexpr int kNetRole = Qt::UserRole;
constexpr int kEntryIndexRole = Qt::UserRole + 1;

/// Stops a scroll over an unfocused drop-down from silently changing a simulation setting: the
/// wheel then scrolls the panel instead, as on macOS.
class IgnoreUnfocusedWheel : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Wheel) {
            auto* widget = static_cast<QWidget*>(watched);
            if (!widget->hasFocus()) {
                event->ignore();
                return true;
            }
        }
        return false;
    }
};

/// `page` in a vertically scrolling area, for a tab that can outgrow the window.
QScrollArea* scrolling(QWidget* page) {
    auto* scroll = new QScrollArea;
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return scroll;
}

/// `widget` with its own margins, for a tab page.
QWidget* page(QWidget* widget) {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(widget);
    layout->addStretch();
    return page;
}

QPushButton* smallButton(const QString& text, const QString& toolTip) {
    auto* button = new QPushButton(text);
    button->setToolTip(toolTip);
    button->setFixedWidth(28);
    return button;
}

QString minus() {
    return QString::fromUtf8("\xE2\x88\x92");
}

} // namespace

Inspector::Inspector(Document* document, QWidget* parent) : QWidget(parent), _document(document) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    _tabs = new QTabWidget;
    _tabs->setDocumentMode(true);
    outer->addWidget(_tabs);

    // --- Simulation tab
    _simulationGroup = new QGroupBox(tr("Simulation"));
    auto* form = new QFormLayout(_simulationGroup);
    _name = new QLineEdit;
    _differentialPair = new QCheckBox(tr("Differential pair"));
    _differentialPair->setToolTip(tr("Simulate the involved nets as a differential pair: each pin choice is mirrored "
                                     "onto its partner, the partner driven in antiphase, and results reported in "
                                     "differential and common mode."));
    _groundNet = new QComboBox;
    _groundNet->setEditable(true);
    _groundNet->setInsertPolicy(QComboBox::NoInsert);
    _plane = new QComboBox;
    _plane->setToolTip(tr("The copper layer ports measure against: the ground plane under the excited pad."));
    _excitations = new QListWidget;
    _excitations->setMaximumHeight(90);
    _excitations->setToolTip(tr("The pins driven in this simulation. Pick a pin on the board to change its "
                                "excitation."));
    _removeExcitation = smallButton(minus(), tr("Stop exciting the selected pin."));
    _nets = new QListWidget;
    _nets->setMaximumHeight(120);
    _nets->setToolTip(tr("Nets in this simulation. Geometry-only nets add their copper without growing the "
                         "simulated area. Pick a trace on the board to change how a net is included."));
    _removeNet = smallButton(minus(), tr("Remove the selected net from the simulation."));
    _edgeNets = new QListWidget;
    _edgeNets->setMaximumHeight(90);
    _edgeNets->setToolTip(tr("Nets that run off the simulated area are terminated in their impedance at its edge "
                             "rather than left open."));
    _edgeNetPicker = new QComboBox;
    _edgeNetPicker->setEditable(true);
    _edgeNetPicker->setInsertPolicy(QComboBox::NoInsert);
    _addEdgeNet = smallButton("+", tr("Add the net chosen here."));
    _removeEdgeNet = smallButton(minus(), tr("Remove the selected net."));
    auto listWithButtons = [](QListWidget* list, std::initializer_list<QWidget*> buttons, QWidget* before = nullptr) {
        auto* column = new QVBoxLayout;
        column->setContentsMargins(0, 0, 0, 0);
        column->addWidget(list);
        auto* row = new QHBoxLayout;
        if (before) row->addWidget(before, 1);
        else row->addStretch();
        for (QWidget* button : buttons) row->addWidget(button);
        column->addLayout(row);
        return column;
    };
    form->addRow(tr("Name:"), _name);
    form->addRow(QString(), _differentialPair);
    form->addRow(tr("Ground net:"), _groundNet);
    form->addRow(tr("Reference plane:"), _plane);
    form->addRow(tr("Excitations:"), listWithButtons(_excitations, {_removeExcitation}));
    form->addRow(tr("Involved nets:"), listWithButtons(_nets, {_removeNet}));
    form->addRow(tr("Edge-terminated nets:"), listWithButtons(_edgeNets, {_addEdgeNet, _removeEdgeNet}, _edgeNetPicker));

    // --- The other tabs
    _selection = new SelectionPanel(document);
    _settings = new SettingsPanel(document);
    _tests = new TestsPanel(document);
    _selectionPage = scrolling(page(_selection));
    _tabs->addTab(scrolling(page(_simulationGroup)), tr("Simulation"));
    _tabs->addTab(_selectionPage, tr("Selection"));
    _tabs->addTab(scrolling(page(_settings)), tr("Settings"));
    _tabs->addTab(scrolling(page(_tests)), tr("Tests"));

    auto* wheelGuard = new IgnoreUnfocusedWheel(this);
    for (QComboBox* combo : {_groundNet, _plane, _edgeNetPicker}) {
        // Long net names mustn't widen the panel past its column.
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(8);
        combo->setFocusPolicy(Qt::StrongFocus);
        combo->installEventFilter(wheelGuard);
    }

    connect(_name, &QLineEdit::editingFinished, this, [this] {
        const QString name = _name->text().trimmed();
        if (_updating || _simulation < 0 || name.isEmpty() || name == _document->simulationName(_simulation)) return;
        _document->renameSimulation(_simulation, _document->uniqueSimulationName(name));
    });
    connect(_differentialPair, &QCheckBox::toggled, this, [this](bool on) {
        if (_updating || _simulation < 0) return;
        _document->setSimulationValue(_simulation, "/is_differential_pair", on);
        // What was set up before the box was ticked gets its partner halves, as if it had been on.
        if (on) SimulationEditor(*_document, _board.get(), _simulation).reconcileDifferentialPairs();
    });
    connect(_groundNet, &QComboBox::currentTextChanged, this, [this](const QString& net) {
        if (!_updating && _simulation >= 0 && !net.isEmpty()) _document->setGroundNet(_simulation, net);
    });
    connect(_plane, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!_updating && _simulation >= 0 && index >= 0) _document->setReferencePlane(_simulation, index);
    });
    connect(_removeExcitation, &QPushButton::clicked, this, [this] {
        const QListWidgetItem* item = _excitations->currentItem();
        if (_simulation < 0 || !item) return;
        SimulationEditor(*_document, _board.get(), _simulation)
            .setPinExcited(item->data(kFootprintRole).toString(), item->data(kPinRole).toString(), false);
    });
    connect(_removeNet, &QPushButton::clicked, this, [this] {
        const QListWidgetItem* item = _nets->currentItem();
        if (_simulation < 0 || !item) return;
        const QString net = item->data(kNetRole).toString();
        if (!net.isEmpty()) {
            SimulationEditor(*_document, _board.get(), _simulation).setNetIncluded(net, false);
            return;
        }
        // A net-class or pin entry, which only the file or the macOS app makes: removed as it is.
        const int index = item->data(kEntryIndexRole).toInt();
        _document->editSimulation(_simulation, [index](nlohmann::json& sim) {
            if (sim.contains("involved_nets") && index < static_cast<int>(sim["involved_nets"].size())) {
                sim["involved_nets"].erase(static_cast<std::size_t>(index));
            }
        });
    });
    connect(_addEdgeNet, &QPushButton::clicked, this, [this] {
        const QString net = _edgeNetPicker->currentText().trimmed();
        if (_simulation < 0 || net.isEmpty()) return;
        nlohmann::json nets = _document->simulationValue(_simulation, "/edge_terminated_nets");
        if (!nets.is_array()) nets = nlohmann::json::array();
        if (std::find(nets.begin(), nets.end(), net.toStdString()) != nets.end()) return;
        nets.push_back(net.toStdString());
        _document->setSimulationValue(_simulation, "/edge_terminated_nets", nets);
    });
    connect(_removeEdgeNet, &QPushButton::clicked, this, [this] {
        if (_simulation < 0 || !_edgeNets->currentItem()) return;
        nlohmann::json nets = _document->simulationValue(_simulation, "/edge_terminated_nets");
        const std::string net = _edgeNets->currentItem()->text().toStdString();
        nlohmann::json remaining = nlohmann::json::array();
        for (const auto& entry : nets) {
            if (entry != net) remaining.push_back(entry);
        }
        _document->setSimulationValue(_simulation, "/edge_terminated_nets", remaining);
    });
    connect(_document, &Document::changed, this, &Inspector::refresh);
    refresh();
}

void Inspector::setBoard(std::shared_ptr<const BoardData> board) {
    _board = std::move(board);
    _selection->setBoard(_board);
    populateBoardLists();
    refresh();
}

void Inspector::setSimulation(int simulation) {
    _simulation = simulation;
    _selection->setSimulation(simulation);
    _settings->setSimulation(simulation);
    _tests->setSimulation(simulation);
    refresh();
}

void Inspector::setPick(const BoardPick& pick) {
    _selection->setPick(pick);
    if (!pick.net.isEmpty() || !pick.footprint.isEmpty()) _tabs->setCurrentWidget(_selectionPage);
}

void Inspector::populateBoardLists() {
    _updating = true;
    _groundNet->clear();
    _plane->clear();
    _edgeNetPicker->clear();
    if (_board) {
        _groundNet->addItems(_board->nets);
        _edgeNetPicker->addItems(_board->nets);
        for (const QString& layer : _board->copperLayers) _plane->addItem(layer);
    }
    _updating = false;
}

void Inspector::refresh() {
    _updating = true;
    const bool haveSimulation = _document->isOpen() && _simulation >= 0 && _simulation < _document->simulationCount();
    _simulationGroup->setEnabled(haveSimulation);
    _excitations->clear();
    _nets->clear();
    _edgeNets->clear();
    if (haveSimulation) {
        _name->setText(_document->simulationName(_simulation));
        _differentialPair->setChecked(_document->simulationValue(_simulation, "/is_differential_pair").get<bool>());
        _groundNet->setCurrentText(_document->groundNet(_simulation));
        const auto plane = _document->referencePlane(_simulation);
        _plane->setCurrentIndex(plane && *plane < _plane->count() ? *plane : (_plane->count() > 2 ? 1 : -1));

        const nlohmann::json* sim = _document->simulationJson(_simulation);
        for (const auto& entry : sim->value("excitations", nlohmann::json::array())) {
            if (!entry.contains("footprint") || !entry.contains("pin")) continue;
            const QString footprint = QString::fromStdString(entry["footprint"].get<std::string>());
            const QString pin = entry["pin"].is_string() ? QString::fromStdString(entry["pin"].get<std::string>())
                                                         : QString::number(entry["pin"].get<long long>());
            QString text = tr("%1 pin %2").arg(footprint, pin);
            if (entry.value("main", false)) {
                text += tr(" - main");
            } else {
                text += tr(" - adversarial");
                if (entry.contains("frequency")) {
                    text += ", " + QuantityEdit::format(QuantityEdit::Kind::Frequency, entry["frequency"].get<double>());
                }
            }
            auto* item = new QListWidgetItem(text, _excitations);
            item->setData(kFootprintRole, footprint);
            item->setData(kPinRole, pin);
        }

        const auto& entries = sim->value("involved_nets", nlohmann::json::array());
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            QString text;
            QString net;
            if (entry.contains("net")) {
                net = QString::fromStdString(entry["net"].get<std::string>());
                text = net;
            } else if (entry.contains("net_class")) {
                text = tr("Class %1").arg(QString::fromStdString(entry["net_class"].get<std::string>()));
            } else if (entry.contains("footprint")) {
                text = tr("Pins of %1").arg(QString::fromStdString(entry["footprint"].get<std::string>()));
            }
            if (entry.value("geometry_only", false)) text += tr(" (geometry only)");
            auto* item = new QListWidgetItem(text, _nets);
            item->setData(kNetRole, net);
            item->setData(kEntryIndexRole, static_cast<int>(i));
        }
        for (const auto& net : _document->simulationValue(_simulation, "/edge_terminated_nets")) {
            if (net.is_string()) _edgeNets->addItem(QString::fromStdString(net.get<std::string>()));
        }
    } else {
        _name->clear();
        _differentialPair->setChecked(false);
    }
    _updating = false;
}

} // namespace kiems_gui
