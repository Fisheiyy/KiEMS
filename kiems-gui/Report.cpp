#include "Report.hpp"

#include "Document.hpp"
#include "EyeView.hpp"
#include "PowerAnalysis.hpp"
#include "PowerView.hpp"
#include "ResultsView.hpp"

#include <QChart>
#include <QChartView>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QLocale>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <functional>
#include <memory>
#include <vector>

namespace kiems_gui {

namespace {

const QColor kInk(0x1f, 0x23, 0x28);
const QColor kSecondary(0x5f, 0x63, 0x68);
// Darker than the screen's green and red, which are too light to read on paper.
const QColor kPassGreen(0x1e, 0x8e, 0x3e);
const QColor kFailRed(0xc5, 0x22, 0x1f);
const QColor kWarningOrange(0xc2, 0x6a, 0x00);
const QColor kNotRunGray(0x6e, 0x6e, 0x73);

// Letter, 40 pt margins, laid out in pixels at 96 per inch so widgets render at their usual size.
constexpr int kResolution = 96;
constexpr double kMarginPoints = 40;
// Room kept at the foot of each page for its number.
constexpr double kFooter = 22;

QColor verdictColor(SimulationVerdict::Kind kind) {
    switch (kind) {
    case SimulationVerdict::Kind::Pass: return kPassGreen;
    case SimulationVerdict::Kind::Fail: return kFailRed;
    case SimulationVerdict::Kind::Warning: return kWarningOrange;
    case SimulationVerdict::Kind::NotRun: return kNotRunGray;
    }
    return kInk;
}

QFont font(double points, QFont::Weight weight = QFont::Normal) {
    QFont f;
    f.setPointSizeF(points);
    f.setWeight(weight);
    return f;
}

/// Something drawn at the top of a rectangle of the page's width and its own height.
struct Part {
    double height = 0;
    std::function<void(QPainter&, const QRectF&)> draw;
};

/// One piece of a page, never split across two: a heading, a note, a chart with its headings.
struct Block {
    std::vector<Part> parts;
    double spacing = 6;     // between its parts
    double spaceBefore = 0; // above it, unless it starts a page
    bool startsPage = false;

    double height() const {
        double total = 0;
        for (const Part& part : parts) total += part.height;
        return total + spacing * static_cast<double>(std::max<std::size_t>(parts.size(), 1) - 1);
    }
};

class Pages {
public:
    Pages(QPdfWriter& writer, QString footer) : _writer(writer), _footer(std::move(footer)) {}

    bool begin() { return _painter.begin(&_writer); }

    double width() const { return _writer.width(); }
    double height() const { return _writer.height() - kFooter; }
    QPaintDevice* device() { return &_writer; }

    Part text(const QString& text, const QFont& f, const QColor& color, double indent = 0) {
        const QFontMetricsF metrics(f, &_writer);
        const double w = width() - indent;
        const double h = metrics.boundingRect(QRectF(0, 0, w, 1e6), Qt::TextWordWrap, text).height();
        return {h, [=](QPainter& painter, const QRectF& rect) {
                    painter.setFont(f);
                    painter.setPen(color);
                    painter.drawText(rect.adjusted(indent, 0, 0, 0), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, text);
                }};
    }

    /// `widget`, laid out at the page's width and `height`, drawn as vectors where it can be.
    Part widget(QWidget* widget, double height, double width = -1) {
        const double w = width > 0 ? width : this->width();
        widget->setAttribute(Qt::WA_DontShowOnScreen);
        // Its on-screen minimum mustn't make it taller than the room it's given on the page.
        widget->setMinimumSize(0, 0);
        widget->resize(static_cast<int>(w), static_cast<int>(height));
        widget->show();
        QCoreApplication::sendPostedEvents(); // charts lay themselves out on a posted event
        return {height, [widget](QPainter& painter, const QRectF& rect) {
                    painter.save();
                    painter.translate(rect.topLeft());
                    widget->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
                    painter.restore();
                }};
    }

    void add(const Block& block) {
        double space = _pageEmpty ? 0 : block.spaceBefore;
        const double h = block.height();
        if (!_pageEmpty && (block.startsPage || _y + space + h > height())) {
            newPage();
            space = 0;
        }
        _y += space;
        // A block taller than a page is scaled down to fit one.
        const double scale = h > height() ? height() / h : 1.0;
        _painter.save();
        _painter.translate(0, _y);
        _painter.scale(scale, scale);
        double y = 0;
        for (const Part& part : block.parts) {
            part.draw(_painter, QRectF(0, y, width(), part.height));
            y += part.height + block.spacing;
        }
        _painter.restore();
        _y += h * scale;
        _pageEmpty = false;
    }

    void finish() {
        drawFooter();
        _painter.end();
    }

private:
    void newPage() {
        drawFooter();
        _writer.newPage();
        ++_page;
        _y = 0;
        _pageEmpty = true;
    }

    void drawFooter() {
        _painter.setFont(font(8));
        _painter.setPen(kSecondary);
        _painter.drawText(QRectF(0, _writer.height() - kFooter + 6, width(), kFooter - 6), Qt::AlignRight | Qt::AlignVCenter,
                          QObject::tr("%1 · page %2").arg(_footer).arg(_page));
    }

    QPdfWriter& _writer;
    QPainter _painter;
    QString _footer;
    double _y = 0;
    bool _pageEmpty = true;
    int _page = 1;
};

/// Charts meant for paper: no frame, no shadow, white around them.
void prepareForPrint(QWidget* widget) {
    if (auto* view = qobject_cast<QChartView*>(widget)) {
        view->setFrameShape(QFrame::NoFrame);
        view->setBackgroundBrush(Qt::white);
        view->chart()->setDropShadowEnabled(false);
    }
}

struct Entry {
    QString name;
    SimulationVerdict verdict;
    SimulationResults results;
    EyeResults eyes;
    bool haveResults = false;
};

/// One summary row: the verdict, the name, and why, lined up after the verdict.
void addVerdictRow(Pages& pages, const QString& name, const SimulationVerdict& verdict) {
    constexpr double badgeWidth = 92;
    Block row;
    row.spaceBefore = 12;
    row.spacing = 3;
    const QString badge = verdictTitle(verdict.kind);
    const QColor color = verdictColor(verdict.kind);
    Part title = pages.text(name, font(12, QFont::DemiBold), kInk, badgeWidth);
    const auto drawTitle = title.draw;
    title.draw = [=](QPainter& painter, const QRectF& rect) {
        painter.setFont(font(10, QFont::Bold));
        painter.setPen(color);
        painter.drawText(QRectF(rect.left(), rect.top(), badgeWidth, rect.height()), Qt::AlignLeft | Qt::AlignVCenter, badge);
        drawTitle(painter, rect);
    };
    row.parts.push_back(title);
    for (const QString& note : verdict.notes) row.parts.push_back(pages.text(note, font(9), kSecondary, badgeWidth));
    pages.add(row);
}

struct PowerEntry {
    QString name;
    QString net;
    std::shared_ptr<const PowerRun> run;
    SimulationVerdict verdict;
};

void addSummary(Pages& pages, const Document& document, const std::vector<Entry>& entries,
                const std::vector<PowerEntry>& power) {
    Block title;
    title.parts.push_back(pages.text(QObject::tr("KiEMS Report"), font(26, QFont::Bold), kInk));
    title.parts.push_back(pages.text(document.displayName(), font(15, QFont::DemiBold), kInk));
    QString about = QObject::tr("Board: %1").arg(QFileInfo(document.kicadPcbPath()).fileName());
    about += "\n" + QObject::tr("Generated %1").arg(QLocale().toString(QDateTime::currentDateTime(), QLocale::LongFormat));
    title.parts.push_back(pages.text(about, font(9), kSecondary));
    pages.add(title);

    Block heading;
    heading.spaceBefore = 26;
    heading.parts.push_back(pages.text(QObject::tr("Simulations"), font(15, QFont::DemiBold), kInk));
    pages.add(heading);
    if (entries.empty()) {
        Block none;
        none.spaceBefore = 8;
        none.parts.push_back(pages.text(QObject::tr("This document has no simulations."), font(10), kSecondary));
        pages.add(none);
    }

    for (const Entry& entry : entries) addVerdictRow(pages, entry.name, entry.verdict);

    if (power.empty()) return;
    Block powerHeading;
    powerHeading.spaceBefore = 26;
    powerHeading.parts.push_back(pages.text(QObject::tr("DC Power Analyses"), font(15, QFont::DemiBold), kInk));
    pages.add(powerHeading);
    for (const PowerEntry& entry : power) addVerdictRow(pages, entry.name, entry.verdict);
}

/// Rows of text in columns: the first row a header.
Part table(Pages& pages, const std::vector<QStringList>& rows, const std::vector<double>& widths) {
    const QFont body = font(8.5);
    const QFont header = font(8.5, QFont::DemiBold);
    const double rowHeight = QFontMetricsF(body, pages.device()).height() + 3;
    return {rowHeight * static_cast<double>(rows.size()), [=](QPainter& painter, const QRectF& rect) {
                double y = rect.top();
                for (std::size_t r = 0; r < rows.size(); ++r) {
                    painter.setFont(r == 0 ? header : body);
                    painter.setPen(r == 0 ? kInk : kSecondary);
                    double x = rect.left();
                    for (int c = 0; c < rows[r].size() && c < static_cast<int>(widths.size()); ++c) {
                        painter.drawText(QRectF(x, y, widths[static_cast<std::size_t>(c)] - 4, rowHeight),
                                         Qt::AlignLeft | Qt::AlignVCenter, rows[r][c]);
                        x += widths[static_cast<std::size_t>(c)];
                    }
                    y += rowHeight;
                }
            }};
}

void addPowerAnalysis(Pages& pages, const PowerEntry& entry) {
    Block heading;
    heading.startsPage = true;
    heading.spacing = 6;
    heading.parts.push_back(pages.text(QObject::tr("DC Power Analysis"), font(13, QFont::DemiBold), kSecondary));
    heading.parts.push_back(pages.text(entry.name, font(28, QFont::Bold), kInk));
    heading.parts.push_back(pages.text(verdictTitle(entry.verdict.kind), font(13, QFont::Bold), verdictColor(entry.verdict.kind)));
    for (const QString& note : entry.verdict.notes) heading.parts.push_back(pages.text(note, font(9.5), kSecondary));
    pages.add(heading);
    const PowerRun* run = entry.run.get();
    if (!run || !run->ok()) return;

    // The pins: each load's voltage, and across its part when the return was solved.
    const std::map<std::string, double> grounds =
        run->returnResult ? kiems::dc::partVoltages(*run->returnResult) : std::map<std::string, double>();
    std::vector<QStringList> rows{{QObject::tr("Pin"), QObject::tr("Role"), QObject::tr("Voltage"), QObject::tr("Ground"),
                                   QObject::tr("Across"), QObject::tr("Current")}};
    for (const auto& terminal : run->result.terminals) {
        const bool source = terminal.kind == kiems::dc::Terminal::Kind::Source;
        const auto ground = grounds.find(terminal.name.substr(0, terminal.name.find(" pin ")));
        const bool haveGround = terminal.connected && ground != grounds.end();
        rows.push_back({QString::fromStdString(terminal.name), source ? QObject::tr("Source") : QObject::tr("Load"),
                        terminal.connected ? QString("%1 V").arg(terminal.voltage, 0, 'f', 4) : QObject::tr("not connected"),
                        haveGround ? QString("%1 mV").arg(ground->second * 1e3, 0, 'f', 2) : QString(),
                        haveGround ? QString("%1 V").arg(terminal.voltage - ground->second, 0, 'f', 4) : QString(),
                        QString("%1 A").arg(terminal.current, 0, 'g', 4)});
    }
    Block pins;
    pins.spaceBefore = 22;
    pins.parts.push_back(pages.text(QObject::tr("Pins"), font(15, QFont::DemiBold), kInk));
    const double w = pages.width();
    pins.parts.push_back(table(pages, rows, {w * 0.26, w * 0.16, w * 0.15, w * 0.13, w * 0.15, w * 0.15}));
    pages.add(pins);

    // The busiest vias.
    std::vector<const kiems::dc::BarrelResult*> vias;
    for (const auto& barrel : run->result.barrels) {
        if (barrel.connected && barrel.current > 1e-6) vias.push_back(&barrel);
    }
    std::sort(vias.begin(), vias.end(), [](const auto* a, const auto* b) { return a->current > b->current; });
    if (!vias.empty()) {
        std::vector<QStringList> viaRows{{QObject::tr("Via"), QObject::tr("Current"), QObject::tr("Resistance")}};
        for (std::size_t i = 0; i < std::min<std::size_t>(vias.size(), 12); ++i) {
            viaRows.push_back({QString::fromStdString(vias[i]->name), QString("%1 A").arg(vias[i]->current, 0, 'g', 4),
                               QString("%1 m\u03A9").arg(vias[i]->resistance * 1e3, 0, 'f', 3)});
        }
        Block block;
        block.spaceBefore = 18;
        block.parts.push_back(pages.text(QObject::tr("Busiest vias"), font(12, QFont::DemiBold), kInk));
        block.parts.push_back(table(pages, viaRows, {w * 0.5, w * 0.25, w * 0.25}));
        pages.add(block);
    }

    // The maps: the rail's voltage and current density on each layer, then its return's density.
    std::vector<std::unique_ptr<QWidget>> widgets;
    const auto addMap = [&](const QString& title, int layer, bool onReturn, PowerCanvas::Quantity quantity) {
        auto canvas = std::make_unique<PowerCanvas>();
        const double height = 330;
        canvas->setAttribute(Qt::WA_DontShowOnScreen);
        canvas->resize(static_cast<int>(pages.width()), static_cast<int>(height));
        canvas->setQuantity(quantity);
        canvas->setRun(entry.run, false);
        canvas->setLayer(layer, onReturn);
        canvas->fit();
        Block block;
        block.spaceBefore = 18;
        block.parts.push_back(pages.text(title, font(12, QFont::DemiBold), kInk));
        block.parts.push_back(pages.widget(canvas.get(), height));
        pages.add(block);
        widgets.push_back(std::move(canvas));
    };
    for (int l = 0; l < static_cast<int>(run->result.layers.size()); ++l) {
        const auto& layer = run->result.layers[static_cast<std::size_t>(l)];
        if (layer.triangles.empty()) continue;
        const QString name = QString::fromStdString(layer.name);
        addMap(QObject::tr("%1: voltage").arg(name), l, false, PowerCanvas::Quantity::Voltage);
        addMap(QObject::tr("%1: current density").arg(name), l, false, PowerCanvas::Quantity::CurrentDensity);
    }
    if (run->returnResult) {
        for (int l = 0; l < static_cast<int>(run->returnResult->layers.size()); ++l) {
            const auto& layer = run->returnResult->layers[static_cast<std::size_t>(l)];
            if (layer.triangles.empty()) continue;
            addMap(QObject::tr("Return on %1, %2: current density")
                       .arg(QString::fromStdString(run->analysis.referenceNet), QString::fromStdString(layer.name)),
                   l, true, PowerCanvas::Quantity::CurrentDensity);
        }
    }
}

void addEyes(Pages& pages, const Entry& entry, std::vector<std::unique_ptr<QWidget>>& widgets) {
    if (entry.eyes.eyes.empty()) return;
    Block category;
    category.spaceBefore = 22;
    category.parts.push_back(pages.text(QObject::tr("Eye Diagrams"), font(15, QFont::DemiBold), kInk));
    category.parts.push_back(pages.text(eyeTestSummary(entry.eyes), font(9), kSecondary));
    pages.add(category);
    for (const EyeEntry& eye : entry.eyes.eyes) {
        const QString name = eye.name + (eye.differential ? QObject::tr(" (differential)") : QString());
        auto view = [&](bool noisy, const QString& title) {
            const auto& verdict = noisy ? eye.noisyMask : eye.mask;
            const EyeOpeningData& opening = noisy && eye.noisyOpening ? *eye.noisyOpening : eye.opening;
            Block block;
            block.spaceBefore = 16;
            block.spacing = 4;
            block.parts.push_back(pages.text(title, font(12, QFont::DemiBold), kInk));
            if (verdict) {
                block.parts.push_back(
                    pages.text(eyeVerdictText(verdict), font(9.5, QFont::DemiBold), verdict->passed ? kPassGreen : kFailRed));
            }
            widgets.emplace_back(makeEyeChart(eye, entry.eyes, noisy, ChartStyle::Print));
            block.parts.push_back(pages.widget(widgets.back().get(), 290));
            QString figures = eyeFigures(opening);
            if (noisy && eye.noiseLowest && eye.noiseHighest) {
                figures += "\n" + QObject::tr("Across independent sets of draws the height ranged %1 to %2 mV.")
                                      .arg(eye.noiseLowest->heightV * 1e3, 0, 'f', 1)
                                      .arg(eye.noiseHighest->heightV * 1e3, 0, 'f', 1);
            }
            block.parts.push_back(pages.text(figures, font(8.5), kSecondary));
            pages.add(block);
        };
        view(false, name);
        if (!eye.noisyTraces.empty()) {
            view(true, eye.aggressors == 1
                           ? QObject::tr("%1, with noise from the adversarial excitation").arg(name)
                           : QObject::tr("%1, with noise from %2 adversarial excitations").arg(name).arg(eye.aggressors));
        }
    }
}

void addCharts(Pages& pages, const Entry& entry, std::vector<std::unique_ptr<QWidget>>& widgets) {
    for (const ResultsCategory& category : makeResultsCategories(entry.results, ChartStyle::Print)) {
        // Every widget is owned here, whether or not it's drawn.
        for (const ResultsSection& section : category.sections) {
            for (QWidget* graph : section.graphs) widgets.emplace_back(graph);
        }
        if (category.sections.isEmpty()) continue; // an empty tab says nothing worth printing
        // The category's heading travels with its first chart, and each section's with its own,
        // so no heading ends a page.
        bool categoryPending = true;
        for (const ResultsSection& section : category.sections) {
            for (int g = 0; g < section.graphs.size(); ++g) {
                Block block;
                block.spaceBefore = categoryPending ? 22 : g == 0 ? 16 : 10;
                if (categoryPending) {
                    block.parts.push_back(pages.text(category.title, font(15, QFont::DemiBold), kInk));
                    categoryPending = false;
                }
                if (g == 0 && !section.title.isEmpty()) {
                    block.parts.push_back(pages.text(section.title, font(12, QFont::DemiBold), kInk));
                }
                QWidget* graph = section.graphs[g];
                prepareForPrint(graph);
                if (category.fill) {
                    // The Smith chart: square, centred.
                    const double side = std::min(pages.width(), 460.0);
                    Part part = pages.widget(graph, side, side);
                    const auto draw = part.draw;
                    const double inset = (pages.width() - side) / 2;
                    part.draw = [=](QPainter& painter, const QRectF& rect) { draw(painter, rect.translated(inset, 0)); };
                    block.parts.push_back(part);
                } else {
                    block.parts.push_back(pages.widget(graph, std::max(260, graph->minimumHeight() * 3 / 4)));
                }
                pages.add(block);
            }
        }
    }
}

void addSimulation(Pages& pages, const Entry& entry) {
    Block heading;
    heading.startsPage = true;
    heading.spacing = 6;
    heading.parts.push_back(pages.text(QObject::tr("Simulation"), font(13, QFont::DemiBold), kSecondary));
    heading.parts.push_back(pages.text(entry.name, font(28, QFont::Bold), kInk));
    heading.parts.push_back(pages.text(verdictTitle(entry.verdict.kind), font(13, QFont::Bold), verdictColor(entry.verdict.kind)));
    for (const QString& note : entry.verdict.notes) heading.parts.push_back(pages.text(note, font(9.5), kSecondary));
    pages.add(heading);
    if (!entry.haveResults) return;
    std::vector<std::unique_ptr<QWidget>> widgets;
    addEyes(pages, entry, widgets);
    addCharts(pages, entry, widgets);
}

} // namespace

QString verdictTitle(SimulationVerdict::Kind kind) {
    switch (kind) {
    case SimulationVerdict::Kind::Pass: return QObject::tr("PASS");
    case SimulationVerdict::Kind::Fail: return QObject::tr("FAIL");
    case SimulationVerdict::Kind::Warning: return QObject::tr("WARNING");
    case SimulationVerdict::Kind::NotRun: return QObject::tr("NOT RUN");
    }
    return {};
}

SimulationVerdict judgeSimulation(bool haveResults, const EyeResults& eyes) {
    if (!haveResults) return {SimulationVerdict::Kind::NotRun, {QObject::tr("Simulation has not been run.")}};
    QStringList failures, warnings, passes;
    std::vector<const EyeEntry*> masked;
    for (const EyeEntry& eye : eyes.eyes) {
        if (eye.mask || eye.noisyMask) masked.push_back(&eye);
    }
    if (eyes.eyes.empty()) warnings << QObject::tr("No eye diagrams to test.");
    else if (masked.empty()) warnings << QObject::tr("No eye mask is set, so the eyes aren't tested.");
    for (const EyeEntry* eye : masked) {
        if (eye->failsMask()) {
            QStringList modes;
            if (eye->mask && !eye->mask->passed) modes << QObject::tr("noise-free");
            if (eye->noisyMask && !eye->noisyMask->passed) modes << QObject::tr("with adversarial signals");
            failures << QObject::tr("%1 leaves the eye mask (%2).").arg(eye->name, modes.join(QObject::tr(" and ")));
            continue;
        }
        passes << QObject::tr("%1 passes the eye mask.").arg(eye->name);
        // As the macOS app judges it: the replicates must agree to within 5% of the opening.
        if (eye->noisyOpening && eye->noiseLowest && eye->noiseHighest) {
            const double heightReference = std::max({eye->opening.heightV, std::abs(eye->noisyOpening->heightV), 1e-15});
            const double widthReference = std::max({eye->opening.widthUI, eye->noisyOpening->widthUI, 1e-15});
            const bool stable = eye->noiseHighest->heightV - eye->noiseLowest->heightV <= 0.05 * heightReference &&
                                eye->noiseHighest->widthUI - eye->noiseLowest->widthUI <= 0.05 * widthReference;
            if (!stable) {
                warnings << QObject::tr("%1's adversarial result isn't yet stable; increase Adversarial draws in the "
                                        "Settings tab.")
                                .arg(eye->name);
            }
        }
    }
    if (!failures.isEmpty()) return {SimulationVerdict::Kind::Fail, failures + warnings};
    if (!warnings.isEmpty()) return {SimulationVerdict::Kind::Warning, warnings};
    return {SimulationVerdict::Kind::Pass, passes};
}

SimulationVerdict judgePowerAnalysis(const PowerRun* run) {
    if (!run) return {SimulationVerdict::Kind::NotRun, {QObject::tr("The board wasn't loaded to run it on.")}};
    if (!run->ok()) return {SimulationVerdict::Kind::Fail, {QObject::tr("It couldn't run: %1").arg(run->error)}};
    QStringList notes;
    for (const auto& violation : run->violations) notes << QString::fromStdString(violation.message);
    if (!notes.isEmpty()) return {SimulationVerdict::Kind::Fail, notes};
    QStringList warnings;
    for (const auto& warning : run->result.warnings) warnings << QString::fromStdString(warning);
    for (const auto& note : run->notes) warnings << QString::fromStdString(note);
    if (!warnings.isEmpty()) return {SimulationVerdict::Kind::Warning, warnings};
    // What it found, briefly.
    double lowest = std::numeric_limits<double>::infinity();
    QString lowestName;
    const std::map<std::string, double> grounds =
        run->returnResult ? kiems::dc::partVoltages(*run->returnResult) : std::map<std::string, double>();
    for (const auto& terminal : run->result.terminals) {
        if (terminal.kind != kiems::dc::Terminal::Kind::Load || !terminal.connected) continue;
        const auto ground = grounds.find(terminal.name.substr(0, terminal.name.find(" pin ")));
        const double v = terminal.voltage - (ground != grounds.end() ? ground->second : 0.0);
        if (v < lowest) {
            lowest = v;
            lowestName = QString::fromStdString(terminal.name);
        }
    }
    double density = 0, via = 0;
    for (const auto& layer : run->result.layers) density = std::max(density, layer.maxCurrentDensity);
    for (const auto& barrel : run->result.barrels) via = std::max(via, barrel.current);
    if (std::isfinite(lowest)) {
        notes << QObject::tr("Lowest load: %1 at %2 V%3.")
                     .arg(lowestName)
                     .arg(lowest, 0, 'f', 4)
                     .arg(run->returnResult ? QObject::tr(" across its part") : QString());
    }
    notes << QObject::tr("Up to %1 A/mm\u00B2 in the copper and %2 A through a via.").arg(density, 0, 'g', 3).arg(via, 0, 'g', 3);
    return {SimulationVerdict::Kind::Pass, notes};
}

bool writeReport(const Document& document, const QString& pdfPath, QString* error, const BoardData* board) {
    std::vector<Entry> entries;
    for (int i = 0; i < document.simulationCount(); ++i) {
        // A Simulation Set's template never runs; the simulations it generated are reported.
        if (document.isSimulationSet(i)) continue;
        Entry entry;
        entry.name = document.simulationName(i);
        entry.results = SimulationResults::load(document.resultsDir(i), document.simulationDir(i));
        entry.haveResults = entry.results.error.isEmpty();
        if (entry.haveResults) entry.eyes = EyeResults::load(document.resultsDir(i));
        entry.verdict = judgeSimulation(entry.haveResults, entry.eyes);
        entries.push_back(std::move(entry));
    }
    // Power analyses run afresh: a run takes a fraction of a second, and none is kept on disk.
    std::vector<PowerEntry> power;
    for (const kiems::dc::AnalysisConfig& analysis : powerAnalyses(document)) {
        PowerEntry entry;
        entry.name = QString::fromStdString(analysis.name);
        entry.net = QString::fromStdString(analysis.net);
        if (board) entry.run = runPowerAnalysis(*board, analysis);
        entry.verdict = judgePowerAnalysis(entry.run.get());
        power.push_back(std::move(entry));
    }

    QPdfWriter writer(pdfPath);
    writer.setTitle(QObject::tr("KiEMS Report: %1").arg(document.displayName()));
    writer.setCreator(QStringLiteral("KiEMS"));
    writer.setResolution(kResolution);
    writer.setPageLayout(QPageLayout(QPageSize(QPageSize::Letter), QPageLayout::Portrait,
                                     QMarginsF(kMarginPoints, kMarginPoints, kMarginPoints, kMarginPoints),
                                     QPageLayout::Point));
    Pages pages(writer, document.displayName());
    if (!pages.begin()) {
        if (error) *error = QObject::tr("The report couldn't be written to %1.").arg(pdfPath);
        return false;
    }
    addSummary(pages, document, entries, power);
    for (const Entry& entry : entries) addSimulation(pages, entry);
    for (const PowerEntry& entry : power) addPowerAnalysis(pages, entry);
    pages.finish();
    return true;
}

} // namespace kiems_gui
