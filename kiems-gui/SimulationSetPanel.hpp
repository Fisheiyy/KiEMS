// The inspector's controls for a Simulation Set template, the macOS app's SimulationSetViewController:
// which IC drives each generated simulation and on which nets (a pattern or a list), how its pin is
// driven and terminated, and which other ICs' pins on those nets are probed. A summary says how many
// simulations that generates, or what's still missing. Edits go into the template's
// simulation_set; the main window regenerates the set's simulations from it.
#pragma once

#include "BoardModel.hpp"
#include "QuantityEdit.hpp"

#include "kiems/config.hpp"

#include <QWidget>

#include <functional>
#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QVBoxLayout;

namespace kiems_gui {

class Document;

class SimulationSetPanel : public QWidget {
    Q_OBJECT

public:
    explicit SimulationSetPanel(Document* document, QWidget* parent = nullptr);

    void setBoard(std::shared_ptr<const BoardData> board);
    void setSimulation(int simulation);
    void refresh();

private:
    struct ProbeControls {
        QGroupBox* group;
        QComboBox* footprint;
        QCheckBox* absorbing;
        QuantityEdit* loadImpedance;
        QuantityEdit* loadCapacitance;
        QFormLayout* form;
    };

    void edit(const std::function<void(kiems::SimulationSetConfig&)>& change);
    void editProbe(int index, const std::function<void(kiems::SimulationSetProbe&)>& change);
    void rebuildProbes(int count);
    void populateFootprints();
    void commitAdditionalPatterns();
    QComboBox* footprintCombo();

    Document* _document;
    std::shared_ptr<const BoardData> _board;
    int _simulation = -1;
    bool _updating = false;
    QStringList _references;

    QFormLayout* _form;
    QComboBox* _excitationIC;
    QComboBox* _selectionMode;
    QLineEdit* _pattern;
    QWidget* _netNamesBox;
    QListWidget* _netNames;
    QComboBox* _netPicker;
    QPushButton* _addNet;
    QPushButton* _removeNet;
    QWidget* _additionalBox;
    QListWidget* _additionalPatterns;
    QPushButton* _addPattern;
    QPushButton* _removePattern;
    QuantityEdit* _phase;
    QuantityEdit* _amplitude;
    QuantityEdit* _driveImpedance;
    QCheckBox* _probed;
    QCheckBox* _probeNetImpedance;
    QCheckBox* _absorbing;
    QuantityEdit* _loadImpedance;
    QuantityEdit* _loadCapacitance;
    QVBoxLayout* _probesLayout;
    std::vector<ProbeControls> _probes;
    QPushButton* _addProbe;
    QLabel* _summary;
};

} // namespace kiems_gui
