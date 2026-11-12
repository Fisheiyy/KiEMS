// Checks libkiems/kiems/trace_profile.hpp against cases with exact answers: a step through a delay,
// the TDR of a mismatched line section, a deconvolved probe, ringing measures and routing
// distances. Run by CTest, or directly: it prints each check and exits non-zero if any failed.
#include "kiems/trace_profile.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void near(double actual, double expected, double tolerance, const std::string& what) {
    const bool ok = std::abs(actual - expected) <= tolerance;
    char text[96];
    std::snprintf(text, sizeof text, ": %.6g (expected %.6g)", actual, expected);
    check(ok, what + text);
}

using namespace kiems::trace;
using Complex = std::complex<double>;

std::vector<double> uniformTimes(double end, double step) {
    std::vector<double> t;
    for (double x = 0; x <= end + 1e-18; x += step) t.push_back(x);
    return t;
}

std::vector<double> sweep(double stop, std::size_t count) {
    std::vector<double> f(count);
    for (std::size_t k = 0; k < count; ++k) f[k] = 1e6 + (stop - 1e6) * static_cast<double>(k) / static_cast<double>(count - 1);
    return f;
}

void delayedStep() {
    // A pure delay of 200 ps: the step crosses half way at 200 ps and settles at 1.
    const std::vector<double> f = sweep(10e9, 2001);
    std::vector<Complex> h(f.size());
    for (std::size_t k = 0; k < f.size(); ++k) h[k] = std::polar(1.0, -2 * std::numbers::pi * f[k] * 200e-12);
    const double rise = 130e-12;
    check(rise >= minimumRiseTime(10e9), "a 130 ps step fits in 10 GHz");
    const std::vector<double> t = uniformTimes(1e-9, 2e-12);
    const std::vector<double> s = stepResponse(f, h, rise, t);
    const Ringing r = measureRinging(t, s, rise);
    near(r.arrival, 200e-12, 2e-12, "delayed step arrives at 200 ps");
    near(r.finalValue, 1.0, 0.01, "and settles at 1");
    check(r.overshoot < 0.01 && r.deviation < 0.01, "with no ringing");
    // Its 10-90 % rise is the step's.
    double t10 = 0, t90 = 0;
    for (std::size_t i = 1; i < t.size(); ++i) {
        if (s[i - 1] < 0.1 && s[i] >= 0.1) t10 = t[i];
        if (s[i - 1] < 0.9 && s[i] >= 0.9) t90 = t[i];
    }
    near(t90 - t10, rise, 6e-12, "10-90 % rise time, s");
}

void tdrSection() {
    // 50 ohm port -> 25 mm of 70 ohm line (round trip 2 x 150 ps) -> matched 50 ohm load.
    const double z0 = 50, z1 = 70, tau = 150e-12;
    const std::vector<double> f = sweep(20e9, 4001);
    std::vector<Complex> s11(f.size());
    const double g = (z1 - z0) / (z1 + z0);
    for (std::size_t k = 0; k < f.size(); ++k) {
        const Complex delay2 = std::polar(1.0, -2 * 2 * std::numbers::pi * f[k] * tau);
        s11[k] = (g - g * delay2) / (1.0 - g * g * delay2);
    }
    const double rise = 30e-12;
    const std::vector<double> t = uniformTimes(600e-12, 2e-12);
    const std::vector<double> rho = stepResponse(f, s11, rise, t);
    // Inside the section (round trip 50-250 ps), the line's own impedance; past it, back to 50.
    near(impedanceFromReflection(rho[static_cast<std::size_t>(150e-12 / 2e-12)], z0), z1, 0.5,
         "TDR in the 70 ohm section, ohms");
    near(impedanceFromReflection(rho[static_cast<std::size_t>(500e-12 / 2e-12)], z0), z0, 1.5,
         "TDR past it (with the re-reflections), ohms");
}

void deconvolvedProbe() {
    // A probe seeing the port's incident pulse delayed 120 ps and halved: its step response is a
    // half-height step arriving at 120 ps, whatever the pulse's shape.
    Series incident, probe;
    const double dt = 1e-12;
    for (int n = 0; n < 3000; ++n) {
        const double t = n * dt;
        const auto pulse = [](double x) {
            const double fc = 6e9, t0 = 9 / (2 * std::numbers::pi * fc);
            return std::cos(2 * std::numbers::pi * 6e9 * (x - t0)) *
                   std::exp(-std::pow(2 * std::numbers::pi * fc * x / 3 - 3, 2));
        };
        incident.time.push_back(t);
        incident.value.push_back(pulse(t));
        probe.time.push_back(t);
        probe.value.push_back(0.5 * pulse(t - 120e-12));
    }
    // The probe decimated, as a trace probe is.
    Series coarse;
    for (std::size_t n = 0; n < probe.time.size(); n += 8) {
        coarse.time.push_back(probe.time[n]);
        coarse.value.push_back(probe.value[n]);
    }
    const std::vector<double> f = frequencyGrid(3e-9, 12e9);
    const auto h = transferFunction(incident, coarse, f);
    const double rise = minimumRiseTime(12e9) * 1.5;
    const std::vector<double> t = uniformTimes(2e-9, 4e-12);
    const Ringing r = measureRinging(t, stepResponse(f, h, rise, t), rise);
    near(r.finalValue, 0.5, 0.02, "deconvolved probe settles at half");
    near(r.arrival, 120e-12, 6e-12, "and arrives at 120 ps");
}

void ringingMeasures() {
    // A step overshooting 20 %, then ringing down: 1 + 0.2 e^(-t/tau) cos(w t) after arrival.
    std::vector<double> t = uniformTimes(10e-9, 5e-12), s(t.size());
    for (std::size_t i = 0; i < t.size(); ++i) {
        const double x = t[i] - 1e-9;
        s[i] = x < 0 ? 0 : 1 + 0.2 * std::exp(-x / 1e-9) * std::cos(2 * std::numbers::pi * 1e9 * x);
    }
    const Ringing r = measureRinging(t, s, 50e-12);
    near(r.overshoot, 0.2, 0.005, "overshoot");
    near(r.ringback, 0.2 * std::exp(-0.5), 0.01, "ringback, half a period on");
    near(r.arrival, 1e-9, 5e-12, "arrival");
    // Inside 5 % once 0.2 e^(-t/tau) < 0.05: about 1.39 ns.
    check(r.settling > 1.0e-9 && r.settling < 1.45e-9, "settling " + std::to_string(r.settling * 1e9) + " ns");
}

void routing() {
    // An L on F.Cu (10 mm then 5 mm), a via to B.Cu, 3 mm on, and a 2 mm stub teed off the first leg.
    Routing r({{0, 0, 10, 0, 0.2, 0, "a"}, {10, 0, 10, 5, 0.2, 0, "b"}, {10, 5, 13, 5, 0.2, 1, "c"},
               {4, 0, 4, 2, 0.2, 0, "stub"}},
              {{10, 5, 0.3}});
    check(r.measureFrom(0, 0, 0, 0.5), "routing measured");
    near(r.distanceOn(2, 13, 5), 18, 1e-9, "through the via to the far end, mm");
    near(r.distanceOn(3, 4, 2), 6, 1e-9, "to the stub's end, mm");
    near(r.distanceOn(0, 7, 0), 7, 1e-9, "part way along, mm");
    check(r.trackAt(10, 2.5, 0, "", 0.2) == 1, "the track under a point");
    check(r.trackAt(12, 5, 0, "", 0.2) == Routing::npos, "but not on another layer");
    near(r.reach(), 18, 1e-9, "farthest, mm");
}

void delayMap() {
    const DelayMap map = DelayMap::fromSamples({{10, 60e-12}, {0, 0}, {20, 130e-12}, {15, 50e-12}});
    near(map.distanceAt(30e-12), 5, 1e-9, "delay map interpolates, mm");
    near(map.distanceAt(95e-12), 15, 1e-9, "skipping a sample that runs backwards, mm");
    near(map.distanceAt(130e-12 + 65e-12), 30, 1e-6, "and extrapolates at the mean velocity, mm");
}

} // namespace

int main() {
    delayedStep();
    tdrSection();
    deconvolvedProbe();
    ringingMeasures();
    routing();
    delayMap();
    std::printf("%s\n", failures == 0 ? "All trace profile checks passed." : "Some trace profile checks FAILED.");
    return failures == 0 ? 0 : 1;
}
