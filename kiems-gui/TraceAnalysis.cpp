#include "TraceAnalysis.hpp"

#include "kiems/net_name.hpp"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kiems_gui {

namespace {

using kiems::trace::kNaN;
using kiems::trace::Series;

/// An openEMS-format probe file: '%' comment lines, then "time<TAB>value" rows.
Series readProbe(const QString& path) {
    Series series;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return series;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        if (line.isEmpty() || line.startsWith('%')) continue;
        const QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() < 2) continue;
        bool okTime = false, okValue = false;
        const double t = parts[0].toDouble(&okTime), v = parts[1].toDouble(&okValue);
        if (okTime && okValue) {
            series.time.push_back(t);
            series.value.push_back(v);
        }
    }
    return series;
}

QString runDirectory(const QString& simulationDirectory, int port) {
    return QDir(simulationDirectory).filePath(QString::number(port));
}

QString displayNet(const std::string& raw) { return QString::fromStdString(kiems::NetName(raw).unescaped()); }

} // namespace

std::vector<TracePort> tracePorts(const QString& simulationDirectory) {
    std::vector<TracePort> ports;
    const std::vector<int> runs = excitedRuns(simulationDirectory);
    if (runs.empty()) return ports;
    QFile file(QDir(runDirectory(simulationDirectory, runs.front())).filePath("ports.txt"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return ports;
    static const QRegularExpression pad(QStringLiteral(R"(^(\S+) pin (\S+) \()"));
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (line.startsWith('#')) continue;
        const QStringList parts = line.split('\t');
        bool ok = false;
        TracePort port;
        port.index = parts.value(0).toInt(&ok);
        if (!ok || parts.size() < 3) continue;
        port.name = parts[1];
        for (const QString& part : parts) {
            if (part.startsWith("net=")) port.net = part.mid(4);
            else if (part.startsWith("Z=")) port.impedance = part.mid(2).toDouble();
        }
        port.excited = parts.value(3).split(',').contains("excited");
        if (const auto match = pad.match(port.name); match.hasMatch()) {
            port.footprint = match.captured(1);
            port.pin = match.captured(2);
        }
        ports.push_back(port);
    }
    // ports.txt marks only its own run's port excited: mark every port that has a run.
    for (TracePort& port : ports) port.excited = std::find(runs.begin(), runs.end(), port.index) != runs.end();
    return ports;
}

std::vector<int> excitedRuns(const QString& simulationDirectory) {
    std::vector<int> runs;
    const QDir directory(simulationDirectory);
    for (const QString& name : directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok = false;
        const int port = name.toInt(&ok);
        if (ok && QFile::exists(directory.filePath(name + "/ports.txt"))) runs.push_back(port);
    }
    std::sort(runs.begin(), runs.end());
    return runs;
}

double TraceAnalysis::impedanceAt(double distanceMm) const {
    if (!std::isfinite(distanceMm) || tdrDistance.empty()) return kNaN;
    // The first time the step had reached that far.
    for (std::size_t i = 1; i < tdrDistance.size(); ++i) {
        if (tdrDistance[i] >= distanceMm) {
            const double span = tdrDistance[i] - tdrDistance[i - 1];
            const double t = span > 0 ? (distanceMm - tdrDistance[i - 1]) / span : 0;
            return tdrImpedance[i - 1] + t * (tdrImpedance[i] - tdrImpedance[i - 1]);
        }
    }
    return kNaN;
}

std::shared_ptr<const TraceAnalysis> analyseTrace(const BoardData& board, const QString& simulationDirectory,
                                                  int excitedPort, double fMax, double riseTime, double fMin) {
    auto analysis = std::make_shared<TraceAnalysis>();
    const QString run = runDirectory(simulationDirectory, excitedPort);
    const std::vector<TracePort> ports = tracePorts(simulationDirectory);
    const auto driver = std::find_if(ports.begin(), ports.end(), [&](const TracePort& p) { return p.index == excitedPort; });
    if (driver == ports.end()) {
        analysis->error = QObject::tr("There's no run of port %1.").arg(excitedPort);
        return analysis;
    }
    analysis->driver = *driver;
    analysis->targetImpedance = driver->impedance;
    analysis->fMax = fMax;
    analysis->riseTime = std::max(riseTime, kiems::trace::minimumRiseTime(fMax));

    // The incident and reflected waves at the driver: u_inc = (u + Z i) / 2, u_ref = (u - Z i) / 2,
    // the current (sampled half a step later) brought onto the voltage's times.
    const Series u = readProbe(QDir(run).filePath(QString("port_ut_%1").arg(excitedPort)));
    const Series i = readProbe(QDir(run).filePath(QString("port_it_%1").arg(excitedPort)));
    if (u.time.size() < 16 || i.time.size() < 16) {
        analysis->error = QObject::tr("The run of %1 has no port waveforms to analyse.").arg(driver->name);
        return analysis;
    }
    const double z0 = driver->impedance;
    Series incident, reflected;
    for (std::size_t n = 0; n < u.time.size() && n < i.time.size(); ++n) {
        const double current = n > 0 ? 0.5 * (i.value[n - 1] + i.value[n]) : i.value[n];
        incident.time.push_back(u.time[n]);
        reflected.time.push_back(u.time[n]);
        incident.value.push_back(0.5 * (u.value[n] + z0 * current));
        reflected.value.push_back(0.5 * (u.value[n] - z0 * current));
    }
    const double duration = u.time.back();
    analysis->recordSeconds = duration;
    analysis->timestepSeconds = u.time.size() > 1 ? u.time[1] - u.time[0] : 0;
    if (const double fc = (fMax - fMin) / 2; fc > 0) analysis->pulseSeconds = 2 * 9 / (2 * std::numbers::pi * fc);
    const std::vector<double> grid = kiems::trace::frequencyGrid(duration, fMax);
    // From a little before the step (its edge is centred on t = 0) to the end of the run.
    const double step = std::min(analysis->riseTime / 6, 1 / (10 * fMax));
    for (double t = -1.5 * analysis->riseTime; t <= duration; t += step) analysis->times.push_back(t);
    const auto incidentSpectrum = kiems::trace::spectrum(incident, grid);
    const auto stepOf = [&](const Series& output) {
        return kiems::trace::stepResponse(grid, kiems::trace::spectrumRatio(kiems::trace::spectrum(output, grid), incidentSpectrum),
                                          analysis->riseTime, analysis->times);
    };

    // The simulated nets' copper, and distances along the driven net from the driver's pad.
    QStringList nets;
    QFile probeList(QDir(run).filePath("trace_probes.txt"));
    struct ProbeLine {
        int index;
        TraceProbe probe;
        QString uuid;
    };
    std::vector<ProbeLine> probeLines;
    if (probeList.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&probeList);
        while (!stream.atEnd()) {
            const QString line = stream.readLine();
            if (line.startsWith('#')) continue;
            const QStringList parts = line.split('\t');
            bool ok = false;
            ProbeLine entry{parts.value(0).toInt(&ok), {}, parts.value(6)};
            if (!ok || parts.size() < 5) continue;
            entry.probe.net = parts[1];
            entry.probe.xMm = parts[2].toDouble();
            entry.probe.yMm = parts[3].toDouble();
            entry.probe.layer = parts[4].section("->", 0, 0).toInt();
            if (!nets.contains(entry.probe.net)) nets << entry.probe.net;
            probeLines.push_back(entry);
        }
    }
    if (!nets.contains(driver->net)) nets << driver->net;
    const auto layerIndex = [&](const std::string& name) {
        return static_cast<int>(board.copperLayers.indexOf(QString::fromStdString(name)));
    };
    std::vector<kiems::trace::Routing::Track> driven;
    std::vector<std::size_t> drivenIndex;
    for (const auto& [rawNet, segment] : board.tracks) {
        const QString net = displayNet(rawNet);
        if (!nets.contains(net)) continue;
        TraceTrack track;
        track.track = {segment.startXMm, segment.startYMm, segment.endXMm, segment.endYMm, segment.widthMm,
                       layerIndex(segment.copperLayerName), segment.uuid};
        track.net = net;
        track.driven = net == driver->net;
        if (track.driven) {
            drivenIndex.push_back(analysis->tracks.size());
            driven.push_back(track.track);
        }
        analysis->tracks.push_back(std::move(track));
    }
    std::vector<kiems::trace::Routing::Via> vias;
    for (const auto& hole : board.throughHoles) {
        if (displayNet(hole.netName) != driver->net) continue;
        vias.push_back({hole.xMm, hole.yMm, 0.5 * std::max({hole.padWidthMm, hole.padHeightMm, hole.drillWidthMm})});
    }
    kiems::trace::Routing routing(driven, vias);
    bool measured = false;
    if (const libkicad::PadPosition* pad = board.pad(driver->footprint, driver->pin)) {
        const int layer = layerIndex(pad->copperLayerName);
        measured = routing.measureFrom(pad->xMm, pad->yMm, layer, 0.5 * std::max(pad->widthMm, pad->heightMm) + 0.05);
    }
    if (measured) {
        analysis->netLength = routing.reach();
        for (std::size_t k = 0; k < driven.size(); ++k) {
            analysis->tracks[drivenIndex[k]].startDistance = routing.startDistance(k);
            analysis->tracks[drivenIndex[k]].endDistance = routing.endDistance(k);
        }
    }

    // Each probe's step response and ringing; on the driven net, where along it.
    std::vector<std::pair<double, double>> arrivals;
    std::vector<std::size_t> drivenProbes;
    for (ProbeLine& entry : probeLines) {
        const Series voltage = readProbe(QDir(run).filePath(QString("trace_ut_%1").arg(entry.index)));
        if (voltage.time.size() < 4) continue;
        TraceProbe& probe = entry.probe;
        probe.step = stepOf(voltage);
        probe.ringing = kiems::trace::measureRinging(analysis->times, probe.step, analysis->riseTime);
        for (std::size_t t = 0; t < analysis->tracks.size(); ++t) {
            const auto& candidate = analysis->tracks[t];
            if (candidate.net != probe.net || candidate.track.uuid != entry.uuid.toStdString()) continue;
            probe.track = t;
            break;
        }
        if (measured && probe.net == driver->net) {
            const std::size_t k = routing.trackAt(probe.xMm, probe.yMm, probe.layer, entry.uuid.toStdString(), 0.5);
            if (k != kiems::trace::Routing::npos) probe.distanceMm = routing.distanceOn(k, probe.xMm, probe.yMm);
            if (std::isfinite(probe.distanceMm)) drivenProbes.push_back(analysis->probes.size());
        }
        analysis->probes.push_back(std::move(probe));
    }
    // Along the driven net the step settles where it does at the driver (no current flows at DC
    // but the load's, through copper); a probe that settles somewhere else isn't measuring against
    // a plane.
    analysis->driverStep = stepOf(u);
    const double settled = kiems::trace::measureRinging(analysis->times, analysis->driverStep, analysis->riseTime).finalValue;
    if (std::isfinite(settled)) {
        for (std::size_t p : drivenProbes) {
            TraceProbe& probe = analysis->probes[p];
            const double final = probe.ringing.finalValue;
            if (std::isfinite(final) && std::abs(final - settled) <= 0.25 * std::abs(settled)) {
                // Only where a clear step arrives does its timing place the trace.
                arrivals.emplace_back(probe.distanceMm, probe.ringing.arrival);
                continue;
            }
            probe.referenced = false;
            probe.ringing = {};
            ++analysis->unreferencedProbes;
        }
    }

    // The other ports on the driven net: their steps, and where along it they are, which places
    // the trace too.
    double lastArrival = 0;
    for (const TracePort& port : ports) {
        if (port.index == excitedPort || port.net != driver->net) continue;
        const Series v = readProbe(QDir(run).filePath(QString("port_ut_%1").arg(port.index)));
        if (v.time.size() < 4) continue;
        analysis->portSteps.emplace_back(port.name, stepOf(v));
        const auto ringing = kiems::trace::measureRinging(analysis->times, analysis->portSteps.back().second, analysis->riseTime);
        if (!std::isfinite(ringing.arrival)) continue;
        lastArrival = std::max(lastArrival, ringing.arrival);
        const libkicad::PadPosition* pad = board.pad(port.footprint, port.pin);
        if (!measured || !pad) continue;
        const std::size_t k = routing.trackAt(pad->xMm, pad->yMm, layerIndex(pad->copperLayerName), {},
                                              0.5 * std::max(pad->widthMm, pad->heightMm) + 0.05);
        if (k != kiems::trace::Routing::npos) arrivals.emplace_back(routing.distanceOn(k, pad->xMm, pad->yMm), ringing.arrival);
    }
    for (const auto& arrival : arrivals) lastArrival = std::max(lastArrival, arrival.second);
    // And the driver's own pad: the TDR's times and the probes' arrivals are both from there.
    if (measured && !arrivals.empty()) arrivals.emplace_back(0.0, 0.0);
    analysis->windowEnd = std::min(analysis->times.back(), 4 * lastArrival + 5 * analysis->riseTime);

    // The impedance profile, placed along the trace by the probes' delays -- or, without them, at
    // the speed of light in a typical board's microstrip (effective permittivity ~3).
    kiems::trace::DelayMap delays = kiems::trace::DelayMap::fromSamples(arrivals);
    analysis->delayMeasured = !delays.empty();
    if (!analysis->delayMeasured) delays = kiems::trace::DelayMap::uniform(299792458e3 / std::sqrt(3.0));
    analysis->velocity = delays.velocity();
    const std::vector<double> rho = stepOf(reflected);
    for (std::size_t n = 0; n < analysis->times.size(); ++n) {
        if (analysis->times[n] < 0) continue;
        analysis->tdrTime.push_back(analysis->times[n]);
        analysis->tdrImpedance.push_back(kiems::trace::impedanceFromReflection(rho[n], z0));
        analysis->tdrDistance.push_back(delays.distanceAt(analysis->times[n] / 2));
        if (std::isfinite(analysis->tdrDistance.back())) {
            analysis->profileReachMm = std::max(analysis->profileReachMm, analysis->tdrDistance.back());
        }
    }
    return analysis;
}

} // namespace kiems_gui
