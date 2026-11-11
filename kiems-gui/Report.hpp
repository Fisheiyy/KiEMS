// File > Save Report's PDF, as the macOS app's ResultsReport makes it: a summary page with every
// simulation's verdict and why, then each simulation's pages -- its eye diagrams with their masks,
// then every chart its Results view shows (makeResultsCategories), in print colours. Headings,
// notes and charts are laid out as blocks down Letter pages, a block that doesn't fit starting
// the next page.
#pragma once

#include <QString>
#include <QStringList>

namespace kiems_gui {

class Document;
struct EyeResults;

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

/// Writes the report on every simulation in `document` to `pdfPath`.
bool writeReport(const Document& document, const QString& pdfPath, QString* error);

} // namespace kiems_gui
