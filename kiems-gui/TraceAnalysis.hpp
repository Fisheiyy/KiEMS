// The Trace page's numbers for one excited port's run (see libkiems/kiems/trace_profile.hpp for the
// method): the impedance a step meets along the driven net (TDR of the port's own incident and
// reflected waves), the step response and ringing at every trace probe the run recorded, and the
// simulated nets' tracks with their distance from the driver along the copper.
//
// It reads ems/simulation/<simulation>/<port>/: ports.txt, the port's port_ut_/port_it_ waveforms,
// trace_probes.txt and the trace_ut_<n> waveforms. A run from before trace probes still gets its
// impedance profile, placed along the trace at an assumed velocity.
#pragma once

#include "BoardModel.hpp"

#include "kiems/trace_profile.hpp"

#include <QString>

#include <memory>
#include <utility>
#include <vector>

namespace kiems_gui {

/// One of a run's ports, from its ports.txt.
struct TracePort {
    int index = -1;
    QString name; // e.g. "U1 pin 19 (/U1D+)"
    QString net;
    QString footprint, pin; // empty for a port that isn't on a pad
    double impedance = 50;
    bool excited = false;
};

/// The ports a simulation's runs list; empty if it hasn't been run.
std::vector<TracePort> tracePorts(const QString& simulationDirectory);
/// The excited ports with a run directory, in index order.
std::vector<int> excitedRuns(const QString& simulationDirectory);

struct TraceProbe {
    QString net;
    double xMm = 0, yMm = 0; // board millimetres (libkicad's frame)
    int layer = 0;
    /// Along the driven net from the driver, mm; NaN on another net, or copper it can't reach.
    double distanceMm = kiems::trace::kNaN;
    std::size_t track = kiems::trace::Routing::npos; // in TraceAnalysis::tracks
    /// Whether it measures against a plane: it settles where the driven net's other probes do. One
    /// whose track runs over no copper on its plane's layer (a trace returning through copper
    /// beside it, say) measures nothing meaningful, and is left out.
    bool referenced = true;
    std::vector<double> step; // the step response, at TraceAnalysis::times
    kiems::trace::Ringing ringing;
};

struct TraceTrack {
    kiems::trace::Routing::Track track;
    QString net;
    bool driven = false; // on the driver's net
    double startDistance = kiems::trace::kNaN, endDistance = kiems::trace::kNaN;
};

struct TraceAnalysis {
    QString error;
    TracePort driver;
    double riseTime = 0, fMax = 0;
    double targetImpedance = 0; // the driver's port impedance: what the trace should be
    std::vector<double> times;  // seconds, for every step response
    std::vector<TraceProbe> probes;
    std::vector<TraceTrack> tracks;
    /// The impedance profile: at each round-trip time, the impedance seen and where on the driven
    /// net the step had reached then.
    std::vector<double> tdrTime, tdrImpedance, tdrDistance;
    double netLength = 0;       // the driven net's farthest copper from the driver, mm
    bool delayMeasured = false; // distances come from the probes' arrival times, not an assumed velocity
    int unreferencedProbes = 0; // on the driven net, with no plane under them (see TraceProbe::referenced)
    /// Where the plots look by default: from just before the step to a few times its last arrival.
    double windowEnd = 0;
    double velocity = 0;        // mm/s: the mean along the trace
    double recordSeconds = 0;   // how long the run ran
    double timestepSeconds = 0;
    /// How long the excitation's Gaussian pulse lasts (Copper's 2 x 9 / (2 pi fc)): a run shorter
    /// than this stopped before its excitation had played, and its results are mostly noise.
    double pulseSeconds = 0;
    /// How far along the driven net the impedance profile reaches: a step has to get there and back
    /// within the run.
    double profileReachMm = 0;
    std::vector<double> driverStep; // the driver port's own voltage, for a unit incident step
    /// Every other port on the driven net: its voltage for a unit incident step at the driver.
    std::vector<std::pair<QString, std::vector<double>>> portSteps;

    bool ok() const { return error.isEmpty(); }
    /// The profile's impedance where the step had reached `distanceMm`; NaN outside it.
    double impedanceAt(double distanceMm) const;
};

/// Analyses the run of `excitedPort` in `simulationDirectory` on `board`. `fMax` is the top of the
/// simulation's sweep; `riseTime` the step's, raised to what `fMax` can resolve; `pulseSeconds` how
/// long its excitation pulse plays (kiems::Frequency::pulseLength()), 0 when unknown.
std::shared_ptr<const TraceAnalysis> analyseTrace(const BoardData& board, const QString& simulationDirectory,
                                                  int excitedPort, double fMax, double riseTime, double pulseSeconds = 0);

} // namespace kiems_gui
