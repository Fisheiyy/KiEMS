#include "excitation_postprocess.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>

#include <matplot/matplot.h>
#include <nlohmann/json.hpp>

#include "logging.hpp"
#include "path_utf8.hpp"
#include "plot_figure.hpp"

namespace kiems {

using namespace Cu;

namespace {

// Port names are "<net>@<footprintRef>.<padNumber>" (port_resolution.cpp) -- net names routinely
// contain '/' (KiCad hierarchical net paths, e.g. "/MCU/USB/Upstream/Tx_{2}+"), which would
// otherwise be read as bogus path separators in an output filename.
std::string _sanitizeForFilename(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':') {
            c = '_';
        }
    }
    return name;
}

matplot::figure_handle _newFigure() { return newPlotFigure(); }

void _saveFigure(const matplot::figure_handle& fig, const std::filesystem::path& path, bool transparent) {
    if (transparent) {
        fig->color({0.0F, 1.0F, 1.0F, 1.0F});
    }
    // Matplot++ embeds the name in a gnuplot double-quoted string, where a backslash starts an
    // escape sequence, so native Windows separators (as in "C:\new\temp") would become
    // control characters. Forward slashes work everywhere, and are what macOS and Linux already use.
    fig->save(pathToGenericUtf8(path));
}

} // namespace

double primaryRunDuration(const SimulationConfig& simConfig, const std::map<std::int32_t, double>& runDurations) {
    double longest = 0;
    for (const auto& excitation : simConfig.excitations()) {
        if (!excitation.isMain() || !excitation.drivenPortIndex().has_value()) {
            continue;
        }
        if (const auto it = runDurations.find(*excitation.drivenPortIndex()); it != runDurations.end()) {
            longest = std::max(longest, it->second);
        }
    }
    return longest;
}

double excitationRecordEnd(const ExcitationConfig& excitation, const SimulationConfig& simConfig,
                           const std::map<std::int32_t, double>& runDurations, const Frequency& frequency) {
    const double primary = primaryRunDuration(simConfig, runDurations);
    // Margin past a configured end when no FDTD run length is known, to see its decay/response.
    const double configuredEnd = 1.2 * (excitation.startTime() + excitation.duration());
    if (excitation.isMain() || excitation.durationMode() == ExcitationDurationMode::Continuous) {
        return primary > 0 ? primary : configuredEnd;
    }
    // Limited: the tone's end plus however long this port's own FDTD run took to decay once its
    // own (Gaussian) excitation stopped -- the same structure ringing down from the same port --
    // capped at the primary run's length.
    const auto run = excitation.drivenPortIndex().has_value() ? runDurations.find(*excitation.drivenPortIndex())
                                                               : runDurations.end();
    if (run == runDurations.end()) {
        return primary > 0 ? std::min(primary, configuredEnd) : configuredEnd;
    }
    const double fc = (frequency.stop() - frequency.start()) / 2.0;
    const double pulseLength = fc > 0 ? 9.0 / (std::numbers::pi * fc) : 0.0; // CalcGaussianPulsExcitation's
    const double decay = std::max(0.0, run->second - pulseLength);
    const double end = excitation.startTime() + excitation.duration() + decay;
    return primary > 0 ? std::min(primary, end) : end;
}

ExcitationPostprocessor::ExcitationPostprocessor(const SimulationConfig& simConfig, const Postprocessor& sParams,
                                                   std::vector<double> frequencies, const Frequency& frequency,
                                                   std::map<std::int32_t, double> runDurations)
    : _simConfig(simConfig),
      _sParams(sParams),
      _frequencies(std::move(frequencies)),
      _frequency(frequency),
      _runDurations(std::move(runDurations)) {}

std::map<std::int32_t, double> ExcitationPostprocessor::loadRunDurations(const std::filesystem::path& simulationDir,
                                                                          const SimulationConfig& simConfig,
                                                                          const PortRenumbering* renumbering) {
    std::map<std::int32_t, double> durations;
    for (std::size_t index = 0; index < simConfig.ports().size(); ++index) {
        if (!simConfig.ports()[index].excite()) {
            continue;
        }
        const auto current = static_cast<std::int32_t>(index);
        const std::filesystem::path runDir =
            simulationDir / std::to_string(renumbering ? renumbering->saved(current) : current);
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(runDir, error)) {
            // Every port's voltage probe ("<prefix>port_ut_<n>[suffix]", see Port::_label) spans the
            // whole run, so any one of them gives its length.
            if (!entry.is_regular_file() || entry.path().filename().string().find("port_ut_") == std::string::npos) {
                continue;
            }
            std::ifstream file(entry.path());
            std::string line;
            std::optional<double> lastTime;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '%') {
                    continue;
                }
                std::istringstream iss(line);
                double t = 0;
                if (iss >> t) {
                    lastTime = t;
                }
            }
            if (lastTime.has_value()) {
                durations[static_cast<std::int32_t>(index)] = *lastTime;
                break;
            }
        }
    }
    return durations;
}

double ExcitationPostprocessor::_pickDt() const {
    // 8x oversampling above the highest analysis frequency -- comfortably past Nyquist, giving
    // clean time-domain resolution for the reconstructed waveform.
    const double stopFreq = _frequency.stop();
    return stopFreq > 0 ? 1.0 / (8.0 * stopFreq) : 1e-12;
}

void ExcitationPostprocessor::run() {
    if (_simConfig.excitations().empty()) {
        return;
    }

    _dt = _pickDt();
    double latestEnd = 0;
    for (const auto& excitation : _simConfig.excitations()) {
        latestEnd = std::max(latestEnd, excitationRecordEnd(excitation, _simConfig, _runDurations, _frequency));
    }
    // A response rebuilt from the sweep repeats every 1/df, so nothing past that is new. Without
    // this, an excitation lasting seconds (where nanoseconds were meant), with no FDTD run lengths
    // to go by, sized every port's record at billions of samples.
    if (_frequencies.size() >= 2) {
        const double step = (_frequencies.back() - _frequencies.front()) / static_cast<double>(_frequencies.size() - 1);
        if (step > 0 && latestEnd > 1.0 / step) {
            logWarning() << "The excitations of \"" << _simConfig.name() << "\" would be recorded for " << latestEnd
                         << " s; the sweep's " << step << " Hz spacing resolves only " << 1.0 / step
                         << " s, so they're recorded for that long. Check the excitation durations.";
            latestEnd = 1.0 / step;
        }
    }
    const std::size_t sampleCount = static_cast<std::size_t>(std::ceil(latestEnd / _dt)) + 1;
    const std::size_t portCount = _simConfig.ports().size();
    _responses.assign(portCount, TimeWaveform{_dt, std::vector<double>(sampleCount, 0.0)});

    for (std::size_t outputPort = 0; outputPort < portCount; ++outputPort) {
        // Absorb-only terminations aren't recorded, so there's no transfer to them to apply.
        const PortConfig& output = _simConfig.ports()[outputPort];
        if (!output.probe() && !output.excite() && output.absorbSignal()) {
            continue;
        }
        std::vector<TimeWaveform> contributions;

        for (const auto& excitation : _simConfig.excitations()) {
            if (!excitation.drivenPortIndex().has_value()) {
                continue; // Should never happen post-resolveSimulationPorts(); skip defensively.
            }
            const auto drivenPort = static_cast<std::int32_t>(*excitation.drivenPortIndex());

            const TimeWaveform stimulus =
                excitation.isMain()
                    // amplitude() defaults to 1.0 (the FDTD's own real per-port drive level) when
                    // unset -- a plain, single main excitation never needs to set it explicitly; it
                    // only matters once a second main excitation on the same net (e.g. a
                    // differential pair's other leg) needs a different relative level/sign.
                    ? synthesizeMainStimulus(_frequency, excitation.startTime(), excitation.duration(),
                                              excitation.phaseDegrees(), _dt, sampleCount,
                                              excitation.amplitude().value_or(1.0))
                : excitation.durationMode() == ExcitationDurationMode::Continuous
                    ? synthesizeContinuousTone(*excitation.frequency(), *excitation.amplitude(),
                                                excitation.phaseDegrees(), excitation.startTime(), _dt, sampleCount)
                    : synthesizeToneBurst(*excitation.frequency(), *excitation.amplitude(), excitation.phaseDegrees(),
                                           excitation.startTime(), excitation.duration(), _dt, sampleCount);

            const std::optional<std::vector<std::complex<double>>> transferFn =
                _sParams.getSParam(static_cast<std::int32_t>(outputPort), drivenPort);
            if (!transferFn.has_value()) {
                continue; // No S-parameter data for this port pair (e.g. never excited/measured).
            }

            const std::vector<std::complex<double>> stimulusSpectrum = forwardTransform(stimulus, _frequencies);
            std::vector<std::complex<double>> productSpectrum(_frequencies.size());
            for (std::size_t k = 0; k < _frequencies.size(); ++k) {
                productSpectrum[k] = stimulusSpectrum[k] * (*transferFn)[k];
            }

            TimeWaveform contribution = inverseTransform(_frequencies, productSpectrum, _dt, sampleCount);
            const auto recordedSamples =
                std::min(sampleCount, static_cast<std::size_t>(std::ceil(excitationRecordEnd(excitation, _simConfig, _runDurations, _frequency) / _dt)) + 1);
            std::fill(contribution.samples.begin() + static_cast<std::ptrdiff_t>(recordedSamples),
                      contribution.samples.end(), 0.0);
            contributions.push_back(std::move(contribution));
        }

        if (!contributions.empty()) {
            _responses[outputPort] = superpose(contributions);
        }
    }
}

const TimeWaveform& ExcitationPostprocessor::responseFor(std::int32_t portIndex) const {
    return _responses.at(static_cast<std::size_t>(portIndex));
}

void ExcitationPostprocessor::saveToFile(const std::filesystem::path& outputDir) const {
    const auto& ports = _simConfig.ports();
    for (std::size_t i = 0; i < _responses.size() && i < ports.size(); ++i) {
        if (_responses[i].samples.empty()) {
            continue;
        }
        const std::filesystem::path path =
            outputDir / (_sanitizeForFilename(ports[i].name()) + "_response.csv");
        std::ofstream file(path);
        file << "time_s,voltage_v\n";
        for (std::size_t n = 0; n < _responses[i].samples.size(); ++n) {
            file << (static_cast<double>(n) * _responses[i].dt) << "," << _responses[i].samples[n] << "\n";
        }
        logInfo("Saved excitation response to " + path.string());
    }
}

void ExcitationPostprocessor::renderPlots(const std::filesystem::path& outputDir, bool transparent) const {
    const auto& ports = _simConfig.ports();
    for (std::size_t i = 0; i < _responses.size() && i < ports.size(); ++i) {
        const TimeWaveform& response = _responses[i];
        if (response.samples.empty()) {
            continue;
        }
        std::vector<double> timeNs(response.samples.size());
        for (std::size_t n = 0; n < timeNs.size(); ++n) {
            timeNs[n] = static_cast<double>(n) * response.dt * 1e9;
        }

        auto fig = _newFigure();
        matplot::plot(timeNs, response.samples);
        matplot::xlabel("Time [ns]");
        matplot::ylabel("Voltage [V]");
        matplot::title(ports[i].name());
        matplot::grid(true);
        _saveFigure(fig, outputDir / (_sanitizeForFilename(ports[i].name()) + "_response.png"), transparent);
    }
}

// ---------------------------------------------------------------------------------------------
// Eyes

std::vector<SimulationEye> computeSimulationEyes(const Postprocessor& postprocessor, const SimulationConfig& simConfig,
                                                 const Frequency& frequency,
                                                 const std::map<std::int32_t, double>& runDurations) {
    std::vector<SimulationEye> eyes;
    const auto portCount = static_cast<std::int32_t>(simConfig.ports().size());
    const std::size_t sampleCount = postprocessor.frequencies().size();
    const double bitRate = simConfig.eyeBitRate() > 0 ? simConfig.eyeBitRate() : frequency.stop();

    EyeNoiseOptions noiseOptions;
    noiseOptions.drawCount = simConfig.eyeDrawCount();
    noiseOptions.sharedClock = simConfig.adversarialSharedClock();
    EyeTestConfig test = simConfig.eyeTest();
    // computeEyeDiagram() rejects a non-positive level; its own default beats losing every eye.
    if (!(test.signalLevel > 0) || !std::isfinite(test.signalLevel)) test.signalLevel = 1.0;

    // The channel from `input` to the voltage at `output`, per unit incident voltage at `input`: the
    // S-parameter for an absorbing output, the equivalent probe transfer for a passive one, so both
    // kinds of receiver give eyes on the same scale.
    const auto receivedTransfer = [&](std::int32_t output, std::int32_t input) {
        return simConfig.ports()[static_cast<std::size_t>(output)].absorbSignal()
                   ? postprocessor.getSParam(output, input)
                   : postprocessor.getProbeTransfer(output, input);
    };

    // `inputs` are the eye's own driven ports, never aggressors on it. The received quantity is the
    // voltage at `positiveOutput`, minus `negativeOutput`'s for a differential eye, so an
    // aggressor's transfer to it is S(positiveOutput, a) - S(negativeOutput, a).
    const auto appendEye = [&](std::string name, bool differential, const std::vector<std::complex<double>>& transfer,
                               const std::set<std::int32_t>& inputs, std::int32_t positiveOutput,
                               std::optional<std::int32_t> negativeOutput) {
        std::vector<EyeAggressor> aggressors;
        for (const ExcitationConfig& excitation : simConfig.excitations()) {
            if (excitation.isMain() || !excitation.drivenPortIndex().has_value() ||
                inputs.contains(*excitation.drivenPortIndex()) || !excitation.frequency().has_value() ||
                !excitation.amplitude().has_value()) {
                continue;
            }
            const std::int32_t port = *excitation.drivenPortIndex();
            auto aggressorTransfer = receivedTransfer(positiveOutput, port);
            if (!aggressorTransfer.has_value() || aggressorTransfer->size() != sampleCount) continue;
            if (negativeOutput.has_value()) {
                const auto negative = receivedTransfer(*negativeOutput, port);
                if (!negative.has_value() || negative->size() != aggressorTransfer->size()) continue;
                for (std::size_t f = 0; f < aggressorTransfer->size(); ++f) (*aggressorTransfer)[f] -= (*negative)[f];
            }
            EyeAggressor aggressor;
            aggressor.transferFunction = std::move(*aggressorTransfer);
            aggressor.frequencyHz = *excitation.frequency();
            aggressor.amplitude = *excitation.amplitude();
            aggressor.phaseDegrees = excitation.phaseDegrees();
            aggressor.startTime = excitation.startTime();
            aggressor.continuous = excitation.durationMode() == ExcitationDurationMode::Continuous;
            aggressor.duration = excitation.duration();
            aggressor.ringDown = std::max(0.0, excitationRecordEnd(excitation, simConfig, runDurations, frequency) -
                                                   (excitation.startTime() + excitation.duration()));
            aggressors.push_back(std::move(aggressor));
        }
        auto eye = computeEyeDiagram(postprocessor.frequencies(), transfer, bitRate, aggressors, noiseOptions,
                                     test.signalLevel, test.mask);
        if (!eye.has_value()) return;
        eyes.push_back({std::move(name), differential, aggressors.size(), std::move(*eye)});
    };

    // Mixed-mode first. A pair's four ports are left out of the single-ended eyes below: the useful
    // received quantity is Vp-Vn, not four charts of the pair's individual conductors.
    std::set<std::int32_t> differentialPorts;
    for (const DifferentialPairConfig& pair : simConfig.diffPairs()) {
        if (!pair.correct() || !pair.positiveExcitation().resolvedIndex().has_value() ||
            !pair.negativeExcitation().resolvedIndex().has_value() || !pair.positiveProbe().resolvedIndex().has_value() ||
            !pair.negativeProbe().resolvedIndex().has_value()) {
            continue;
        }
        const std::int32_t sp = *pair.positiveExcitation().resolvedIndex();
        const std::int32_t sn = *pair.negativeExcitation().resolvedIndex();
        const std::int32_t ep = *pair.positiveProbe().resolvedIndex();
        const std::int32_t en = *pair.negativeProbe().resolvedIndex();
        differentialPorts.insert({sp, sn, ep, en});
        const auto sEpSp = receivedTransfer(ep, sp);
        const auto sEpSn = receivedTransfer(ep, sn);
        const auto sEnSp = receivedTransfer(en, sp);
        const auto sEnSn = receivedTransfer(en, sn);
        if (!sEpSp || !sEpSn || !sEnSp || !sEnSn || sEpSp->size() != sampleCount || sEpSn->size() != sampleCount ||
            sEnSp->size() != sampleCount || sEnSn->size() != sampleCount) {
            continue;
        }
        std::vector<std::complex<double>> hdd(sampleCount);
        for (std::size_t f = 0; f < sampleCount; ++f) {
            hdd[f] = 0.5 * ((*sEpSp)[f] - (*sEpSn)[f] - (*sEnSp)[f] + (*sEnSn)[f]);
        }
        appendEye(pair.displayName(), true, hdd, {sp, sn}, ep, en);
    }

    std::set<std::int32_t> mainInputs;
    for (const ExcitationConfig& excitation : simConfig.excitations()) {
        if (excitation.isMain() && excitation.drivenPortIndex().has_value()) mainInputs.insert(*excitation.drivenPortIndex());
    }
    for (const std::int32_t input : mainInputs) {
        if (differentialPorts.contains(input)) continue;
        for (std::int32_t output = 0; output < portCount; ++output) {
            const PortConfig& outputPort = simConfig.ports()[static_cast<std::size_t>(output)];
            // One eye per probed receiver; an absorb-only termination isn't something asked to be
            // looked at (see PortConfig::probe()).
            if (outputPort.excite() || !outputPort.probe() || outputPort.isTraceProbe() ||
                differentialPorts.contains(output)) {
                continue;
            }
            const auto transfer = receivedTransfer(output, input);
            if (!transfer.has_value() || transfer->size() != sampleCount) continue;
            std::string name = "Response at " + outputPort.footprintRef() + " pin " + outputPort.padNumber() +
                               " \xE2\x80\x94 from " + simConfig.ports()[static_cast<std::size_t>(input)].name();
            appendEye(std::move(name), false, *transfer, {input}, output, std::nullopt);
        }
    }
    return eyes;
}

namespace {

/// Seven significant figures: far finer than an eye shows, at any signal level.
double rounded(double value) {
    if (!std::isfinite(value) || value == 0.0) return 0.0;
    const double scale = std::pow(10.0, 6 - std::floor(std::log10(std::fabs(value))));
    return std::round(value * scale) / scale;
}

nlohmann::json roundedArray(const std::vector<double>& values) {
    nlohmann::json array = nlohmann::json::array();
    for (const double value : values) array.push_back(rounded(value));
    return array;
}

nlohmann::json openingJson(const EyeOpening& opening) {
    return {{"height_v", rounded(opening.heightV)}, {"width_ui", rounded(opening.widthUI)},
            {"sampling_ui", rounded(opening.samplingUI)}, {"one_level_v", rounded(opening.oneLevelV)},
            {"zero_level_v", rounded(opening.zeroLevelV)}, {"q_factor", rounded(opening.qFactor)},
            {"jitter_ui", rounded(opening.jitterUI)}};
}

nlohmann::json maskJson(const std::optional<EyeMaskResult>& result) {
    if (!result.has_value()) return nullptr;
    return {{"traces", result->traceCount}, {"keep_out_violations", result->keepOutViolations},
            {"outer_limit_violations", result->outerLimitViolations}, {"passed", result->passed()}};
}

} // namespace

void saveEyeDiagrams(const std::vector<SimulationEye>& eyes, const EyeTestConfig& test,
                     const std::filesystem::path& path) {
    nlohmann::json list = nlohmann::json::array();
    for (const SimulationEye& entry : eyes) {
        const EyeDiagramData& eye = entry.eye;
        nlohmann::json traces = nlohmann::json::array();
        for (const auto& trace : eye.traces) traces.push_back(roundedArray(trace));
        nlohmann::json noisyTraces = nlohmann::json::array();
        for (const auto& trace : eye.noisyTraces) noisyTraces.push_back(roundedArray(trace));
        nlohmann::json noise = nullptr;
        if (eye.noise.has_value()) {
            noise = {{"draws", eye.noise->drawCount},
                     {"replicates", eye.noise->replicateCount},
                     {"lowest", openingJson(eye.noise->lowest)},
                     {"highest", openingJson(eye.noise->highest)}};
        }
        list.push_back({{"name", entry.name},
                        {"differential", entry.differential},
                        {"aggressors", entry.aggressorCount},
                        {"bit_rate", eye.bitRate},
                        {"threshold_v", rounded(eye.thresholdV)},
                        {"time_ui", roundedArray(eye.timeUI)},
                        {"traces", std::move(traces)},
                        {"opening", openingJson(eye.opening)},
                        {"mask", maskJson(eye.maskResult)},
                        {"noisy_traces", std::move(noisyTraces)},
                        {"noisy_opening", eye.noisyOpening ? openingJson(*eye.noisyOpening) : nlohmann::json(nullptr)},
                        {"noisy_mask", maskJson(eye.noisyMaskResult)},
                        {"noise", std::move(noise)}});
    }
    const nlohmann::json document = {{"format", 1}, {"test", test}, {"eyes", std::move(list)}};
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        logWarning() << "Couldn't write " << pathToUtf8(path);
        return;
    }
    out << document.dump();
}

} // namespace kiems
