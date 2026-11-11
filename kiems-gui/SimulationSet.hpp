// Simulation Sets: a simulation marked as a template (kiems::SimulationSetConfig) that generates
// one ordinary simulation per net it selects, each driving that net's pin on one IC and probing the
// matching pins of others. The rules are the macOS app's (SimulationSetExpansion and
// WholeBoardViewController.regenerateSimulationSet), so a document behaves the same in either app:
//
//  - The primary nets are the board's nets matching the whole-name pattern, or those listed.
//  - Each additional pattern is a substitution template expanded against the primary net's match
//    ($1 for its first group, \ to escape), whose matches join that simulation as simulated nets.
//  - A generated simulation is a copy of the template named "<set> <net>", with the excitation IC's
//    pin on the net driven as its main excitation and every probe IC's pin on any matching net
//    probed. Regenerating keeps a simulation whose name is unchanged in place, appends new ones and
//    removes those that no longer match.
#pragma once

#include "BoardModel.hpp"
#include "Document.hpp"

#include "kiems/config.hpp"

#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>

namespace kiems_gui {

/// One probe IC's pins on a generated simulation's nets.
struct SetProbeMatch {
    kiems::SimulationSetProbe probe;
    QStringList pins;
};

/// One simulation a set generates.
struct SetEntry {
    QString net;
    /// The excitation IC's pin on `net`; empty when it has none, and then nothing is generated.
    QString sourcePin;
    /// `net` and every net an additional pattern adds.
    QSet<QString> nets;
    QList<SetProbeMatch> probes;
};

/// `pattern` as a whole-name regular expression; invalid (with `error` set) if it doesn't compile.
QRegularExpression setNetPattern(const QString& pattern, QString* error = nullptr);

/// The simulations `set` selects on `board`, sorted by net name as numbers sort.
QList<SetEntry> expandSimulationSet(const kiems::SimulationSetConfig& set, const BoardData& board);

QString generatedSimulationName(const QString& setName, const QString& net);

/// The set a template simulation holds; nullopt for any other simulation.
std::optional<kiems::SimulationSetConfig> simulationSet(const Document& document, int simulation);
void setSimulationSet(Document& document, int simulation, const kiems::SimulationSetConfig& set);

/// The simulations generated from the template at `templateIndex`, sorted by name.
QList<int> generatedSimulations(const Document& document, int templateIndex);

/// What a summary line under the set's controls says: how many simulations it generates, or what's
/// missing before it can.
QStringList describeSimulationSet(const kiems::SimulationSetConfig& set, const BoardData* board);

struct SetRegeneration {
    int added = 0;
    int changed = 0;
    int removed = 0;
};

/// Rewrites the simulations generated from the template at `templateIndex` (see the top of this
/// file), as one change to the document. Nullopt, with nothing changed, when it isn't a template
/// or there's no board to match against.
std::optional<SetRegeneration> regenerateSimulationSet(Document& document, const BoardData* board, int templateIndex);

} // namespace kiems_gui
