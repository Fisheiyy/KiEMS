#include "SettingsPanel.hpp"

#include "Document.hpp"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace kiems_gui {

namespace {

std::optional<double> number(const nlohmann::json& value) {
    return value.is_number() ? std::optional(value.get<double>()) : std::nullopt;
}

QLabel* note(const QString& text = {}) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setEnabled(false); // the palette's muted text
    return label;
}

} // namespace

SettingsPanel::SettingsPanel(Document* document, QWidget* parent) : QWidget(parent), _document(document) {
    using Kind = QuantityEdit::Kind;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // --- This simulation
    _simulationGroup = new QGroupBox(tr("This simulation"));
    auto* simulationForm = new QFormLayout(_simulationGroup);
    _viaEdgeDistance = simulationField(Kind::Length, "/via_edge_distance",
                                       tr("How far inside the simulated area's edge the ground stitching vias that close "
                                          "it are placed."));
    _viaSpacing = simulationField(Kind::Length, "/via_spacing",
                                  tr("The spacing between ground stitching vias along the simulated area's edge."));
    _excitationDuration = new QuantityEdit(Kind::Time);
    _excitationDuration->setMinimum(0);
    _excitationDuration->setToolTip(tr("The main excitation's pulse window: how long a stimulus the results' "
                                       "time-domain responses are shown for."));
    _bindings.emplace_back(_excitationDuration, [this] { return _document->mainExcitationDuration(_simulation); });
    connect(_excitationDuration, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        if (value && _simulation >= 0) _document->setMainExcitationDuration(_simulation, *value);
    });
    _eyeDrawCount = simulationField(Kind::Integer, "/eye_draw_count",
                                    tr("How many random draws of the adversarial signals' timing an eye's noisy "
                                       "result is averaged over."));
    _eyeDrawCount->setMinimum(1);
    _sharedClock = new QCheckBox(tr("Adversarial signals share a clock"));
    _sharedClock->setToolTip(tr("Whether the adversarial excitations switch in step with the eye's own bit clock "
                                "rather than at independent random times."));
    connect(_sharedClock, &QCheckBox::toggled, this, [this](bool on) {
        if (_simulation >= 0) _document->setSimulationValue(_simulation, "/adversarial_shared_clock", on);
    });
    _terminateGround = new QCheckBox(tr("Absorb the ground pour where the cut crosses it"));
    _terminateGround->setToolTip(
        tr("Terminates the ground pour along the simulated area's edge, wherever that edge cuts through the "
           "board rather than following its real outline. A cut pour otherwise rings like a patch antenna, "
           "which puts false resonances in the results and makes runs much longer."));
    connect(_terminateGround, &QCheckBox::toggled, this, [this](bool on) {
        if (_simulation >= 0) _document->setSimulationValue(_simulation, "/terminate_ground_at_cut", on);
    });
    simulationForm->addRow(_terminateGround);
    simulationForm->addRow(tr("Stitching inset:"), _viaEdgeDistance);
    simulationForm->addRow(tr("Stitching spacing:"), _viaSpacing);
    simulationForm->addRow(tr("Excitation duration:"), _excitationDuration);
    simulationForm->addRow(tr("Adversarial draws:"), _eyeDrawCount);
    simulationForm->addRow(_sharedClock);
    layout->addWidget(_simulationGroup);

    // --- Every simulation in the document
    _documentGroup = new QGroupBox(tr("All simulations in this document"));
    auto* documentForm = new QFormLayout(_documentGroup);
    _frequencyStart = new QuantityEdit(Kind::Frequency);
    _frequencyStop = new QuantityEdit(Kind::Frequency);
    for (QuantityEdit* field : {_frequencyStart, _frequencyStop}) {
        field->setMinimum(1);
        field->setToolTip(tr("The band the simulations' broadband excitation pulse covers, and their results span."));
    }
    _bindings.emplace_back(_frequencyStart, [this] { return number(_document->documentValue("/frequency/start")); });
    _bindings.emplace_back(_frequencyStop, [this] { return number(_document->documentValue("/frequency/stop")); });
    // The range has to stay a range: an edit that would invert it is undone.
    connect(_frequencyStart, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        const auto stop = number(_document->documentValue("/frequency/stop"));
        if (value && (!stop || *value < *stop)) _document->setDocumentValue("/frequency/start", *value);
        refresh();
    });
    connect(_frequencyStop, &QuantityEdit::valueEdited, this, [this](std::optional<double> value) {
        const auto start = number(_document->documentValue("/frequency/start"));
        if (value && (!start || *value > *start)) _document->setDocumentValue("/frequency/stop", *value);
        refresh();
    });
    auto* frequencyRange = new QHBoxLayout;
    frequencyRange->setContentsMargins(0, 0, 0, 0);
    frequencyRange->addWidget(_frequencyStart, 1);
    frequencyRange->addWidget(new QLabel(QString(QChar(0x2013))));
    frequencyRange->addWidget(_frequencyStop, 1);

    _maxSteps = documentField(Kind::Integer, "/max_steps",
                              tr("The most timesteps any port's run takes, even if its energy hasn't decayed by then. "
                                 "Too few cut the response short, which shows up as ripple in the S-parameters."));
    _maxSteps->setMinimum(1);
    _gridDensity = documentField(Kind::Length, "/grid/optimal",
                                 tr("The mesh's target cell size around copper: the main control of how fine, and how "
                                    "slow, a simulation is."));
    _gridDensity->setMinimum(1e-3);
    _absorbingBoundary = documentField(Kind::Integer, "/grid/absorbing_boundary_cells",
                                       tr("How many cells deep the absorbing boundary is on every face of the domain."));
    _absorbingBoundary->setMinimum(1);
    _absorbingBoundary->setMaximum(64);
    _marginWidth = documentField(Kind::Length, "/grid/margin/xy",
                                 tr("The space the mesh leaves between the board and the absorbing boundary in X and Y."));
    _marginWidth->setMinimum(0);
    _platingThickness = documentField(Kind::Length, "/via/plating_thickness",
                                      tr("The thickness of the copper plating on via walls."));
    _platingThickness->setMinimum(0);
    _fillingEpsilon = documentField(Kind::Number, "/via/filling_epsilon",
                                    tr("The relative permittivity of what fills vias' holes (1 for air)."));
    _fillingEpsilon->setMinimum(1);
    _wavelengthNote = note();

    documentForm->addRow(tr("Frequency range:"), frequencyRange);
    documentForm->addRow(tr("Max timesteps:"), _maxSteps);
    documentForm->addRow(tr("Min resolution:"), _gridDensity);
    documentForm->addRow(tr("Absorbing boundary (cells):"), _absorbingBoundary);
    documentForm->addRow(tr("Margin width:"), _marginWidth);
    documentForm->addRow(tr("Via plating thickness:"), _platingThickness);
    documentForm->addRow(tr("Via filling epsilon:"), _fillingEpsilon);
    documentForm->addRow(_wavelengthNote);
    layout->addWidget(_documentGroup);
    layout->addWidget(note(tr("Changes take effect the next time a simulation runs. Lengths accept mm, " "\xC2\xB5" "m, mil "
                              "and in; frequencies and times accept SI prefixes (2.4 GHz, 500 ps).")));
    layout->addStretch();

    connect(_document, &Document::changed, this, &SettingsPanel::refresh);
    refresh();
}

QuantityEdit* SettingsPanel::documentField(QuantityEdit::Kind kind, const QString& pointer, const QString& toolTip) {
    auto* field = new QuantityEdit(kind);
    field->setToolTip(toolTip);
    _bindings.emplace_back(field, [this, pointer] { return number(_document->documentValue(pointer)); });
    connect(field, &QuantityEdit::valueEdited, this, [this, pointer, kind](std::optional<double> value) {
        if (!value) return;
        if (kind == QuantityEdit::Kind::Integer) _document->setDocumentValue(pointer, std::llround(*value));
        else _document->setDocumentValue(pointer, *value);
    });
    return field;
}

QuantityEdit* SettingsPanel::simulationField(QuantityEdit::Kind kind, const QString& pointer, const QString& toolTip) {
    auto* field = new QuantityEdit(kind);
    field->setToolTip(toolTip);
    field->setMinimum(0);
    _bindings.emplace_back(field, [this, pointer] {
        return _simulation >= 0 ? number(_document->simulationValue(_simulation, pointer)) : std::nullopt;
    });
    connect(field, &QuantityEdit::valueEdited, this, [this, pointer, kind](std::optional<double> value) {
        if (!value || _simulation < 0) return;
        if (kind == QuantityEdit::Kind::Integer) _document->setSimulationValue(_simulation, pointer, std::llround(*value));
        else _document->setSimulationValue(_simulation, pointer, *value);
    });
    return field;
}

void SettingsPanel::setSimulation(int simulation) {
    _simulation = simulation;
    refresh();
}

void SettingsPanel::refresh() {
    const bool open = _document->isOpen();
    if (_simulation >= _document->simulationCount()) _simulation = -1;
    const bool haveSimulation = open && _simulation >= 0;
    _documentGroup->setEnabled(open);
    _simulationGroup->setEnabled(haveSimulation);
    for (const auto& [field, read] : _bindings) field->refreshValue(open ? read() : std::nullopt);

    // Every main excitation's window at once; differing ones show as Mixed until one is entered.
    const QList<double> durations = haveSimulation ? _document->mainExcitationDurations(_simulation) : QList<double>();
    const bool mixed = std::any_of(durations.begin(), durations.end(), [&](double d) { return d != durations.first(); });
    _excitationDuration->setEnabled(!durations.isEmpty());
    if (mixed) _excitationDuration->refreshValue(std::nullopt);
    _excitationDuration->setPlaceholderText(!haveSimulation  ? QString()
                                            : durations.isEmpty() ? tr("No main excitation")
                                            : mixed               ? tr("Mixed")
                                                                  : QString());
    {
        const QSignalBlocker blocker(_sharedClock);
        _sharedClock->setChecked(haveSimulation &&
                                 _document->simulationValue(_simulation, "/adversarial_shared_clock").get<bool>());
    }
    {
        const QSignalBlocker blocker(_terminateGround);
        _terminateGround->setChecked(haveSimulation &&
                                     _document->simulationValue(_simulation, "/terminate_ground_at_cut").get<bool>());
    }

    // What the frequency range implies for the mesh and margin -- libkiems' own rules, shown so the
    // numbers above aren't a surprise.
    QString text;
    if (open) {
        const auto start = number(_document->documentValue("/frequency/start"));
        const auto stop = number(_document->documentValue("/frequency/stop"));
        if (stop && *stop > 0) {
            // Grid::applyFrequencyConstraint(): cells no larger than a tenth of the shortest
            // wavelength in FR-4 (permittivity 4.13).
            const double shortestCell = 3e8 * 1e6 / std::sqrt(4.13) / *stop / 10;
            text = tr("At %1 the largest mesh cell is capped at %2.")
                       .arg(QuantityEdit::format(QuantityEdit::Kind::Frequency, *stop),
                            QuantityEdit::format(QuantityEdit::Kind::Length, std::floor(shortestCell)));
        }
        if (start && *start > 0) {
            // Margin::suggested(): a quarter of the vacuum wavelength at the start frequency.
            const double suggestedMargin = 299792458.0 * 1e6 / *start / 4.0;
            _marginWidth->setToolTip(tr("The space the mesh leaves between the board and the absorbing boundary in X and "
                                        "Y. A quarter wavelength at %1, the antenna rule of thumb, would be %2; for "
                                        "signal integrity a few dielectric heights past the copper is usually enough.")
                                         .arg(QuantityEdit::format(QuantityEdit::Kind::Frequency, *start),
                                              QuantityEdit::format(QuantityEdit::Kind::Length, suggestedMargin)));
        }
    }
    _wavelengthNote->setText(text);
}

} // namespace kiems_gui
