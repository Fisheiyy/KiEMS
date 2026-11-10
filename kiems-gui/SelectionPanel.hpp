// The inspector's Selection section: what was picked on the board and how the selected simulation
// uses it, as the macOS app's Info panel shows a picked pin, component and net.
//  - Pin: whether it's excited (and how: the broadband main drive or a narrowband adversarial
//    one), probed and absorbing, and its port's impedance and load capacitance.
//  - Component, for a passive (R, L, C) part: whether it's modelled, and whether its pads grow
//    the simulated area.
//  - Net: whether its copper is included, whether it's a simulated net that grows the simulated
//    area (and by how much), its port impedance, and whether its traces get impedance probes.
// Every edit goes through SimulationEditor, which applies the macOS app's rules.
#pragma once

#include "BoardModel.hpp"
#include "BoardView.hpp"

#include <QWidget>

#include <memory>
#include <optional>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QWidget;

namespace kiems_gui {

class Document;
class QuantityEdit;
class SimulationEditor;

class SelectionPanel : public QWidget {
    Q_OBJECT

public:
    explicit SelectionPanel(Document* document, QWidget* parent = nullptr);
    ~SelectionPanel() override;

    void setBoard(std::shared_ptr<const BoardData> board);
    void setSimulation(int simulation);
    void setPick(const BoardPick& pick);
    void refresh();

private:
    SimulationEditor editor() const;
    /// The reference plane a simulation's first net takes: libkiems' default (the first inner
    /// layer) on a multilayer board; on a two-layer board, the outer layer opposite the pad.
    std::optional<int> defaultPlane() const;
    bool pinPicked() const;
    bool componentPicked() const;

    Document* _document;
    std::shared_ptr<const BoardData> _board;
    int _simulation = -1;
    BoardPick _pick;
    bool _updating = false;

    QLabel* _nothing;

    QGroupBox* _pinGroup;
    QLabel* _pinLabel;
    QCheckBox* _excited;
    QWidget* _excitation;
    QComboBox* _excitationType;
    QuantityEdit* _startTime;
    QuantityEdit* _phase;
    QuantityEdit* _amplitude;
    QuantityEdit* _frequency;
    QComboBox* _durationMode;
    QuantityEdit* _duration;
    QFormLayout* _excitationForm;
    QCheckBox* _probed;
    QCheckBox* _absorbing;
    QuantityEdit* _pinImpedance;
    QuantityEdit* _loadCapacitance;

    QGroupBox* _componentGroup;
    QLabel* _componentLabel;
    QCheckBox* _componentIncluded;
    QCheckBox* _componentHull;
    QuantityEdit* _componentPadding;

    QGroupBox* _netGroup;
    QLabel* _netLabel;
    QLabel* _netClassLabel;
    QCheckBox* _netIncluded;
    QCheckBox* _netSimulated;
    QuantityEdit* _netPadding;
    QuantityEdit* _netImpedance;
    QCheckBox* _impedanceProbed;
};

} // namespace kiems_gui
