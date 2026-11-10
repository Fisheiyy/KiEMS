// Board-aware edits to one simulation: which nets it includes and how fully, each pin's ports
// (excited, probed, absorbing, impedance, load capacitance), its excitations, and the passive
// components it models. The rules are the macOS app's (WholeBoardViewController), applied through
// libkiems' own configuration classes so they mean exactly what the pipeline will read:
//
//  - "Included" adds a net's copper to the geometry only; "Contributes to hull" (or probing or
//    exciting one of its pins) makes it a simulated net.
//  - A pin with no explicit choice absorbs by the resolver's own default rule (an input,
//    bidirectional or power-input pin on a multi-pad, non-ground net); an excited pin always does.
//  - A net is first included with Board Setup's defaults: its impedance, its pins' impedances and
//    absorbing choices, and its noise-generating pins as adversarial excitations.
//  - In a differential-pair simulation, each change is mirrored onto the same footprint's pin on
//    the partner net, an excitation as its antiphase complement.
#pragma once

#include "BoardModel.hpp"
#include "Document.hpp"

#include "kiems/config.hpp"

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace kiems_gui {

enum class NetLevel { None, GeometryOnly, Simulated };

/// One pin's effective port settings, as the pipeline will resolve them.
struct PinState {
    QString net;
    bool excited = false;
    bool probed = false;
    bool absorbing = false;
    /// The pin's own impedance override; netImpedance applies without one.
    std::optional<double> impedance;
    double netImpedance = 45;
    std::optional<double> loadCapacitance;
};

class SimulationEditor {
public:
    /// `defaultPlane` is the reference plane a simulation's first net takes (later ones share the
    /// simulation's); unset leaves libkiems' default.
    SimulationEditor(Document& document, const BoardData* board, int simulation,
                     std::optional<int> defaultPlane = std::nullopt);

    bool isValid() const;

    // --- Nets
    NetLevel netLevel(const QString& net) const;
    /// Includes the net's copper (geometry only) or removes the net from the simulation.
    void setNetIncluded(const QString& net, bool included);
    /// Makes an included net a simulated one, whose copper grows the hull, or demotes it.
    void setNetSimulated(const QString& net, bool simulated);
    double hullPadding(const QString& net) const;
    void setHullPadding(const QString& net, double micrometres);
    bool impedanceProbed(const QString& net) const;
    void setImpedanceProbed(const QString& net, bool probed);
    /// The impedance the net's ports are built with (its entry's, else Board Setup's default).
    double netImpedance(const QString& net) const;
    void setNetImpedance(const QString& net, double ohms);

    // --- Pins
    /// The pin's net on the board; empty if the pin isn't known or isn't connected.
    QString pinNet(const QString& footprint, const QString& pin) const;
    PinState pinState(const QString& footprint, const QString& pin) const;
    void setPinExcited(const QString& footprint, const QString& pin, bool excited);
    void setPinProbed(const QString& footprint, const QString& pin, bool probed);
    void setPinAbsorbing(const QString& footprint, const QString& pin, bool absorbing);
    void setPinImpedance(const QString& footprint, const QString& pin, std::optional<double> ohms);
    void setPinLoadCapacitance(const QString& footprint, const QString& pin, std::optional<double> farads);

    // --- Excitations
    std::optional<kiems::ExcitationConfig> excitation(const QString& footprint, const QString& pin) const;
    /// Changes the pin's excitation (and, in a differential pair, its partner's complement).
    void updateExcitation(const QString& footprint, const QString& pin,
                          const std::function<void(kiems::ExcitationConfig&)>& change);
    /// Switches the pin's excitation between the broadband main drive and a narrowband adversarial
    /// one, which starts continuous at the bottom of the frequency range.
    void setExcitationMain(const QString& footprint, const QString& pin, bool main);

    // --- Components
    /// Whether `reference` is a part the simulator can model as a lumped component (R, L or C).
    static bool isPassive(const QString& reference);
    bool componentIncluded(const QString& reference) const;
    void setComponentIncluded(const QString& reference, bool included);
    bool componentContributesToHull(const QString& reference) const;
    void setComponentContributesToHull(const QString& reference, bool contributes);
    double componentHullPadding(const QString& reference) const;
    void setComponentHullPadding(const QString& reference, double micrometres);

    /// Brings an existing simulation to the state it would have reached had it been a differential
    /// pair all along: each net's partner included at the same level, the pair marked, and pin
    /// choices and excitations mirrored. Run when the Differential pair box is ticked.
    void reconcileDifferentialPairs();

    // --- Heuristics shared with the macOS app
    static bool isPowerNet(const QString& net);
    static QStringList differentialPartnerCandidates(const QString& net);

private:
    using Json = nlohmann::json;

    const libkicad::FootprintPin* boardPin(const QString& footprint, const QString& pin) const;
    /// The partner net of `net` that exists on the board, if the name suggests one.
    QString partnerNet(const QString& net) const;
    /// The same footprint's pin on the partner of `net`, for differential-pair mirroring.
    std::optional<QString> partnerPin(const QString& footprint, const QString& net) const;
    bool isDifferentialPair() const;
    QStringList groundNets() const;
    bool absorbsByDefault(const QString& footprint, const QString& pin, const QString& net) const;
    kiems::BoardSetupConfig boardSetup() const;
    std::optional<int> planeForNewNet(const Json& sim) const;

    // Edits on the simulation's JSON, each entry round-tripped through libkiems' class.
    static int netEntryIndex(const Json& sim, const std::string& net);
    std::optional<kiems::InvolvedNetConfig> netEntry(const QString& net) const;
    /// Finds or creates the net's entry: `simulated` promotes an existing one (and, when it's
    /// created or promoted, adds Board Setup's noise sources); otherwise an existing entry keeps
    /// its level and a new one starts geometry-only.
    kiems::InvolvedNetConfig& includeNet(Json& sim, std::vector<kiems::InvolvedNetConfig>& entries,
                                         const std::string& net, bool simulated) const;
    void editNets(const std::function<void(Json& sim, std::vector<kiems::InvolvedNetConfig>& entries)>& edit);
    void addNoiseSources(Json& sim, const std::string& net) const;
    static int excitationIndex(const Json& sim, const std::string& footprint, const std::string& pin);
    void applyPinProbed(Json& sim, std::vector<kiems::InvolvedNetConfig>& entries, const QString& footprint,
                        const QString& pin, const QString& net, bool probed, bool absorbing) const;
    void applyPinExcited(Json& sim, std::vector<kiems::InvolvedNetConfig>& entries, const QString& footprint,
                         const QString& pin, const QString& net, bool excited) const;
    static void markPair(std::vector<kiems::InvolvedNetConfig>& entries, const std::string& first,
                         const std::string& second);
    static void complement(kiems::ExcitationConfig& partner, const kiems::ExcitationConfig& source);

    Document& _document;
    const BoardData* _board;
    int _simulation;
    std::optional<int> _defaultPlane;
};

} // namespace kiems_gui
