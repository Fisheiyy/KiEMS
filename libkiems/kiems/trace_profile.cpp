#include "trace_profile.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <queue>

namespace kiems::trace {

namespace {

/// A step with Gaussian edges: its 10-90 % rise time is this many standard deviations
/// (2 x 1.2816, the normal distribution's 90th percentile).
constexpr double kRisePerSigma = 2.5631031310892007;

double normalCdf(double x) { return 0.5 * std::erfc(-x / std::numbers::sqrt2); }

/// The spectrum of `series` at `frequency`: its samples integrated by the trapezoid rule.
std::complex<double> spectrumAt(const Series& series, double frequency) {
    const std::size_t n = std::min(series.time.size(), series.value.size());
    if (n < 2) return {};
    const double omega = 2 * std::numbers::pi * frequency;
    std::complex<double> sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double before = i > 0 ? series.time[i] - series.time[i - 1] : 0;
        const double after = i + 1 < n ? series.time[i + 1] - series.time[i] : 0;
        const double weight = 0.5 * (before + after);
        const double phase = -omega * series.time[i];
        sum += series.value[i] * weight * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return sum;
}

double segmentDistance(double px, double py, double ax, double ay, double bx, double by, double* along) {
    const double dx = bx - ax, dy = by - ay;
    const double length2 = dx * dx + dy * dy;
    double t = length2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / length2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    if (along) *along = t;
    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

} // namespace

double minimumRiseTime(double fMax) {
    if (!(fMax > 0)) return kNaN;
    // The Gaussian edge's spectrum, exp(-2 pi^2 sigma^2 f^2), is 0.01 at fMax.
    const double sigma = std::sqrt(std::log(100.0) / 2) / (std::numbers::pi * fMax);
    return kRisePerSigma * sigma;
}

std::vector<double> stepResponse(const std::vector<double>& frequencyHz,
                                 const std::vector<std::complex<double>>& transfer, double riseTime,
                                 const std::vector<double>& times) {
    std::vector<double> response(times.size(), kNaN);
    const std::size_t n = std::min(frequencyHz.size(), transfer.size());
    if (n == 0 || !(riseTime > 0)) return response;
    // The transfer at DC: its first finite value's real part.
    double dc = 0;
    for (std::size_t k = 0; k < n; ++k) {
        if (std::isfinite(transfer[k].real()) && std::isfinite(transfer[k].imag())) {
            dc = transfer[k].real();
            break;
        }
    }
    const double sigma = riseTime / kRisePerSigma;
    // s(t) = H(0) Phi(t / sigma) + integral over f > 0 of (Xr sin wt + Xi cos wt) / (pi f) df,
    // X = (H(f) - H(0)) G(f) and G the edge's spectrum: a step through H, with no running sum to
    // drift. The integrand stays finite at f -> 0, where X -> 0.
    std::vector<double> f, xr, xi;
    for (std::size_t k = 0; k < n; ++k) {
        if (!(frequencyHz[k] > 0)) continue;
        std::complex<double> h = transfer[k];
        if (!std::isfinite(h.real()) || !std::isfinite(h.imag())) h = 0;
        const double g = std::exp(-2 * std::numbers::pi * std::numbers::pi * sigma * sigma * frequencyHz[k] * frequencyHz[k]);
        const std::complex<double> x = (h - dc) * g;
        f.push_back(frequencyHz[k]);
        xr.push_back(x.real());
        xi.push_back(x.imag());
    }
    for (std::size_t i = 0; i < times.size(); ++i) {
        const double t = times[i];
        double integral = 0, previous = 0;
        for (std::size_t k = 0; k < f.size(); ++k) {
            const double w = 2 * std::numbers::pi * f[k] * t;
            const double value = (xr[k] * std::sin(w) + xi[k] * std::cos(w)) / (std::numbers::pi * f[k]);
            // From DC to the first frequency at the first value: the integrand's limit at 0 is finite.
            integral += k > 0 ? 0.5 * (value + previous) * (f[k] - f[k - 1]) : value * f[k];
            previous = value;
        }
        response[i] = dc * normalCdf(t / sigma) + integral;
    }
    return response;
}

std::vector<std::complex<double>> spectrum(const Series& series, const std::vector<double>& frequencyHz) {
    std::vector<std::complex<double>> result(frequencyHz.size());
    for (std::size_t k = 0; k < frequencyHz.size(); ++k) result[k] = spectrumAt(series, frequencyHz[k]);
    return result;
}

std::vector<std::complex<double>> spectrumRatio(const std::vector<std::complex<double>>& output,
                                                const std::vector<std::complex<double>>& input, double floor) {
    double largest = 0;
    for (const auto& v : input) largest = std::max(largest, std::abs(v));
    std::vector<std::complex<double>> result(std::min(output.size(), input.size()));
    for (std::size_t k = 0; k < result.size(); ++k) {
        result[k] = std::abs(input[k]) > floor * largest ? output[k] / input[k] : std::complex<double>(kNaN, kNaN);
    }
    return result;
}

std::vector<std::complex<double>> transferFunction(const Series& input, const Series& output,
                                                   const std::vector<double>& frequencyHz, double floor) {
    return spectrumRatio(spectrum(output, frequencyHz), spectrum(input, frequencyHz), floor);
}

std::vector<double> frequencyGrid(double duration, double fMax) {
    std::vector<double> grid;
    if (!(duration > 0) || !(fMax > 0)) return grid;
    // Fine enough that the response it rebuilds doesn't wrap round within four records.
    const double step = 1 / (4 * duration);
    for (double f = 0; f <= fMax * (1 + 1e-9); f += step) grid.push_back(f);
    return grid;
}

double impedanceFromReflection(double rho, double z0) {
    if (!std::isfinite(rho)) return kNaN;
    rho = std::clamp(rho, -0.98, 0.98);
    return z0 * (1 + rho) / (1 - rho);
}

Ringing measureRinging(const std::vector<double>& times, const std::vector<double>& step, double riseTime) {
    Ringing r;
    const std::size_t n = std::min(times.size(), step.size());
    if (n < 5) return r;
    const std::size_t tail = n - std::max<std::size_t>(1, n / 5);
    double sum = 0, largest = 0;
    for (std::size_t i = 0; i < n; ++i) largest = std::max(largest, std::abs(step[i]));
    for (std::size_t i = tail; i < n; ++i) sum += step[i];
    const double final = sum / static_cast<double>(n - tail);
    // A response that ends near zero (a neighbouring net's crosstalk, say) settles nowhere to
    // measure its ringing against.
    if (!(std::abs(final) > 0.1 * largest) || !(largest > 0)) return r;
    r.finalValue = final;
    // Normalised to the final value from here on.
    std::size_t arrived = n;
    for (std::size_t i = 0; i < n; ++i) {
        if (step[i] / final >= 0.5) {
            arrived = i;
            break;
        }
    }
    if (arrived == n) return r;
    if (arrived == 0) {
        r.arrival = times[0];
    } else {
        const double a = step[arrived - 1] / final, b = step[arrived] / final;
        const double t = b != a ? (0.5 - a) / (b - a) : 0;
        r.arrival = times[arrived - 1] + t * (times[arrived] - times[arrived - 1]);
    }
    double peak = 1;
    std::size_t peakAt = arrived;
    for (std::size_t i = arrived; i < n; ++i) {
        if (step[i] / final > peak) {
            peak = step[i] / final;
            peakAt = i;
        }
    }
    r.overshoot = peak - 1;
    double dip = 1;
    for (std::size_t i = peakAt; i < n; ++i) dip = std::min(dip, step[i] / final);
    r.ringback = 1 - dip;
    double deviation = 0;
    double lastOutside = r.arrival;
    for (std::size_t i = arrived; i < n; ++i) {
        const double off = std::abs(step[i] / final - 1);
        if (times[i] >= r.arrival + riseTime) deviation = std::max(deviation, off);
        if (off > 0.05) lastOutside = times[i];
    }
    r.deviation = deviation;
    r.settling = lastOutside - r.arrival;
    return r;
}

Routing::Routing(std::vector<Track> tracks, std::vector<Via> vias) : _tracks(std::move(tracks)), _vias(std::move(vias)) {
    _startDistance.assign(_tracks.size(), kNaN);
    _endDistance.assign(_tracks.size(), kNaN);
}

bool Routing::measureFrom(double x, double y, int layer, double reach) {
    const std::size_t count = _tracks.size();
    if (count == 0) return false;
    // Node 2i is track i's start, 2i + 1 its end.
    const auto point = [&](std::size_t node) {
        const Track& t = _tracks[node / 2];
        return node % 2 == 0 ? std::pair(t.x0, t.y0) : std::pair(t.x1, t.y1);
    };
    std::vector<std::vector<std::pair<std::size_t, double>>> edges(2 * count);
    const auto join = [&](std::size_t a, std::size_t b, double length) {
        edges[a].emplace_back(b, length);
        edges[b].emplace_back(a, length);
    };
    for (std::size_t i = 0; i < count; ++i) {
        const Track& t = _tracks[i];
        join(2 * i, 2 * i + 1, std::hypot(t.x1 - t.x0, t.y1 - t.y0));
    }
    for (std::size_t a = 0; a < 2 * count; ++a) {
        const Track& ta = _tracks[a / 2];
        const auto [ax, ay] = point(a);
        for (std::size_t j = 0; j < count; ++j) {
            if (j == a / 2) continue;
            const Track& tj = _tracks[j];
            if (tj.layer != ta.layer) continue;
            double along = 0;
            const double d = segmentDistance(ax, ay, tj.x0, tj.y0, tj.x1, tj.y1, &along);
            if (d > std::max(0.5 * std::max(ta.width, tj.width), 0.005)) continue;
            // Meeting track j at one of its ends, or part way along it (a T).
            const double length = std::hypot(tj.x1 - tj.x0, tj.y1 - tj.y0);
            join(a, 2 * j, along * length + d);
            join(a, 2 * j + 1, (1 - along) * length + d);
        }
    }
    for (const Via& via : _vias) {
        std::vector<std::size_t> near;
        for (std::size_t a = 0; a < 2 * count; ++a) {
            const auto [ax, ay] = point(a);
            if (std::hypot(ax - via.x, ay - via.y) <= via.radius + 0.5 * _tracks[a / 2].width) near.push_back(a);
        }
        for (std::size_t i = 0; i < near.size(); ++i) {
            for (std::size_t j = i + 1; j < near.size(); ++j) {
                const auto [ax, ay] = point(near[i]);
                const auto [bx, by] = point(near[j]);
                join(near[i], near[j], std::hypot(ax - bx, ay - by));
            }
        }
    }

    std::vector<double> distance(2 * count, std::numeric_limits<double>::infinity());
    using Entry = std::pair<double, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    double nearest = std::numeric_limits<double>::infinity();
    std::size_t nearestNode = 0;
    for (std::size_t a = 0; a < 2 * count; ++a) {
        if (layer >= 0 && _tracks[a / 2].layer != layer) continue;
        const auto [ax, ay] = point(a);
        const double d = std::hypot(ax - x, ay - y);
        if (d <= reach) {
            distance[a] = d;
            queue.emplace(d, a);
        }
        if (d < nearest) {
            nearest = d;
            nearestNode = a;
        }
    }
    if (queue.empty()) {
        if (!std::isfinite(nearest)) return false;
        distance[nearestNode] = nearest;
        queue.emplace(nearest, nearestNode);
    }
    while (!queue.empty()) {
        const auto [d, node] = queue.top();
        queue.pop();
        if (d > distance[node]) continue;
        for (const auto& [next, length] : edges[node]) {
            if (d + length < distance[next]) {
                distance[next] = d + length;
                queue.emplace(distance[next], next);
            }
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        _startDistance[i] = std::isfinite(distance[2 * i]) ? distance[2 * i] : kNaN;
        _endDistance[i] = std::isfinite(distance[2 * i + 1]) ? distance[2 * i + 1] : kNaN;
    }
    return true;
}

double Routing::distanceOn(std::size_t track, double x, double y) const {
    if (track >= _tracks.size()) return kNaN;
    const Track& t = _tracks[track];
    double along = 0;
    segmentDistance(x, y, t.x0, t.y0, t.x1, t.y1, &along);
    const double length = std::hypot(t.x1 - t.x0, t.y1 - t.y0);
    const double viaStart = _startDistance[track] + along * length;
    const double viaEnd = _endDistance[track] + (1 - along) * length;
    if (std::isnan(viaStart)) return viaEnd;
    if (std::isnan(viaEnd)) return viaStart;
    return std::min(viaStart, viaEnd);
}

std::size_t Routing::trackAt(double x, double y, int layer, const std::string& uuid, double tolerance) const {
    std::size_t best = npos;
    double bestDistance = tolerance;
    bool bestMatchesUuid = false;
    for (std::size_t i = 0; i < _tracks.size(); ++i) {
        const Track& t = _tracks[i];
        if (layer >= 0 && t.layer != layer) continue;
        const double d = segmentDistance(x, y, t.x0, t.y0, t.x1, t.y1, nullptr);
        const bool matches = !uuid.empty() && t.uuid == uuid;
        if (d > tolerance) continue;
        if ((matches && !bestMatchesUuid) || (matches == bestMatchesUuid && d <= bestDistance)) {
            best = i;
            bestDistance = d;
            bestMatchesUuid = matches;
        }
    }
    return best;
}

double Routing::reach() const {
    double farthest = 0;
    for (std::size_t i = 0; i < _tracks.size(); ++i) {
        for (double d : {_startDistance[i], _endDistance[i]}) {
            if (std::isfinite(d)) farthest = std::max(farthest, d);
        }
    }
    return farthest;
}

DelayMap DelayMap::fromSamples(std::vector<std::pair<double, double>> samples) {
    DelayMap map;
    std::erase_if(samples, [](const auto& s) { return !std::isfinite(s.first) || !std::isfinite(s.second); });
    std::sort(samples.begin(), samples.end());
    for (const auto& [distance, delay] : samples) {
        if (!map._delay.empty() && delay < map._delay.back()) continue;
        map._distance.push_back(distance);
        map._delay.push_back(delay);
    }
    if (map._distance.size() < 2) return {};
    const double span = map._delay.back() - map._delay.front();
    map._velocity = span > 0 ? (map._distance.back() - map._distance.front()) / span : 0;
    if (!(map._velocity > 0)) return {};
    return map;
}

DelayMap DelayMap::uniform(double millimetresPerSecond) {
    DelayMap map;
    map._velocity = millimetresPerSecond;
    return map;
}

double DelayMap::distanceAt(double delay) const {
    if (!std::isfinite(delay)) return kNaN;
    if (_distance.empty()) return _velocity > 0 ? std::max(0.0, delay * _velocity) : kNaN;
    if (delay <= _delay.front()) return std::max(0.0, _distance.front() - (_delay.front() - delay) * _velocity);
    if (delay >= _delay.back()) return _distance.back() + (delay - _delay.back()) * _velocity;
    const auto it = std::upper_bound(_delay.begin(), _delay.end(), delay);
    const std::size_t i = static_cast<std::size_t>(it - _delay.begin());
    const double span = _delay[i] - _delay[i - 1];
    const double t = span > 0 ? (delay - _delay[i - 1]) / span : 0;
    return _distance[i - 1] + t * (_distance[i] - _distance[i - 1]);
}

} // namespace kiems::trace
