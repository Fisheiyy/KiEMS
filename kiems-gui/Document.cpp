#include "Document.hpp"

#include "kiems/config.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>

#include <filesystem>

namespace kiems_gui {

namespace {

std::string utf8(const QString& text) {
    return text.toStdString();
}

QString qstr(const std::string& text) {
    return QString::fromStdString(text);
}

std::filesystem::path fsPath(const QString& path) {
    return std::filesystem::path(path.toStdU16String());
}

nlohmann::json::json_pointer pointerFor(const QString& pointer) {
    return nlohmann::json::json_pointer(utf8(pointer));
}

/// The first component of a JSON pointer: "/grid/margin/xy" -> "grid".
std::string topKey(const QString& pointer) {
    return utf8(pointer.section('/', 1, 1));
}

/// libkiems' document-level defaults: what a document without these keys is parsed as.
const nlohmann::json& documentDefaults() {
    static const nlohmann::json defaults = kiems::EMSConfig().toJson();
    return defaults;
}

/// A simulation's own defaults (kiems::SimulationConfig's member initializers).
const nlohmann::json& simulationDefaults() {
    static const nlohmann::json defaults = {
        {"via_edge_distance", 1500.0},
        {"via_spacing", 1500.0},
        {"eye_bit_rate", 0.0},
        {"eye_draw_count", 64},
        {"adversarial_shared_clock", false},
        {"is_differential_pair", false},
        {"edge_terminated_nets", nlohmann::json::array()},
        {"terminate_ground_at_cut", false},
        {"eye_test",
         {{"standard", "custom"}, {"signal_level", 1.0}, {"mask_x1", 0.0}, {"mask_x2", 0.0}, {"mask_y1", 0.0},
          {"mask_y2", 0.0}}},
    };
    return defaults;
}

/// Keys SimulationConfig's to_json() leaves out while they hold their default.
bool omittedAtDefault(const std::string& key) {
    return key == "eye_bit_rate" || key == "eye_draw_count" || key == "adversarial_shared_clock" ||
           key == "is_differential_pair" || key == "edge_terminated_nets" || key == "terminate_ground_at_cut" ||
           key == "eye_test";
}

/// Numbers compare by value, so 64 and 64.0 (or a document's 1500 and the default 1500.0) match.
bool sameValue(const nlohmann::json& a, const nlohmann::json& b) {
    if (a.is_number() && b.is_number()) return a.get<double>() == b.get<double>();
    if (a.is_object() && b.is_object()) {
        if (a.size() != b.size()) return false;
        for (auto it = a.begin(); it != a.end(); ++it) {
            if (!b.contains(it.key()) || !sameValue(it.value(), b.at(it.key()))) return false;
        }
        return true;
    }
    return a == b;
}

} // namespace

Document::Document(QObject* parent) : QObject(parent) {}

bool Document::open(const QString& path, QString* error) {
    QFileInfo info(path);
    const QString configPath = info.isDir() ? QDir(path).filePath("simulation.json") : info.absoluteFilePath();
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = "Couldn't open " + QDir::toNativeSeparators(configPath) + ": " + file.errorString();
        return false;
    }
    try {
        _json = nlohmann::json::parse(file.readAll().toStdString());
    } catch (const nlohmann::json::exception& exception) {
        if (error) *error = QDir::toNativeSeparators(configPath) + " isn't valid JSON: " + exception.what();
        return false;
    }
    if (!_json.is_object()) _json = nlohmann::json::object();
    if (!_json.contains("simulations") || !_json["simulations"].is_array()) _json["simulations"] = nlohmann::json::array();
    _configPath = QFileInfo(configPath).absoluteFilePath();
    _modified = false;
    emit modifiedChanged(false);
    emit changed();
    return true;
}

bool Document::create(const QString& packagePath, const QString& kicadPcbPath, QString* error) {
    if (!QDir().mkpath(packagePath)) {
        if (error) *error = "Couldn't create " + QDir::toNativeSeparators(packagePath);
        return false;
    }
    // A brand-new config, every field at libkiems' own defaults -- what a new macOS-app document has.
    kiems::EMSConfig config;
    config.setKicadPcbPath(fsPath(QFileInfo(kicadPcbPath).absoluteFilePath()));
    _json = config.toJson();
    if (!_json.contains("simulations")) _json["simulations"] = nlohmann::json::array();
    _configPath = QDir(packagePath).absoluteFilePath("simulation.json");
    _modified = true;
    return save(error);
}

bool Document::save(QString* error) {
    if (!isOpen()) return false;
    QSaveFile file(_configPath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = "Couldn't write " + QDir::toNativeSeparators(_configPath) + ": " + file.errorString();
        return false;
    }
    const std::string text = _json.dump(4) + "\n";
    file.write(text.data(), static_cast<qint64>(text.size()));
    if (!file.commit()) {
        if (error) *error = "Couldn't write " + QDir::toNativeSeparators(_configPath) + ": " + file.errorString();
        return false;
    }
    _modified = false;
    emit modifiedChanged(false);
    return true;
}

QString Document::packageDir() const {
    return QFileInfo(_configPath).absolutePath();
}

QString Document::displayName() const {
    if (!isOpen()) return {};
    const QFileInfo package(packageDir());
    return package.suffix() == "g2esim" ? package.completeBaseName() : package.fileName();
}

QString Document::kicadPcbPath() const {
    const auto it = _json.find("kicad_pcb_path");
    if (it == _json.end() || !it->is_string()) return {};
    const QString path = qstr(it->get<std::string>());
    // Stored absolute by both apps; resolve a relative one against the package, as the CLI does.
    return QFileInfo(path).isAbsolute() ? path : QDir(packageDir()).absoluteFilePath(path);
}

void Document::setKicadPcbPath(const QString& path) {
    _json["kicad_pcb_path"] = utf8(QFileInfo(path).absoluteFilePath());
    touch();
}

nlohmann::json* Document::simulation(int index) {
    auto& simulations = _json["simulations"];
    if (index < 0 || index >= static_cast<int>(simulations.size())) return nullptr;
    return &simulations[static_cast<std::size_t>(index)];
}

const nlohmann::json* Document::simulation(int index) const {
    const auto it = _json.find("simulations");
    if (it == _json.end() || index < 0 || index >= static_cast<int>(it->size())) return nullptr;
    return &(*it)[static_cast<std::size_t>(index)];
}

int Document::simulationCount() const {
    const auto it = _json.find("simulations");
    return it == _json.end() ? 0 : static_cast<int>(it->size());
}

QString Document::simulationName(int index) const {
    const nlohmann::json* sim = simulation(index);
    return sim ? qstr(sim->value("name", std::string())) : QString();
}

int Document::simulationIndex(const QString& name) const {
    for (int i = 0; i < simulationCount(); ++i) {
        if (simulationName(i) == name) return i;
    }
    return -1;
}

QString Document::uniqueSimulationName(const QString& base) const {
    QString name = base;
    for (int n = 2; simulationIndex(name) >= 0; ++n) name = QString("%1 %2").arg(base).arg(n);
    return name;
}

int Document::addSimulation(const QString& name, const QString& groundNet) {
    nlohmann::json sim = {{"name", utf8(name)},
                          {"involved_nets", nlohmann::json::array()},
                          {"ground_net", {{"net", utf8(groundNet)}}}};
    // Eyes are built at the board's bit rate when Board Setup has one, otherwise at the top of the
    // frequency range -- what the macOS app gives a new simulation.
    const nlohmann::json boardBitRate = documentValue("/board_setup/bit_rate");
    const double bitRate = boardBitRate.is_number() ? boardBitRate.get<double>() : 0.0;
    sim["eye_bit_rate"] = bitRate > 0 ? bitRate : documentValue("/frequency/stop").get<double>();
    _json["simulations"].push_back(std::move(sim));
    touch();
    return simulationCount() - 1;
}

void Document::removeSimulation(int index) {
    if (!simulation(index)) return;
    _json["simulations"].erase(static_cast<std::size_t>(index));
    touch();
}

void Document::renameSimulation(int index, const QString& name) {
    if (name == simulationName(index)) return;
    if (nlohmann::json* sim = simulation(index)) {
        const QString from = simulationName(index);
        (*sim)["name"] = utf8(name);
        touch();
        emit simulationRenamed(from, name);
    }
}

QStringList Document::involvedNets(int index) const {
    QStringList nets;
    if (const nlohmann::json* sim = simulation(index)) {
        for (const auto& entry : sim->value("involved_nets", nlohmann::json::array())) {
            if (entry.contains("net")) nets << qstr(entry["net"].get<std::string>());
        }
    }
    return nets;
}

void Document::setNetIncluded(int index, const QString& net, bool included, std::optional<int> planeForFirstNet) {
    nlohmann::json* sim = simulation(index);
    if (!sim) return;
    auto& nets = (*sim)["involved_nets"];
    if (!nets.is_array()) nets = nlohmann::json::array();
    const std::string name = utf8(net);
    for (std::size_t i = 0; i < nets.size(); ++i) {
        if (nets[i].value("net", std::string()) == name) {
            if (!included) {
                nets.erase(i);
                touch();
            }
            return;
        }
    }
    if (included) {
        nlohmann::json entry = {{"net", name}};
        // New nets share the simulation's reference plane, so one setting governs them all.
        if (auto plane = referencePlane(index)) entry["plane"] = *plane;
        else if (planeForFirstNet) entry["plane"] = *planeForFirstNet;
        nets.push_back(std::move(entry));
        touch();
    }
}

QString Document::groundNet(int index) const {
    const nlohmann::json* sim = simulation(index);
    if (!sim || !sim->contains("ground_net")) return {};
    return qstr((*sim)["ground_net"].value("net", std::string()));
}

void Document::setGroundNet(int index, const QString& net) {
    if (net == groundNet(index)) return;
    if (nlohmann::json* sim = simulation(index)) {
        (*sim)["ground_net"] = {{"net", utf8(net)}};
        touch();
    }
}

std::optional<int> Document::referencePlane(int index) const {
    if (const nlohmann::json* sim = simulation(index)) {
        for (const auto& entry : sim->value("involved_nets", nlohmann::json::array())) {
            if (entry.contains("plane")) return entry["plane"].get<int>();
        }
    }
    return std::nullopt;
}

void Document::setReferencePlane(int index, int plane) {
    nlohmann::json* sim = simulation(index);
    if (!sim || referencePlane(index) == plane) return;
    for (auto& entry : (*sim)["involved_nets"]) entry["plane"] = plane;
    touch();
}

std::optional<PinRef> Document::mainExcitation(int index) const {
    if (const nlohmann::json* sim = simulation(index)) {
        for (const auto& entry : sim->value("excitations", nlohmann::json::array())) {
            if (entry.value("main", false) && entry.contains("footprint") && entry.contains("pin")) {
                const auto& pin = entry["pin"];
                return PinRef{qstr(entry["footprint"].get<std::string>()),
                              pin.is_string() ? qstr(pin.get<std::string>()) : QString::number(pin.get<long long>())};
            }
        }
    }
    return std::nullopt;
}

void Document::setMainExcitation(int index, const std::optional<PinRef>& pin) {
    nlohmann::json* sim = simulation(index);
    if (!sim) return;
    auto& excitations = (*sim)["excitations"];
    if (!excitations.is_array()) excitations = nlohmann::json::array();
    nlohmann::json replacement = {{"main", true}};
    for (std::size_t i = 0; i < excitations.size();) {
        if (excitations[i].value("main", false)) {
            for (const char* key : {"start_time", "duration", "end_time", "phase"}) {
                if (excitations[i].contains(key)) replacement[key] = excitations[i][key];
            }
            excitations.erase(i);
        } else {
            ++i;
        }
    }
    if (pin) {
        replacement["footprint"] = utf8(pin->footprint);
        replacement["pin"] = utf8(pin->pin);
        excitations.push_back(std::move(replacement));
    }
    touch();
}

std::optional<double> Document::mainExcitationDuration(int index) const {
    if (const nlohmann::json* sim = simulation(index)) {
        for (const auto& entry : sim->value("excitations", nlohmann::json::array())) {
            if (!entry.value("main", false)) continue;
            if (entry.contains("duration")) return entry["duration"].get<double>();
            // The older form libkiems still reads: an end time rather than a duration.
            if (entry.contains("end_time")) return entry["end_time"].get<double>() - entry.value("start_time", 0.0);
            return 0.0;
        }
    }
    return std::nullopt;
}

QList<double> Document::mainExcitationDurations(int index) const {
    QList<double> durations;
    if (const nlohmann::json* sim = simulation(index)) {
        for (const auto& entry : sim->value("excitations", nlohmann::json::array())) {
            if (!entry.value("main", false)) continue;
            if (entry.contains("duration")) durations << entry["duration"].get<double>();
            else if (entry.contains("end_time")) durations << entry["end_time"].get<double>() - entry.value("start_time", 0.0);
            else durations << 0.0;
        }
    }
    return durations;
}

void Document::setMainExcitationDuration(int index, double seconds) {
    editSimulation(index, [seconds](nlohmann::json& sim) {
        if (!sim.contains("excitations")) return;
        for (auto& entry : sim["excitations"]) {
            if (!entry.value("main", false)) continue;
            entry.erase("end_time");
            entry["duration"] = seconds;
        }
    });
}

void Document::editSimulation(int index, const std::function<void(nlohmann::json&)>& edit) {
    nlohmann::json* sim = simulation(index);
    if (!sim) return;
    const nlohmann::json before = *sim;
    edit(*sim);
    if (*sim != before) touch();
}

nlohmann::json Document::documentValue(const QString& pointer) const {
    const auto path = pointerFor(pointer);
    if (_json.contains(path)) return _json.at(path);
    return documentDefaults().contains(path) ? documentDefaults().at(path) : nlohmann::json();
}

void Document::setDocumentValue(const QString& pointer, const nlohmann::json& value) {
    const auto path = pointerFor(pointer);
    if (sameValue(documentValue(pointer), value)) return;
    // A setting inside an object the document doesn't have yet starts that object from libkiems'
    // defaults, so its siblings keep meaning what they did.
    const std::string key = topKey(pointer);
    if (!_json.contains(key) && documentDefaults().contains(key)) _json[key] = documentDefaults().at(key);
    _json[path] = value;
    touch();
}

nlohmann::json Document::simulationValue(int index, const QString& pointer) const {
    const auto path = pointerFor(pointer);
    if (const nlohmann::json* sim = simulation(index); sim && sim->contains(path)) return sim->at(path);
    return simulationDefaults().contains(path) ? simulationDefaults().at(path) : nlohmann::json();
}

void Document::setSimulationValue(int index, const QString& pointer, const nlohmann::json& value) {
    nlohmann::json* sim = simulation(index);
    if (!sim) return;
    const auto path = pointerFor(pointer);
    if (sameValue(simulationValue(index, pointer), value)) return;
    const std::string key = topKey(pointer);
    if (!sim->contains(key) && simulationDefaults().contains(key)) (*sim)[key] = simulationDefaults().at(key);
    (*sim)[path] = value;
    if (omittedAtDefault(key) && sameValue((*sim)[key], simulationDefaults().at(key))) sim->erase(key);
    touch();
}

QString Document::resultsDir(int index) const {
    return QDir(packageDir()).filePath("ems/results/" + simulationName(index));
}

QString Document::simulationDir(int index) const {
    return QDir(packageDir()).filePath("ems/simulation/" + simulationName(index));
}

bool Document::hasGeometry(int index) const {
    return QFileInfo::exists(QDir(packageDir()).filePath("ems/geometry/" + simulationName(index) + "/geometry.json"));
}

bool Document::hasResults(int index) const {
    return !QDir(resultsDir(index)).entryList({"Port_*_data.csv"}, QDir::Files).isEmpty();
}

QString Document::validate() const {
    QTemporaryDir directory;
    if (!directory.isValid()) return {};
    const QString path = directory.filePath("simulation.json");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    file.write(QByteArray::fromStdString(_json.dump()));
    file.close();
    auto parsed = kiems::EMSConfig::parse(fsPath(path), false);
    return parsed ? QString() : qstr(parsed.error());
}

void Document::touch() {
    const bool wasModified = _modified;
    _modified = true;
    if (!wasModified) emit modifiedChanged(true);
    emit changed();
}

} // namespace kiems_gui
