// A KiEMS simulation document: a .g2esim package folder holding simulation.json and, once
// simulations have run, the pipeline's fab/ and ems/ output -- the same packages the macOS app
// reads and writes. (A bare simulation.json is accepted too; its folder plays the package's role.)
//
// The configuration is held as the simulation.json JSON itself. The app edits only the fields it
// has controls for, so everything else a document contains -- including settings only the macOS
// app or a hand-edited file uses -- survives a save untouched. libkiems' own parser validates it.
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>

namespace kiems_gui {

struct PinRef {
    QString footprint;
    QString pin;
    bool operator==(const PinRef&) const = default;
};

class Document : public QObject {
    Q_OBJECT

public:
    explicit Document(QObject* parent = nullptr);

    /// Opens a .g2esim package folder, a folder holding simulation.json, or a simulation.json.
    bool open(const QString& path, QString* error);
    /// Creates a new package folder at `packagePath`, linked to `kicadPcbPath`, and saves it.
    bool create(const QString& packagePath, const QString& kicadPcbPath, QString* error);
    bool save(QString* error);

    bool isOpen() const { return !_configPath.isEmpty(); }
    bool isModified() const { return _modified; }
    QString packageDir() const;
    QString configPath() const { return _configPath; }
    QString displayName() const;

    /// The linked .kicad_pcb, as an absolute path; empty if none.
    QString kicadPcbPath() const;
    void setKicadPcbPath(const QString& path);

    int simulationCount() const;
    QString simulationName(int simulation) const;
    int simulationIndex(const QString& name) const;
    int addSimulation(const QString& name, const QString& groundNet);
    void removeSimulation(int simulation);
    void renameSimulation(int simulation, const QString& name);

    // --- Simulation Sets (see SimulationSet.hpp)
    /// Adds a Simulation Set template, with a fresh id and nothing chosen yet.
    int addSimulationSet(const QString& name, const QString& groundNet);
    /// Whether the simulation is a Simulation Set template, which generates simulations rather than
    /// running itself.
    bool isSimulationSet(int simulation) const { return !simulationSetId(simulation).isEmpty(); }
    /// A Simulation Set template's id; empty for any other simulation.
    QString simulationSetId(int simulation) const;
    /// The id of the set the simulation was generated from; empty if it wasn't. The set's template
    /// may since have been removed.
    QString generatedBySet(int simulation) const;
    /// The template of the set `simulation` was generated from, if it still exists; -1 otherwise.
    int generatingTemplate(int simulation) const;
    /// Writes a simulation's JSON outright; `simulation` == simulationCount() appends it. Returns its
    /// index. For regenerating a set's simulations.
    int putSimulationJson(int simulation, nlohmann::json json);

    /// Holds changed() back until the outermost batch ends, then emits it once -- and only if the
    /// document's JSON differs from what it was when the batch began, so a batch of edits that
    /// cancel out leaves the document unmodified.
    class ChangeBatch {
    public:
        explicit ChangeBatch(Document& document);
        ~ChangeBatch();
        ChangeBatch(const ChangeBatch&) = delete;
        ChangeBatch& operator=(const ChangeBatch&) = delete;

    private:
        Document& _document;
    };

    QStringList involvedNets(int simulation) const;
    /// Adds or removes an involved net. A new net takes the simulation's reference plane, or
    /// `planeForFirstNet` when no net has set one yet.
    void setNetIncluded(int simulation, const QString& net, bool included, std::optional<int> planeForFirstNet = {});
    QString groundNet(int simulation) const;
    void setGroundNet(int simulation, const QString& net);
    /// The reference-plane metal index shared by the simulation's involved nets: the layer, counted
    /// from F.Cu = 0, that ports measure against. nullopt if none set one (libkiems then uses 1).
    std::optional<int> referencePlane(int simulation) const;
    void setReferencePlane(int simulation, int plane);
    std::optional<PinRef> mainExcitation(int simulation) const;
    /// Replaces the main excitation; its timing carries over to a newly picked pin.
    void setMainExcitation(int simulation, const std::optional<PinRef>& pin);
    /// The main excitation's pulse window in seconds; nullopt without a main excitation.
    std::optional<double> mainExcitationDuration(int simulation) const;
    /// Every main excitation's pulse window, in order.
    QList<double> mainExcitationDurations(int simulation) const;
    /// Sets every main excitation's pulse window.
    void setMainExcitationDuration(int simulation, double seconds);

    /// The simulation's own JSON, for SimulationEditor; null if there's no such simulation.
    const nlohmann::json* simulationJson(int simulation) const { return this->simulation(simulation); }
    /// Applies `edit` to the simulation's JSON, signalling a change only if it changed anything.
    void editSimulation(int simulation, const std::function<void(nlohmann::json&)>& edit);

    /// A document-wide setting -- the frequency range, mesh, vias and timestep limit every
    /// simulation shares -- at `pointer` (a JSON pointer, e.g. "/grid/optimal"), or libkiems' own
    /// default when the document doesn't set it.
    nlohmann::json documentValue(const QString& pointer) const;
    void setDocumentValue(const QString& pointer, const nlohmann::json& value);
    /// One simulation's own setting at `pointer` (relative to the simulation, e.g. "/via_spacing"),
    /// or its default. Settings libkiems leaves out of the file at their default are removed again
    /// when set back to it, so a document only the GUI touched reads exactly as libkiems writes it.
    nlohmann::json simulationValue(int simulation, const QString& pointer) const;
    void setSimulationValue(int simulation, const QString& pointer, const nlohmann::json& value);

    /// Pipeline output for one simulation, under the package's ems/ folder.
    QString resultsDir(int simulation) const;
    QString simulationDir(int simulation) const;
    /// The geometry stage's output for one simulation (ems/geometry/<name>/geometry.json).
    QString geometryFile(int simulation) const;
    bool hasGeometry(int simulation) const;
    bool hasResults(int simulation) const;

    /// Checks the configuration with libkiems' own parser; empty when it's valid.
    QString validate() const;
    /// A name not yet used by any simulation, based on `base`.
    QString uniqueSimulationName(const QString& base) const;

signals:
    void changed();
    void modifiedChanged(bool modified);
    void simulationRenamed(const QString& from, const QString& to);

private:
    nlohmann::json* simulation(int index);
    const nlohmann::json* simulation(int index) const;
    void touch();

    QString _configPath;
    nlohmann::json _json;
    bool _modified = false;
    int _batchDepth = 0;
    nlohmann::json _batchStart;
};

} // namespace kiems_gui
