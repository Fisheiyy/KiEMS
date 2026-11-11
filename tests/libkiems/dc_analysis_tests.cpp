// Checks the DC power analysis solver (libkiems/kiems/dc_analysis.hpp) against cases with exact
// answers. Run by CTest, or directly: it prints each check and exits non-zero if any failed.
#include "kiems/dc_analysis.hpp"

#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

/// Within `tolerance` (relative) of `expected`.
void near(double actual, double expected, double tolerance, const std::string& what) {
    const bool ok = std::abs(actual - expected) <= tolerance * std::abs(expected);
    check(ok, what + ": " + std::to_string(actual) + " (expected " + std::to_string(expected) + ")");
}

Cu::Polygon rectangle(double x0, double y0, double x1, double y1) {
    return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}

Cu::Polygon circle(double x, double y, double r, int n, bool clockwise = false) {
    Cu::Polygon loop;
    for (int i = 0; i < n; ++i) {
        const double a = (clockwise ? -1 : 1) * 2 * std::numbers::pi * i / n;
        loop.emplace_back(x + r * std::cos(a), y + r * std::sin(a));
    }
    return loop;
}

kiems::dc::Terminal terminal(const std::string& name, kiems::dc::Terminal::Kind kind, int layer, Cu::PolygonSet pad,
                             double value) {
    kiems::dc::Terminal t;
    t.name = name;
    t.kind = kind;
    t.layer = layer;
    t.pad = std::move(pad);
    t.value = value;
    return t;
}

constexpr double kRho = 1.72e-8;
constexpr double kThickness = 0.035; // mm
const double kSheet = kRho / (kThickness * 1e-3); // ohms per square

using Kind = kiems::dc::Terminal::Kind;

void strip() {
    // A 10 x 1 mm strip, a 0.15 mm pad at each end, 1 A: 9.7 squares of copper between the pads.
    kiems::dc::Problem problem;
    problem.layers.push_back({"F.Cu", kThickness, 0, {rectangle(0, 0, 10, 1)}});
    problem.terminals.push_back(terminal("source", Kind::Source, 0, {rectangle(0, 0, 0.15, 1)}, 5.0));
    problem.terminals.push_back(terminal("load", Kind::Load, 0, {rectangle(9.85, 0, 10, 1)}, 1.0));
    const auto result = kiems::dc::solve(problem);
    check(result.has_value(), "strip solves");
    if (!result) return;
    // The pads' edges are only as sharp as the mesh nodes along them, which costs a little.
    near(5.0 - result->terminals[1].voltage, 9.7 * kSheet, 2e-3, "strip drop, V");
    near(result->terminals[0].current, 1.0, 1e-6, "strip source current, A");
    // Uniform away from the pads.
    double lowest = 1e9, highest = 0;
    const auto& layer = result->layers[0];
    for (std::size_t e = 0; e < layer.triangles.size(); ++e) {
        double cx = 0;
        for (int k = 0; k < 3; ++k) cx += layer.nodes[static_cast<std::size_t>(layer.triangles[e][k])].x() / 3;
        if (cx < 3 || cx > 7) continue;
        const double j = std::hypot(layer.currentDensity[e][0], layer.currentDensity[e][1]);
        lowest = std::min(lowest, j);
        highest = std::max(highest, j);
    }
    near(lowest, 1.0 / (1.0 * kThickness), 1e-3, "strip current density (lowest mid-strip), A/mm^2");
    near(highest, 1.0 / (1.0 * kThickness), 1e-3, "strip current density (highest mid-strip), A/mm^2");
    near(result->powerLoss, 5.0 - result->terminals[1].voltage, 1e-6, "strip power is drop x current, W");
}

void distributedLoad() {
    // The same strip, its load drawn evenly from its 0.15 mm pad: the current tapers to nothing
    // across the pad, whose average voltage is a third of the way along that taper further down.
    kiems::dc::Problem problem;
    problem.layers.push_back({"F.Cu", kThickness, 0, {rectangle(0, 0, 10, 1)}});
    problem.terminals.push_back(terminal("source", Kind::Source, 0, {rectangle(0, 0, 0.15, 1)}, 5.0));
    problem.terminals.push_back(terminal("load", Kind::Load, 0, {rectangle(9.85, 0, 10, 1)}, 1.0));
    problem.options.equipotentialLoadPads = false;
    problem.options.maxElementMm = 0.1;
    const auto result = kiems::dc::solve(problem);
    check(result.has_value(), "distributed load solves");
    if (!result) return;
    near(5.0 - result->terminals[1].voltage, (9.7 + 0.15 / 3) * kSheet, 3e-3, "distributed load's average drop, V");
}

void disc() {
    // Spreading from a 1 mm disc to a ring at 10 mm: R = rho ln(b/a) / (2 pi t).
    kiems::dc::Problem problem;
    problem.layers.push_back({"F.Cu", kThickness, 0, {circle(0, 0, 10.5, 256)}});
    problem.terminals.push_back(terminal("source", Kind::Source, 0, {circle(0, 0, 1, 256)}, 1.0));
    problem.terminals.push_back(
        terminal("load", Kind::Load, 0, {circle(0, 0, 10.5, 256), circle(0, 0, 10, 256, true)}, 1.0));
    problem.options.maxElementMm = 0.5;
    const auto result = kiems::dc::solve(problem);
    check(result.has_value(), "disc solves" + (result ? std::string() : ": " + result.error()));
    if (!result) return;
    near(1.0 - result->terminals[1].voltage, kSheet * std::log(10.0) / (2 * std::numbers::pi), 5e-3,
         "disc spreading drop, V");
}

void via() {
    // Two 5 x 1 mm strips on layers 1.51 mm apart, overlapping at one 0.3 mm via, 1 A end to end.
    kiems::dc::Problem problem;
    problem.layers.push_back({"F.Cu", kThickness, 0, {rectangle(0, 0, 5.5, 1)}});
    problem.layers.push_back({"B.Cu", kThickness, -1.51, {rectangle(4.5, 0, 10, 1)}});
    problem.barrels.push_back({"via", 5.0, 0.5, 0.3, 0.6, 0, 1});
    problem.terminals.push_back(terminal("source", Kind::Source, 0, {rectangle(0, 0, 0.15, 1)}, 5.0));
    problem.terminals.push_back(terminal("load", Kind::Load, 1, {rectangle(9.85, 0, 10, 1)}, 1.0));
    const auto result = kiems::dc::solve(problem);
    check(result.has_value(), "via solves");
    if (!result) return;
    const double barrel = kiems::dc::barrelResistance(kRho, 1.51, 0.3, 0.025);
    near(barrel, kRho * 1.51e-3 / (std::numbers::pi * 0.325e-3 * 0.025e-3), 1e-12, "barrel resistance formula");
    near(result->barrels[0].current, 1.0, 1e-6, "via carries the whole current, A");
    near(result->barrels[0].resistance, barrel, 1e-12, "via resistance, ohms");
    // The strips' own drop (the ring shortens each by about its radius) plus the barrel's.
    const double drop = 5.0 - result->terminals[1].voltage;
    check(drop > barrel + 2 * 4.5 * kSheet && drop < barrel + 2 * 5.4 * kSheet,
          "via chain drop " + std::to_string(drop) + " V is the strips' plus the barrel's");
}

void islands() {
    // A second piece of copper, not joined to the first, with a load on it.
    kiems::dc::Problem problem;
    problem.layers.push_back({"F.Cu", kThickness, 0, {rectangle(0, 0, 10, 1), rectangle(0, 3, 10, 4)}});
    problem.terminals.push_back(terminal("source", Kind::Source, 0, {rectangle(0, 0, 0.15, 1)}, 5.0));
    problem.terminals.push_back(terminal("load", Kind::Load, 0, {rectangle(9.85, 0, 10, 1)}, 1.0));
    problem.terminals.push_back(terminal("stranded", Kind::Load, 0, {rectangle(9.85, 3, 10, 4)}, 1.0));
    problem.terminals.push_back(terminal("nowhere", Kind::Load, 0, {rectangle(20, 20, 21, 21)}, 1.0));
    const auto result = kiems::dc::solve(problem);
    check(result.has_value(), "islands solve");
    if (!result) return;
    check(result->terminals[1].connected && !result->terminals[2].connected, "the stranded load is reported");
    check(std::isnan(result->terminals[2].voltage), "and has no voltage");
    check(result->warnings.size() == 2, "a warning each for the stranded and off-copper loads");
    near(result->terminals[0].current, 1.0, 1e-6, "only the connected load's current flows, A");

    kiems::dc::Problem noSource = problem;
    noSource.terminals.erase(noSource.terminals.begin());
    check(!kiems::dc::solve(noSource).has_value(), "no source is an error");
}

} // namespace

int main() {
    strip();
    distributedLoad();
    disc();
    via();
    islands();
    std::printf("%s\n", failures == 0 ? "All DC analysis checks passed." : "Some DC analysis checks FAILED.");
    return failures == 0 ? 0 : 1;
}
