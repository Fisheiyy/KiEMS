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

namespace kiems_gui {

namespace {

const QColor kChartBackground(0x1e, 0x20, 0x24);
const QColor kGrid(0x3a, 0x3d, 0x44);
const QColor kText(0xd6, 0xd8, 0xdc);
const QList<QColor> kSeriesColors = {QColor(0xff, 0x8c, 0x2a), QColor(0x8f, 0xd1, 0x4f), QColor(0xe0, 0x5a, 0x7b),
                                     QColor(0x5a, 0xb4, 0xff), QColor(0xf2, 0xd0, 0x4b), QColor(0xb4, 0x8c, 0xff),
                                     QColor(0x4c, 0xd9, 0xc0), QColor(0xff, 0xa0, 0xa0)};

QString portLabel(const SimulationResults& results, int port) {
    const auto it = results.portNames.find(port);
    return it != results.portNames.end() ? it->second : QObject::tr("port %1").arg(port);
}

/// One chart: frequency (GHz, log scale) against `yTitle`.
QChartView* makeChart(const QString& yTitle) {
    auto* chart = new QChart;
    chart->setBackgroundBrush(kChartBackground);
    chart->setBackgroundRoundness(0);
    chart->setMargins(QMargins(8, 8, 8, 8));
    chart->legend()->setLabelColor(kText);
    chart->legend()->setAlignment(Qt::AlignTop);
    auto* x = new QLogValueAxis;
    x->setTitleText(QObject::tr("Frequency [GHz]"));
    x->setLabelFormat("%g");
    x->setBase(10);
    x->setMinorTickCount(8);
    auto* y = new QValueAxis;
    y->setTitleText(yTitle);
    for (QAbstractAxis* axis : {static_cast<QAbstractAxis*>(x), static_cast<QAbstractAxis*>(y)}) {
        axis->setLabelsColor(kText);
        axis->setTitleBrush(kText);
        axis->setGridLineColor(kGrid);
        axis->setMinorGridLineColor(kGrid.darker(130));
        axis->setLinePenColor(kGrid);
    }
    chart->addAxis(x, Qt::AlignBottom);
    chart->addAxis(y, Qt::AlignLeft);
    auto* view = new QChartView(chart);
    view->setRenderHint(QPainter::Antialiasing);
    view->setMinimumHeight(300);
    return view;
}

/// One chart of time (ns) against volts.
QChartView* makeTimeChart() {
    QChartView* view = makeChart(QObject::tr("Voltage [V]"));
    QChart* chart = view->chart();
    QAbstractAxis* logAxis = chart->axes(Qt::Horizontal).value(0);
    chart->removeAxis(logAxis);
    delete logAxis;
    auto* x = new QValueAxis;
    x->setTitleText(QObject::tr("Time [ns]"));
    x->setLabelFormat("%g");
    x->setLabelsColor(kText);
    x->setTitleBrush(kText);
    x->setGridLineColor(kGrid);
    x->setLinePenColor(kGrid);
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
    explicit SmithChart(QWidget* parent = nullptr) : QWidget(parent) { setMinimumSize(360, 360); }

    struct Trace {
        QString name;
        QColor color;
        std::vector<std::complex<double>> gamma;
    };
    std::vector<Trace> traces;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), kChartBackground);
        painter.setRenderHint(QPainter::Antialiasing);
        const double radius = std::min(width(), height()) / 2.0 - 40;
        const QPointF centre(width() / 2.0, height() / 2.0 + 10);
        auto toScreen = [&](std::complex<double> g) { return centre + QPointF(g.real() * radius, -g.imag() * radius); };

        painter.save();
        QPainterPath unit;
        unit.addEllipse(centre, radius, radius);
        painter.setClipPath(unit);
        painter.setPen(QPen(kGrid, 1));
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
        painter.setPen(QPen(kText, 1));
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
            painter.setPen(kText);
            painter.drawText(QPointF(38, legendY + 4), trace.name);
            legendY += 18;
        }
    }
};

/// Splits one CSV row, trimming each field.
QStringList fields(const QString& line) {
    QStringList parts = line.split(',');
    for (QString& part : parts) part = part.trimmed();
    return parts;
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

    QList<QWidget*> sParameters, impedance, delays;
    auto* smith = new SmithChart;
    int colorIndex = 0;
    for (const PortResults& port : results.ports) {
        const QString excited = portLabel(results, port.excitedPort);
        sParameters << heading(tr("Excited port: %1").arg(excited));
        QChartView* magnitudeChart = makeChart(tr("Magnitude [dB]"));
        QChartView* phaseChart = makeChart(tr("Phase [°]"));
        int c = 0;
        for (const auto& [received, values] : port.s) {
            const bool returned = received == port.excitedPort;
            const QString name = returned ? tr("Returned to source") : tr("Received at %1").arg(portLabel(results, received));
            std::vector<double> db, degrees;
            for (const auto& s : values) {
                db.push_back(20 * std::log10(std::max(std::abs(s), 1e-12)));
                degrees.push_back(std::arg(s) * 180 / std::numbers::pi);
            }
            const QColor color = kSeriesColors[c++ % kSeriesColors.size()];
            addSeries(magnitudeChart, name, port.frequencyHz, db, color, !returned);
            addSeries(phaseChart, name, port.frequencyHz, degrees, color, !returned);
            if (returned) smith->traces.push_back({excited, kSeriesColors[colorIndex++ % kSeriesColors.size()], values});
        }
        fitYAxis(magnitudeChart);
        fitYAxis(phaseChart);
        sParameters << magnitudeChart << phaseChart;

        if (!port.impedance.empty()) {
            impedance << heading(tr("Impedance at %1").arg(excited));
            QChartView* zChart = makeChart(tr("|Z| [Ω]"));
            std::vector<double> magnitudes;
            for (const auto& z : port.impedance) magnitudes.push_back(std::abs(z));
            addSeries(zChart, tr("|Z|"), port.frequencyHz, magnitudes, kSeriesColors[0], true);
            fitYAxis(zChart);
            impedance << zChart;
        }
        if (!port.delay.empty()) {
            delays << heading(tr("Delays from %1").arg(excited));
            QChartView* delayChart = makeChart(tr("Delay [ps]"));
            int d = 0;
            for (const auto& [received, values] : port.delay) {
                if (received == port.excitedPort) continue;
                std::vector<double> ps;
                for (double seconds : values) ps.push_back(seconds * 1e12);
                addSeries(delayChart, tr("To %1").arg(portLabel(results, received)), port.frequencyHz, ps,
                          kSeriesColors[d++ % kSeriesColors.size()], true);
            }
            fitYAxis(delayChart);
            delays << delayChart;
        }
    }
    _tabs->addTab(scrollingColumn(sParameters), tr("S-Parameters"));
    _tabs->addTab(impedance.isEmpty() ? message(tr("No impedance data.")) : scrollingColumn(impedance), tr("Impedance"));
    _tabs->addTab(smith, tr("Smith"));
    _tabs->addTab(delays.isEmpty() ? message(tr("No delay data.")) : scrollingColumn(delays), tr("Trace Delays"));

    // Every port's response with every excitation driving it, on one time axis.
    if (results.responses.empty()) {
        _tabs->addTab(message(tr("No time-domain responses: they're computed for simulations with excitations.")),
                      tr("Time Domain"));
    } else {
        QChartView* timeChart = makeTimeChart();
        double peak = 0;
        for (const TimeResponse& response : results.responses) {
            for (const double v : response.volts) {
                if (std::isfinite(v)) peak = std::max(peak, std::abs(v));
            }
        }
        const auto [scale, unit] = peak < 1e-3 ? std::pair{1e6, QString::fromUtf8("\xC2\xB5V")}
                                 : peak < 1    ? std::pair{1e3, QString("mV")}
                                               : std::pair{1.0, QString("V")};
        for (QAbstractAxis* axis : timeChart->chart()->axes(Qt::Vertical)) axis->setTitleText(tr("Voltage [%1]").arg(unit));
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
            series->setPen(QPen(kSeriesColors[t++ % kSeriesColors.size()], 1.6));
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
        _tabs->addTab(scrollingColumn({heading(tr("Voltage at each port, every excitation together")), timeChart}),
                      tr("Time Domain"));
    }

    const EyeResults eyes = EyeResults::load(results.resultsDir);
    _tabs->addTab(eyes.eyes.empty() ? message(eyes.error) : makeEyesPage(eyes), tr("Eyes"));
}

} // namespace kiems_gui
