// DC power analyses as a document defines them, and how one becomes a dc::Problem on a KiCad board.
//
// A document's simulation.json may hold a "dc_analyses" array beside "simulations":
//   {"name": "+5V", "net": "+5V",
//    "sources": [{"footprint": "U2", "pin": "5", "voltage": 5.0}],
//    "loads":   [{"footprint": "J2", "pin": "1", "current": 0.5}, ...],
//    "temperature": 20, "plating": 0.025, "max_element": 0.5, "load_pads": "equipotential"}
// Temperatures are in C, lengths in mm, voltages in V and currents in A. "load_pads" is
// "equipotential" (the default: a soldered lead ties each load's pad together) or "distributed" (the
// load draws evenly from its pad's copper; see dc::Options::equipotentialLoadPads).
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dc_analysis.hpp"
#include "libkicad/libkicad.hpp"

namespace kiems::dc {

/// A pin and its value: volts for a source, amps drawn for a load.
struct PinValue {
    std::string footprint;
    std::string pin;
    double value = 0;
    /// A load's lowest acceptable voltage; a violation below it.
    std::optional<double> minimumVoltage;

    bool operator==(const PinValue&) const = default;
};

/// What counts as too much; unset limits aren't checked.
struct Limits {
    std::optional<double> outerCurrentDensity; // A/mm^2, on the outermost copper layers
    std::optional<double> innerCurrentDensity; // A/mm^2, on the others
    std::optional<double> viaCurrent;          // A, through any one barrel

    bool operator==(const Limits&) const = default;
};

struct AnalysisConfig {
    std::string name;
    std::string net;
    std::vector<PinValue> sources;
    std::vector<PinValue> loads;
    Options options;
    Limits limits;

    bool operator==(const AnalysisConfig&) const = default;
};

/// A limit the solution exceeds.
struct Violation {
    enum class Kind { Voltage, CurrentDensity, ViaCurrent };
    Kind kind = Kind::Voltage;
    std::string message;
    double value = 0;
    double limit = 0;
    /// Where it is: the layer (-1 for a via or pin) and the point, mm.
    int layer = -1;
    double x = 0, y = 0;
};

/// The limits `config` sets that `result` exceeds.
std::vector<Violation> findViolations(const Problem& problem, const Result& result, const AnalysisConfig& config);

void to_json(nlohmann::json& j, const AnalysisConfig& config);
void from_json(const nlohmann::json& j, AnalysisConfig& config);

/// The analyses in a document's JSON ("dc_analyses"); empty if it has none.
std::vector<AnalysisConfig> analysesIn(const nlohmann::json& document);

/// The net's copper on each layer, its plated holes, and the pins' pads, from the board: the input
/// the solver needs. Through holes are taken to span every layer (KiCad's stock queries don't
/// distinguish blind and buried vias).
std::expected<Problem, std::string> buildProblem(const libkicad::Board& board, const AnalysisConfig& config);
/// The same, from the board's stackup, geometry and plated holes already read.
std::expected<Problem, std::string> buildProblem(const std::vector<libkicad::StackupLayer>& stackup,
                                                 const libkicad::BoardGeometry& geometry,
                                                 const std::vector<libkicad::ThroughHole>& holes,
                                                 const AnalysisConfig& config);

/// A starting point from KiCad's pin types: every power output pin on the net a source at `volts`,
/// and every power input pin a load, `totalCurrent` shared equally between them.
std::expected<AnalysisConfig, std::string> automaticAnalysis(const libkicad::Board& board, const std::string& net,
                                                             double volts, double totalCurrent);
/// The same, from the board's pads already read.
AnalysisConfig automaticAnalysis(const std::vector<libkicad::PadPosition>& pads, const std::string& net, double volts,
                                 double totalCurrent);

/// The volts a rail's name implies ("+5V" 5, "+3.3V" 3.3, "+1V8" 1.8, "VCC_12V" 12); 0 if none.
double voltageFromNetName(const std::string& net);

/// Writes `result` to `directory`: result.json (terminal voltages and currents, barrel currents,
/// each layer's extremes and the warnings) and, per layer, a voltage and a current density map
/// (<layer>_voltage.png, <layer>_current_density.png).
std::expected<void, std::string> writeResult(const Result& result, const AnalysisConfig& config,
                                             const std::vector<Violation>& violations,
                                             const std::filesystem::path& directory, bool plots = true);

} // namespace kiems::dc
