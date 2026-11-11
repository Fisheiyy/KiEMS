// A document's DC power analyses (its "dc_analyses", see libkiems/kiems/dc_board.hpp) and running
// one on the loaded board. The solver runs in this process: it needs only the board's copper, plated
// holes and stackup, which BoardData already holds, and takes a fraction of a second to seconds.
#pragma once

#include "BoardModel.hpp"
#include "Document.hpp"

#include "kiems/dc_board.hpp"

#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace kiems_gui {

std::vector<kiems::dc::AnalysisConfig> powerAnalyses(const Document& document);
int powerAnalysisCount(const Document& document);
std::optional<kiems::dc::AnalysisConfig> powerAnalysis(const Document& document, int index);
void setPowerAnalysis(Document& document, int index, const kiems::dc::AnalysisConfig& analysis);
/// Appends `analysis`, renamed if its name is taken, and returns its index.
int addPowerAnalysis(Document& document, kiems::dc::AnalysisConfig analysis);
void removePowerAnalysis(Document& document, int index);

/// A new analysis of `net`: its sources and loads from KiCad's pin types (power outputs at the
/// voltage its name implies, power inputs sharing `totalCurrent`).
kiems::dc::AnalysisConfig newPowerAnalysis(const BoardData& board, const QString& net, double totalCurrent = 1.0);

/// The board's nets, likeliest power rails first.
QStringList powerNetCandidates(const BoardData& board);

/// One finished run of an analysis.
struct PowerRun {
    kiems::dc::AnalysisConfig analysis;
    kiems::dc::Problem problem;
    kiems::dc::Result result;
    std::vector<kiems::dc::Violation> violations;
    QString error;
    double seconds = 0;
    bool ok() const { return error.isEmpty(); }
};

/// Builds and solves `analysis` on `board`. Thread-safe: it touches only its arguments.
std::shared_ptr<const PowerRun> runPowerAnalysis(const BoardData& board, const kiems::dc::AnalysisConfig& analysis);

} // namespace kiems_gui
