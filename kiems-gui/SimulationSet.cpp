#include "SimulationSet.hpp"

#include "SimulationEditor.hpp"

#include <QCollator>
#include <QHash>
#include <QObject>

#include <algorithm>

namespace kiems_gui {

namespace {

QString qstr(const std::string& text) {
    return QString::fromStdString(text);
}

QCollator numericCollator() {
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator;
}

const libkicad::FootprintInfo* footprint(const BoardData& board, const std::string& reference) {
    for (const libkicad::FootprintInfo& info : board.footprints) {
        if (info.reference == reference) return &info;
    }
    return nullptr;
}

/// An additional net pattern with the primary match's groups substituted, as NSRegularExpression
/// expands a replacement template: $n is group n (taking further digits while they still name a
/// group), ${name} a named group, and a backslash makes the next character literal.
QString expandTemplate(const QRegularExpressionMatch& match, int groups, const QString& pattern) {
    QString out;
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        const QChar c = pattern[i];
        if (c == '\\' && i + 1 < pattern.size()) {
            out += pattern[++i];
            continue;
        }
        if (c == '$' && i + 1 < pattern.size()) {
            if (pattern[i + 1] == '{') {
                const qsizetype close = pattern.indexOf('}', i + 2);
                if (close > 0) {
                    out += match.captured(pattern.mid(i + 2, close - i - 2));
                    i = close;
                    continue;
                }
            } else if (pattern[i + 1].isDigit()) {
                int group = pattern[i + 1].digitValue();
                qsizetype j = i + 2;
                while (j < pattern.size() && pattern[j].isDigit() && group * 10 + pattern[j].digitValue() <= groups) {
                    group = group * 10 + pattern[j].digitValue();
                    ++j;
                }
                if (group <= groups) out += match.captured(group);
                i = j - 1;
                continue;
            }
        }
        out += c;
    }
    return out;
}

/// One generated simulation's own additions to its copy of the template: what picking the pins on
/// the board and ticking their boxes would do.
void applyEntry(Document& document, const BoardData* board, int simulation, const kiems::SimulationSetConfig& set,
                const SetEntry& entry) {
    SimulationEditor editor(document, board, simulation);
    const QString reference = qstr(set.excitationFootprint);
    const QString& pin = entry.sourcePin;
    editor.setPinExcited(reference, pin, true);
    editor.updateExcitation(reference, pin, [&](kiems::ExcitationConfig& excitation) {
        excitation.setIsMain(true);
        excitation.setPhaseDegrees(set.phaseDegrees);
        excitation.setAmplitude(set.amplitude);
    });
    editor.setPinImpedance(reference, pin, set.driveImpedance);
    editor.setPinPort(reference, pin, set.probed, set.absorbing);
    if (set.absorbing) editor.setPinLoadCapacitance(reference, pin, set.loadCapacitance);
    if (set.probeNetImpedance) editor.setImpedanceProbed(entry.net, true);
    QStringList others(entry.nets.begin(), entry.nets.end());
    std::sort(others.begin(), others.end(), [collator = numericCollator()](const QString& a, const QString& b) {
        return collator.compare(a, b) < 0;
    });
    for (const QString& net : others) {
        if (net != entry.net) editor.setNetSimulated(net, true);
    }
    for (const SetProbeMatch& match : entry.probes) {
        const QString probeReference = qstr(match.probe.footprint);
        for (const QString& probePin : match.pins) {
            editor.setPinPort(probeReference, probePin, true, match.probe.absorbing);
            if (match.probe.absorbing) {
                editor.setPinImpedance(probeReference, probePin, match.probe.loadImpedance);
                editor.setPinLoadCapacitance(probeReference, probePin, match.probe.loadCapacitance);
            }
        }
    }
    if (set.excitationDuration) document.setMainExcitationDuration(simulation, *set.excitationDuration);
}

} // namespace

QRegularExpression setNetPattern(const QString& pattern, QString* error) {
    QRegularExpression expression(QStringLiteral("^(?:%1)$").arg(pattern));
    if (!expression.isValid() && error) *error = expression.errorString();
    return expression;
}

QString generatedSimulationName(const QString& setName, const QString& net) {
    return setName + ' ' + net;
}

QList<SetEntry> expandSimulationSet(const kiems::SimulationSetConfig& set, const BoardData& board) {
    QSet<QString> allNets;
    for (const libkicad::FootprintInfo& info : board.footprints) {
        for (const libkicad::FootprintPin& pin : info.pins) {
            if (!pin.netName.empty()) allNets.insert(qstr(pin.netName));
        }
    }

    struct Primary {
        QString net;
        QRegularExpressionMatch match;
    };
    QList<Primary> primaries;
    QRegularExpression primary;
    const bool byList = set.netSelectionMode == kiems::SimulationSetNetSelectionMode::List;
    if (byList) {
        QSet<QString> seen;
        for (const std::string& name : set.netNames) {
            const QString net = qstr(name);
            if (allNets.contains(net) && !seen.contains(net)) {
                seen.insert(net);
                primaries.append({net, {}});
            }
        }
    } else {
        if (set.netNamePattern.empty()) return {};
        primary = setNetPattern(qstr(set.netNamePattern));
        if (!primary.isValid()) return {};
        for (const QString& net : allNets) {
            QRegularExpressionMatch match = primary.match(net);
            if (match.hasMatch()) primaries.append({net, std::move(match)});
        }
    }

    const libkicad::FootprintInfo* source = footprint(board, set.excitationFootprint);
    QList<SetEntry> entries;
    for (const Primary& candidate : primaries) {
        SetEntry entry;
        entry.net = candidate.net;
        entry.nets.insert(candidate.net);
        if (!byList) {
            for (const std::string& additional : set.additionalNetPatterns) {
                const QRegularExpression secondary =
                    setNetPattern(expandTemplate(candidate.match, primary.captureCount(), qstr(additional)));
                if (!secondary.isValid()) continue;
                for (const QString& net : allNets) {
                    if (secondary.match(net).hasMatch()) entry.nets.insert(net);
                }
            }
        }
        if (source) {
            for (const libkicad::FootprintPin& pin : source->pins) {
                if (qstr(pin.netName) == entry.net) {
                    entry.sourcePin = qstr(pin.number);
                    break;
                }
            }
        }
        for (const kiems::SimulationSetProbe& probe : set.probes) {
            SetProbeMatch match{probe, {}};
            if (const libkicad::FootprintInfo* info = footprint(board, probe.footprint)) {
                for (const libkicad::FootprintPin& pin : info->pins) {
                    if (!pin.netName.empty() && entry.nets.contains(qstr(pin.netName))) match.pins << qstr(pin.number);
                }
            }
            entry.probes.append(std::move(match));
        }
        entries.append(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [collator = numericCollator()](const SetEntry& a, const SetEntry& b) {
        return collator.compare(a.net, b.net) < 0;
    });
    return entries;
}

std::optional<kiems::SimulationSetConfig> simulationSet(const Document& document, int simulation) {
    const nlohmann::json* sim = document.simulationJson(simulation);
    if (!sim || !sim->contains("simulation_set")) return std::nullopt;
    try {
        return sim->at("simulation_set").get<kiems::SimulationSetConfig>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void setSimulationSet(Document& document, int simulation, const kiems::SimulationSetConfig& set) {
    document.editSimulation(simulation, [&](nlohmann::json& sim) {
        if (!sim.contains("simulation_set")) return;
        // Unchanged settings keep their exact original text.
        try {
            if (sim["simulation_set"].get<kiems::SimulationSetConfig>() == set) return;
        } catch (const std::exception&) {
        }
        sim["simulation_set"] = set;
    });
}

QList<int> generatedSimulations(const Document& document, int templateIndex) {
    const QString id = document.simulationSetId(templateIndex);
    QList<int> generated;
    if (id.isEmpty()) return generated;
    for (int i = 0; i < document.simulationCount(); ++i) {
        if (!document.isSimulationSet(i) && document.generatedBySet(i) == id) generated << i;
    }
    std::sort(generated.begin(), generated.end(), [&document, collator = numericCollator()](int a, int b) {
        return collator.compare(document.simulationName(a), document.simulationName(b)) < 0;
    });
    return generated;
}

QStringList describeSimulationSet(const kiems::SimulationSetConfig& set, const BoardData* board) {
    const bool byList = set.netSelectionMode == kiems::SimulationSetNetSelectionMode::List;
    QString error;
    if (!byList && !set.netNamePattern.empty() && !setNetPattern(qstr(set.netNamePattern), &error).isValid()) {
        return {QObject::tr("Invalid net name pattern: %1").arg(error)};
    }
    if (set.excitationFootprint.empty() || (!byList && set.netNamePattern.empty()) || (byList && set.netNames.empty())) {
        return {byList ? QObject::tr("Choose an excitation IC and the nets to simulate.")
                       : QObject::tr("Choose an excitation IC and a net name pattern.")};
    }
    if (!board) return {QObject::tr("Waiting for the board to load...")};
    if (!footprint(*board, set.excitationFootprint)) {
        return {QObject::tr("No component %1 on the board.").arg(qstr(set.excitationFootprint))};
    }
    const QList<SetEntry> entries = expandSimulationSet(set, *board);
    QStringList missingSource;
    int generated = 0;
    for (const SetEntry& entry : entries) {
        if (entry.sourcePin.isEmpty()) missingSource << entry.net;
        else ++generated;
    }
    QStringList lines{QObject::tr("Generates %n simulation(s).", nullptr, generated)};
    if (!missingSource.isEmpty()) {
        lines << QObject::tr("No pin on %1 for: %2").arg(qstr(set.excitationFootprint), missingSource.join(", "));
    }
    for (int i = 0; i < static_cast<int>(set.probes.size()); ++i) {
        if (set.probes[static_cast<std::size_t>(i)].footprint.empty()) continue;
        const bool matched = std::any_of(entries.begin(), entries.end(), [i](const SetEntry& entry) {
            return !entry.sourcePin.isEmpty() && !entry.probes[i].pins.isEmpty();
        });
        if (!matched) {
            lines << QObject::tr("%1 has no pins on the matching nets.")
                         .arg(qstr(set.probes[static_cast<std::size_t>(i)].footprint));
        }
    }
    return lines;
}

std::optional<SetRegeneration> regenerateSimulationSet(Document& document, const BoardData* board, int templateIndex) {
    const auto set = simulationSet(document, templateIndex);
    if (!set || !board || board->footprints.empty()) return std::nullopt;
    const QString id = document.simulationSetId(templateIndex);
    const QString setName = document.simulationName(templateIndex);

    QHash<QString, int> existing;
    for (int i = 0; i < document.simulationCount(); ++i) {
        if (!document.isSimulationSet(i) && document.generatedBySet(i) == id) existing.insert(document.simulationName(i), i);
    }
    nlohmann::json base = *document.simulationJson(templateIndex);
    base.erase("simulation_set");
    base["generated_by_set"] = id.toStdString();

    SetRegeneration result;
    const Document::ChangeBatch batch(document);
    QSet<int> kept;
    for (const SetEntry& entry : expandSimulationSet(*set, *board)) {
        if (entry.sourcePin.isEmpty()) continue;
        const QString name = generatedSimulationName(setName, entry.net);
        const int target = existing.value(name, -1);
        const nlohmann::json previous = target >= 0 ? *document.simulationJson(target) : nlohmann::json();
        nlohmann::json fresh = base;
        fresh["name"] = name.toStdString();
        const int index = document.putSimulationJson(target >= 0 ? target : document.simulationCount(), std::move(fresh));
        applyEntry(document, board, index, *set, entry);
        if (target < 0) ++result.added;
        else if (*document.simulationJson(index) != previous) ++result.changed;
        kept.insert(index);
    }
    QList<int> stale;
    for (int index : existing) {
        if (!kept.contains(index)) stale << index;
    }
    std::sort(stale.begin(), stale.end(), std::greater<>());
    for (int index : stale) document.removeSimulation(index);
    result.removed = static_cast<int>(stale.size());
    return result;
}

} // namespace kiems_gui
