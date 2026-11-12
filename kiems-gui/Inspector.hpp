// The right-hand panel, in tabs as the macOS app's Info column is:
//  - Simulation: the selected simulation's networks -- name, ground net, reference plane, its
//    excitations, involved and edge-terminated nets -- and, for a Simulation Set, the set's own
//    controls (SimulationSetPanel).
//  - Selection: whatever was picked on the board, and how the simulation uses it (SelectionPanel).
//  - Board Setup: the board-wide defaults new simulations and newly included nets take, and a
//    picked pin's, net's or part's own (BoardSetupPanel).
//  - Settings: its geometry and resolution settings (SettingsPanel).
//  - Tests: its eye test (TestsPanel).
#pragma once

#include "BoardModel.hpp"
#include "BoardView.hpp"
#include "Document.hpp"

#include <QWidget>

#include <memory>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QLayout;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTabWidget;

namespace kiems_gui {

class BoardSetupPanel;
class SelectionPanel;
class SettingsPanel;
class SimulationSetPanel;
class TestsPanel;

class Inspector : public QWidget {
    Q_OBJECT

public:
    Inspector(Document* document, QWidget* parent = nullptr);

    void setBoard(std::shared_ptr<const BoardData> board);
    void setSimulation(int simulation);
    /// Switches to the Board Setup tab.
    void showBoardSetup();
    /// Shows `pick` in the Selection and Board Setup tabs, switching to Selection when something was
    /// picked unless Board Setup is open.
    void setPick(const BoardPick& pick);
    /// Re-reads everything from the document.
    void refresh();

private:
    void populateBoardLists();

    Document* _document;
    std::shared_ptr<const BoardData> _board;
    int _simulation = -1;
    bool _updating = false;

    QTabWidget* _tabs;
    QWidget* _selectionPage;
    SelectionPanel* _selection;
    QWidget* _boardSetupPage;
    BoardSetupPanel* _boardSetup;
    SettingsPanel* _settings;
    TestsPanel* _tests;

    SimulationSetPanel* _setPanel;
    QGroupBox* _simulationGroup;
    QFormLayout* _simulationForm;
    QLayout* _excitationsRow;
    QLineEdit* _name;
    QCheckBox* _differentialPair;
    QComboBox* _groundNet;
    QComboBox* _plane;
    QListWidget* _excitations;
    QPushButton* _removeExcitation;
    QListWidget* _nets;
    QPushButton* _removeNet;
    QListWidget* _edgeNets;
    QComboBox* _edgeNetPicker;
    QPushButton* _addEdgeNet;
    QPushButton* _removeEdgeNet;
};

} // namespace kiems_gui
