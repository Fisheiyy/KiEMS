//
//  main.cpp
//  kiems
//
//  CLI entry point. Ported from kiems/main.py.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(_WIN32)
namespace kiems_cli {
// platform_win32.cpp
std::filesystem::path currentExecutablePath();
void addGnuplotToPathIfMissing();
void reportUncaughtExceptions();
}
#endif

#include "arguments.hpp"

#include <nlohmann/json.hpp>

#include "kiems/config.hpp"
#include "kiems/constants.hpp"
#include "kiems/excitation_postprocess.hpp"
#include "kiems/geometry_result.hpp"
#include "kiems/importer.hpp"
#include "logging.hpp"
#include "kiems/path_utf8.hpp"
#include "kiems/paths_config.hpp"
#include "kiems/port_resolution.hpp"
#include "kiems/postprocess_result.hpp"
#include "kiems/simulation.hpp"
#include "kiems/simulation_result.hpp"

// Forward-declare-only boundary header (see its own file comment) -- it keeps Copper's private
// implementation details out of the CLI while accepting libkiems's CSXCAD geometry. This lets the
// CLI run Copper's GPU engine in-process (see
// runPortInProcess() below) instead of posix_spawning copper_fdtd_worker as a separate process --
// the CLI links Copper.framework directly (see the Xcode project's own build settings), while
// libkiems itself still never does.
#include "CopperFDTDRunner.h"

using namespace kiems;
using namespace Cu;

namespace {

std::vector<double> linspace(double start, double stop, std::int32_t num) {
    std::vector<double> result(static_cast<std::size_t>(num));
    if (num == 1) {
        result[0] = start;
        return result;
    }
    for (std::int32_t i = 0; i < num; ++i) {
        result[static_cast<std::size_t>(i)] = start + static_cast<double>(i) * (stop - start) / (num - 1);
    }
    return result;
}

void printUsage() {
    std::cout << "Usage: EM-Simulator [-c CONFIG_FILE] [--update-config] [-g] [-s] [-p] [-a]\n"
                 "                    [--export-field [{outer,cu-outer,cu-inner,substrate} ...]]\n"
                 "                    [--oversampling N] [--absorbing-boundary-cells N] [-t] [--plot-phase] [-i INPUT] [-o OUTPUT]\n"
                 "                    [-d | -l {DEBUG,INFO,WARNING,ERROR}]\n\n"
                 "This application performs EM simulations directly from KiCad PCB files.\n\n"
                 "  -c, --config CONFIG_FILE   Path to config file [default: ./simulation.json]\n"
                 "  --update-config            Add missing fields to config file\n"
                 "  -g, --geometry             Create geometry\n"
                 "  -s, --simulate             Run simulation\n"
                 "  -p, --postprocess          Postprocess the data\n"
                 "  -a, --all                  Execute all steps (geometry, simulation, postprocessing)\n"
                 "  --simulation NAME          Run only the named simulation (repeatable) [default: all]\n"
                 "  --export-field, --ef [...] [s] Export electric field data from the simulation\n"
                 "  --oversampling N           [s] Field dump time-oversampling (default: 4)\n"
                 "  --absorbing-boundary-cells N\n"
                 "                             [g] Depth in cells of the absorbing boundary on every face\n"
                 "                                 (CPML, plus the matched ring on an irregular domain),\n"
                 "                                 overriding grid.absorbing_boundary_cells (default: 8).\n"
                 "                                 Changes the geometry, so needs -g or -a\n"
                 "  -t, --transparent          [p] Export graphs with transparent background\n"
                 "  --plot-phase               [p] Plot phase on S-param graphs\n"
                 "  -i, --input INPUT          [p] Directory with input S-param files, OR a .kicad_pcb\n"
                 "                                 file to import directly into ./fab/ before running\n"
                 "  -o, --output OUTPUT        [p] Directory where results will be placed\n"
                 "  -d, --debug                Enable debug logging\n"
                 "  -l, --log LEVEL            Set log level (DEBUG, INFO, WARNING, ERROR)\n"
                 "  --backend {cpu,gpu}        [s] FDTD engine: openEMS CPU (default) or Copper GPU\n"
                 "  --gpu-device DEVICE        [s] GPU for --backend gpu: an index, name, or part of a name\n"
                 "                                 from --list-gpus [default: the most capable GPU]\n"
                 "  --list-gpus                Print the GPUs --backend gpu can use, as JSON, and exit\n"
                 "  --field-frames             [s] Store each port's field frames for a field viewer, in\n"
                 "                                 ems/simulation/<name>/field_frames.kemsf\n"
                 "  --field-frame-interval N   [s] Timesteps between stored frames [default: 1000]\n"
                 "  --field-frame-precision {float32,float16,int12,int8}\n"
                 "                             [s] How stored frames' samples are kept [default: float32]\n"
                 "  --dump-early-frames DIR    [s] Diagnostic only, GPU backend: instead of a normal run,\n"
                 "                                 step the first N timesteps one at a time, writing raw\n"
                 "                                 field components + coupling coefficients (cropped to a\n"
                 "                                 box around the excitation) to DIR/port<N>/ -- see\n"
                 "                                 copper::dumpEarlyFrames()'s own doc comment\n"
                 "  --dump-frame-count N       [s] Frame count for --dump-early-frames [default: 100]\n"
                 "  --dump-margin-cells N      [s] Crop margin (cells) for --dump-early-frames [default: 25]\n"
                 "  --dump-detailed-trace      [s] Diagnostic only, GPU backend: instead of a normal run,\n"
                 "                                 prints every intermediate update term (curl components,\n"
                 "                                 coefficients, excitation contribution) for a small box\n"
                 "                                 around the excitation over --dump-trace-steps timesteps --\n"
                 "                                 see copper::dumpDetailedTrace()'s own doc comment\n"
                 "  --dump-trace-steps N       [s] Step count for --dump-detailed-trace [default: 4]\n"
                 "  --dump-trace-box-side N    [s] Box side (cells) for --dump-detailed-trace [default: 4]\n";
}

/// CLI-only diagnostic knobs for --dump-early-frames/--dump-detailed-trace -- deliberately not part
/// of Arguments/RunOptions (Arguments belongs to this CLI; these are still separate one-off
/// debugging aids that do not belong in its normal command-line state).
struct DumpOptions {
    std::optional<std::filesystem::path> dir;
    bool detailedTrace = false;
    std::uint32_t traceSteps = 4;
    std::uint32_t traceBoxSide = 4;
    std::uint32_t frameCount = 100;
    std::uint32_t marginCells = 25;
};

[[noreturn]] void printUsageAndExit(int code) {
    printUsage();
    std::exit(code);
}

bool looksLikeFlag(const std::string& s) { return s.size() > 1 && s[0] == '-'; }

[[noreturn]] void missingValue(const std::string& flag) {
    std::cerr << "Missing value for " << flag << "\n";
    printUsageAndExit(2);
}

Arguments parseArguments(int argc, char** argv, DumpOptions& dumpOptions) {
    Arguments args;
    // Left relative: main() resolves these against the config file's own directory once parsing is
    // done (see the PathsConfig setup in main()), so a relative default -- or a relative -i/-o --
    // correctly resolves relative to simulation.json's location, not wherever the tool happened to
    // be invoked from.
    args.setInput(constants::simulationDir);
    args.setOutput(constants::resultsDir);

    static const std::vector<std::string> exportFieldChoices = {"outer", "cu-outer", "cu-inner", "substrate"};
    static const std::vector<std::string> logChoices = {"DEBUG", "INFO", "WARNING", "ERROR"};

    const std::vector<std::string> tokens(argv + 1, argv + argc);
    std::size_t i = 0;
    while (i < tokens.size()) {
        const std::string& tok = tokens[i];
        if (tok == "-h" || tok == "--help") {
            printUsageAndExit(0);
        } else if (tok == "-c" || tok == "--config") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            args.setConfigPath(tokens[i]);
        } else if (tok == "--update-config") {
            args.setUpdateConfig(true);
        } else if (tok == "-g" || tok == "--geometry") {
            args.setGeometry(true);
        } else if (tok == "-s" || tok == "--simulate") {
            args.setSimulate(true);
        } else if (tok == "-p" || tok == "--postprocess") {
            args.setPostprocess(true);
        } else if (tok == "-a" || tok == "--all") {
            args.setAll(true);
        } else if (tok == "--export-field" || tok == "--ef") {
            std::vector<std::string> values;
            while (i + 1 < tokens.size() && !looksLikeFlag(tokens[i + 1])) {
                ++i;
                if (std::find(exportFieldChoices.begin(), exportFieldChoices.end(), tokens[i]) ==
                    exportFieldChoices.end()) {
                    std::cerr << "argument --export-field: invalid choice: '" << tokens[i] << "'\n";
                    printUsageAndExit(2);
                }
                values.push_back(tokens[i]);
            }
            args.setExportField(values);
        } else if (tok == "--oversampling") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            args.setOversampling(std::stoi(tokens[i]));
        } else if (tok == "--absorbing-boundary-cells") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            const std::int32_t depth = std::stoi(tokens[i]);
            if (depth < 1 || depth > 64) {
                std::cerr << "argument --absorbing-boundary-cells: must be between 1 and 64\n";
                printUsageAndExit(2);
            }
            args.setAbsorbingBoundaryCells(depth);
        } else if (tok == "-t" || tok == "--transparent") {
            args.setTransparent(true);
        } else if (tok == "--plot-phase") {
            args.setPlotPhase(true);
        } else if (tok == "--simulation") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            args.addSimulation(tokens[i]);
        } else if (tok == "-i" || tok == "--input") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            args.setInput(std::filesystem::path(tokens[i]));
        } else if (tok == "-o" || tok == "--output") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            args.setOutput(std::filesystem::path(tokens[i]));
        } else if (tok == "-d" || tok == "--debug") {
            if (args.logLevel().has_value()) {
                std::cerr << "argument -d/--debug: not allowed with argument -l/--log\n";
                printUsageAndExit(2);
            }
            args.setDebug(true);
        } else if (tok == "-l" || tok == "--log") {
            if (args.debug()) {
                std::cerr << "argument -l/--log: not allowed with argument -d/--debug\n";
                printUsageAndExit(2);
            }
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            if (std::find(logChoices.begin(), logChoices.end(), tokens[i]) == logChoices.end()) {
                std::cerr << "argument -l/--log: invalid choice: '" << tokens[i] << "'\n";
                printUsageAndExit(2);
            }
            args.setLogLevel(tokens[i]);
        } else if (tok == "--backend") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            if (tokens[i] == "cpu") {
                args.setBackend(FDTDBackend::OpenEMSCPU);
            } else if (tokens[i] == "gpu") {
                args.setBackend(FDTDBackend::CopperGPU);
            } else {
                std::cerr << "argument --backend: invalid choice: '" << tokens[i] << "'\n";
                printUsageAndExit(2);
            }
        } else if (tok == "--gpu-device") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            args.setGpuDevice(tokens[i]);
        } else if (tok == "--list-gpus") {
            args.setListGPUs(true);
        } else if (tok == "--field-frames") {
            args.setFieldFrames(true);
        } else if (tok == "--field-frame-interval") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            const long interval = std::stol(tokens[i]);
            if (interval < 1) {
                std::cerr << "argument --field-frame-interval: must be at least 1\n";
                printUsageAndExit(2);
            }
            args.setFieldFrameInterval(static_cast<std::uint32_t>(interval));
        } else if (tok == "--field-frame-precision") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            static const std::vector<std::string> precisions = {"float32", "float16", "int12", "int8"};
            if (std::find(precisions.begin(), precisions.end(), tokens[i]) == precisions.end()) {
                std::cerr << "argument --field-frame-precision: invalid choice: '" << tokens[i] << "'\n";
                printUsageAndExit(2);
            }
            args.setFieldFramePrecision(tokens[i]);
        } else if (tok == "--dump-early-frames") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            dumpOptions.dir = std::filesystem::path(tokens[i]);
        } else if (tok == "--dump-frame-count") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            dumpOptions.frameCount = static_cast<std::uint32_t>(std::stoul(tokens[i]));
        } else if (tok == "--dump-margin-cells") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            dumpOptions.marginCells = static_cast<std::uint32_t>(std::stoul(tokens[i]));
        } else if (tok == "--dump-detailed-trace") {
            dumpOptions.detailedTrace = true;
        } else if (tok == "--dump-trace-steps") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            dumpOptions.traceSteps = static_cast<std::uint32_t>(std::stoul(tokens[i]));
        } else if (tok == "--dump-trace-box-side") {
            if (++i >= tokens.size()) {
                missingValue(tok);
            }
            dumpOptions.traceBoxSide = static_cast<std::uint32_t>(std::stoul(tokens[i]));
        } else {
            std::cerr << "Unknown argument: " << tok << "\n";
            printUsageAndExit(2);
        }
        ++i;
    }
    return args;
}

void setupLogging(const Arguments& args) {
    LogLevel level = LogLevel::Info;
    if (args.debug()) {
        level = LogLevel::Debug;
    }
    if (args.logLevel().has_value()) {
        const std::string& lvl = *args.logLevel();
        if (lvl == "DEBUG") {
            level = LogLevel::Debug;
        } else if (lvl == "INFO") {
            level = LogLevel::Info;
        } else if (lvl == "WARNING") {
            level = LogLevel::Warning;
        } else if (lvl == "ERROR") {
            level = LogLevel::Error;
        }
    }
    setLogLevel(level);
}

#if defined(_WIN32)
constexpr char kPathListSeparator = ';';
constexpr const char* kExecutableSuffix = ".exe";
constexpr const char* kKicadCliName = "kicad-cli.exe";
#else
constexpr char kPathListSeparator = ':';
constexpr const char* kExecutableSuffix = "";
constexpr const char* kKicadCliName = "kicad-cli";
#endif

// macOS's KiCad.app doesn't symlink kicad-cli anywhere on a typical PATH -- it ships only inside
// the app bundle. Only the CLI is allowed to do this kind of PATH-scanning/fallback-guessing (a
// sandboxed GUI can't, and libkiems itself never does -- see importer.hpp); it's a convenience
// specific to this unsandboxed developer tool.
std::filesystem::path resolveKicadCli() {
    const char* pathEnv = std::getenv("PATH");
    if (pathEnv != nullptr) {
        std::stringstream pathStream(pathEnv);
        std::string dir;
        while (std::getline(pathStream, dir, kPathListSeparator)) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(std::filesystem::path(dir) / kKicadCliName, ec)) {
                return kKicadCliName;
            }
        }
    }
    static const std::array<std::filesystem::path, 1> kKicadCliFallbackPaths = {
        "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli",
    };
    for (const auto& candidate : kKicadCliFallbackPaths) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            logDebug("kicad-cli not found on PATH; using " + candidate.string());
            return candidate;
        }
    }
    return kKicadCliName; // Let the process launcher's own error reporting handle the "truly not found" case.
}

// Absolute path to the currently-running executable's directory.  Worker executables are sibling
// build products of this CLI.
std::filesystem::path executableDir() {
#if defined(__APPLE__)
    std::array<char, 4096> buffer{};
    std::uint32_t size = static_cast<std::uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::canonical(buffer.data(), ec);
    return (ec ? std::filesystem::path(buffer.data()) : resolved).parent_path();
#elif defined(__linux__)
    std::error_code ec;
    const std::filesystem::path executable = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return {};
    }
    const std::filesystem::path resolved = std::filesystem::canonical(executable, ec);
    return (ec ? executable : resolved).parent_path();
#elif defined(_WIN32)
    return kiems_cli::currentExecutablePath().parent_path();
#else
    return {};
#endif
}

void createDir(const std::filesystem::path& directoryPath) {
    std::error_code error;
    std::filesystem::create_directories(directoryPath, error);
}

/// Empties `directory` of everything but the entries named in `keep`. A file another program
/// holds open can't be deleted on Windows, so a failure names what couldn't be removed rather than
/// escaping as an exception.
std::expected<void, std::string> clearDirectory(const std::filesystem::path& directory,
                                                const std::vector<std::string>& keep = {}) {
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) return {};
    // Listed first: removing entries while iterating the directory is unspecified.
    std::vector<std::filesystem::path> entries;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) entries.push_back(entry.path());
    if (error) return std::unexpected("Couldn't read " + pathToUtf8(directory) + ": " + error.message());
    for (const std::filesystem::path& entry : entries) {
        if (std::find(keep.begin(), keep.end(), pathToUtf8(entry.filename())) != keep.end()) continue;
        std::filesystem::remove_all(entry, error);
        if (error) {
            return std::unexpected("Couldn't remove " + pathToUtf8(entry) + " from the last run: " + error.message() +
                                   ". If another program (or a viewer of this simulation's files) has it open, "
                                   "close it and run again.");
        }
    }
    return {};
}

/// Clears a stage's output (ems/geometry, ems/simulation or ems/results) for this run: only the
/// simulations named with --simulation when there are any, so other simulations' results survive,
/// and otherwise the whole stage. `keep` names entries inside each simulation's folder to leave.
std::expected<void, std::string> clearStage(const std::filesystem::path& stageDirectory,
                                            const std::vector<std::string>& simulations,
                                            const std::vector<std::string>& keep = {}) {
    createDir(stageDirectory);
    if (simulations.empty()) {
        std::error_code error;
        std::vector<std::filesystem::path> entries;
        for (const auto& entry : std::filesystem::directory_iterator(stageDirectory, error)) entries.push_back(entry.path());
        for (const std::filesystem::path& entry : entries) {
            if (!keep.empty() && std::filesystem::is_directory(entry, error)) {
                if (auto cleared = clearDirectory(entry, keep); !cleared) return cleared;
            } else if (std::filesystem::remove_all(entry, error); error) {
                return std::unexpected("Couldn't remove " + pathToUtf8(entry) + " from the last run: " +
                                       error.message() + ". If another program has it open, close it and run again.");
            }
        }
        return {};
    }
    for (const std::string& simulation : simulations) {
        if (auto cleared = clearDirectory(stageDirectory / pathFromUtf8(simulation), keep); !cleared) return cleared;
    }
    return {};
}

/// Every simulation's frequency-domain results, once postprocessed -- writes CSVs/PNGs to
/// args.output()/<simName>, mirroring what the old postprocess() free function did directly
/// against a raw Postprocessor. Excitation postprocessing (a downstream consumer of S-parameters,
/// not part of the geometry->simulate->postprocess pipeline itself) still reaches into the
/// underlying Postprocessor via PostprocessResult::postprocessorFor() -- see its doc comment.
void saveAndRenderResults(const PostprocessResult& results, const Arguments& args) {
    for (const auto& simConfig : results.config().simulations()) {
        const std::filesystem::path outDir = args.output() / simConfig.name();
        std::filesystem::create_directories(outDir);

        results.saveToFile(simConfig.name(), outDir);
        results.renderSParams(simConfig.name(), args.plotPhase(), args.transparent(), outDir);
        results.renderImpedance(simConfig.name(), args.transparent(), outDir);
        results.renderSmith(simConfig.name(), args.transparent(), outDir);
        results.renderDiffPairSParams(simConfig.name(), args.transparent(), outDir);
        results.renderDiffImpedance(simConfig.name(), args.transparent(), outDir);
        results.renderTraceDelays(simConfig.name(), args.transparent(), outDir);
        results.renderProbes(simConfig.name(), args.transparent(), outDir);

        if (!simConfig.excitations().empty()) {
            const Postprocessor* post = results.postprocessorFor(simConfig.name());
            if (post == nullptr) {
                continue;
            }
            const std::map<std::int32_t, double> runDurations =
                ExcitationPostprocessor::loadRunDurations(args.input() / simConfig.name(), simConfig);
            // The eyes the results show, tested against the simulation's eye mask (eyes.json).
            saveEyeDiagrams(computeSimulationEyes(*post, simConfig, results.config().frequency(), runDurations),
                            simConfig.eyeTest(), outDir / "eyes.json");
            const std::filesystem::path excDir = outDir / "excitations";
            std::filesystem::create_directories(excDir);
            const std::vector<double> frequencies =
                linspace(results.config().frequency().start(), results.config().frequency().stop(),
                         constants::frequencySampleCount);
            ExcitationPostprocessor excPost(simConfig, *post, frequencies, results.config().frequency(), runDurations);
            excPost.run();
            excPost.saveToFile(excDir);
            excPost.renderPlots(excDir, args.transparent());
        }
    }
}

/// Renders one GeometryProgress update -- the CLI's own consumer of GeometryResult::build()'s
/// progress contract (which simulation, out of how many, and which of its two phases -- see
/// GeometryPhase's own doc comment), in the same plain logInfo()-per-update style as
/// printCopperProgress() below.
void printGeometryProgress(const GeometryProgress& progress) {
    std::ostringstream line;
    line << "[Geometry] [" << progress.simulationName << " " << (progress.simulationIndex + 1) << "/"
         << progress.simulationCount << "] ";
    switch (progress.phase) {
    case GeometryPhase::SlicingBoard:
        line << "Slicing board: " << (progress.currentStep == 0 ? "starting..." : "complete");
        break;
    case GeometryPhase::PlacingGrid:
        line << "Placing grid: " << (progress.currentStep == 0 ? "starting..." : "complete");
        break;
    }
    logInfo(line.str());
}

/// Renders one copper::CopperFDTDProgress update -- the CLI's own consumer of the 3-axis progress
/// contract (major phase / progress through that phase's own step count / progress toward the
/// energy-decay end criteria) copper::runFDTDPortOnGPU() reports, in place of its own default
/// stdout printing (see CopperFDTDRunner.h's own doc comment on `onProgress`). Deliberately a plain
/// logInfo() line per update (matching every other stage's own "Creating geometry"/"Running
/// simulation" style in this file) rather than an in-place-updating status line -- keeps this
/// readable in a piped/redirected log too, not just an interactive terminal.
void printCopperProgress(const copper::CopperFDTDProgress& progress) {
    std::ostringstream line;
    line << "[Copper] ";
    switch (progress.phase) {
    case copper::CopperFDTDPhase::Setup:
        line << "Setup: " << (progress.currentStep == 0 ? "starting..." : "complete");
        break;
    case copper::CopperFDTDPhase::FDTDRun: {
        const double stepPercent = progress.totalSteps > 0
                                        ? 100.0 * static_cast<double>(progress.currentStep) /
                                              static_cast<double>(progress.totalSteps)
                                        : 0.0;
        const double energyPercent =
            progress.targetEnergyChangeDB > 0.0
                ? std::min(100.0, 100.0 * progress.energyChangeDB / progress.targetEnergyChangeDB)
                : 0.0;
        line << "FDTD run: step " << progress.currentStep << "/" << progress.totalSteps << " (" << std::fixed
             << std::setprecision(1) << stepPercent << "%) | energy decay " << std::setprecision(2)
             << progress.energyChangeDB << "/" << progress.targetEnergyChangeDB << "dB (" << std::setprecision(1)
             << energyPercent << "%)";
        break;
    }
    case copper::CopperFDTDPhase::Postprocessing:
        line << "Postprocessing...";
        break;
    }
    logInfo(line.str());
}

/// --field-frames: how frames are stored, and the series each port wrote, by simulation directory.
struct FieldFrameCapture {
    std::uint32_t intervalSteps = 1000;
    copper::FieldFrameStorage storage;
    /// The slot (0 or 1) each simulation's series use this run -- the other one than its last
    /// run's, as the macOS app alternates them, so a viewer with the old series open is never
    /// reading a file being truncated.
    std::map<std::filesystem::path, std::uint64_t> slots;
    std::map<std::filesystem::path, std::vector<std::pair<std::int32_t, std::filesystem::path>>> series;
};

constexpr const char* kFieldFrameManifest = "field_frames_current.txt";
constexpr const char* kFieldFramePackage = "field_frames.kemsf";

/// The slot `simulationDirectory`'s last run didn't use, from its manifest's file names.
std::uint64_t nextFieldFrameSlot(const std::filesystem::path& simulationDirectory) {
    std::ifstream manifest(simulationDirectory / kFieldFrameManifest);
    std::string line;
    while (std::getline(manifest, line)) {
        if (line.ends_with("_0.h5")) return 1;
        if (line.ends_with("_1.h5")) return 0;
    }
    return 0;
}

/// Writes each simulation's field-frame manifest: one "port path" line per series, the path
/// relative to the simulation directory -- the macOS app's field_frames_current.txt.
void writeFieldFrameManifests(const FieldFrameCapture& capture) {
    for (const auto& [directory, series] : capture.series) {
        const std::filesystem::path temporary = directory / (std::string(kFieldFrameManifest) + ".tmp");
        {
            std::ofstream out(temporary, std::ios::trunc);
            for (const auto& [port, path] : series) {
                out << port << ' ' << path.lexically_relative(directory).generic_string() << '\n';
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, directory / kFieldFrameManifest, error);
        if (error) logWarning("Couldn't write the field-frame manifest in " + pathToUtf8(directory) + ": " + error.message());
    }
}

/// The SimulationResult::FDTDPortRunner passed to SimulationResult::run() when the run happens in
/// this process: always for the GPU backend (`onGPU`), and for the CPU one when field frames are
/// stored (`capture`), which the worker processes can't do. It replaces the default
/// sim.run(excitedPortNumber), which spawns a separate worker. By the time generateResults()
/// (simulation_data.hpp) hands `sim` to this callback it has already had adoptSlicedBoard()/
/// adoptGridLines()/populateGeometry()/setupPorts() called on it, so this only does the FDTD part
/// (prepareRunDirectory(), then Copper's run), mirroring copper_fdtd_worker/main.cpp's sequence.
std::expected<std::uint32_t, std::string> runPortInProcess(Simulation& sim, std::int32_t excitedPortNumber, bool onGPU,
                                                           FieldFrameCapture* capture) {
    const std::filesystem::path cwd = std::filesystem::current_path();
    if (auto result = sim.prepareRunDirectory(excitedPortNumber); !result) {
        return std::unexpected(result.error());
    }
    const std::filesystem::path probeDir = std::filesystem::current_path();
    const double cpmlAlphaMax = copper::cpmlAlphaMaxForFrequency(sim.config().frequency().start());
    copper::CopperFDTDPortConfig portConfig;
    portConfig.boundaryIsPEC = sim.boundaryIsPEC();
    portConfig.f0 = sim.excitationF0();
    portConfig.fc = sim.excitationFc();
    portConfig.maxTimesteps = sim.maxTimesteps();

    std::optional<copper::FieldFrameSeriesRequest> fieldSeries;
    if (capture) {
        // prepareRunDirectory() entered ems/simulation/<name>/<port>.
        const std::filesystem::path simulationDirectory = probeDir.parent_path();
        if (!capture->slots.contains(simulationDirectory)) {
            capture->slots[simulationDirectory] = nextFieldFrameSlot(simulationDirectory);
        }
        const std::filesystem::path package = simulationDirectory / kFieldFramePackage;
        std::error_code error;
        std::filesystem::create_directories(package, error);
        if (error) return std::unexpected("Couldn't create " + pathToUtf8(package) + ": " + error.message());
        // The board's copper spans the substrates, top surface at z = 0 (simulation units to metres).
        double boardThickness = 0.0;
        for (const auto& substrate : sim.config().getSubstrates()) boardThickness += substrate.thickness();
        constexpr double kSimulationUnitsToMetres = constants::baseUnit / static_cast<double>(constants::unitMultiplier);
        copper::FieldFrameSeriesRequest request;
        request.path = package / ("port_" + std::to_string(excitedPortNumber) + "_" +
                                  std::to_string(capture->slots[simulationDirectory]) + ".h5");
        request.simulationName = pathToUtf8(simulationDirectory.filename());
        request.excitedPort = excitedPortNumber;
        request.boardZMin = -boardThickness * kSimulationUnitsToMetres;
        request.boardZMax = 0.0;
        request.storage = capture->storage;
        request.captureIntervalSteps = capture->intervalSteps;
        fieldSeries = std::move(request);
    }

    const auto absorbing = static_cast<std::uint32_t>(sim.config().grid().absorbingBoundaryCells());
    const copper::CopperFDTDRunResult gpuResult =
        onGPU ? copper::runFDTDPortOnGPU(sim.csx(), portConfig, printCopperProgress, cpmlAlphaMax, absorbing, {},
                                         fieldSeries)
              : copper::runFDTDPortOnCPU(sim.csx(), portConfig, printCopperProgress, cpmlAlphaMax, absorbing, {},
                                         fieldSeries);
    std::filesystem::current_path(cwd);
    if (!gpuResult.success) {
        return std::unexpected(gpuResult.errorMessage);
    }
    if (capture && gpuResult.fieldFrameSeriesPath) {
        capture->series[probeDir.parent_path()].emplace_back(excitedPortNumber, *gpuResult.fieldFrameSeriesPath);
    }

    // runFDTDPortOnGPU() no longer writes probe files itself -- write them explicitly here so
    // getPortParameters()'s existing on-disk S-parameter pipeline keeps working unmodified (see
    // copper_fdtd_worker/main.cpp's own identical step) via CopperProbeResult::data().
    for (const copper::CopperProbeResult& probeResult : gpuResult.probes) {
        std::ofstream probeFile(probeDir / probeResult.name);
        if (!probeFile.is_open()) {
            return std::unexpected("Failed to open probe file for writing: " +
                                    (probeDir / probeResult.name).string());
        }
        probeFile << probeResult.data();
    }
    return gpuResult.timestepsRun;
}

/// --dump-early-frames/--dump-detailed-trace's own FDTDPortRunner -- same prepareRunDirectory()/
/// cwd-restore shape as runPortInProcess() above, but calls copper::dumpEarlyFrames() and/or
/// copper::dumpDetailedTrace() instead of copper::runFDTDPortOnGPU(): a one-off diagnostic capture,
/// not a normal run, so there are no probe files to write afterward. dumpEarlyFrames() writes into
/// `dumpOptions.dir`/port<excitedPortNumber>/ so a multi-port config doesn't clobber one port's dump
/// with another's; dumpDetailedTrace() just prints to stdout, no directory needed.
std::expected<std::uint32_t, std::string> dumpGPUPortInProcess(Simulation& sim, std::int32_t excitedPortNumber,
                                                        const DumpOptions& dumpOptions) {
    const std::filesystem::path cwd = std::filesystem::current_path();
    if (auto result = sim.prepareRunDirectory(excitedPortNumber); !result) {
        return std::unexpected(result.error());
    }
    const double cpmlAlphaMax = copper::cpmlAlphaMaxForFrequency(sim.config().frequency().start());
    copper::CopperFDTDPortConfig portConfig;
    portConfig.boundaryIsPEC = sim.boundaryIsPEC();
    portConfig.f0 = sim.excitationF0();
    portConfig.fc = sim.excitationFc();
    portConfig.maxTimesteps = sim.maxTimesteps();
    if (dumpOptions.dir.has_value()) {
        const std::filesystem::path portDir = *dumpOptions.dir / ("port" + std::to_string(excitedPortNumber));
        const std::string error =
            copper::dumpEarlyFrames(sim.csx(), portConfig, portDir, dumpOptions.frameCount,
                                     dumpOptions.marginCells, cpmlAlphaMax, static_cast<std::uint32_t>(sim.config().grid().absorbingBoundaryCells()));
        if (!error.empty()) {
            std::filesystem::current_path(cwd);
            return std::unexpected(error);
        }
    }
    if (dumpOptions.detailedTrace) {
        std::fprintf(stdout, "Copper: dumpDetailedTrace for excited port %d\n", excitedPortNumber);
        const std::string error =
            copper::dumpDetailedTrace(sim.csx(), portConfig, dumpOptions.traceSteps,
                                       dumpOptions.traceBoxSide, cpmlAlphaMax, static_cast<std::uint32_t>(sim.config().grid().absorbingBoundaryCells()));
        if (!error.empty()) {
            std::filesystem::current_path(cwd);
            return std::unexpected(error);
        }
    }
    std::filesystem::current_path(cwd);
    return std::uint32_t{0}; // a diagnostic capture, not a full run
}

/// --list-gpus: Copper's view of the GPUs, as a JSON array (see copper::CopperGPUDevice) on stdout.
/// An empty array, with the reason on stderr, when there is no usable GPU engine.
int listGPUs() {
    std::string error;
    const std::vector<copper::CopperGPUDevice> devices = copper::listGPUDevices(&error);
    nlohmann::json list = nlohmann::json::array();
    for (const copper::CopperGPUDevice& device : devices) {
        list.push_back({{"index", device.index},
                        {"name", device.name},
                        {"kind", device.kind},
                        {"api", device.api},
                        {"memory_bytes", device.memoryBytes},
                        {"usable", device.usable},
                        {"problem", device.problem},
                        {"default", device.isDefault}});
    }
    std::cout << list.dump(2) << std::endl;
    if (devices.empty() && !error.empty()) std::cerr << "No GPU engine available: " << error << "\n";
    return EXIT_SUCCESS;
}

/// --gpu-device: Copper reads its device choice from COPPER_VULKAN_DEVICE, both in this process
/// (the GPU backend runs in-process) and in any worker it starts, which inherits the environment.
void setGpuDevice(const std::string& device) {
#if defined(_WIN32)
    ::_putenv_s("COPPER_VULKAN_DEVICE", device.c_str());
#else
    ::setenv("COPPER_VULKAN_DEVICE", device.c_str(), 1);
#endif
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    kiems_cli::reportUncaughtExceptions();
    kiems_cli::addGnuplotToPathIfMissing();
#endif
    DumpOptions dumpOptions;
    Arguments args = parseArguments(argc, argv, dumpOptions);
    if (args.listGPUs()) {
        return listGPUs();
    }
    if (args.gpuDevice().has_value()) {
        setGpuDevice(*args.gpuDevice());
    }

    // Every relative path in this tool (fab/, ems/, etc., and -i/-o if given as relative paths) is
    // meant to be relative to simulation.json's own location, not to wherever the tool happened to
    // be invoked from -- resolve the config path against the invoking shell's cwd once, here, and
    // build every other path explicitly off its parent directory below (no chdir: the library
    // itself never assumes a shared process cwd, and this CLI doesn't need one either).
    const std::filesystem::path cfgPath = std::filesystem::absolute(
        args.configPath().has_value() ? std::filesystem::path(*args.configPath()) : constants::defaultConfigPath);
    args.setConfigPath(cfgPath.string());
    const std::filesystem::path configDir = cfgPath.parent_path();
    if (args.input().is_relative()) {
        args.setInput(configDir / args.input());
    }
    if (args.output().is_relative()) {
        args.setOutput(configDir / args.output());
    }

    const PathsConfig paths = PathsConfig::forConfigFile(cfgPath, resolveKicadCli(),
                                                          executableDir() / (std::string("kiems_fdtd_worker") + kExecutableSuffix),
                                                          executableDir() / (std::string("copper_fdtd_worker") + kExecutableSuffix));

    // Owned by main so the loaded KiCad board is torn down before main returns, while KiCad's own
    // process-wide state is still alive -- not from exit()'s static destructors.
    auto runtime = libkicad::Runtime::create();
    if (!runtime) {
        logError(runtime.error());
        return EXIT_FAILURE;
    }

    // Use what KiCad has, including changes it has only autosaved (resolveKicadSources() logs it).
    std::optional<KicadSources> sources;
    if (args.input().extension() == ".kicad_pcb") {
        sources = resolveKicadSources(*runtime, args.input(), paths.fabDir / "kicad-source");
        if (auto result = exportKicadPcb(paths, sources->boardPath); !result) {
            logError(result.error());
            return EXIT_FAILURE;
        }
    }
    auto configResult = EMSConfig::parse(cfgPath, args.updateConfig());
    if (!configResult) {
        logError(configResult.error());
        return EXIT_FAILURE;
    }
    EMSConfig config = std::move(*configResult);
    if (sources.has_value()) config.setKicadPcbPath(sources->boardPath);
    if (args.absorbingBoundaryCells().has_value()) {
        // A standalone -s reuses the cached grid, built with whatever depth -g had: running it with
        // another would grade the wrong cells as CPML.
        if (!args.geometry() && !args.all()) {
            logError("--absorbing-boundary-cells changes the geometry: run it with -g or -a");
            return EXIT_FAILURE;
        }
        config.grid().setAbsorbingBoundaryCells(*args.absorbingBoundaryCells());
    }
    setupLogging(args);
    if (args.updateConfig()) {
        return EXIT_SUCCESS;
    }
    if (!args.simulations().empty()) {
        // Only the named simulations, in the config's order. The config file itself is untouched:
        // this happens after --update-config's write, and only to the in-memory copy.
        for (const std::string& name : args.simulations()) {
            const auto& all = config.simulations();
            if (std::none_of(all.begin(), all.end(), [&](const SimulationConfig& s) { return s.name() == name; })) {
                logError("No simulation named \"" + name + "\" in " + pathToUtf8(cfgPath));
                return EXIT_FAILURE;
            }
        }
        std::erase_if(config.simulations(), [&](const SimulationConfig& s) {
            return std::find(args.simulations().begin(), args.simulations().end(), s.name()) == args.simulations().end();
        });
    }

    if (!args.geometry() && !args.simulate() && !args.postprocess() && !args.all()) {
        logInfo(R"(No steps selected. Exiting. To select steps use "-g", "-s", "-p", "-a" flags)");
        return EXIT_SUCCESS;
    }

    // Resolved unconditionally (not just for -g/-a): -s/-p invoked standalone, in a separate
    // process from whichever earlier invocation ran -g, still need every simulation's ports()
    // populated -- simulate() reads which ports to excite, postprocess() needs the right port
    // count to load Sx<port>.csv files. Requires fab/board.kicad_pcb (persisted by exportKicadPcb())
    // from this invocation's own -i or an earlier one, and fab/stackup.json (importStackup()) for
    // resolveSimulationPorts()'s copper-layer-index lookup.
    const libkicad::Board board(*runtime, paths.kicadBoardPaths());

    if (auto result = importStackup(board, config); !result) {
        logError(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = resolveSimulationPorts(config, board); !result) {
        logError(result.error());
        return EXIT_FAILURE;
    }

    createDir(paths.baseDir);

    RunOptions options;
    options.oversampling = args.oversampling();
    options.exportField = args.exportField();
    options.transparent = args.transparent();
    options.plotPhase = args.plotPhase();
    options.backend = args.backend();
    // --dump-early-frames/--dump-detailed-trace only exist on Copper's GPU engine -- silently
    // forcing the backend here (rather than requiring --backend gpu too) keeps either one-off
    // diagnostic invocation to a single flag.
    const bool dumpRequested = dumpOptions.dir.has_value() || dumpOptions.detailedTrace;
    if (dumpRequested) {
        options.backend = FDTDBackend::CopperGPU;
    }

    // Each stage's result carries its own EMSConfig forward (see geometry_result.hpp), so `config`
    // itself is only ever consumed once, by whichever of build()/load() below runs first -- every
    // later stage reads through the previous stage's result instead of touching `config` again.
    std::optional<GeometryResult> geometryResult;
    std::optional<SimulationResult> simulationResult;
    std::optional<PostprocessResult> postprocessResult;

    if (args.geometry() || args.all()) {
        logInfo("Creating geometry");
        if (auto cleared = clearStage(paths.geometryDir, args.simulations()); !cleared) {
            logError(cleared.error());
            return EXIT_FAILURE;
        }
        auto result = GeometryResult::build(std::move(config), options, paths, board, printGeometryProgress);
        if (!result) {
            logError(result.error());
            return EXIT_FAILURE;
        }
        geometryResult = std::move(*result);
    }
    if (args.simulate() || args.all()) {
        logInfo("Running simulation");
        // Storing fields keeps the last run's series: this run writes the other of its two slots,
        // so a viewer that has the old one open isn't disturbed. Without --field-frames they'd be
        // stale, so they go with everything else.
        const std::vector<std::string> keep = args.fieldFrames()
            ? std::vector<std::string>{kFieldFramePackage, kFieldFrameManifest}
            : std::vector<std::string>{};
        if (auto cleared = clearStage(paths.simulationDir, args.simulations(), keep); !cleared) {
            logError(cleared.error());
            return EXIT_FAILURE;
        }
        if (!geometryResult.has_value()) {
            // -s invoked standalone, in a separate process from whichever -g produced this data.
            auto loaded = GeometryResult::load(std::move(config), paths);
            if (!loaded) {
                logError(loaded.error());
                return EXIT_FAILURE;
            }
            geometryResult = std::move(*loaded);
        }
        // Copper's GPU backend runs directly in this process (see runPortInProcess()'s own doc
        // comment for why, and why libkiems itself still never depends on Copper) rather than
        // posix_spawning copper_fdtd_worker -- the whole point being live progress reporting through
        // this process's own stdout, not a separate process's.
        // Field frames are written by the run itself, so storing them runs the CPU engine in this
        // process too.
        std::optional<FieldFrameCapture> capture;
        if (args.fieldFrames()) {
            capture.emplace();
            capture->intervalSteps = args.fieldFrameInterval();
            const std::string& precision = args.fieldFramePrecision();
            capture->storage.precision = precision == "float16" ? copper::FieldSamplePrecision::Float16
                                       : precision == "int12"   ? copper::FieldSamplePrecision::Int12
                                       : precision == "int8"    ? copper::FieldSamplePrecision::Int8
                                                                : copper::FieldSamplePrecision::Float32;
        }
        const bool onGPU = options.backend == FDTDBackend::CopperGPU;
        auto result =
            dumpRequested
                ? SimulationResult::run(*geometryResult, options, board,
                                        [&dumpOptions](Simulation& sim, std::int32_t excitedPortNumber) {
                                            return dumpGPUPortInProcess(sim, excitedPortNumber, dumpOptions);
                                        })
            : onGPU || capture
                ? SimulationResult::run(*geometryResult, options, board,
                                        [&](Simulation& sim, std::int32_t excitedPortNumber) {
                                            return runPortInProcess(sim, excitedPortNumber, onGPU,
                                                                    capture ? &*capture : nullptr);
                                        })
                : SimulationResult::run(*geometryResult, options, board);
        if (capture) writeFieldFrameManifests(*capture);
        if (!result) {
            logError(result.error());
            return EXIT_FAILURE;
        }
        if (dumpRequested) {
            logInfo("Dump diagnostic complete; skipping postprocessing (there are no valid S-parameters from a "
                     "short diagnostic run)");
            return EXIT_SUCCESS;
        }
        simulationResult = std::move(*result);
    }
    if (args.postprocess() || args.all()) {
        logInfo("Postprocessing");
        if (auto cleared = clearStage(paths.resultsDir, args.simulations()); !cleared) {
            logError(cleared.error());
            return EXIT_FAILURE;
        }
        if (!simulationResult.has_value()) {
            // -p invoked standalone: reload geometry (if this invocation didn't just build it) and
            // Sx<port>.csv from args.input() -- the CLI's own -i override, defaulting to
            // paths.simulationDir -- rather than re-running FDTD.
            if (!geometryResult.has_value()) {
                auto loaded = GeometryResult::load(std::move(config), paths);
                if (!loaded) {
                    logError(loaded.error());
                    return EXIT_FAILURE;
                }
                geometryResult = std::move(*loaded);
            }
            auto loaded = SimulationResult::load(*geometryResult, args.input());
            if (!loaded) {
                logError(loaded.error());
                return EXIT_FAILURE;
            }
            simulationResult = std::move(*loaded);
        }
        auto result = PostprocessResult::compute(*simulationResult);
        if (!result) {
            logError(result.error());
            return EXIT_FAILURE;
        }
        postprocessResult = std::move(*result);
        saveAndRenderResults(*postprocessResult, args);
    }

    return EXIT_SUCCESS;
}
