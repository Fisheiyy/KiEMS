#include "PowerAnalysis.hpp"

#include "SimulationEditor.hpp"

#include <algorithm>
#include <chrono>
#include <optional>

namespace kiems_gui {

namespace {

nlohmann::json analysesJson(const Document& document) {
    const nlohmann::json list = document.documentValue("/dc_analyses");
    return list.is_array() ? list : nlohmann::json::array();
}

} // namespace

std::vector<kiems::dc::AnalysisConfig> powerAnalyses(const Document& document) {
    std::vector<kiems::dc::AnalysisConfig> analyses;
    for (const nlohmann::json& entry : analysesJson(document)) {
        try {
            analyses.push_back(entry.get<kiems::dc::AnalysisConfig>());
        } catch (const std::exception&) {
            // One the app can't read (hand-edited, say) is listed by its name, with nothing in it.
            kiems::dc::AnalysisConfig unreadable;
            unreadable.name = entry.value("name", std::string("?"));
            analyses.push_back(unreadable);
        }
    }
    return analyses;
}

int powerAnalysisCount(const Document& document) {
    return static_cast<int>(analysesJson(document).size());
}

std::optional<kiems::dc::AnalysisConfig> powerAnalysis(const Document& document, int index) {
    const auto analyses = powerAnalyses(document);
    if (index < 0 || index >= static_cast<int>(analyses.size())) return std::nullopt;
    return analyses[static_cast<std::size_t>(index)];
}

void setPowerAnalysis(Document& document, int index, const kiems::dc::AnalysisConfig& analysis) {
    nlohmann::json list = analysesJson(document);
    if (index < 0 || index >= static_cast<int>(list.size())) return;
    // Unchanged settings keep their exact original text.
    try {
        if (list[static_cast<std::size_t>(index)].get<kiems::dc::AnalysisConfig>() == analysis) return;
    } catch (const std::exception&) {
    }
    list[static_cast<std::size_t>(index)] = analysis;
    document.setDocumentValue("/dc_analyses", list);
}

int addPowerAnalysis(Document& document, kiems::dc::AnalysisConfig analysis) {
    nlohmann::json list = analysesJson(document);
    const std::string base = analysis.name.empty() ? analysis.net : analysis.name;
    std::string name = base;
    for (int n = 2;; ++n) {
        const bool taken = std::any_of(list.begin(), list.end(), [&](const nlohmann::json& entry) {
            return entry.value("name", std::string()) == name;
        });
        if (!taken) break;
        name = base + " " + std::to_string(n);
    }
    analysis.name = name;
    list.push_back(analysis);
    document.setDocumentValue("/dc_analyses", list);
    return static_cast<int>(list.size()) - 1;
}

void removePowerAnalysis(Document& document, int index) {
    nlohmann::json list = analysesJson(document);
    if (index < 0 || index >= static_cast<int>(list.size())) return;
    list.erase(static_cast<std::size_t>(index));
    document.setDocumentValue("/dc_analyses", list);
}

kiems::dc::AnalysisConfig newPowerAnalysis(const BoardData& board, const QString& net, double totalCurrent) {
    return kiems::dc::automaticAnalysis(board.pads, net.toStdString(), 0, totalCurrent);
}

QStringList powerNetCandidates(const BoardData& board) {
    QStringList power, others;
    for (const QString& net : board.nets) {
        if (net.isEmpty() || net.startsWith("unconnected-") || net.startsWith("Net-(")) continue;
        (SimulationEditor::isPowerNet(net) ? power : others) << net;
    }
    return power + others;
}

std::shared_ptr<const PowerRun> runPowerAnalysis(const BoardData& board, const kiems::dc::AnalysisConfig& analysis) {
    auto run = std::make_shared<PowerRun>();
    run->analysis = analysis;
    const auto started = std::chrono::steady_clock::now();
    try {
        auto problem = kiems::dc::buildProblem(board.stackup, board.geometry, board.throughHoles, analysis);
        if (!problem) {
            run->error = QString::fromStdString(problem.error());
            return run;
        }
        run->problem = std::move(*problem);
        auto result = kiems::dc::solve(run->problem);
        if (!result) {
            run->error = QString::fromStdString(result.error());
            return run;
        }
        run->result = std::move(*result);
        run->violations = kiems::dc::findViolations(run->problem, run->result, analysis);
    } catch (const std::exception& exception) {
        run->error = QString::fromStdString(exception.what());
    }
    run->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return run;
}

} // namespace kiems_gui
