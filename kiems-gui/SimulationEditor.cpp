#include "SimulationEditor.hpp"

#include "kiems/port_resolution.hpp"

#include <QRegularExpression>

#include <algorithm>

namespace kiems_gui {

namespace {

using Json = nlohmann::json;

std::string utf8(const QString& text) {
    return text.toStdString();
}

QString qstr(const std::string& text) {
    return QString::fromStdString(text);
}

/// An excitation's or entry's pin, which older documents may store as a number.
std::string pinText(const Json& pin) {
    if (pin.is_string()) return pin.get<std::string>();
    if (pin.is_number_integer()) return std::to_string(pin.get<long long>());
    return pin.dump();
}

/// The key an involved-net entry is matched by when deciding whether an edit changed it.
std::string entryKey(const Json& entry) {
    if (entry.contains("net")) return "net:" + entry["net"].get<std::string>();
    if (entry.contains("net_class")) return "class:" + entry["net_class"].get<std::string>();
    if (entry.contains("footprint")) return "pin:" + entry["footprint"].get<std::string>() + "." + entry.value("pins", Json::array()).dump();
    return entry.dump();
}

std::string entryKey(const kiems::InvolvedNetConfig& entry) {
    return entryKey(Json(entry));
}

/// GroundNetHeuristic.bestGuess(among: [name]) != nil, from the macOS app.
bool looksLikeGround(const QString& name) {
    static const QStringList wellKnown = {"GND", "GROUND", "VSS", "GRND"};
    for (const QString& candidate : wellKnown) {
        if (name.compare(candidate, Qt::CaseInsensitive) == 0) return true;
    }
    static const QRegularExpression qualifiedPrefix(QStringLiteral("^(?:GND|GROUND|VSS|GRND)[-_. ]?[A-Za-z0-9]*$"),
                                                    QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression qualifiedSuffix(QStringLiteral("^[A-Za-z0-9]*[-_. ]?(?:GND|GROUND|VSS|GRND)$"),
                                                    QRegularExpression::CaseInsensitiveOption);
    if (qualifiedPrefix.match(name).hasMatch() || qualifiedSuffix.match(name).hasMatch()) return true;
    return name.contains("gnd", Qt::CaseInsensitive) || name.contains("ground", Qt::CaseInsensitive);
}

} // namespace

SimulationEditor::SimulationEditor(Document& document, const BoardData* board, int simulation,
                                   std::optional<int> defaultPlane)
    : _document(document), _board(board), _simulation(simulation), _defaultPlane(defaultPlane) {}

bool SimulationEditor::isValid() const {
    return _document.simulationJson(_simulation) != nullptr;
}

// ---------------------------------------------------------------------------------------------
// Heuristics (the macOS app's PowerNetHeuristic, DifferentialPairNetHeuristic and isPassive)

bool SimulationEditor::isPowerNet(const QString& net) {
    // KiCad hierarchical nets are "/sheet/NAME"; only the last component names the rail.
    const QString leaf = net.section('/', -1).toUpper();
    static const QRegularExpression patterns[] = {
        QRegularExpression(QStringLiteral(R"(^[+-]?[0-9]+([.,][0-9]+)?V[0-9]*$)")),
        QRegularExpression(
            QStringLiteral(R"(^[AD]?(VCC|VDD|VEE|VSS|VBUS|VBAT|VIN|VSYS|VREF|VPP|VMOT|PWR|POWER)([-_. ]?[A-Z0-9.]*)?$)")),
        QRegularExpression(QStringLiteral(R"(^[ADP]?(GND|GROUND|GRND)([-_. ]?[A-Z0-9]*)?$)")),
    };
    for (const QRegularExpression& pattern : patterns) {
        if (pattern.match(leaf).hasMatch()) return true;
    }
    return looksLikeGround(net);
}

QStringList SimulationEditor::differentialPartnerCandidates(const QString& net) {
    static const std::pair<QString, QString> suffixes[] = {
        {"_P", "_N"}, {"_N", "_P"}, {"+", "-"}, {"-", "+"}, {"P", "N"}, {"N", "P"},
    };
    QStringList candidates;
    for (const auto& [suffix, replacement] : suffixes) {
        if (net.endsWith(suffix) && net.size() > suffix.size()) candidates << net.chopped(suffix.size()) + replacement;
    }
    return candidates;
}

bool SimulationEditor::isPassive(const QString& reference) {
    QString prefix;
    for (const QChar c : reference) {
        if (!c.isLetter()) break;
        prefix += c;
    }
    prefix = prefix.toUpper();
    return prefix == "R" || prefix == "L" || prefix == "C";
}

// ---------------------------------------------------------------------------------------------
// Board queries

const libkicad::FootprintPin* SimulationEditor::boardPin(const QString& footprint, const QString& pin) const {
    if (!_board) return nullptr;
    const std::string reference = utf8(footprint), number = utf8(pin);
    for (const libkicad::FootprintInfo& info : _board->footprints) {
        if (info.reference != reference) continue;
        for (const libkicad::FootprintPin& candidate : info.pins) {
            if (candidate.number == number) return &candidate;
        }
    }
    return nullptr;
}

QString SimulationEditor::pinNet(const QString& footprint, const QString& pin) const {
    const libkicad::FootprintPin* info = boardPin(footprint, pin);
    return info ? qstr(info->netName) : QString();
}

QString SimulationEditor::partnerNet(const QString& net) const {
    if (!_board) return {};
    for (const QString& candidate : differentialPartnerCandidates(net)) {
        const std::string name = utf8(candidate);
        for (const libkicad::FootprintInfo& info : _board->footprints) {
            for (const libkicad::FootprintPin& pin : info.pins) {
                if (pin.netName == name) return candidate;
            }
        }
    }
    return {};
}

std::optional<QString> SimulationEditor::partnerPin(const QString& footprint, const QString& net) const {
    if (!_board) return std::nullopt;
    const std::string reference = utf8(footprint);
    for (const libkicad::FootprintInfo& info : _board->footprints) {
        if (info.reference != reference) continue;
        for (const QString& candidate : differentialPartnerCandidates(net)) {
            for (const libkicad::FootprintPin& pin : info.pins) {
                if (pin.netName == utf8(candidate)) return qstr(pin.number);
            }
        }
    }
    return std::nullopt;
}

bool SimulationEditor::isDifferentialPair() const {
    const Json value = _document.simulationValue(_simulation, "/is_differential_pair");
    return value.is_boolean() && value.get<bool>();
}

QStringList SimulationEditor::groundNets() const {
    const Json* sim = _document.simulationJson(_simulation);
    if (!sim || !sim->contains("ground_net")) return {};
    const Json& ground = (*sim)["ground_net"];
    if (ground.contains("net")) return {qstr(ground["net"].get<std::string>())};
    QStringList nets;
    if (ground.contains("net_class") && _board) {
        const std::string netClass = ground["net_class"].get<std::string>();
        for (const auto& [net, assigned] : _board->netClasses) {
            if (assigned == netClass) nets << qstr(net);
        }
    }
    return nets;
}

bool SimulationEditor::absorbsByDefault(const QString& footprint, const QString& pin, const QString& net) const {
    const libkicad::FootprintPin* info = boardPin(footprint, pin);
    if (!info || !_board) return false;
    // A pad alone on its net (an NC ball) has no line to terminate.
    int padsOnNet = 0;
    for (const libkicad::FootprintInfo& footprintInfo : _board->footprints) {
        for (const libkicad::FootprintPin& other : footprintInfo.pins) {
            if (other.netName == info->netName) ++padsOnNet;
        }
    }
    return padsOnNet > 1 && kiems::pinTypeAbsorbsByDefault(info->pinType, groundNets().contains(net));
}

kiems::BoardSetupConfig SimulationEditor::boardSetup() const {
    const Json value = _document.documentValue("/board_setup");
    if (!value.is_object()) return {};
    try {
        return value.get<kiems::BoardSetupConfig>();
    } catch (const std::exception&) {
        return {};
    }
}

std::optional<int> SimulationEditor::planeForNewNet(const Json& sim) const {
    // A new net shares the simulation's reference plane, so one setting governs them all.
    for (const Json& entry : sim.value("involved_nets", Json::array())) {
        if (entry.contains("plane")) return entry["plane"].get<int>();
    }
    return _defaultPlane;
}

// ---------------------------------------------------------------------------------------------
// JSON plumbing

int SimulationEditor::netEntryIndex(const Json& sim, const std::string& net) {
    const Json& entries = sim.value("involved_nets", Json::array());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].value("net", std::string()) == net && !entries[i].contains("net_class")) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::optional<kiems::InvolvedNetConfig> SimulationEditor::netEntry(const QString& net) const {
    const Json* sim = _document.simulationJson(_simulation);
    if (!sim) return std::nullopt;
    const int index = netEntryIndex(*sim, utf8(net));
    if (index < 0) return std::nullopt;
    try {
        return (*sim)["involved_nets"][static_cast<std::size_t>(index)].get<kiems::InvolvedNetConfig>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void SimulationEditor::editNets(const std::function<void(Json& sim, std::vector<kiems::InvolvedNetConfig>& entries)>& edit) {
    _document.editSimulation(_simulation, [&](Json& sim) {
        const Json originals = sim.value("involved_nets", Json::array());
        std::vector<kiems::InvolvedNetConfig> entries;
        try {
            for (const Json& entry : originals) entries.push_back(entry.get<kiems::InvolvedNetConfig>());
        } catch (const std::exception&) {
            return; // an entry libkiems can't read: leave the simulation alone
        }
        edit(sim, entries);
        // Entries the edit didn't change keep their exact original text.
        Json result = Json::array();
        for (const kiems::InvolvedNetConfig& entry : entries) {
            Json written = entry;
            const std::string key = entryKey(written);
            for (const Json& original : originals) {
                if (entryKey(original) != key) continue;
                if (Json(original.get<kiems::InvolvedNetConfig>()) == written) written = original;
                break;
            }
            result.push_back(std::move(written));
        }
        sim["involved_nets"] = std::move(result);
    });
}

kiems::InvolvedNetConfig& SimulationEditor::includeNet(Json& sim, std::vector<kiems::InvolvedNetConfig>& entries,
                                                       const std::string& net, bool simulated) const {
    for (kiems::InvolvedNetConfig& entry : entries) {
        if (entry.kind() != kiems::NetSelectorKind::Net || entry.net() != net) continue;
        if (simulated && entry.inclusionLevel() != kiems::NetInclusionLevel::SimulationNet) {
            entry.setInclusionLevel(kiems::NetInclusionLevel::SimulationNet);
            addNoiseSources(sim, net);
        }
        return entry;
    }
    kiems::InvolvedNetConfig entry;
    entry.setKind(kiems::NetSelectorKind::Net);
    entry.setNet(net);
    entry.useExplicitPinSelections();
    entry.setInclusionLevel(simulated ? kiems::NetInclusionLevel::SimulationNet : kiems::NetInclusionLevel::GeometryOnly);
    if (const auto plane = planeForNewNet(sim)) entry.setPlane(*plane);

    // Board Setup's defaults, applied once on creation so a later edit here is never overwritten.
    const kiems::BoardSetupConfig setup = boardSetup();
    std::string netClass;
    if (_board) {
        const auto it = _board->netClasses.find(net);
        if (it != _board->netClasses.end()) netClass = it->second;
    }
    entry.setImpedance(setup.effectiveNetImpedance(net, netClass, isPowerNet(qstr(net))));
    if (_board) {
        for (const libkicad::FootprintInfo& info : _board->footprints) {
            for (const libkicad::FootprintPin& pin : info.pins) {
                if (pin.netName != net) continue;
                const kiems::BoardPinSetup* pinSetup = setup.pin(info.reference, pin.number);
                if (!pinSetup) continue;
                if (pinSetup->impedance) entry.setPinImpedance(info.reference, pin.number, pinSetup->impedance);
                if (pinSetup->absorbing) entry.setPinAbsorbOnly(info.reference, pin.number, *pinSetup->absorbing);
            }
        }
    }
    entries.push_back(std::move(entry));
    if (simulated) addNoiseSources(sim, net);
    return entries.back();
}

void SimulationEditor::addNoiseSources(Json& sim, const std::string& net) const {
    // Board Setup's noise-generating pins become adversarial excitations once their net is
    // simulated; a pin that already has an excitation keeps it.
    if (!_board) return;
    const kiems::BoardSetupConfig setup = boardSetup();
    for (const libkicad::FootprintInfo& info : _board->footprints) {
        for (const libkicad::FootprintPin& pin : info.pins) {
            if (pin.netName != net) continue;
            const kiems::BoardPinSetup* pinSetup = setup.pin(info.reference, pin.number);
            if (!pinSetup || !pinSetup->noiseGenerating || excitationIndex(sim, info.reference, pin.number) >= 0) continue;
            kiems::ExcitationConfig excitation;
            excitation.setFootprint(info.reference);
            excitation.setPin(pin.number);
            excitation.setIsMain(false);
            excitation.setDurationMode(kiems::ExcitationDurationMode::Continuous);
            excitation.setFrequency(pinSetup->noiseFrequency);
            excitation.setAmplitude(pinSetup->noiseAmplitude);
            if (!sim.contains("excitations")) sim["excitations"] = Json::array();
            sim["excitations"].push_back(Json(excitation));
        }
    }
}

int SimulationEditor::excitationIndex(const Json& sim, const std::string& footprint, const std::string& pin) {
    const Json& excitations = sim.value("excitations", Json::array());
    for (std::size_t i = 0; i < excitations.size(); ++i) {
        const Json& entry = excitations[i];
        if (entry.contains("hull_cut_port") || !entry.contains("footprint") || !entry.contains("pin")) continue;
        if (entry["footprint"].get<std::string>() == footprint && pinText(entry["pin"]) == pin) return static_cast<int>(i);
    }
    return -1;
}

void SimulationEditor::markPair(std::vector<kiems::InvolvedNetConfig>& entries, const std::string& first,
                                const std::string& second) {
    kiems::InvolvedNetConfig* a = nullptr;
    kiems::InvolvedNetConfig* b = nullptr;
    for (kiems::InvolvedNetConfig& entry : entries) {
        if (entry.kind() != kiems::NetSelectorKind::Net) continue;
        if (entry.net() == first) a = &entry;
        if (entry.net() == second) b = &entry;
    }
    if (!a || !b) return;
    a->setDifferentialPairPartner(second);
    b->setDifferentialPairPartner(first);
    a->setSimulateAsDifferentialPair(true);
    b->setSimulateAsDifferentialPair(true);
}

void SimulationEditor::complement(kiems::ExcitationConfig& partner, const kiems::ExcitationConfig& source) {
    // An ideal odd-mode drive relative to `source`.
    partner.setIsMain(source.isMain());
    partner.setDurationMode(source.durationMode());
    partner.setStartTime(source.startTime());
    partner.setDuration(source.duration());
    partner.setFrequency(source.frequency());
    partner.setPhaseDegrees(source.phaseDegrees());
    partner.setAmplitude(-source.amplitude().value_or(1.0));
}

// ---------------------------------------------------------------------------------------------
// Nets

NetLevel SimulationEditor::netLevel(const QString& net) const {
    const auto entry = netEntry(net);
    if (!entry) return NetLevel::None;
    return entry->inclusionLevel() == kiems::NetInclusionLevel::SimulationNet ? NetLevel::Simulated
                                                                              : NetLevel::GeometryOnly;
}

void SimulationEditor::setNetIncluded(const QString& net, bool included) {
    if (net.isEmpty()) return;
    const QString partner = isDifferentialPair() ? partnerNet(net) : QString();
    editNets([&](Json& sim, std::vector<kiems::InvolvedNetConfig>& entries) {
        auto apply = [&](const QString& name) {
            if (included) {
                includeNet(sim, entries, utf8(name), false);
            } else {
                std::erase_if(entries, [&](const kiems::InvolvedNetConfig& entry) {
                    return entry.kind() == kiems::NetSelectorKind::Net && entry.net() == utf8(name);
                });
            }
        };
        apply(net);
        if (!partner.isEmpty()) {
            apply(partner);
            if (included) markPair(entries, utf8(net), utf8(partner));
        }
    });
}

void SimulationEditor::setNetSimulated(const QString& net, bool simulated) {
    if (net.isEmpty()) return;
    editNets([&](Json& sim, std::vector<kiems::InvolvedNetConfig>& entries) {
        if (simulated) {
            includeNet(sim, entries, utf8(net), true);
            return;
        }
        // Demoting never removes the entry: the net's copper stays in the geometry.
        for (kiems::InvolvedNetConfig& entry : entries) {
            if (entry.kind() == kiems::NetSelectorKind::Net && entry.net() == utf8(net)) {
                entry.setInclusionLevel(kiems::NetInclusionLevel::GeometryOnly);
            }
        }
    });
}

double SimulationEditor::hullPadding(const QString& net) const {
    const auto entry = netEntry(net);
    return entry ? entry->hullPadding() : kiems::InvolvedNetConfig().hullPadding();
}

void SimulationEditor::setHullPadding(const QString& net, double micrometres) {
    editNets([&](Json&, std::vector<kiems::InvolvedNetConfig>& entries) {
        for (kiems::InvolvedNetConfig& entry : entries) {
            if (entry.kind() == kiems::NetSelectorKind::Net && entry.net() == utf8(net)) {
                entry.setHullPadding(std::max(0.0, micrometres));
            }
        }
    });
}

bool SimulationEditor::impedanceProbed(const QString& net) const {
    const auto entry = netEntry(net);
    return entry && entry->probeImpedance();
}

void SimulationEditor::setImpedanceProbed(const QString& net, bool probed) {
    editNets([&](Json&, std::vector<kiems::InvolvedNetConfig>& entries) {
        for (kiems::InvolvedNetConfig& entry : entries) {
            if (entry.kind() == kiems::NetSelectorKind::Net && entry.net() == utf8(net)) entry.setProbeImpedance(probed);
        }
    });
}

double SimulationEditor::netImpedance(const QString& net) const {
    if (const auto entry = netEntry(net)) return entry->impedance();
    std::string netClass;
    if (_board) {
        const auto it = _board->netClasses.find(utf8(net));
        if (it != _board->netClasses.end()) netClass = it->second;
    }
    return boardSetup().effectiveNetImpedance(utf8(net), netClass, isPowerNet(net));
}

void SimulationEditor::setNetImpedance(const QString& net, double ohms) {
    editNets([&](Json&, std::vector<kiems::InvolvedNetConfig>& entries) {
        for (kiems::InvolvedNetConfig& entry : entries) {
            if (entry.kind() == kiems::NetSelectorKind::Net && entry.net() == utf8(net)) entry.setImpedance(ohms);
        }
    });
}

// ---------------------------------------------------------------------------------------------
// Pins

PinState SimulationEditor::pinState(const QString& footprint, const QString& pin) const {
    PinState state;
    state.net = pinNet(footprint, pin);
    const Json* sim = _document.simulationJson(_simulation);
    if (!sim) return state;
    const std::string reference = utf8(footprint), number = utf8(pin);
    state.excited = excitationIndex(*sim, reference, number) >= 0;
    const bool automatic = absorbsByDefault(footprint, pin, state.net);
    state.netImpedance = netImpedance(state.net);
    const auto entry = state.net.isEmpty() ? std::nullopt : netEntry(state.net);
    if (!entry) {
        state.absorbing = automatic;
    } else {
        state.impedance = entry->pinImpedance(reference, number);
        state.loadCapacitance = entry->pinLoadCapacitance(reference, number);
        if (entry->hasExplicitPinSelections()) {
            state.probed = entry->probedPinIsProbe(reference, number).value_or(false);
            state.absorbing = entry->probedPinAbsorbs(reference, number).value_or(automatic);
        } else {
            // A legacy entry implicitly probes every pin it doesn't exclude, when simulated.
            state.probed = entry->inclusionLevel() == kiems::NetInclusionLevel::SimulationNet &&
                           !entry->isPinExcluded(reference, number);
            state.absorbing = state.probed || automatic;
        }
    }
    // A driven pin always gets an absorbing port, whatever its own entry says.
    if (state.excited) state.absorbing = true;
    return state;
}

void SimulationEditor::applyPinProbed(Json& sim, std::vector<kiems::InvolvedNetConfig>& entries,
                                      const QString& footprint, const QString& pin, const QString& net, bool probed,
                                      bool absorbing) const {
    const std::string reference = utf8(footprint), number = utf8(pin);
    if (probed) {
        includeNet(sim, entries, utf8(net), true).setPinProbed(reference, number, absorbing);
    } else {
        // A termination needs its copper present but doesn't make the net a simulated path; a
        // deliberate "no port" is stored too, so it outlives the automatic default.
        includeNet(sim, entries, utf8(net), false).setPinAbsorbOnly(reference, number, absorbing);
    }
}

void SimulationEditor::setPinProbed(const QString& footprint, const QString& pin, bool probed) {
    const PinState state = pinState(footprint, pin);
    if (state.net.isEmpty()) return;
    const auto partner = isDifferentialPair() ? partnerPin(footprint, state.net) : std::nullopt;
    const QString partnerNetName = partner ? pinNet(footprint, *partner) : QString();
    editNets([&](Json& sim, std::vector<kiems::InvolvedNetConfig>& entries) {
        applyPinProbed(sim, entries, footprint, pin, state.net, probed, state.absorbing);
        if (partner && *partner != pin && !partnerNetName.isEmpty()) {
            applyPinProbed(sim, entries, footprint, *partner, partnerNetName, probed, state.absorbing);
            if (probed) markPair(entries, utf8(state.net), utf8(partnerNetName));
        }
    });
}

void SimulationEditor::setPinAbsorbing(const QString& footprint, const QString& pin, bool absorbing) {
    const PinState state = pinState(footprint, pin);
    if (state.net.isEmpty()) return;
    const auto partner = isDifferentialPair() ? partnerPin(footprint, state.net) : std::nullopt;
    const QString partnerNetName = partner ? pinNet(footprint, *partner) : QString();
    editNets([&](Json& sim, std::vector<kiems::InvolvedNetConfig>& entries) {
        applyPinProbed(sim, entries, footprint, pin, state.net, state.probed, absorbing);
        if (partner && *partner != pin && !partnerNetName.isEmpty()) {
            applyPinProbed(sim, entries, footprint, *partner, partnerNetName, state.probed, absorbing);
            markPair(entries, utf8(state.net), utf8(partnerNetName));
        }
    });
}

void SimulationEditor::applyPinExcited(Json& sim, std::vector<kiems::InvolvedNetConfig>& entries,
                                       const QString& footprint, const QString& pin, const QString& net,
                                       bool excited) const {
    const std::string reference = utf8(footprint), number = utf8(pin);
    const int index = excitationIndex(sim, reference, number);
    if (excited) {
        includeNet(sim, entries, utf8(net), true);
        if (index >= 0) return;
        // The normal case is a broadband drive over the simulation's sweep; it can be made an
        // adversarial, narrowband one afterwards.
        kiems::ExcitationConfig excitation;
        excitation.setFootprint(reference);
        excitation.setPin(number);
        excitation.setIsMain(true);
        excitation.setAmplitude(1.0);
        if (!sim.contains("excitations")) sim["excitations"] = Json::array();
        sim["excitations"].push_back(Json(excitation));
    } else if (index >= 0) {
        sim["excitations"].erase(static_cast<std::size_t>(index));
    }
}

void SimulationEditor::setPinExcited(const QString& footprint, const QString& pin, bool excited) {
    const QString net = pinNet(footprint, pin);
    if (net.isEmpty()) return;
    const auto partner = isDifferentialPair() ? partnerPin(footprint, net) : std::nullopt;
    const QString partnerNetName = partner ? pinNet(footprint, *partner) : QString();
    editNets([&](Json& sim, std::vector<kiems::InvolvedNetConfig>& entries) {
        applyPinExcited(sim, entries, footprint, pin, net, excited);
        if (!partner || *partner == pin || partnerNetName.isEmpty()) return;
        const bool partnerExcited = excitationIndex(sim, utf8(footprint), utf8(*partner)) >= 0;
        if (partnerExcited == excited) return;
        applyPinExcited(sim, entries, footprint, *partner, partnerNetName, excited);
        if (!excited) return;
        const int sourceIndex = excitationIndex(sim, utf8(footprint), utf8(pin));
        const int partnerIndex = excitationIndex(sim, utf8(footprint), utf8(*partner));
        auto mirrored = sim["excitations"][static_cast<std::size_t>(partnerIndex)].get<kiems::ExcitationConfig>();
        complement(mirrored, sim["excitations"][static_cast<std::size_t>(sourceIndex)].get<kiems::ExcitationConfig>());
        sim["excitations"][static_cast<std::size_t>(partnerIndex)] = Json(mirrored);
        markPair(entries, utf8(net), utf8(partnerNetName));
    });
}

void SimulationEditor::setPinImpedance(const QString& footprint, const QString& pin, std::optional<double> ohms) {
    const QString net = pinNet(footprint, pin);
    if (net.isEmpty()) return;
    editNets([&](Json& sim, std::vector<kiems::InvolvedNetConfig>& entries) {
        includeNet(sim, entries, utf8(net), false)
            .setPinImpedance(utf8(footprint), utf8(pin), ohms ? std::optional(std::max(0.0, *ohms)) : std::nullopt);
    });
}

void SimulationEditor::setPinLoadCapacitance(const QString& footprint, const QString& pin,
                                             std::optional<double> farads) {
    const QString net = pinNet(footprint, pin);
    if (net.isEmpty()) return;
    editNets([&](Json& sim, std::vector<kiems::InvolvedNetConfig>& entries) {
        includeNet(sim, entries, utf8(net), false).setPinLoadCapacitance(utf8(footprint), utf8(pin), farads);
    });
}

// ---------------------------------------------------------------------------------------------
// Excitations

std::optional<kiems::ExcitationConfig> SimulationEditor::excitation(const QString& footprint, const QString& pin) const {
    const Json* sim = _document.simulationJson(_simulation);
    if (!sim) return std::nullopt;
    const int index = excitationIndex(*sim, utf8(footprint), utf8(pin));
    if (index < 0) return std::nullopt;
    try {
        return (*sim)["excitations"][static_cast<std::size_t>(index)].get<kiems::ExcitationConfig>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void SimulationEditor::updateExcitation(const QString& footprint, const QString& pin,
                                        const std::function<void(kiems::ExcitationConfig&)>& change) {
    const QString net = pinNet(footprint, pin);
    const auto partner = isDifferentialPair() && !net.isEmpty() ? partnerPin(footprint, net) : std::nullopt;
    _document.editSimulation(_simulation, [&](Json& sim) {
        const int index = excitationIndex(sim, utf8(footprint), utf8(pin));
        if (index < 0) return;
        Json& stored = sim["excitations"][static_cast<std::size_t>(index)];
        kiems::ExcitationConfig excitation;
        try {
            excitation = stored.get<kiems::ExcitationConfig>();
        } catch (const std::exception&) {
            return;
        }
        change(excitation);
        stored = Json(excitation);
        if (!partner || *partner == pin) return;
        const int partnerIndex = excitationIndex(sim, utf8(footprint), utf8(*partner));
        if (partnerIndex < 0) return;
        Json& partnerStored = sim["excitations"][static_cast<std::size_t>(partnerIndex)];
        try {
            auto mirrored = partnerStored.get<kiems::ExcitationConfig>();
            complement(mirrored, excitation);
            partnerStored = Json(mirrored);
        } catch (const std::exception&) {
        }
    });
}

void SimulationEditor::setExcitationMain(const QString& footprint, const QString& pin, bool main) {
    const double startFrequency = _document.documentValue("/frequency/start").get<double>();
    updateExcitation(footprint, pin, [&](kiems::ExcitationConfig& excitation) {
        const bool wasMain = excitation.isMain();
        excitation.setIsMain(main);
        if (main) return;
        if (wasMain) excitation.setDurationMode(kiems::ExcitationDurationMode::Continuous);
        if (!excitation.frequency() || *excitation.frequency() == 0) excitation.setFrequency(startFrequency);
        if (!excitation.amplitude()) excitation.setAmplitude(1.0);
    });
}

// ---------------------------------------------------------------------------------------------
// Components

namespace {

int componentIndex(const Json& sim, const std::string& reference) {
    const Json& components = sim.value("included_components", Json::array());
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (components[i].value("reference", std::string()) == reference) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

bool SimulationEditor::componentIncluded(const QString& reference) const {
    const Json* sim = _document.simulationJson(_simulation);
    return sim && componentIndex(*sim, utf8(reference)) >= 0;
}

void SimulationEditor::setComponentIncluded(const QString& reference, bool included) {
    _document.editSimulation(_simulation, [&](Json& sim) {
        const int index = componentIndex(sim, utf8(reference));
        if (included && index < 0) {
            kiems::IncludedComponentConfig component;
            component.reference = utf8(reference);
            if (!sim.contains("included_components")) sim["included_components"] = Json::array();
            sim["included_components"].push_back(Json(component));
        } else if (!included && index >= 0) {
            sim["included_components"].erase(static_cast<std::size_t>(index));
            if (sim["included_components"].empty()) sim.erase("included_components");
        }
    });
}

bool SimulationEditor::componentContributesToHull(const QString& reference) const {
    const Json* sim = _document.simulationJson(_simulation);
    if (!sim) return false;
    const int index = componentIndex(*sim, utf8(reference));
    return index >= 0 && (*sim)["included_components"][static_cast<std::size_t>(index)].value("contributes_to_hull", false);
}

void SimulationEditor::setComponentContributesToHull(const QString& reference, bool contributes) {
    _document.editSimulation(_simulation, [&](Json& sim) {
        const int index = componentIndex(sim, utf8(reference));
        if (index >= 0) sim["included_components"][static_cast<std::size_t>(index)]["contributes_to_hull"] = contributes;
    });
}

double SimulationEditor::componentHullPadding(const QString& reference) const {
    const Json* sim = _document.simulationJson(_simulation);
    const double fallback = kiems::IncludedComponentConfig().hullPadding;
    if (!sim) return fallback;
    const int index = componentIndex(*sim, utf8(reference));
    return index >= 0 ? (*sim)["included_components"][static_cast<std::size_t>(index)].value("hull_padding", fallback)
                      : fallback;
}

void SimulationEditor::setComponentHullPadding(const QString& reference, double micrometres) {
    _document.editSimulation(_simulation, [&](Json& sim) {
        const int index = componentIndex(sim, utf8(reference));
        if (index >= 0) {
            sim["included_components"][static_cast<std::size_t>(index)]["hull_padding"] = std::max(0.0, micrometres);
        }
    });
}

// ---------------------------------------------------------------------------------------------
// Differential pairs

void SimulationEditor::reconcileDifferentialPairs() {
    if (!_board) return;
    editNets([&](Json& sim, std::vector<kiems::InvolvedNetConfig>& entries) {
        std::vector<std::string> names;
        for (const kiems::InvolvedNetConfig& entry : entries) {
            if (entry.kind() == kiems::NetSelectorKind::Net && entry.net()) names.push_back(*entry.net());
        }
        for (const std::string& name : names) {
            auto find = [&](const std::string& net) -> kiems::InvolvedNetConfig* {
                for (kiems::InvolvedNetConfig& entry : entries) {
                    if (entry.kind() == kiems::NetSelectorKind::Net && entry.net() == net) return &entry;
                }
                return nullptr;
            };
            const QString partner = partnerNet(qstr(name));
            kiems::InvolvedNetConfig* entry = find(name);
            if (partner.isEmpty() || !entry) continue;
            const std::string partnerName = utf8(partner);
            // Entries already paired with some other net are left alone.
            if (entry->differentialPairPartner() && *entry->differentialPairPartner() != partnerName) continue;
            const bool simulated = entry->inclusionLevel() == kiems::NetInclusionLevel::SimulationNet;
            if (kiems::InvolvedNetConfig* existing = find(partnerName)) {
                if (existing->differentialPairPartner() && *existing->differentialPairPartner() != name) continue;
                if (simulated) existing->setInclusionLevel(kiems::NetInclusionLevel::SimulationNet);
            } else {
                includeNet(sim, entries, partnerName, simulated);
            }
            markPair(entries, name, partnerName);

            // Mirror each explicit pin choice the partner side doesn't already have.
            entry = find(name);
            const std::vector<kiems::ProbedPin> pins = entry->probedPins();
            for (const kiems::ProbedPin& pin : pins) {
                const auto mirroredPin = partnerPin(qstr(pin.footprint), qstr(name));
                if (!mirroredPin || utf8(*mirroredPin) == pin.pin ||
                    pinNet(qstr(pin.footprint), *mirroredPin) != partner) {
                    continue;
                }
                kiems::InvolvedNetConfig* partnerEntry = find(partnerName);
                if (!partnerEntry || partnerEntry->probedPinIsProbe(pin.footprint, utf8(*mirroredPin)).has_value()) continue;
                if (pin.probe) partnerEntry->setPinProbed(pin.footprint, utf8(*mirroredPin), pin.absorbSignal);
                else partnerEntry->setPinAbsorbOnly(pin.footprint, utf8(*mirroredPin), pin.absorbSignal);
            }
        }

        // Mirror excitations onto partner pins of paired nets, as antiphase complements.
        const Json excitations = sim.value("excitations", Json::array());
        for (const Json& stored : excitations) {
            if (stored.contains("hull_cut_port") || !stored.contains("footprint")) continue;
            kiems::ExcitationConfig source;
            try {
                source = stored.get<kiems::ExcitationConfig>();
            } catch (const std::exception&) {
                continue;
            }
            const QString footprint = qstr(source.footprint());
            const QString net = pinNet(footprint, qstr(source.pin()));
            const auto mirroredPin = net.isEmpty() ? std::nullopt : partnerPin(footprint, net);
            if (!mirroredPin || utf8(*mirroredPin) == source.pin()) continue;
            const QString mirroredNet = pinNet(footprint, *mirroredPin);
            bool paired = false;
            for (const kiems::InvolvedNetConfig& entry : entries) {
                if (entry.kind() == kiems::NetSelectorKind::Net && entry.net() == utf8(net)) {
                    paired = entry.differentialPairPartner() == utf8(mirroredNet);
                }
            }
            if (!paired || excitationIndex(sim, source.footprint(), utf8(*mirroredPin)) >= 0) continue;
            applyPinExcited(sim, entries, footprint, *mirroredPin, mirroredNet, true);
            const int index = excitationIndex(sim, source.footprint(), utf8(*mirroredPin));
            auto mirrored = sim["excitations"][static_cast<std::size_t>(index)].get<kiems::ExcitationConfig>();
            complement(mirrored, source);
            sim["excitations"][static_cast<std::size_t>(index)] = Json(mirrored);
        }
    });
}

} // namespace kiems_gui
