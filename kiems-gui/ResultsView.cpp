#include "ResultsView.hpp"

#include "EyeView.hpp"

#include <QChart>
#include <QChartView>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineSeries>
#include <QLogValueAxis>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTabWidget>
#include <QTextStream>
#include <QValueAxis>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace kiems_gui {

namespace {

/// A results chart's colours in each style.
struct ChartPalette {
    QColor background, grid, text;
    QList<QColor> series;

    QColor seriesColor(int index) const { return series[index % series.size()]; }

    static const ChartPalette& of(ChartStyle style) {
        static const ChartPalette screen{QColor(0x1e, 0x20, 0x24),
                                         QColor(0x3a, 0x3d, 0x44),
                                         QColor(0xd6, 0xd8, 0xdc),
                                         {QColor(0xff, 0x8c, 0x2a), QColor(0x8f, 0xd1, 0x4f), QColor(0xe0, 0x5a, 0x7b),
                                          QColor(0x5a, 0xb4, 0xff), QColor(0xf2, 0xd0, 0x4b), QColor(0xb4, 0x8c, 0xff),
                                          QColor(0x4c, 0xd9, 0xc0), QColor(0xff, 0xa0, 0xa0)}};
        // Darker inks, which read on white paper.
        static const ChartPalette print{Qt::white,
                                        QColor(0xd4, 0xd4, 0xd8),
                                        QColor(0x1f, 0x23, 0x28),
                                        {QColor(0xd9, 0x48, 0x0f), QColor(0x2b, 0x8a, 0x3e), QColor(0xc2, 0x25, 0x5c),
                                         QColor(0x19, 0x71, 0xc2), QColor(0xb0, 0x7d, 0x00), QColor(0x67, 0x41, 0xd9),
                                         QColor(0x0c, 0x85, 0x99), QColor(0x86, 0x2e, 0x9c)}};
        return style == ChartStyle::Print ? print : screen;
    }
};

QString portLabel(const SimulationResults& results, int port) {
    const auto it = results.portNames.find(port);
    return it != results.portNames.end() ? it->second : QObject::tr("port %1").arg(port);
}

/// One chart: frequency (GHz, log scale) against `yTitle`.
QChartView* makeChart(const QString& yTitle, ChartStyle style) {
    const ChartPalette& colors = ChartPalette::of(style);
    auto* chart = new QChart;
    chart->setBackgroundBrush(colors.background);
    chart->setBackgroundRoundness(0);
    chart->setMargins(QMargins(8, 8, 8, 8));
    chart->legend()->setLabelColor(colors.text);
    chart->legend()->setAlignment(Qt::AlignTop);
    auto* x = new QLogValueAxis;
    x->setTitleText(QObject::tr("Frequency [GHz]"));
    x->setLabelFormat("%g");
    x->setBase(10);
    x->setMinorTickCount(8);
    auto* y = new QValueAxis;
    y->setTitleText(yTitle);
    for (QAbstractAxis* axis : {static_cast<QAbstractAxis*>(x), static_cast<QAbstractAxis*>(y)}) {
        axis->setLabelsColor(colors.text);
        axis->setTitleBrush(colors.text);
        axis->setGridLineColor(colors.grid);
        axis->setMinorGridLineColor(style == ChartStyle::Print ? colors.grid.lighter(110) : colors.grid.darker(130));
        axis->setLinePenColor(colors.grid);
    }
    chart->addAxis(x, Qt::AlignBottom);
    chart->addAxis(y, Qt::AlignLeft);
    auto* view = new QChartView(chart);
    view->setRenderHint(QPainter::Antialiasing);
    view->setMinimumHeight(300);
    return view;
}

/// One chart of time (ns) against volts.
QChartView* makeTimeChart(ChartStyle style) {
    const ChartPalette& colors = ChartPalette::of(style);
    QChartView* view = makeChart(QObject::tr("Voltage [V]"), style);
    QChart* chart = view->chart();
    QAbstractAxis* logAxis = chart->axes(Qt::Horizontal).value(0);
    chart->removeAxis(logAxis);
    delete logAxis;
    auto* x = new QValueAxis;
    x->setTitleText(QObject::tr("Time [ns]"));
    x->setLabelFormat("%g");
    x->setLabelsColor(colors.text);
    x->setTitleBrush(colors.text);
    x->setGridLineColor(colors.grid);
    x->setLinePenColor(colors.grid);
    chart->addAxis(x, Qt::AlignBottom);
    return view;
}

void addSeries(QChartView* view, const QString& name, const std::vector<double>& frequencyHz,
               const std::vector<double>& values, const QColor& color, bool emphasised) {
    auto* series = new QLineSeries;
    series->setName(name);
    QList<QPointF> points;
    for (std::size_t i = 0; i < frequencyHz.size() && i < values.size(); ++i) {
        if (frequencyHz[i] > 0 && std::isfinite(values[i])) points.append({frequencyHz[i] / 1e9, values[i]});
    }
    series->replace(points);
    series->setPen(QPen(color, emphasised ? 2.2 : 1.3));
    QChart* chart = view->chart();
    chart->addSeries(series);
    for (QAbstractAxis* axis : chart->axes()) series->attachAxis(axis);
}

/// Sets the y range to the data, padded, so flat curves don't sit on the frame.
void fitYAxis(QChartView* view) {
    double low = INFINITY, high = -INFINITY;
    for (QAbstractSeries* abstract : view->chart()->series()) {
        for (const QPointF& point : static_cast<QLineSeries*>(abstract)->points()) {
            low = std::min(low, point.y());
            high = std::max(high, point.y());
        }
    }
    if (!std::isfinite(low)) return;
    const double pad = std::max(1e-9, (high - low) * 0.08 + (high == low ? std::abs(high) * 0.1 + 1 : 0));
    for (QAbstractAxis* axis : view->chart()->axes(Qt::Vertical)) axis->setRange(low - pad, high + pad);
}

QWidget* scrollingColumn(const QList<QWidget*>& widgets) {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    for (QWidget* widget : widgets) layout->addWidget(widget);
    layout->addStretch();
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}

QLabel* heading(const QString& text) {
    auto* label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 1.1);
    label->setFont(font);
    return label;
}

QLabel* message(const QString& text) {
    auto* label = new QLabel(text);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    return label;
}

/// A Smith chart of each excited port's reflection coefficient.
class SmithChart : public QWidget {
public:
    explicit SmithChart(ChartStyle style, QWidget* parent = nullptr) : QWidget(parent), _colors(ChartPalette::of(style)) {
        setMinimumSize(360, 360);
    }

    struct Trace {
        QString name;
        QColor color;
        std::vector<std::complex<double>> gamma;
    };
    std::vector<Trace> traces;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), _colors.background);
        painter.setRenderHint(QPainter::Antialiasing);
        const double radius = std::min(width(), height()) / 2.0 - 40;
        const QPointF centre(width() / 2.0, height() / 2.0 + 10);
        auto toScreen = [&](std::complex<double> g) { return centre + QPointF(g.real() * radius, -g.imag() * radius); };

        painter.save();
        QPainterPath unit;
        unit.addEllipse(centre, radius, radius);
        painter.setClipPath(unit);
        painter.setPen(QPen(_colors.grid, 1));
        painter.setBrush(Qt::NoBrush);
        // Constant-resistance circles: centre r/(1+r), radius 1/(1+r).
        for (double r : {0.0, 0.2, 0.5, 1.0, 2.0, 5.0}) {
            const double c = r / (1 + r), rr = 1 / (1 + r);
            painter.drawEllipse(toScreen({c, 0}), rr * radius, rr * radius);
        }
        // Constant-reactance arcs: centre (1, 1/x), radius 1/|x|.
        for (double x : {0.2, 0.5, 1.0, 2.0, 5.0, -0.2, -0.5, -1.0, -2.0, -5.0}) {
            const double rr = 1 / std::abs(x);
            painter.drawEllipse(toScreen({1, 1 / x}), rr * radius, rr * radius);
        }
        painter.drawLine(toScreen({-1, 0}), toScreen({1, 0}));
        painter.restore();
        painter.setPen(QPen(_colors.text, 1));
        painter.drawEllipse(centre, radius, radius);

        double legendY = 16;
        for (const Trace& trace : traces) {
            QPainterPath path;
            for (std::size_t i = 0; i < trace.gamma.size(); ++i) {
                const QPointF p = toScreen(trace.gamma[i]);
                if (i == 0) path.moveTo(p);
                else path.lineTo(p);
            }
            painter.setPen(QPen(trace.color, 2));
            painter.drawPath(path);
            painter.drawLine(QPointF(12, legendY), QPointF(32, legendY));
            painter.setPen(_colors.text);
            painter.drawText(QPointF(38, legendY + 4), trace.name);
            legendY += 18;
        }
    }

private:
    const ChartPalette& _colors;
};

/// Splits one CSV row, trimming each field.
QStringList fields(const QString& line) {
    QStringList parts = line.split(',');
    for (QString& part : parts) part = part.trimmed();
    return parts;
}

/// Every Probe<n>.csv in `simulationDir`. A column is "re(<q>-<excited port>)" or
/// "im(<q>-<excited port>)" for q in V, I, Z and T; an excitation that never reached the probe, and
/// Z for a probe that doesn't measure impedance, are all NaN and left out.
std::vector<ProbeResults> loadProbes(const QString& simulationDir) {
    static const QRegularExpression fileName(QStringLiteral(R"(^Probe(\d+)\.csv$)"));
    static const QRegularExpression column(QStringLiteral(R"(^(re|im)\(([VIZT])-(\d+)\)$)"));
    std::vector<ProbeResults> probes;
    QStringList names = QDir(simulationDir).entryList({"Probe*.csv"}, QDir::Files);
    std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) {
        return fileName.match(a).captured(1).toInt() < fileName.match(b).captured(1).toInt();
    });
    for (const QString& name : names) {
        const auto match = fileName.match(name);
        if (!match.hasMatch()) continue;
        QFile file(QDir(simulationDir).filePath(name));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QTextStream stream(&file);
        const QStringList header = fields(stream.readLine());
        struct Column {
            bool real = true;
            QChar quantity;
            int excited = 0;
        };
        std::vector<std::optional<Column>> columns(static_cast<std::size_t>(header.size()));
        for (int c = 1; c < header.size(); ++c) {
            if (auto m = column.match(header[c]); m.hasMatch()) {
                columns[static_cast<std::size_t>(c)] = Column{m.captured(1) == "re", m.captured(2).at(0), m.captured(3).toInt()};
            }
        }
        ProbeResults probe;
        probe.port = match.captured(1).toInt();
        std::map<int, std::vector<std::complex<double>>> transfer, impedance;
        while (!stream.atEnd()) {
            const QStringList row = fields(stream.readLine());
            if (row.size() < 2) continue;
            const std::size_t at = probe.frequencyHz.size();
            probe.frequencyHz.push_back(row[0].toDouble() * 1e6); // Frequency [MHz]
            for (int c = 1; c < row.size() && c < header.size(); ++c) {
                const auto& col = columns[static_cast<std::size_t>(c)];
                if (!col || (col->quantity != 'T' && col->quantity != 'Z')) continue;
                auto& values = (col->quantity == 'T' ? transfer : impedance)[col->excited];
                values.resize(at + 1, {NAN, NAN});
                bool ok = false;
                double value = row[c].toDouble(&ok);
                if (!ok) value = NAN; // "nan" and "-nan" don't parse
                if (col->real) values[at].real(value);
                else values[at].imag(value);
            }
        }
        auto keepMeasured = [&](std::map<int, std::vector<std::complex<double>>>& from,
                                std::map<int, std::vector<std::complex<double>>>& into) {
            for (auto& [excited, values] : from) {
                values.resize(probe.frequencyHz.size(), {NAN, NAN});
                const bool measured = std::any_of(values.begin(), values.end(), [](const std::complex<double>& v) {
                    return std::isfinite(v.real()) && std::isfinite(v.imag());
                });
                if (measured) into[excited] = std::move(values);
            }
        };
        keepMeasured(transfer, probe.transfer);
        keepMeasured(impedance, probe.impedance);
        if (!probe.transfer.empty() || !probe.impedance.empty()) probes.push_back(std::move(probe));
    }
    return probes;
}

} // namespace

SimulationResults SimulationResults::load(const QString& resultsDir, const QString& simulationDir) {
    SimulationResults results;
    // Port names: "<index>\t<name>\t..." in any excitation's ports.txt (they all list every port).
    const QStringList runs = QDir(simulationDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& run : runs) {
        QFile ports(QDir(simulationDir).filePath(run + "/ports.txt"));
        if (!ports.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QTextStream stream(&ports);
        while (!stream.atEnd()) {
            const QString line = stream.readLine();
            if (line.startsWith('#')) continue;
            const QStringList parts = line.split('\t');
            bool ok = false;
            const int index = parts.value(0).toInt(&ok);
            if (ok && parts.size() > 1) results.portNames[index] = parts[1];
        }
        break;
    }

    static const QRegularExpression csvName(QStringLiteral(R"(^Port_(\d+)_data\.csv$)"));
    static const QRegularExpression magnitude(QStringLiteral(R"(^\|S(\d+)-(\d+)\|)"));
    static const QRegularExpression phase(QStringLiteral(R"(^Arg\(S(\d+)-(\d+)\))"));
    static const QRegularExpression delay(QStringLiteral(R"(^Delay (\d+)>(\d+))"));
    for (const QString& name : QDir(resultsDir).entryList({"Port_*_data.csv"}, QDir::Files, QDir::Name)) {
        const auto match = csvName.match(name);
        if (!match.hasMatch()) continue;
        QFile file(QDir(resultsDir).filePath(name));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QTextStream stream(&file);
        const QStringList header = fields(stream.readLine());
        PortResults port;
        port.excitedPort = match.captured(1).toInt();
        std::map<int, std::vector<double>> magnitudes, phases;
        std::vector<double> zMagnitude, zPhase;
        while (!stream.atEnd()) {
            const QStringList row = fields(stream.readLine());
            if (row.size() < header.size()) continue;
            port.frequencyHz.push_back(row[0].toDouble() * 1e6); // Frequency [MHz]
            for (int c = 1; c < header.size(); ++c) {
                const double value = row[c].toDouble();
                const QString& column = header[c];
                if (auto m = magnitude.match(column); m.hasMatch()) magnitudes[m.captured(1).toInt()].push_back(value);
                else if (auto p = phase.match(column); p.hasMatch()) phases[p.captured(1).toInt()].push_back(value);
                else if (auto d = delay.match(column); d.hasMatch()) port.delay[d.captured(2).toInt()].push_back(value);
                else if (column.startsWith("|Z")) zMagnitude.push_back(value);
                else if (column.startsWith("Arg(Z")) zPhase.push_back(value);
            }
        }
        for (const auto& [received, mags] : magnitudes) {
            const auto& args = phases[received];
            auto& s = port.s[received];
            for (std::size_t i = 0; i < mags.size(); ++i) s.push_back(std::polar(mags[i], i < args.size() ? args[i] : 0.0));
        }
        for (std::size_t i = 0; i < zMagnitude.size(); ++i) {
            port.impedance.push_back(std::polar(zMagnitude[i], i < zPhase.size() ? zPhase[i] : 0.0));
        }
        results.ports.push_back(std::move(port));
    }
    // Time-domain responses: "time_s,voltage_v" per port.
    results.resultsDir = resultsDir;
    const QDir excitations(QDir(resultsDir).filePath("excitations"));
    for (const QString& name : excitations.entryList({"*_response.csv"}, QDir::Files, QDir::Name)) {
        QFile file(excitations.filePath(name));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QTextStream stream(&file);
        stream.readLine();
        TimeResponse response;
        // The file name is the port's, with "/" (not allowed in a file name) stored as "_".
        response.name = name.chopped(QString("_response.csv").size());
        for (const auto& [index, portName] : results.portNames) {
            if (QString(portName).replace('/', '_') == response.name) response.name = portName;
        }
        while (!stream.atEnd()) {
            const QStringList row = fields(stream.readLine());
            if (row.size() < 2) continue;
            response.timeSeconds.push_back(row[0].toDouble());
            response.volts.push_back(row[1].toDouble());
        }
        if (!response.timeSeconds.empty()) results.responses.push_back(std::move(response));
    }
    results.probes = loadProbes(simulationDir);
    if (results.ports.empty()) results.error = QObject::tr("No results yet. Run the simulation to produce them.");
    return results;
}

ResultsView::ResultsView(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    _title = heading(QString());
    _tabs = new QTabWidget;
    _tabs->setDocumentMode(true);
    layout->addWidget(_title);
    layout->addWidget(_tabs, 1);
}

QList<ResultsCategory> makeResultsCategories(const SimulationResults& results, ChartStyle style) {
    const ChartPalette& colors = ChartPalette::of(style);
    ResultsCategory sParameters{QObject::tr("S-Parameters")};
    ResultsCategory impedance{QObject::tr("Impedance"), {}, QObject::tr("No impedance data.")};
    ResultsCategory smithCategory{QObject::tr("Smith")};
    smithCategory.fill = true;
    ResultsCategory delays{QObject::tr("Trace Delays"), {}, QObject::tr("No delay data.")};
    auto* smith = new SmithChart(style);
    int colorIndex = 0;
    for (const PortResults& port : results.ports) {
        const QString excited = portLabel(results, port.excitedPort);
        QChartView* magnitudeChart = makeChart(QObject::tr("Magnitude [dB]"), style);
        QChartView* phaseChart = makeChart(QObject::tr("Phase [°]"), style);
        int c = 0;
        for (const auto& [received, values] : port.s) {
            const bool returned = received == port.excitedPort;
            const QString name = returned ? QObject::tr("Returned to source")
                                          : QObject::tr("Received at %1").arg(portLabel(results, received));
            std::vector<double> db, degrees;
            for (const auto& s : values) {
                db.push_back(20 * std::log10(std::max(std::abs(s), 1e-12)));
                degrees.push_back(std::arg(s) * 180 / std::numbers::pi);
            }
            const QColor color = colors.seriesColor(c++);
            addSeries(magnitudeChart, name, port.frequencyHz, db, color, !returned);
            addSeries(phaseChart, name, port.frequencyHz, degrees, color, !returned);
            if (returned) smith->traces.push_back({excited, colors.seriesColor(colorIndex++), values});
        }
        fitYAxis(magnitudeChart);
        fitYAxis(phaseChart);
        sParameters.sections.append({QObject::tr("Excited port: %1").arg(excited), {magnitudeChart, phaseChart}});

        if (!port.impedance.empty()) {
            QChartView* zChart = makeChart(QObject::tr("|Z| [Ω]"), style);
            std::vector<double> magnitudes;
            for (const auto& z : port.impedance) magnitudes.push_back(std::abs(z));
            addSeries(zChart, QObject::tr("|Z|"), port.frequencyHz, magnitudes, colors.seriesColor(0), true);
            fitYAxis(zChart);
            impedance.sections.append({QObject::tr("Impedance at %1").arg(excited), {zChart}});
        }
        if (!port.delay.empty()) {
            QChartView* delayChart = makeChart(QObject::tr("Delay [ps]"), style);
            int d = 0;
            for (const auto& [received, values] : port.delay) {
                if (received == port.excitedPort) continue;
                std::vector<double> ps;
                for (double seconds : values) ps.push_back(seconds * 1e12);
                addSeries(delayChart, QObject::tr("To %1").arg(portLabel(results, received)), port.frequencyHz, ps,
                          colors.seriesColor(d++), true);
            }
            fitYAxis(delayChart);
            delays.sections.append({QObject::tr("Delays from %1").arg(excited), {delayChart}});
        }
    }
    smithCategory.sections.append({QString(), {smith}});

    // Every port's response with every excitation driving it, on one time axis.
    ResultsCategory timeDomain{QObject::tr("Time Domain"), {},
                               QObject::tr("No time-domain responses: they're computed for simulations with excitations.")};
    if (!results.responses.empty()) {
        QChartView* timeChart = makeTimeChart(style);
        double peak = 0;
        for (const TimeResponse& response : results.responses) {
            for (const double v : response.volts) {
                if (std::isfinite(v)) peak = std::max(peak, std::abs(v));
            }
        }
        const auto [scale, unit] = peak < 1e-3 ? std::pair{1e6, QString::fromUtf8("\xC2\xB5V")}
                                 : peak < 1    ? std::pair{1e3, QString("mV")}
                                               : std::pair{1.0, QString("V")};
        for (QAbstractAxis* axis : timeChart->chart()->axes(Qt::Vertical)) {
            axis->setTitleText(QObject::tr("Voltage [%1]").arg(unit));
        }
        int t = 0;
        for (const TimeResponse& response : results.responses) {
            auto* series = new QLineSeries;
            series->setName(response.name);
            QList<QPointF> points;
            // Plenty for a chart; long continuous runs hold far more samples than pixels.
            const std::size_t stride = std::max<std::size_t>(1, response.timeSeconds.size() / 4000);
            for (std::size_t i = 0; i < response.timeSeconds.size(); i += stride) {
                if (std::isfinite(response.volts[i])) points.append({response.timeSeconds[i] * 1e9, response.volts[i] * scale});
            }
            series->replace(points);
            series->setPen(QPen(colors.seriesColor(t++), 1.6));
            timeChart->chart()->addSeries(series);
            for (QAbstractAxis* axis : timeChart->chart()->axes()) series->attachAxis(axis);
        }
        double end = 0;
        for (const TimeResponse& response : results.responses) end = std::max(end, response.timeSeconds.back() * 1e9);
        for (QAbstractAxis* axis : timeChart->chart()->axes(Qt::Horizontal)) axis->setRange(0, end);
        fitYAxis(timeChart);
        for (QAbstractAxis* axis : timeChart->chart()->axes()) {
            if (auto* values = qobject_cast<QValueAxis*>(axis)) values->applyNiceNumbers();
        }
        timeChart->setMinimumHeight(420);
        timeDomain.sections.append({QObject::tr("Voltage at each port, every excitation together"), {timeChart}});
    }

    // Each probed pin: its voltage per unit incident voltage, per excitation that reached it (as the
    // macOS app shows it; the raw spectrum is V/Hz, not volts), and its impedance if it measures one.
    ResultsCategory probes{QObject::tr("Probes"), {},
                           QObject::tr("No probes. Tick \"Probed\" on a pin in the Selection tab to see the voltage there.")};
    for (const ProbeResults& probe : results.probes) {
        ResultsSection section{QObject::tr("Probe: %1").arg(portLabel(results, probe.port))};
        if (!probe.transfer.empty()) {
            QChartView* chart = makeChart(QObject::tr("|V / V_inc|"), style);
            int c = 0;
            for (const auto& [excited, values] : probe.transfer) {
                std::vector<double> magnitudes;
                for (const auto& t : values) magnitudes.push_back(std::abs(t));
                addSeries(chart, QObject::tr("Excited at %1").arg(portLabel(results, excited)), probe.frequencyHz,
                          magnitudes, colors.seriesColor(c++), true);
            }
            fitYAxis(chart);
            // A magnitude: the axis starts at zero unless the curves sit far above it.
            for (QAbstractAxis* axis : chart->chart()->axes(Qt::Vertical)) {
                auto* values = static_cast<QValueAxis*>(axis);
                if (values->min() < 0 || values->min() < values->max() * 0.5) values->setMin(0);
            }
            section.graphs << chart;
        }
        if (!probe.impedance.empty()) {
            QChartView* chart = makeChart(QObject::tr("|Z| [Ω]"), style);
            int c = 0;
            for (const auto& [excited, values] : probe.impedance) {
                std::vector<double> magnitudes;
                for (const auto& z : values) magnitudes.push_back(std::abs(z));
                addSeries(chart, QObject::tr("Excited at %1").arg(portLabel(results, excited)), probe.frequencyHz,
                          magnitudes, colors.seriesColor(c++), true);
            }
            fitYAxis(chart);
            section.graphs << chart;
        }
        probes.sections.append(section);
    }
    return {sParameters, impedance, smithCategory, delays, timeDomain, probes};
}

void ResultsView::showResults(const QString& title, const SimulationResults& results) {
    _title->setText(title);
    while (_tabs->count() > 0) {
        QWidget* page = _tabs->widget(0);
        _tabs->removeTab(0);
        page->deleteLater();
    }
    if (!results.error.isEmpty()) {
        _tabs->addTab(message(results.error), tr("Results"));
        return;
    }
    for (const ResultsCategory& category : makeResultsCategories(results, ChartStyle::Screen)) {
        if (category.sections.isEmpty()) {
            _tabs->addTab(message(category.emptyMessage), category.title);
        } else if (category.fill) {
            _tabs->addTab(category.sections.front().graphs.front(), category.title);
        } else {
            QList<QWidget*> column;
            for (const ResultsSection& section : category.sections) {
                if (!section.title.isEmpty()) column << heading(section.title);
                column << section.graphs;
            }
            _tabs->addTab(scrollingColumn(column), category.title);
        }
    }
    const EyeResults eyes = EyeResults::load(results.resultsDir);
    _tabs->addTab(eyes.eyes.empty() ? message(eyes.error) : makeEyesPage(eyes), tr("Eyes"));
}

} // namespace kiems_gui
