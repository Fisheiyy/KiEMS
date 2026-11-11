// File > Save Report's PDF, as the macOS app's ResultsReport makes it: a summary page with every
// simulation's verdict and why, then each simulation's pages -- its eye diagrams with their masks,
// then every chart its Results view shows (makeResultsCategories), in print colours. DC power
// analyses follow, run afresh on the loaded board: their verdicts on the summary page, then each
// one's pins, problems and voltage and current density maps. Headings,
// notes and charts are laid out as blocks down Letter pages, a block that doesn't fit starting
// the next page.
#pragma once

#include <QString>
#include <QStringList>

namespace kiems_gui {

class Document;
struct BoardData;
struct EyeResults;
struct PowerRun;

struct SimulationVerdict {
    enum class Kind { Pass, Fail, Warning, NotRun };

    Kind kind = Kind::NotRun;
    /// Why, one finding per line.
    QStringList notes;
};

QString verdictTitle(SimulationVerdict::Kind kind);

/// The macOS app's rule: FAIL when an eye leaves its mask; NOT RUN without results; WARNING when
/// there's nothing to judge against or an adversarial result hasn't settled; PASS otherwise.
SimulationVerdict judgeSimulation(bool haveResults, const EyeResults& eyes);

/// PASS when no limit is broken; FAIL when one is, or the analysis couldn't run; WARNING when
/// copper or pins weren't connected; NOT RUN without a run (no board to run it on).
SimulationVerdict judgePowerAnalysis(const PowerRun* run);

/// Writes the report on every simulation in `document` to `pdfPath`, and, given the board, on its
/// DC power analyses.
bool writeReport(const Document& document, const QString& pdfPath, QString* error, const BoardData* board = nullptr);

} // namespace kiems_gui
