#include "dc_board.hpp"

#include <matplot/matplot.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <sstream>

#include "path_utf8.hpp"

namespace kiems::dc {

namespace {

Cu::Polygon orientedLoop(const libkicad::PolygonLoop& loop) {
    Cu::Polygon path;
    path.reserve(loop.pointsMm.size());
    for (const auto& [x, y] : loop.pointsMm) path.emplace_back(x, y);
    // Outlines counter-clockwise and holes clockwise, as the polygon operations expect.
    if (path.size() >= 3 && Cu::isPositive(path) == loop.hole) std::reverse(path.begin(), path.end());
    return path;
}

std::string pinKey(const std::string& footprint, const std::string& pin) {
    return footprint + '\x1f' + pin;
}

std::string formatMm(double value) {
    std::ostringstream text;
    text.precision(3);
    text << std::fixed << value;
    return text.str();
}

} // namespace

void to_json(nlohmann::json& j, const AnalysisConfig& config) {
    const auto pins = [](const std::vector<PinValue>& values, const char* key) {
        nlohmann::json list = nlohmann::json::array();
        for (const PinValue& v : values) {
            nlohmann::json entry{{"footprint", v.footprint}, {"pin", v.pin}, {key, v.value}};
            if (v.minimumVoltage) entry["min_voltage"] = *v.minimumVoltage;
            list.push_back(std::move(entry));
        }
        return list;
    };
    j = nlohmann::json{{"name", config.name},
                       {"net", config.net},
                       {"sources", pins(config.sources, "voltage")},
                       {"loads", pins(config.loads, "current")},
                       {"temperature", config.options.temperatureC},
                       {"plating", config.options.platingMm},
                       {"max_element", config.options.maxElementMm},
                       {"load_pads", config.options.equipotentialLoadPads ? "equipotential" : "distributed"}};
    if (config.limits.outerCurrentDensity) j["max_current_density_outer"] = *config.limits.outerCurrentDensity;
    if (config.limits.innerCurrentDensity) j["max_current_density_inner"] = *config.limits.innerCurrentDensity;
    if (config.limits.viaCurrent) j["max_via_current"] = *config.limits.viaCurrent;
}

void from_json(const nlohmann::json& j, AnalysisConfig& config) {
    const auto pins = [&](const char* list, const char* key) {
        std::vector<PinValue> values;
        for (const nlohmann::json& entry : j.value(list, nlohmann::json::array())) {
            const nlohmann::json& pin = entry.at("pin");
            PinValue value{entry.at("footprint").get<std::string>(), pin.is_string() ? pin.get<std::string>() : pin.dump(),
                           entry.value(key, 0.0), std::nullopt};
            if (entry.contains("min_voltage") && entry["min_voltage"].is_number()) {
                value.minimumVoltage = entry["min_voltage"].get<double>();
            }
            values.push_back(std::move(value));
        }
        return values;
    };
    const Options defaults;
    config.net = j.at("net").get<std::string>();
    config.name = j.value("name", config.net);
    config.sources = pins("sources", "voltage");
    config.loads = pins("loads", "current");
    config.options.temperatureC = j.value("temperature", defaults.temperatureC);
    config.options.platingMm = j.value("plating", defaults.platingMm);
    config.options.maxElementMm = j.value("max_element", defaults.maxElementMm);
    config.options.equipotentialLoadPads = j.value("load_pads", std::string("equipotential")) != "distributed";
    const auto limit = [&](const char* key) {
        return j.contains(key) && j[key].is_number() ? std::optional(j[key].get<double>()) : std::nullopt;
    };
    config.limits.outerCurrentDensity = limit("max_current_density_outer");
    config.limits.innerCurrentDensity = limit("max_current_density_inner");
    config.limits.viaCurrent = limit("max_via_current");
}

std::vector<AnalysisConfig> analysesIn(const nlohmann::json& document) {
    std::vector<AnalysisConfig> analyses;
    for (const nlohmann::json& entry : document.value("dc_analyses", nlohmann::json::array())) {
        analyses.push_back(entry.get<AnalysisConfig>());
    }
    return analyses;
}

std::expected<Problem, std::string> buildProblem(const libkicad::Board& board, const AnalysisConfig& config) {
    auto stackup = board.stackup();
    if (!stackup) return std::unexpected("Couldn't read the board's stackup: " + stackup.error());
    auto geometry = board.boardGeometry();
    if (!geometry) return std::unexpected("Couldn't read the board's copper: " + geometry.error());
    auto holes = board.throughHoles();
    if (!holes) return std::unexpected("Couldn't read the board's plated holes: " + holes.error());
    return buildProblem(*stackup, *geometry, *holes, config);
}

std::expected<Problem, std::string> buildProblem(const std::vector<libkicad::StackupLayer>& stackup,
                                                 const libkicad::BoardGeometry& geometry,
                                                 const std::vector<libkicad::ThroughHole>& holes,
                                                 const AnalysisConfig& config) {
    Problem problem;
    problem.options = config.options;

    // Copper layers top to bottom, each at the middle of its thickness.
    std::map<std::string, int> layerIndex;
    double z = 0;
    for (const libkicad::StackupLayer& layer : stackup) {
        if (layer.kind == libkicad::StackupLayerKind::Copper) {
            layerIndex[layer.name] = static_cast<int>(problem.layers.size());
            problem.layers.push_back({layer.name, layer.thicknessMm > 0 ? layer.thicknessMm : 0.035, z - layer.thicknessMm / 2, {}});
        }
        z -= layer.thicknessMm;
    }
    if (problem.layers.empty()) return std::unexpected("The board's stackup has no copper layers.");

    // The net's copper, and each pad's own piece of it.
    std::map<std::string, std::map<int, Cu::PolygonSet>> padCopper; // pin -> layer -> loops
    for (const libkicad::CopperPolygon& polygon : geometry.copper) {
        if (polygon.netName != config.net) continue;
        const auto layer = layerIndex.find(polygon.copperLayerName);
        if (layer == layerIndex.end()) continue;
        Cu::Polygon loop = orientedLoop(polygon.loop);
        if (loop.size() < 3) continue;
        if (!polygon.footprintRef.empty()) padCopper[pinKey(polygon.footprintRef, polygon.padNumber)][layer->second].push_back(loop);
        problem.layers[static_cast<std::size_t>(layer->second)].copper.push_back(std::move(loop));
    }

    for (const libkicad::ThroughHole& hole : holes) {
        if (hole.netName != config.net) continue;
        const double drill = std::min(hole.drillWidthMm, hole.drillHeightMm);
        if (drill <= 0) continue;
        Barrel barrel;
        barrel.name = hole.footprintRef.empty() ? "via at (" + formatMm(hole.xMm) + ", " + formatMm(hole.yMm) + ") mm"
                                                : hole.footprintRef + " pin " + hole.padNumber;
        barrel.x = hole.xMm;
        barrel.y = hole.yMm;
        barrel.drillMm = drill;
        const double pad = std::max(hole.padWidthMm, hole.padHeightMm);
        barrel.ringMm = pad > drill ? pad : drill * 1.5;
        barrel.firstLayer = 0;
        barrel.lastLayer = static_cast<int>(problem.layers.size()) - 1;
        problem.barrels.push_back(std::move(barrel));
    }

    // Each pin's pad, on the topmost layer it has copper on (a through-hole pad's barrel reaches
    // the rest).
    const auto addTerminal = [&](const PinValue& pin, Terminal::Kind kind) -> std::expected<void, std::string> {
        const auto found = padCopper.find(pinKey(pin.footprint, pin.pin));
        if (found == padCopper.end() || found->second.empty()) {
            return std::unexpected(pin.footprint + " pin " + pin.pin + " has no pad on " + config.net + ".");
        }
        Terminal terminal;
        terminal.name = pin.footprint + " pin " + pin.pin;
        terminal.kind = kind;
        terminal.layer = found->second.begin()->first;
        terminal.pad = found->second.begin()->second;
        terminal.value = pin.value;
        problem.terminals.push_back(std::move(terminal));
        return {};
    };
    if (config.sources.empty()) return std::unexpected("The analysis has no source.");
    for (const PinValue& pin : config.sources) {
        if (auto added = addTerminal(pin, Terminal::Kind::Source); !added) return std::unexpected(added.error());
    }
    for (const PinValue& pin : config.loads) {
        if (auto added = addTerminal(pin, Terminal::Kind::Load); !added) return std::unexpected(added.error());
    }
    return problem;
}

double voltageFromNetName(const std::string& net) {
    const std::string leaf = net.substr(net.find_last_of('/') + 1);
    std::smatch match;
    // "1V8" is 1.8 V.
    static const std::regex split(R"((\d+)[Vv](\d+))");
    if (std::regex_search(leaf, match, split)) return std::stod(match[1].str() + "." + match[2].str());
    static const std::regex plain(R"((\d+(?:[.,]\d+)?)\s*[Vv](?![A-Za-z]))");
    if (std::regex_search(leaf, match, plain)) {
        std::string number = match[1].str();
        std::replace(number.begin(), number.end(), ',', '.');
        return std::stod(number);
    }
    return 0;
}

std::expected<AnalysisConfig, std::string> automaticAnalysis(const libkicad::Board& board, const std::string& net,
                                                             double volts, double totalCurrent) {
    auto pads = board.padsOnNet(net);
    if (!pads) return std::unexpected("Couldn't read " + net + "'s pins: " + pads.error());
    return automaticAnalysis(*pads, net, volts, totalCurrent);
}

AnalysisConfig automaticAnalysis(const std::vector<libkicad::PadPosition>& pads, const std::string& net, double volts,
                                 double totalCurrent) {
    AnalysisConfig config;
    config.name = net;
    config.net = net;
    if (volts <= 0) volts = voltageFromNetName(net);
    for (const libkicad::PadPosition& pad : pads) {
        if (pad.netName != net) continue;
        if (pad.pinType == "power_out") config.sources.push_back({pad.footprintRef, pad.padNumber, volts, std::nullopt});
        else if (pad.pinType == "power_in") config.loads.push_back({pad.footprintRef, pad.padNumber, 0, std::nullopt});
    }
    for (PinValue& load : config.loads) load.value = totalCurrent / static_cast<double>(config.loads.size());
    return config;
}

std::vector<Violation> findViolations(const Problem& problem, const Result& result, const AnalysisConfig& config) {
    std::vector<Violation> violations;
    const auto format = [](double value, int precision) {
        std::ostringstream text;
        text.precision(precision);
        text << value;
        return text.str();
    };
    // Loads below their minimum. problem.terminals and result.terminals are in the same order.
    for (std::size_t t = 0; t < result.terminals.size() && t < problem.terminals.size(); ++t) {
        const TerminalResult& terminal = result.terminals[t];
        if (terminal.kind != Terminal::Kind::Load || !terminal.connected) continue;
        for (const PinValue& load : config.loads) {
            if (load.footprint + " pin " + load.pin != terminal.name || !load.minimumVoltage) continue;
            if (terminal.voltage >= *load.minimumVoltage * (1 - 1e-9)) continue; // not for rounding
            Cu::BoundingBox<double> box;
            for (const Cu::Polygon& loop : problem.terminals[t].pad) {
                for (const Cu::Position& p : loop) {
                    box.xMin = std::min(box.xMin, p.x());
                    box.xMax = std::max(box.xMax, p.x());
                    box.yMin = std::min(box.yMin, p.y());
                    box.yMax = std::max(box.yMax, p.y());
                }
            }
            violations.push_back({Violation::Kind::Voltage,
                                  terminal.name + " is at " + format(terminal.voltage, 5) + " V, below its " +
                                      format(*load.minimumVoltage, 5) + " V minimum",
                                  terminal.voltage, *load.minimumVoltage, problem.terminals[t].layer,
                                  (box.xMin + box.xMax) / 2, (box.yMin + box.yMax) / 2});
        }
    }
    // Each layer's densest element, against its layer's limit.
    for (std::size_t l = 0; l < result.layers.size(); ++l) {
        const bool outer = l == 0 || l + 1 == result.layers.size();
        const std::optional<double> limit = outer ? config.limits.outerCurrentDensity : config.limits.innerCurrentDensity;
        const LayerResult& layer = result.layers[l];
        if (!limit || layer.maxCurrentDensity <= *limit * (1 + 1e-9) || layer.triangles.empty()) continue;
        std::size_t worst = 0;
        double highest = -1;
        for (std::size_t e = 0; e < layer.currentDensity.size(); ++e) {
            const double j = std::hypot(layer.currentDensity[e][0], layer.currentDensity[e][1]);
            if (j > highest) {
                highest = j;
                worst = e;
            }
        }
        double x = 0, y = 0;
        for (int k = 0; k < 3; ++k) {
            x += layer.nodes[static_cast<std::size_t>(layer.triangles[worst][k])].x() / 3;
            y += layer.nodes[static_cast<std::size_t>(layer.triangles[worst][k])].y() / 3;
        }
        violations.push_back({Violation::Kind::CurrentDensity,
                              layer.name + " carries up to " + format(highest, 4) + " A/mm^2, over its " +
                                  format(*limit, 4) + " A/mm^2 limit",
                              highest, *limit, static_cast<int>(l), x, y});
    }
    // Barrels over the via limit.
    if (config.limits.viaCurrent) {
        for (std::size_t b = 0; b < result.barrels.size() && b < problem.barrels.size(); ++b) {
            const BarrelResult& barrel = result.barrels[b];
            if (!barrel.connected || barrel.current <= *config.limits.viaCurrent * (1 + 1e-9)) continue;
            violations.push_back({Violation::Kind::ViaCurrent,
                                  barrel.name + " carries " + format(barrel.current, 4) + " A, over the " +
                                      format(*config.limits.viaCurrent, 4) + " A limit",
                                  barrel.current, *config.limits.viaCurrent, -1, problem.barrels[b].x,
                                  problem.barrels[b].y});
        }
    }
    return violations;
}


namespace {

/// One layer's solution sampled on a square grid, NaN off the copper.
struct Raster {
    double xMin = 0, yMin = 0, pixel = 1;
    int width = 0, height = 0;
    std::vector<std::vector<double>> rows; // rows[0] at yMin

    void fill(const LayerResult& layer, bool currentDensity) {
        double xMax = -std::numeric_limits<double>::infinity(), yMax = xMax;
        xMin = yMin = std::numeric_limits<double>::infinity();
        for (const Cu::Position& p : layer.nodes) {
            xMin = std::min(xMin, p.x());
            yMin = std::min(yMin, p.y());
            xMax = std::max(xMax, p.x());
            yMax = std::max(yMax, p.y());
        }
        pixel = std::max(xMax - xMin, yMax - yMin) / 800;
        if (!(pixel > 0)) pixel = 1;
        width = static_cast<int>(std::ceil((xMax - xMin) / pixel)) + 1;
        height = static_cast<int>(std::ceil((yMax - yMin) / pixel)) + 1;
        rows.assign(static_cast<std::size_t>(height),
                    std::vector<double>(static_cast<std::size_t>(width), std::numeric_limits<double>::quiet_NaN()));
        for (std::size_t e = 0; e < layer.triangles.size(); ++e) {
            const auto& t = layer.triangles[e];
            const Cu::Position& a = layer.nodes[static_cast<std::size_t>(t[0])];
            const Cu::Position& b = layer.nodes[static_cast<std::size_t>(t[1])];
            const Cu::Position& c = layer.nodes[static_cast<std::size_t>(t[2])];
            const double det = (b.y() - c.y()) * (a.x() - c.x()) + (c.x() - b.x()) * (a.y() - c.y());
            if (det == 0) continue;
            const double magnitude = std::hypot(layer.currentDensity[e][0], layer.currentDensity[e][1]);
            const int i0 = std::max(0, static_cast<int>((std::min({a.x(), b.x(), c.x()}) - xMin) / pixel));
            const int i1 = std::min(width - 1, static_cast<int>((std::max({a.x(), b.x(), c.x()}) - xMin) / pixel) + 1);
            const int j0 = std::max(0, static_cast<int>((std::min({a.y(), b.y(), c.y()}) - yMin) / pixel));
            const int j1 = std::min(height - 1, static_cast<int>((std::max({a.y(), b.y(), c.y()}) - yMin) / pixel) + 1);
            for (int j = j0; j <= j1; ++j) {
                for (int i = i0; i <= i1; ++i) {
                    const double x = xMin + i * pixel, y = yMin + j * pixel;
                    const double l0 = ((b.y() - c.y()) * (x - c.x()) + (c.x() - b.x()) * (y - c.y())) / det;
                    const double l1 = ((c.y() - a.y()) * (x - c.x()) + (a.x() - c.x()) * (y - c.y())) / det;
                    const double l2 = 1 - l0 - l1;
                    if (l0 < -1e-9 || l1 < -1e-9 || l2 < -1e-9) continue;
                    rows[static_cast<std::size_t>(j)][static_cast<std::size_t>(i)] =
                        currentDensity ? magnitude
                                       : l0 * layer.voltage[static_cast<std::size_t>(t[0])] +
                                             l1 * layer.voltage[static_cast<std::size_t>(t[1])] +
                                             l2 * layer.voltage[static_cast<std::size_t>(t[2])];
                }
            }
        }
    }
};

void plotLayer(const LayerResult& layer, const AnalysisConfig& config, bool currentDensity,
               const std::filesystem::path& path) {
    Raster raster;
    raster.fill(layer, currentDensity);
    auto figure = matplot::figure(true);
    figure->size(1000, 800);
    auto axes = figure->current_axes();
    axes->image(raster.xMin, raster.xMin + raster.pixel * (raster.width - 1), raster.yMin,
                raster.yMin + raster.pixel * (raster.height - 1), raster.rows, true);
    axes->y_axis().reverse(false);
    axes->colormap(currentDensity ? matplot::palette::hot() : matplot::palette::parula());
    axes->color_box(true);
    axes->axis(matplot::equal);
    axes->xlabel("x (mm)");
    axes->ylabel("y (mm)");
    axes->title(config.name + " on " + layer.name + (currentDensity ? ": current density (A/mm^2)" : ": voltage (V)"));
    // Matplot++ writes the name into a gnuplot string, where a backslash would start an escape.
    figure->save(pathToGenericUtf8(path));
}

std::string fileStem(const std::string& name) {
    std::string stem;
    for (char c : name) stem += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_';
    return stem;
}

} // namespace

std::expected<void, std::string> writeResult(const Result& result, const AnalysisConfig& config,
                                             const std::vector<Violation>& violations,
                                             const std::filesystem::path& directory, bool plots) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return std::unexpected("Couldn't create " + pathToUtf8(directory) + ": " + error.message());
    nlohmann::json json = result;
    json["analysis"] = config;
    auto& list = json["violations"] = nlohmann::json::array();
    for (const Violation& v : violations) {
        list.push_back({{"kind", v.kind == Violation::Kind::Voltage          ? "voltage"
                                 : v.kind == Violation::Kind::CurrentDensity ? "current_density"
                                                                             : "via_current"},
                        {"message", v.message},
                        {"value", v.value},
                        {"limit", v.limit},
                        {"x", v.x},
                        {"y", v.y}});
    }
    std::ofstream file(directory / "result.json");
    if (!file) return std::unexpected("Couldn't write " + pathToUtf8(directory / "result.json"));
    file << json.dump(4) << "\n";
    file.close();
    if (!plots) return {};
    for (const LayerResult& layer : result.layers) {
        if (layer.triangles.empty()) continue;
        plotLayer(layer, config, false, directory / (fileStem(layer.name) + "_voltage.png"));
        plotLayer(layer, config, true, directory / (fileStem(layer.name) + "_current_density.png"));
    }
    return {};
}

} // namespace kiems::dc
