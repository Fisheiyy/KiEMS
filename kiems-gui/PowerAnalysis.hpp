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
/// voltage its name implies, power inputs sharing `totalCurrent`), returning on the board's ground.
kiems::dc::AnalysisConfig newPowerAnalysis(const BoardData& board, const QString& net, double totalCurrent = 1.0);

/// The board's nets, likeliest power rails first.
QStringList powerNetCandidates(const BoardData& board);
/// The board's ground net, by its name; empty if none looks like one.
QString groundNetOf(const BoardData& board);

/// One finished run of an analysis.
struct PowerRun {
    kiems::dc::AnalysisConfig analysis;
    kiems::dc::Problem problem;
    kiems::dc::Result result;
    /// The reference net's solution, when the analysis names one.
    std::optional<kiems::dc::Problem> returnProblem;
    std::optional<kiems::dc::Result> returnResult;
    std::vector<kiems::dc::Violation> violations;
    /// How far remote sense moved the sources, when the analysis senses.
    std::optional<double> senseShift;
    /// What the return couldn't model (a load part with no pin on the reference net).
    std::vector<std::string> notes;
    QString error;
    double seconds = 0;
    bool ok() const { return error.isEmpty(); }
};

/// Builds and solves `analysis` on `board`. Thread-safe: it touches only its arguments.
std::shared_ptr<const PowerRun> runPowerAnalysis(const BoardData& board, const kiems::dc::AnalysisConfig& analysis);

} // namespace kiems_gui
