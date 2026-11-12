// Signal integrity along a trace, from a simulation's results: the impedance a step meets as it
// travels down the line, and how much each point along it rings once the step has passed -- what's
// needed to tune a trace's width, stubs and transitions until it's matched and quiet.
//
//  - Impedance: time-domain reflectometry on the driven port's S11. The step response of S11
//    (for a step with Gaussian edges of a chosen rise time) is the reflection coefficient rho(t)
//    seen at round-trip time t, so Z(t) = Z0 (1 + rho) / (1 - rho). Each round-trip time is placed
//    on the trace by the one-way delay the trace probes measured (DelayMap), so the profile is
//    plotted against distance along the copper. Its resolution is the step's: about the distance
//    the rise time covers, there and back.
//  - Ringing: the FDTD run records the voltage between the track and its plane at points along it
//    (SimulationConfig::traceProbes()). Each one's transfer from the incident wave at the driven
//    port gives its step response; its overshoot, ringback and settling are the ringing there.
//
// Everything here is plain numerics on vectors, so it can be checked against cases with exact
// answers (tests/libkiems/trace_profile_tests.cpp).
#pragma once

#include <complex>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace kiems::trace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// A sampled signal: each value at its time (seconds), not necessarily uniformly spaced.
struct Series {
    std::vector<double> time;
    std::vector<double> value;
};

/// The fastest 10-90% rise time a step can have and still be carried by data that stops at
/// `fMax`: its spectrum is down to 1% there.
double minimumRiseTime(double fMax);

/// The response, at `times` (seconds, ascending from 0), of a channel with transfer function
/// `transfer` at `frequencyHz` (ascending, nearly uniform, starting near DC) to a unit step with
/// Gaussian edges of 10-90% rise time `riseTime`, centred on t = 0 -- so a step passed straight
/// through crosses 0.5 at t = 0. Below the first frequency the transfer is taken as the real part
/// of its first value. NaN entries in `transfer` count as zero.
std::vector<double> stepResponse(const std::vector<double>& frequencyHz,
                                 const std::vector<std::complex<double>>& transfer, double riseTime,
                                 const std::vector<double>& times);

/// The spectrum of `series` at `frequencyHz`: its samples integrated by the trapezoid rule.
std::vector<std::complex<double>> spectrum(const Series& series, const std::vector<double>& frequencyHz);

/// output / input, frequency by frequency; NaN where `input` is under `floor` of its largest magnitude.
std::vector<std::complex<double>> spectrumRatio(const std::vector<std::complex<double>>& output,
                                                const std::vector<std::complex<double>>& input, double floor = 1e-3);

/// The transfer function from `input` to `output` at `frequencyHz`: the ratio of their spectra
/// (each integrated over its own samples). NaN where `input` carries under `floor` of its largest
/// spectral magnitude over `frequencyHz`.
std::vector<std::complex<double>> transferFunction(const Series& input, const Series& output,
                                                   const std::vector<double>& frequencyHz, double floor = 1e-3);

/// Frequencies 0, df, 2 df, ... up to `fMax`, with df fine enough for a record `duration` long.
std::vector<double> frequencyGrid(double duration, double fMax);

/// Z0 (1 + rho) / (1 - rho), clamped to a finite range where rho approaches +/-1.
double impedanceFromReflection(double rho, double z0);

/// How one point's step response settles.
struct Ringing {
    double finalValue = kNaN; // where it settles: the mean over the last fifth of the record
    double arrival = kNaN;    // when it first crosses half the final value (s)
    double overshoot = kNaN;  // the highest it goes after arriving, above final, as a fraction of final
    double ringback = kNaN;   // the lowest it then dips below final, as a fraction of final
    double deviation = kNaN;  // the most it strays from final once the edge has passed (arrival +
                              // rise time), as a fraction of final: the ringing figure
    double settling = kNaN;   // from arriving until it stays within 5 % of final (s)
};

/// Measures `step` (a step response at `times`) for a step of rise time `riseTime`. Fields stay NaN
/// when it never settles anywhere (a final value under a tenth of its largest) or never arrives.
Ringing measureRinging(const std::vector<double>& times, const std::vector<double>& step, double riseTime);

/// A net's routed copper as a graph, for distances along it.
class Routing {
public:
    struct Track {
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0; // mm
        double width = 0;                      // mm
        int layer = 0;
        std::string uuid;
    };
    /// A plated hole that joins every layer's copper within `radius` of it.
    struct Via {
        double x = 0, y = 0, radius = 0;
    };

    Routing(std::vector<Track> tracks, std::vector<Via> vias);

    /// Measures every distance from (x, y) on `layer` (-1 for any): from the track ends within
    /// `reach` of it, or the nearest track end if there are none. Returns false if there's no track.
    bool measureFrom(double x, double y, int layer, double reach);
    /// The distance along the copper (mm) to (x, y) on track `track`; NaN if unreached.
    double distanceOn(std::size_t track, double x, double y) const;
    /// The track nearest (x, y) on `layer` (-1 for any), preferring one whose uuid is `uuid`; npos
    /// if there are none within `tolerance` mm.
    std::size_t trackAt(double x, double y, int layer, const std::string& uuid, double tolerance) const;
    /// The distance to each end of each track; NaN if unreached.
    double startDistance(std::size_t track) const { return _startDistance[track]; }
    double endDistance(std::size_t track) const { return _endDistance[track]; }
    const std::vector<Track>& tracks() const { return _tracks; }
    /// The farthest distance reached.
    double reach() const;

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    std::vector<Track> _tracks;
    std::vector<Via> _vias;
    std::vector<double> _startDistance, _endDistance;
};

/// One-way delay against distance along the trace, from where a step arrives at each probe.
class DelayMap {
public:
    /// From (distance mm, delay s) pairs; points that would make delay fall with distance are
    /// dropped. Empty when fewer than two points are left.
    static DelayMap fromSamples(std::vector<std::pair<double, double>> samples);
    /// A uniform velocity, in mm per second.
    static DelayMap uniform(double millimetresPerSecond);

    bool empty() const { return _distance.empty() && !(_velocity > 0); }
    /// The distance a step reaches after one-way `delay`: interpolated between samples, and at the
    /// mean velocity beyond them.
    double distanceAt(double delay) const;
    /// The mean velocity, mm/s.
    double velocity() const { return _velocity; }

private:
    std::vector<double> _distance, _delay;
    double _velocity = 0;
};

} // namespace kiems::trace
