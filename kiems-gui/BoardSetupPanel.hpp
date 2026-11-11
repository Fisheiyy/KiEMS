// The inspector's Board Setup tab: the document's board-wide defaults (kiems::BoardSetupConfig),
// which new simulations start from and nets take as they're included -- the macOS app's Board
// Setup mode of the Info column.
//  - With nothing picked: the ground net (or net class), the edge-terminated nets, the signal,
//    power and per-net-class impedances, and the digital bit rate.
//  - With a pad or trace picked: that pin's impedance, termination and noise source; the net's
//    and its net class's inclusion in new simulations, edge termination and impedance; and, for a
//    passive part, whether new simulations model it.
// Nothing here changes an existing simulation: SimulationEditor copies these values as nets are
// included, and applyBoardSetup() gives them to a new simulation.
#pragma once

#include "BoardModel.hpp"
#include "BoardView.hpp"

#include "kiems/config.hpp"

#include <QWidget>

#include <functional>
#include <memory>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace kiems_gui {

class Document;
class QuantityEdit;

class BoardSetupPanel : public QWidget {
    Q_OBJECT

public:
    explicit BoardSetupPanel(Document* document, QWidget* parent = nullptr);

    void setBoard(std::shared_ptr<const BoardData> board);
    void setPick(const BoardPick& pick);
    void refresh();

private:
    kiems::BoardSetupConfig setup() const;
    void edit(const std::function<void(kiems::BoardSetupConfig&)>& change);
    void populateBoardLists();
    void commitGround();
    QString netClassOf(const QString& net) const;
    QStringList netClasses() const;
    /// Whether the resolver terminates the picked pin when Board Setup doesn't say.
    bool pinAbsorbsByDefault(const kiems::BoardSetupConfig& setup) const;
    double defaultNoiseFrequency() const;
    bool pinPicked() const;
    bool componentPicked() const;

    Document* _document;
    std::shared_ptr<const BoardData> _board;
    BoardPick _pick;
    bool _updating = false;

    // Board-wide
    QGroupBox* _boardGroup;
    QComboBox* _ground;
    QListWidget* _edgeNets;
    QComboBox* _edgeNetPicker;
    QPushButton* _addEdgeNet;
    QPushButton* _removeEdgeNet;
    QuantityEdit* _signalImpedance;
    QuantityEdit* _powerImpedance;
    QWidget* _classImpedances;
    QFormLayout* _classImpedanceForm;
    std::vector<std::pair<QString, QuantityEdit*>> _classImpedanceFields;
    QuantityEdit* _bitRate;

    // Picked pin
    QGroupBox* _pinGroup;
    QLabel* _pinLabel;
    QuantityEdit* _pinImpedance;
    QCheckBox* _pinAbsorbing;
    QCheckBox* _noise;
    QWidget* _noiseControls;
    QuantityEdit* _noiseFrequency;
    QuantityEdit* _noiseAmplitude;

    // Picked passive part
    QGroupBox* _componentGroup;
    QLabel* _componentLabel;
    QCheckBox* _componentIncluded;

    // Picked net and its class
    QGroupBox* _netGroup;
    QLabel* _netLabel;
    QCheckBox* _netIncluded;
    QCheckBox* _netTerminated;
    QuantityEdit* _netImpedance;
    QGroupBox* _netClassGroup;
    QLabel* _netClassLabel;
    QCheckBox* _netClassIncluded;
    QuantityEdit* _netClassImpedance;
};

} // namespace kiems_gui
