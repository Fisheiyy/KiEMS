// A simulation's eye diagrams, as kiems writes them to ems/results/<simulation>/eyes.json: each
// received eye's traces with the eye mask it was tested against, whether it passed, its opening,
// and -- when adversarial excitations reach it -- the same eye with their noise added.
#pragma once

#include "ChartStyle.hpp"

#include <QString>
#include <QWidget>

#include <optional>
#include <vector>

namespace kiems_gui {

struct EyeOpeningData {
    double heightV = 0;
    double widthUI = 0;
    double samplingUI = 0.5;
    double oneLevelV = 0;
    double zeroLevelV = 0;
    double qFactor = 0;
    double jitterUI = 0;
};

struct EyeMaskVerdict {
    std::size_t traces = 0;
    std::size_t keepOutViolations = 0;
    std::size_t outerLimitViolations = 0;
    bool passed = true;
};

struct EyeEntry {
    QString name;
    bool differential = false;
    std::size_t aggressors = 0;
    double bitRate = 0;
    double thresholdV = 0;
    std::vector<double> timeUI;
    std::vector<std::vector<double>> traces;
    EyeOpeningData opening;
    std::optional<EyeMaskVerdict> mask;
    std::vector<std::vector<double>> noisyTraces;
    std::optional<EyeOpeningData> noisyOpening;
    std::optional<EyeMaskVerdict> noisyMask;
    /// The spread of the noisy opening across independent sets of draws.
    std::optional<EyeOpeningData> noiseLowest;
    std::optional<EyeOpeningData> noiseHighest;

    /// Whether it leaves its mask, without noise or with it.
    bool failsMask() const { return (mask && !mask->passed) || (noisyMask && !noisyMask->passed); }
};

struct EyeResults {
    QString error;
    QString standard; ///< the eye test's standard, as simulation.json names it
    double signalLevel = 1;
    double maskX1 = 0, maskX2 = 0, maskY1 = 0, maskY2 = 0;
    std::vector<EyeEntry> eyes;

    bool hasMask() const { return (maskY1 > 0 && maskX1 < 0.5) || maskY2 > 0; }
    /// Reads `resultsDir`/eyes.json; `error` says why when there's nothing to show.
    static EyeResults load(const QString& resultsDir);
};

/// A scrolling page of every eye in `results`.
QWidget* makeEyesPage(const EyeResults& results);

/// One eye's chart: its traces (`noisy`: with the adversarial noise added) and its mask. It draws
/// from `eye` and `results`, which must outlive it.
QWidget* makeEyeChart(const EyeEntry& eye, const EyeResults& results, bool noisy, ChartStyle style);
/// The eye test, its launch level and how many eyes fail, in a sentence.
QString eyeTestSummary(const EyeResults& results);
/// "Passes the eye mask (n traces)." or what fails it; empty without a verdict.
QString eyeVerdictText(const std::optional<EyeMaskVerdict>& verdict);
/// The eye's opening: height, width, amplitude, Q, jitter and sampling point.
QString eyeFigures(const EyeOpeningData& opening);

} // namespace kiems_gui
