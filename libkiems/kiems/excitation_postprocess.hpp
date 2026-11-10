// Synthesizes each of a simulation's excitations' time-domain stimulus, propagates it through the
// already-extracted S-parameter matrix (Postprocessor::getSParam), and superposes the contributions
// at every port -- entirely a postprocessing step, never touches openEMS. See fft_postprocess.hpp
// for the underlying DSP and the "board slicing + net-driven ports and excitations" plan for why
// this is a postprocessing concept rather than a literal FDTD engine excitation.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "config.hpp"
#include "fft_postprocess.hpp"
#include "postprocess.hpp"

namespace kiems {

/// The longest main excitation's own FDTD run (seconds) among `runDurations` (see
/// ExcitationPostprocessor::loadRunDurations()); 0 if none is known.
double primaryRunDuration(const SimulationConfig& simConfig, const std::map<std::int32_t, double>& runDurations);

/// When an excitation's response stops being recorded, on the excitations' shared timeline. A main
/// or Continuous excitation lasts the primary run. A Limited one lasts its tone plus however long
/// its own port's FDTD run took to ring down after its Gaussian pulse, capped at the primary run.
/// Without run lengths, falls back to 1.2x its configured startTime()+duration().
double excitationRecordEnd(const ExcitationConfig& excitation, const SimulationConfig& simConfig,
                           const std::map<std::int32_t, double>& runDurations, const Frequency& frequency);

/// One eye in a simulation's results: what it's named in them, whether it's a differential pair's
/// Vp-Vn, how many adversarial excitations its noisy variant adds, and the eye itself.
struct SimulationEye {
    std::string name;
    bool differential = false;
    std::size_t aggressorCount = 0;
    EyeDiagramData eye;
};

/// Every eye a simulation's results show, as the macOS app builds them: one per differential pair
/// (its mixed-mode channel, Vp-Vn at the pair's probes), then one per probed receiver of each main
/// excitation that isn't part of a pair. Each is built at the simulation's eye bit rate (the top of
/// the sweep without one) and signal level, tested against its eye mask, and given the noise of
/// every adversarial excitation not driving it. `runDurations` as for ExcitationPostprocessor.
std::vector<SimulationEye> computeSimulationEyes(const Postprocessor& postprocessor, const SimulationConfig& simConfig,
                                                 const Frequency& frequency,
                                                 const std::map<std::int32_t, double>& runDurations);

/// Writes `eyes` to `path` as JSON (format 1): the eye test they were built for, then each eye's
/// traces on its time axis, its openings, its mask verdicts and its noise statistics. Values are
/// rounded to seven significant figures, far finer than an eye shows.
void saveEyeDiagrams(const std::vector<SimulationEye>& eyes, const EyeTestConfig& test,
                     const std::filesystem::path& path);

class ExcitationPostprocessor {
public:
    /// `runDurations` is each excited port's own FDTD run length in seconds (see
    /// loadRunDurations()) -- what sizes non-main excitations' tones and record windows. A port
    /// missing from it falls back to the excitation's own configured startTime()+duration().
    ExcitationPostprocessor(const SimulationConfig& simConfig, const Postprocessor& sParams,
                             std::vector<double> frequencies, const Frequency& frequency,
                             std::map<std::int32_t, double> runDurations = {});

    /// Reads each excited port's FDTD run length (its last recorded probe sample's time) back from
    /// `simulationDir`/<port index>/, where Simulation::getPortParameters() also reads them. Ports
    /// with no readable probe file are simply absent.
    static std::map<std::int32_t, double> loadRunDurations(const std::filesystem::path& simulationDir,
                                                            const SimulationConfig& simConfig,
                                                            const PortRenumbering* renumbering = nullptr);

    /// Synthesizes+propagates+superposes every excitation's contribution to every port in
    /// simConfig.ports(). No-op if the simulation has no excitations configured.
    void run();

    const TimeWaveform& responseFor(std::int32_t portIndex) const;

    /// Writes `<portName>_response.csv` (header "time_s,voltage_v") per port with a non-empty
    /// response into outputDir.
    void saveToFile(const std::filesystem::path& outputDir) const;
    /// Writes `<portName>_response.png` (a time-domain line plot) per port with a non-empty
    /// response into outputDir.
    void renderPlots(const std::filesystem::path& outputDir, bool transparent) const;

private:
    double _pickDt() const;

    const SimulationConfig& _simConfig;
    const Postprocessor& _sParams;
    std::vector<double> _frequencies;
    const Frequency& _frequency;
    std::map<std::int32_t, double> _runDurations;
    double _dt = 0;
    std::vector<TimeWaveform> _responses; // indexed like _simConfig.ports(); empty until run()
};

} // namespace kiems
