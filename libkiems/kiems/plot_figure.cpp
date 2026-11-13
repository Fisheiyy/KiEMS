#include "plot_figure.hpp"

#include "path_utf8.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "logging.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace kiems {
using Cu::logError;
namespace {

/// A gnuplot process drawing one script, started by startGnuplot().
struct GnuplotRun {
#ifdef _WIN32
    std::intptr_t process = -1;
#else
    pid_t process = -1;
#endif
    std::filesystem::path script;
};

/// Starts `gnuplot script` (gnuplot found on the PATH, as Matplot++ finds it) without waiting.
bool startGnuplot(const std::filesystem::path& script, GnuplotRun& run) {
    run.script = script;
#ifdef _WIN32
    // _wspawnlp joins its arguments with spaces, so a path with spaces needs its own quotes.
    const std::wstring quoted = L"\"" + script.wstring() + L"\"";
    run.process = _wspawnlp(_P_NOWAIT, L"gnuplot", L"gnuplot", quoted.c_str(), nullptr);
    return run.process != -1;
#else
    const std::string path = script.string();
    std::array<char*, 3> argv = {const_cast<char*>("gnuplot"), const_cast<char*>(path.c_str()), nullptr};
    return posix_spawnp(&run.process, "gnuplot", nullptr, nullptr, argv.data(), environ) == 0;
#endif
}

/// Waits for `run`; deletes its script if gnuplot succeeded, else keeps it (to see why) and says so.
bool finishGnuplot(const GnuplotRun& run) {
    bool ok = false;
#ifdef _WIN32
    int status = 0;
    ok = _cwait(&status, run.process, 0) != -1 && status == 0;
#else
    int status = 0;
    pid_t waited = -1;
    while ((waited = waitpid(run.process, &status, 0)) < 0 && errno == EINTR) {
    }
    ok = waited == run.process && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
    if (ok) {
        std::error_code ignored;
        std::filesystem::remove(run.script, ignored);
    } else {
        logError("gnuplot failed to draw " + pathToUtf8(run.script) + "; the script is kept to see why");
    }
    return ok;
}

/// The plots still being drawn. Each is its own gnuplot process, so they draw side by side: one
/// plot takes gnuplot about half a second, mostly its own startup, and a run's results have a few
/// dozen. At most one per core runs at once.
class PendingPlots {
public:
    ~PendingPlots() { waitAll(); } // nothing exits with a plot half drawn

    void add(GnuplotRun run) {
        std::unique_lock lock(_mutex);
        const std::size_t limit = std::max(2U, std::thread::hardware_concurrency());
        while (_runs.size() >= limit) {
            GnuplotRun oldest = std::move(_runs.front());
            _runs.pop_front();
            lock.unlock();
            _ok = finishGnuplot(oldest) && _ok;
            lock.lock();
        }
        _runs.push_back(std::move(run));
    }

    bool waitAll() {
        std::unique_lock lock(_mutex);
        std::deque<GnuplotRun> runs;
        runs.swap(_runs);
        lock.unlock();
        bool ok = true;
        for (const GnuplotRun& run : runs) ok = finishGnuplot(run) && ok;
        lock.lock();
        ok = ok && _ok;
        _ok = true;
        return ok;
    }

private:
    std::mutex _mutex;
    std::deque<GnuplotRun> _runs;
    bool _ok = true;
};

PendingPlots& pendingPlots() {
    static PendingPlots plots;
    return plots;
}

/// `commands` with its inline data moved into datablocks. Matplot++ follows a plot command naming
/// '-' with that data, ended by a line "e", which gnuplot accepts from a pipe; from a script file it
/// rejects it inside a multiplot (as every Matplot++ figure is), with a warning for each line. So
/// each '-' becomes $kiems_dataN, defined from its block just before the command.
std::string withDatablocks(const std::string& commands) {
    std::vector<std::string_view> lines;
    for (std::size_t start = 0; start < commands.size();) {
        const std::size_t end = std::min(commands.find('\n', start), commands.size());
        lines.push_back(std::string_view(commands).substr(start, end - start));
        start = end + 1;
    }
    const auto trimmed = [](std::string_view line) {
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string_view::npos) return std::string_view();
        return line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
    };
    std::string out;
    out.reserve(commands.size() + 256);
    int blocks = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view command = trimmed(lines[i]);
        const bool plots = command.starts_with("plot ") || command.starts_with("splot ") ||
                           command.starts_with("replot ");
        if (!plots || command.find("'-'") == std::string_view::npos) {
            out.append(lines[i]);
            out += '\n';
            continue;
        }
        // The plot command, with any continuation lines.
        std::string plot(lines[i]);
        while (!plot.empty() && plot.back() == '\\' && i + 1 < lines.size()) {
            plot += '\n';
            plot.append(lines[++i]);
        }
        // Each '-' takes the next block of data lines.
        for (std::size_t at = plot.find("'-'"); at != std::string::npos; at = plot.find("'-'", at)) {
            const std::string name = "$kiems_data" + std::to_string(++blocks);
            out += name + " << EOD\n";
            while (++i < lines.size() && trimmed(lines[i]) != "e") {
                out.append(lines[i]);
                out += '\n';
            }
            out += "EOD\n";
            plot.replace(at, 3, name);
            at += name.size();
        }
        out += plot;
        out += '\n';
    }
    return out;
}

/// The commands Matplot++ sends a gnuplot backend, gathered between frames and run from a file.
class ScriptBackend final : public matplot::backend::backend_interface {
public:
    bool is_interactive() override { return false; }
    const std::string& output() override { return _output; }
    const std::string& output_format() override { return _format; }

    bool output(const std::string& filename) override {
        // The terminal for the extension, as Matplot++'s gnuplot backend picks it: the first
        // listed one gnuplot has (pngcairo before png).
        const std::string extension = pathFromUtf8(filename).extension().string();
        for (const auto& [listed, terminal] : matplot::backend::gnuplot::extension_terminal()) {
            if (listed == extension && matplot::backend::gnuplot::terminal_is_available(terminal)) {
                return output(filename, std::string(terminal));
            }
        }
        return false;
    }
    bool output(const std::string& filename, const std::string& format) override {
        _output = filename;
        _format = format;
        return true;
    }

    unsigned int width() override { return _size[0]; }
    unsigned int height() override { return _size[1]; }
    void width(unsigned int value) override { _size[0] = value; }
    void height(unsigned int value) override { _size[1] = value; }
    unsigned int position_x() override { return _position[0]; }
    unsigned int position_y() override { return _position[1]; }
    void position_x(unsigned int value) override { _position[0] = value; }
    void position_y(unsigned int value) override { _position[1] = value; }

    bool new_frame() override {
        _commands.clear();
        return true;
    }

    bool render_data() override {
        if (_output.empty() || _commands.empty()) return true;
        std::filesystem::path script = pathFromUtf8(_output);
        script += ".gp";
        {
            std::ofstream file(script, std::ios::binary);
            if (!file) return false;
            file << withDatablocks(_commands);
            // gnuplot finishes the output file when the output is changed or it exits.
            file << "unset output\n";
            if (!file) return false;
        }
        _commands.clear();
        // Drawn in the background; waitForPlots() (or the program's exit) waits for it, and a
        // script gnuplot failed on stays, to see why.
        GnuplotRun run;
        if (!startGnuplot(script, run)) {
            logError("Couldn't start gnuplot to draw " + pathToUtf8(script));
            return false;
        }
        pendingPlots().add(std::move(run));
        return true;
    }

    bool supports_fonts() override { return matplot::backend::gnuplot::terminal_has_font_option(_format); }
    bool consumes_gnuplot_commands() override { return true; }

    void run_command(const std::string& command) override {
        _commands += command;
        _commands += '\n';
    }
    void include_comment(const std::string&) override {}

private:
    std::string _output, _format;
    std::array<unsigned int, 2> _size{560, 420};
    std::array<unsigned int, 2> _position{680, 558};
    std::string _commands;
};

} // namespace

bool waitForPlots() { return pendingPlots().waitAll(); }

matplot::figure_handle newPlotFigure() {
    matplot::figure_handle figure = matplot::figure(true);
    // Keep the size Matplot++'s own backend started with.
    auto backend = std::make_shared<ScriptBackend>();
    backend->width(figure->backend()->width());
    backend->height(figure->backend()->height());
    figure->backend(backend);
    return figure;
}

} // namespace kiems
