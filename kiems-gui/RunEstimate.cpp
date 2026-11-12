#include "RunEstimate.hpp"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace kiems_gui {

namespace {

/// The stepping rate is measured over this much of the recent run.
constexpr double kRateWindowSeconds = 15;
/// The energy decay is fitted over the samples in the latter part of the steps run so far, once
/// it's past kFitFromDB. It falls fast while the pulse leaves, then slowly as the board rings down,
/// so a fit reaching back to the fast part would expect the run to end far too soon.
constexpr double kFitFromFraction = 0.5;
constexpr double kFitFromDB = 3;
/// Fewer samples than this in the fit leave the port expected to run to its step limit.
constexpr std::size_t kFitSamples = 4;

} // namespace

void RunEstimator::start(double now, double previousSeconds) {
    *this = RunEstimator();
    _started = now;
    _previous = previousSeconds;
    update(now);
}

bool RunEstimator::feed(const QString& line, double now) {
    // The in-process engine's progress line, then the one every engine prints (the CPU worker's
    // carries the decay at its end).
    static const QRegularExpression fdtdRun(
        QStringLiteral(R"(FDTD run: step (\d+)/(\d+).*energy decay ([\d.]+)/([\d.]+)dB)"));
    static const QRegularExpression timestep(QStringLiteral(R"(timestep (\d+)/(\d+)(?:.*\(-([\d.]+)dB\))?)"));

    if (line.contains(QLatin1String("Simulating with excitation on port #"))) {
        ++_announcedPorts;
        return false;
    }
    if (line.contains(QLatin1String("Running simulation"))) {
        _simulatingSince = now;
        _portSetupFrom = now;
        return false;
    }
    if (line.contains(QLatin1String("[Copper] Setup: starting"))) {
        // The next port's setup: the one before it is done. Its setup time runs from that port's
        // last steps, as rebuilding the port's geometry comes before this line.
        if (!_samples.empty()) {
            _finishedSteps.push_back(_samples.back().step);
            _lastRate = rate();
            _portSetupFrom = _samples.back().time;
            _samples.clear();
        } else if (_portSetupFrom < 0) {
            _portSetupFrom = now;
        }
        return false;
    }
    if (line.contains(QLatin1String("Processing all data from simulation"))) {
        _postprocessing = true;
        update(now);
        return true;
    }
    if (const auto match = fdtdRun.match(line); match.hasMatch()) {
        addSample(now, match.captured(1).toDouble(), match.captured(2).toDouble(), match.captured(3).toDouble(),
                  match.captured(4).toDouble());
        return true;
    }
    if (const auto match = timestep.match(line); match.hasMatch()) {
        addSample(now, match.captured(1).toDouble(), match.captured(2).toDouble(),
                  match.capturedLength(3) > 0 ? match.captured(3).toDouble() : -1, -1);
        return true;
    }
    return false;
}

void RunEstimator::addSample(double now, double step, double total, double decay, double target) {
    if (!_samples.empty() && step < _samples.back().step) {
        // The steps started again without a setup line (a worker's run): the next port.
        _finishedSteps.push_back(_samples.back().step);
        _lastRate = rate();
        _portSetupFrom = _samples.back().time;
        _samples.clear();
    }
    if (_samples.empty()) {
        ++_port;
        if (_portSetupFrom >= 0) _setupSeconds.push_back(now - _portSetupFrom);
    } else if (step == _samples.back().step) {
        // The same tick reported twice (the in-process engine prints two lines per tick).
        if (decay >= 0) _samples.back().decay = decay;
        if (target > 0) _target = target;
        update(now);
        return;
    }
    _samples.push_back({now, step, decay});
    if (total > 0) _total = total;
    if (target > 0) _target = target;
    update(now);
}

double RunEstimator::rate() const {
    if (_samples.size() < 2) return _lastRate;
    const Sample& last = _samples.back();
    const auto first = std::find_if(_samples.begin(), _samples.end() - 1,
                                    [&](const Sample& s) { return s.time >= last.time - kRateWindowSeconds; });
    const double seconds = last.time - first->time, steps = last.step - first->step;
    return seconds > 0.5 && steps > 0 ? steps / seconds : _lastRate;
}

double RunEstimator::predictedEndStep(bool* decayLimited) const {
    *decayLimited = false;
    const Sample& last = _samples.back();
    if (last.decay >= _target) {
        *decayLimited = true;
        return last.step;
    }
    // A straight line through the recent decay, in dB against steps.
    std::vector<const Sample*> fit;
    for (auto it = _samples.rbegin(); it != _samples.rend() && it->step >= last.step * kFitFromFraction; ++it) {
        if (it->decay >= kFitFromDB) fit.push_back(&*it);
    }
    if (fit.size() < kFitSamples) return _total;
    double meanStep = 0, meanDecay = 0;
    for (const Sample* s : fit) {
        meanStep += s->step;
        meanDecay += s->decay;
    }
    meanStep /= static_cast<double>(fit.size());
    meanDecay /= static_cast<double>(fit.size());
    double covariance = 0, variance = 0;
    for (const Sample* s : fit) {
        covariance += (s->step - meanStep) * (s->decay - meanDecay);
        variance += (s->step - meanStep) * (s->step - meanStep);
    }
    if (!(variance > 0) || !(covariance > 0)) return _total;
    const double slope = covariance / variance; // dB per step
    const double atLast = meanDecay + slope * (last.step - meanStep);
    const double end = last.step + std::max(0.0, _target - atLast) / slope;
    if (_total > 0 && end >= _total) return _total;
    *decayLimited = true;
    return end;
}

void RunEstimator::update(double now) {
    _estimateAt = now;
    _decayLimited = false;
    if (_postprocessing) {
        _estimate = 0;
        return;
    }
    if (_samples.empty() && _port < 0) {
        _estimate = _previous > 0 ? std::max(0.0, _previous - (now - _started)) : -1;
        return;
    }
    const double stepsPerSecond = rate();
    if (!(stepsPerSecond > 0) || _samples.empty()) {
        _estimate = -1;
        return;
    }
    const double end = predictedEndStep(&_decayLimited);
    double seconds = (end - _samples.back().step) / stepsPerSecond;
    // The ports still to run: as many steps as the ports so far took, and their setup time.
    const int ports = std::max(_announcedPorts, _port + 1);
    const int left = ports - (_port + 1);
    if (left > 0) {
        double steps = end;
        if (!_finishedSteps.empty()) {
            steps = 0;
            for (double s : _finishedSteps) steps += s;
            steps /= static_cast<double>(_finishedSteps.size());
        }
        double setup = 0;
        for (double s : _setupSeconds) setup += s;
        if (!_setupSeconds.empty()) setup /= static_cast<double>(_setupSeconds.size());
        seconds += left * (steps / stepsPerSecond + setup);
    }
    _estimate = seconds;
}

RunTiming RunEstimator::timing(double now) const {
    RunTiming timing;
    timing.elapsed = std::max(0.0, now - _started);
    timing.port = _port + 1;
    timing.ports = std::max(_announcedPorts, timing.port);
    timing.decayLimited = _decayLimited;
    if (_estimate >= 0) timing.remaining = std::max(0.0, _estimate - (now - _estimateAt));
    if (timing.remaining >= 0 && _simulatingSince >= 0 && _port >= 0) {
        const double spent = now - _simulatingSince;
        timing.simulatingFraction = spent + timing.remaining > 0 ? spent / (spent + timing.remaining) : 1;
    }
    return timing;
}

QString formatDuration(double seconds) {
    const long long total = std::max(0LL, std::llround(seconds));
    if (total < 60) return QString("%1 s").arg(total);
    const long long hours = total / 3600, minutes = total % 3600 / 60, rest = total % 60;
    if (hours > 0) return QString("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QChar('0')).arg(rest, 2, 10, QChar('0'));
    return QString("%1:%2").arg(minutes).arg(rest, 2, 10, QChar('0'));
}

} // namespace kiems_gui
