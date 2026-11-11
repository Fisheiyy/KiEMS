// DC power analysis: the voltage drop (IR drop) and current density across one net's copper when
// it carries a rail's load currents -- what Altium's Power Analyzer (Keysight PDN) shows. See
// docs/dc_power_analysis.md for the physics, the prototype it was checked against, and the plan.
//
// At DC a copper layer of thickness t carries sheet current K = -(t/rho) grad V, and conservation
// of charge makes V obey div((t/rho) grad V) = 0 on each layer's copper, with:
//  - sources (a regulator's output pins) held at their voltage,
//  - loads drawing their current out of their pads,
//  - plated barrels (vias, through-hole pads) joining layers as resistors,
//    R = rho L / (pi (d + p) p) for drill d, plating p and length L,
//  - no current through the copper's edges anywhere else.
// Each layer's copper is meshed with refined, constrained Delaunay triangles (CGAL Mesh_2) and
// solved with linear finite elements; a pad, and a barrel's annular ring on each layer it joins, are
// equipotential. Copper not connected to any source is reported rather than solved.
//
// This file is the solver alone, on geometry in millimetres; dc_board.hpp builds a DCProblem from a
// KiCad board and a DCAnalysisConfig.
#pragma once

#include <array>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "polygon_geometry.hpp"

namespace kiems::dc {

/// One copper layer of the net being analysed.
struct Layer {
    std::string name;
    double thicknessMm = 0.035;
    /// Height in the stackup, for barrel lengths; any origin, as only differences matter.
    double zMm = 0;
    /// The net's copper on this layer, in mm; overlapping pieces are merged.
    Cu::PolygonSet copper;
};

/// A plated hole joining layers: a via, or a through-hole pad's barrel.
struct Barrel {
    std::string name;
    double x = 0, y = 0;
    double drillMm = 0.3;
    /// The annular ring's outer diameter: the copper on each layer tied to the barrel.
    double ringMm = 0.6;
    /// The layers it spans, as indices into DCProblem::layers (a through via spans them all).
    int firstLayer = 0;
    int lastLayer = 0;
};

/// Where current enters or leaves the net: a pin's pad.
struct Terminal {
    enum class Kind { Source, Load };
    std::string name;
    Kind kind = Kind::Load;
    int layer = 0;
    /// The pad's copper, in mm. A through-hole pad is a terminal on one layer; its barrel carries
    /// the current to the others.
    Cu::PolygonSet pad;
    /// Volts for a source; amps drawn for a load.
    double value = 0;
    /// Loads with the same package (>= 0) are one part's pins: their total current is drawn from
    /// a node inside the part, tied to each pin's pad, so the copper decides how it splits between
    /// them. Each such pin's pad is one node. -1 for a pin that draws its own current.
    int package = -1;
};

struct Options {
    /// Copper resistivity at 20 C, ohm metres.
    double resistivity = 1.72e-8;
    /// The working temperature: resistivity rises by 0.393 %/K above 20 C.
    double temperatureC = 20;
    /// Barrel wall plating; KiCad doesn't record it (IPC-6012 class 3 is 25 um).
    double platingMm = 0.025;
    /// The largest triangle edge the mesh allows; finer features get finer elements regardless.
    double maxElementMm = 0.5;
    /// Whether a load's pad is one node -- a soldered lead ties it together, so current passing
    /// through the pad to the next load meets no resistance across it -- or its current is drawn
    /// evenly from the pad's copper, which then conducts like any other.
    bool equipotentialLoadPads = true;
    /// Whether a part's load pins share its current (dc::Terminal::package), letting the copper
    /// decide the split, rather than each drawing the current it's given.
    bool sharedPartLoads = false;
    /// An h-refinement study: after solving on the mesh above, split every element into four and
    /// solve again, until no terminal's voltage moves by more than `studyTolerance` of the worst
    /// drop, nor the power loss by that fraction of itself -- or `studyMaxRefinements` splits on.
    bool refinementStudy = false;
    double studyTolerance = 0.01;
    int studyMaxRefinements = 3;

    bool operator==(const Options&) const = default;
};

struct Problem {
    std::vector<Layer> layers;
    std::vector<Barrel> barrels;
    std::vector<Terminal> terminals;
    Options options;
};

/// One layer's mesh and solution.
struct LayerResult {
    std::string name;
    std::vector<Cu::Position> nodes;              // mm
    std::vector<std::array<int, 3>> triangles;    // node indices
    /// Volts at each node; NaN where the copper isn't connected to a source.
    std::vector<double> voltage;
    /// Each triangle's current density, A/mm^2, as (x, y) components; NaN where unconnected.
    std::vector<std::array<double, 2>> currentDensity;
    double maxCurrentDensity = 0; // A/mm^2
};

struct BarrelResult {
    std::string name;
    double resistance = 0; // ohms, end to end of the layers it joins
    double current = 0;    // amps, the most through any of its spans
    bool connected = false;
};

struct TerminalResult {
    std::string name;
    Terminal::Kind kind = Terminal::Kind::Load;
    double voltage = 0; // volts; NaN when it isn't connected to a source
    double current = 0; // amps out of the net at a load (its share, for a package's pin), into it at a source
    bool connected = false;
};

/// One mesh of an h-refinement study.
struct RefinementLevel {
    int refinements = 0; // times every element of the first mesh was split into four
    int nodes = 0;
    int elements = 0;
    int unknowns = 0;
    /// The largest difference between the terminals' voltages: the worst drop (or rise, on a return).
    double worstDrop = 0;
    double powerLoss = 0;
    /// Not a convergence measure: at a copper corner the true density is infinite, so this keeps rising.
    double maxCurrentDensity = 0;
    /// How far the answer moved from the level before, as a fraction (see Options::studyTolerance);
    /// NaN on the first.
    double change = std::numeric_limits<double>::quiet_NaN();
    double seconds = 0;
};

struct RefinementStudy {
    std::vector<RefinementLevel> levels;
    bool converged = false;
    /// Richardson's estimate of the worst drop on an infinitely fine mesh, from the last levels.
    double extrapolatedWorstDrop = std::numeric_limits<double>::quiet_NaN();
    /// The order of convergence the last three levels show; NaN with fewer, or when they don't
    /// converge steadily (2 is assumed for the extrapolation then).
    double observedOrder = std::numeric_limits<double>::quiet_NaN();
    /// |extrapolated - finest| / finest worst drop: the error left in the result.
    double estimatedError = std::numeric_limits<double>::quiet_NaN();
};

struct Result {
    std::vector<LayerResult> layers;
    std::vector<BarrelResult> barrels;
    std::vector<TerminalResult> terminals;
    double powerLoss = 0; // watts dissipated in the copper
    std::vector<std::string> warnings;
    int unknowns = 0;
    int iterations = 0;
    /// When Options::refinementStudy is set: each mesh's answer. The result is the finest's.
    std::optional<RefinementStudy> study;
};

/// Meshes and solves `problem` -- refining until it converges, with Options::refinementStudy. Fails
/// when nothing can be solved: no source on any copper, or no copper at all.
std::expected<Result, std::string> solve(const Problem& problem);

/// The resistance of a plated barrel wall: rho L / (pi (d + p) p).
double barrelResistance(double resistivity, double lengthMm, double drillMm, double platingMm);

void to_json(nlohmann::json& j, const Result& result);

} // namespace kiems::dc
