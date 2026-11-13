// The inspector's Settings tab: the selected simulation's own geometry and excitation settings,
// and the settings every simulation in the document shares -- the frequency sweep, the timestep
// limit, the mesh and the vias -- the macOS app's Geometry and Resolution sections.
#pragma once

#include "QuantityEdit.hpp"

#include <QWidget>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

class QCheckBox;
class QGroupBox;
class QLabel;

namespace kiems_gui {

class Document;

class SettingsPanel : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPanel(Document* document, QWidget* parent = nullptr);

    void setSimulation(int simulation);
    void refresh();

private:
    /// A field bound to a document-wide number at `pointer`.
    QuantityEdit* documentField(QuantityEdit::Kind kind, const QString& pointer, const QString& toolTip);
    /// A field bound to the selected simulation's number at `pointer`.
    QuantityEdit* simulationField(QuantityEdit::Kind kind, const QString& pointer, const QString& toolTip);

    Document* _document;
    int _simulation = -1;

    QGroupBox* _simulationGroup;
    QuantityEdit* _viaEdgeDistance;
    QuantityEdit* _viaSpacing;
    QuantityEdit* _excitationDuration;
    QuantityEdit* _eyeDrawCount;
    QCheckBox* _sharedClock;
    QCheckBox* _terminateGround;

    QGroupBox* _documentGroup;
    QuantityEdit* _frequencyStart;
    QuantityEdit* _frequencyStop;
    QCheckBox* _shortPulse;
    QuantityEdit* _maxSteps;
    QuantityEdit* _gridDensity;
    QuantityEdit* _absorbingBoundary;
    QuantityEdit* _marginWidth;
    QuantityEdit* _platingThickness;
    QuantityEdit* _fillingEpsilon;
    QLabel* _wavelengthNote;

    /// Each bound field with how to read its value back from the document.
    std::vector<std::pair<QuantityEdit*, std::function<std::optional<double>()>>> _bindings;
};

} // namespace kiems_gui
