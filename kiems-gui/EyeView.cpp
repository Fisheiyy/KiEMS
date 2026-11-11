#include "EyeView.hpp"

#include "EyeTestPresets.hpp"
#include "QuantityEdit.hpp"

#include <nlohmann/json.hpp>

#include <QButtonGroup>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace kiems_gui {

namespace {

const QColor kBackground(0x1e, 0x20, 0x24);
const QColor kGrid(0x3a, 0x3d, 0x44);
const QColor kText(0xd6, 0xd8, 0xdc);
const QColor kTrace(0x5a, 0xb4, 0xff);
const QColor kNoisyTrace(0xff, 0xa0, 0x5a);
const QColor kKeepOut(0xff, 0x45, 0x3a);
const QColor kLimit(0xff, 0x9f, 0x0a);
const QColor kPass(0x4c, 0xd9, 0x64);

/// An eye chart's colours in each style.
struct EyePalette {
    QColor background, grid, text, trace, noisyTrace, keepOut, limit;

    static EyePalette of(ChartStyle style) {
        if (style == ChartStyle::Print) {
            return {Qt::white,          QColor(0xd4, 0xd4, 0xd8), QColor(0x1f, 0x23, 0x28), QColor(0x1f, 0x6f, 0xd1),
                    QColor(0xd9, 0x48, 0x0f), QColor(0xd7, 0x00, 0x15), QColor(0xc7, 0x6b, 0x00)};
        }
        return {kBackground, kGrid, kText, kTrace, kNoisyTrace, kKeepOut, kLimit};
    }
};

std::vector<double> numbers(const nlohmann::json& array) {
    std::vector<double> values;
    if (!array.is_array()) return values;
    values.reserve(array.size());
    for (const auto& value : array) values.push_back(value.is_number() ? value.get<double>() : 0.0);
    return values;
}

std::vector<std::vector<double>> traceList(const nlohmann::json& array) {
    std::vector<std::vector<double>> traces;
    if (!array.is_array()) return traces;
    for (const auto& trace : array) traces.push_back(numbers(trace));
    return traces;
}

std::optional<EyeOpeningData> opening(const nlohmann::json& json) {
    if (!json.is_object()) return std::nullopt;
    EyeOpeningData data;
    data.heightV = json.value("height_v", 0.0);
    data.widthUI = json.value("width_ui", 0.0);
    data.samplingUI = json.value("sampling_ui", 0.5);
    data.oneLevelV = json.value("one_level_v", 0.0);
    data.zeroLevelV = json.value("zero_level_v", 0.0);
    data.qFactor = json.value("q_factor", 0.0);
    data.jitterUI = json.value("jitter_ui", 0.0);
    return data;
}

std::optional<EyeMaskVerdict> verdict(const nlohmann::json& json) {
    if (!json.is_object()) return std::nullopt;
    EyeMaskVerdict data;
    data.traces = json.value("traces", std::size_t(0));
    data.keepOutViolations = json.value("keep_out_violations", std::size_t(0));
    data.outerLimitViolations = json.value("outer_limit_violations", std::size_t(0));
    data.passed = json.value("passed", true);
    return data;
}

QString volts(double value) {
    return QuantityEdit::format(QuantityEdit::Kind::Voltage, value);
}

/// Ticks at 1, 2 or 5 times a power of ten, about `count` of them across [low, high].
std::vector<double> ticks(double low, double high, int count) {
    std::vector<double> result;
    const double span = high - low;
    if (!(span > 0)) return result;
    const double rough = span / count;
    const double power = std::pow(10.0, std::floor(std::log10(rough)));
    const double step = rough / power >= 5 ? 5 * power : rough / power >= 2 ? 2 * power : power;
    for (double v = std::ceil(low / step) * step; v <= high + step * 1e-9; v += step) result.push_back(std::abs(v) < step * 1e-9 ? 0 : v);
    return result;
}

/// One eye: its traces over -0.5...1.5 UI, the mask on the decision threshold, and axes.
class EyeChart : public QWidget {
public:
    EyeChart(const EyeEntry& eye, const EyeResults& test, ChartStyle style = ChartStyle::Screen, QWidget* parent = nullptr)
        : QWidget(parent), _eye(eye), _test(test), _colors(EyePalette::of(style)) {
        setMinimumHeight(300);
    }

    void setNoisy(bool noisy) {
        _noisy = noisy;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), _colors.background);
        const auto& traces = _noisy && !_eye.noisyTraces.empty() ? _eye.noisyTraces : _eye.traces;
        const auto& time = _eye.timeUI;
        if (time.size() < 2 || traces.empty()) return;

        // The range: every displayed trace, and the outer limit when there is one.
        double low = INFINITY, high = -INFINITY;
        for (const auto& trace : traces) {
            for (const double v : trace) {
                low = std::min(low, v);
                high = std::max(high, v);
            }
        }
        if (_test.maskY2 > 0) {
            low = std::min(low, _eye.thresholdV - _test.maskY2);
            high = std::max(high, _eye.thresholdV + _test.maskY2);
        }
        if (!std::isfinite(low) || !(high > low)) return;
        const double pad = (high - low) * 0.08;
        low -= pad;
        high += pad;

        const QFontMetricsF metrics(font());
        const QRectF plot = QRectF(rect()).adjusted(metrics.horizontalAdvance("-000 mV") + 14, 10, -12,
                                                    -(metrics.height() * 2 + 10));
        const double t0 = time.front(), t1 = time.back();
        auto point = [&](double t, double v) {
            return QPointF(plot.left() + (t - t0) / (t1 - t0) * plot.width(), plot.bottom() - (v - low) / (high - low) * plot.height());
        };

        // Grid and labels.
        painter.setPen(QPen(_colors.grid, 1));
        painter.drawRect(plot);
        painter.setFont(font());
        for (const double v : ticks(low, high, 6)) {
            const double y = point(t0, v).y();
            painter.setPen(QPen(_colors.grid, 1));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(_colors.text);
            const QString label = volts(v);
            painter.drawText(QPointF(plot.left() - metrics.horizontalAdvance(label) - 6, y + metrics.ascent() / 2 - 1), label);
        }
        for (const double t : {-0.5, 0.0, 0.5, 1.0, 1.5}) {
            const double x = point(t, low).x();
            painter.setPen(QPen(_colors.grid, 1, t == 0.0 || t == 1.0 ? Qt::DashLine : Qt::SolidLine));
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            painter.setPen(_colors.text);
            const QString label = QString::number(t) + " UI";
            const double labelX = std::clamp(x - metrics.horizontalAdvance(label) / 2, 0.0,
                                             width() - metrics.horizontalAdvance(label) - 2);
            painter.drawText(QPointF(labelX, plot.bottom() + metrics.ascent() + 4), label);
        }

        // The outer limit: out of bounds beyond threshold +- Y2.
        painter.save();
        painter.setClipRect(plot);
        if (_test.maskY2 > 0) {
            QColor fill = _colors.limit;
            fill.setAlphaF(0.18);
            const double top = point(t0, _eye.thresholdV + _test.maskY2).y();
            const double bottom = point(t0, _eye.thresholdV - _test.maskY2).y();
            painter.fillRect(QRectF(QPointF(plot.left(), plot.top()), QPointF(plot.right(), top)), fill);
            painter.fillRect(QRectF(QPointF(plot.left(), bottom), QPointF(plot.right(), plot.bottom())), fill);
            painter.setPen(QPen(_colors.limit, 1.4));
            painter.drawLine(QPointF(plot.left(), top), QPointF(plot.right(), top));
            painter.drawLine(QPointF(plot.left(), bottom), QPointF(plot.right(), bottom));
        }

        // The traces, faint enough to show where they bunch up.
        QColor traceColor = _noisy && !_eye.noisyTraces.empty() ? _colors.noisyTrace : _colors.trace;
        traceColor.setAlphaF(traces.size() > 200 ? 0.18 : traces.size() > 60 ? 0.3 : 0.55);
        painter.setPen(QPen(traceColor, 1.1));
        for (const auto& trace : traces) {
            QPainterPath path;
            const std::size_t n = std::min(trace.size(), time.size());
            for (std::size_t i = 0; i < n; ++i) {
                if (i == 0) path.moveTo(point(time[i], trace[i]));
                else path.lineTo(point(time[i], trace[i]));
            }
            painter.drawPath(path);
        }

        // The keep-out hexagon between the crossings at 0 and 1 UI.
        if (_test.maskY1 > 0 && _test.maskX1 < 0.5) {
            const double v = _eye.thresholdV;
            QPainterPath hexagon;
            hexagon.moveTo(point(_test.maskX1, v));
            hexagon.lineTo(point(_test.maskX2, v + _test.maskY1));
            hexagon.lineTo(point(1 - _test.maskX2, v + _test.maskY1));
            hexagon.lineTo(point(1 - _test.maskX1, v));
            hexagon.lineTo(point(1 - _test.maskX2, v - _test.maskY1));
            hexagon.lineTo(point(_test.maskX2, v - _test.maskY1));
            hexagon.closeSubpath();
            QColor fill = _colors.keepOut;
            fill.setAlphaF(0.3);
            painter.setBrush(fill);
            painter.setPen(QPen(_colors.keepOut, 1.5));
            painter.drawPath(hexagon);
            painter.setBrush(Qt::NoBrush);
        }
        // The decision threshold.
        painter.setPen(QPen(_colors.text, 1, Qt::DotLine));
        painter.drawLine(point(t0, _eye.thresholdV), point(t1, _eye.thresholdV));
        painter.restore();

        painter.setPen(_colors.text);
        const QString axis = QObject::tr("Time, unit intervals at %1")
                                 .arg(QuantityEdit::format(QuantityEdit::Kind::BitRate, _eye.bitRate));
        painter.drawText(QPointF(plot.center().x() - metrics.horizontalAdvance(axis) / 2, height() - 4), axis);
    }

private:
    const EyeEntry& _eye;
    const EyeResults& _test;
    EyePalette _colors;
    bool _noisy = false;
};

} // namespace

QString eyeVerdictText(const std::optional<EyeMaskVerdict>& verdict) {
    if (!verdict) return {};
    if (verdict->passed) return QObject::tr("Passes the eye mask (%1 traces).").arg(verdict->traces);
    QStringList problems;
    if (verdict->keepOutViolations > 0) {
        problems << QObject::tr("%1 of %2 traces enter the keep-out region").arg(verdict->keepOutViolations).arg(verdict->traces);
    }
    if (verdict->outerLimitViolations > 0) {
        problems << QObject::tr("%1 go past the outer limit").arg(verdict->outerLimitViolations);
    }
    return QObject::tr("Fails the eye mask: %1.").arg(problems.join(QObject::tr("; ")));
}

QString eyeFigures(const EyeOpeningData& opening) {
    QStringList parts;
    parts << QObject::tr("Height %1").arg(volts(opening.heightV));
    parts << QObject::tr("width %1 UI").arg(QString::number(opening.widthUI, 'f', 3));
    parts << QObject::tr("amplitude %1").arg(volts(opening.oneLevelV - opening.zeroLevelV));
    if (opening.qFactor > 0) parts << QObject::tr("Q %1").arg(QString::number(opening.qFactor, 'g', 3));
    parts << QObject::tr("jitter %1 UI").arg(QString::number(opening.jitterUI, 'f', 3));
    parts << QObject::tr("sampled at %1 UI").arg(QString::number(opening.samplingUI, 'f', 2));
    return parts.join(QString::fromUtf8(" \xC2\xB7 "));
}

QString eyeTestSummary(const EyeResults& results) {
    QString summary = QObject::tr("Eye test: %1, launched at %2")
                          .arg(EyeTestPreset::titleFor(results.standard), volts(results.signalLevel));
    if (!results.hasMask()) summary += QObject::tr(". No mask is set (the Tests tab sets one), so nothing is judged.");
    std::size_t failures = 0;
    for (const EyeEntry& eye : results.eyes) {
        if (eye.failsMask()) ++failures;
    }
    if (results.hasMask()) {
        summary += failures == 0 ? QObject::tr(". Every eye passes.")
                 : failures == results.eyes.size() && failures == 1 ? QObject::tr(". It fails.")
                 : failures == 1 ? QObject::tr(". One eye fails.")
                                 : QObject::tr(". %1 of %2 eyes fail.").arg(failures).arg(results.eyes.size());
    }
    return summary;
}

QWidget* makeEyeChart(const EyeEntry& eye, const EyeResults& results, bool noisy, ChartStyle style) {
    auto* chart = new EyeChart(eye, results, style);
    chart->setNoisy(noisy);
    return chart;
}

namespace {

/// One eye with its verdict, its figures and, when there's noise, the choice of which to show.
class EyePanel : public QWidget {
public:
    EyePanel(const EyeEntry& eye, const EyeResults& results) : _eye(eye) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 8, 0, 8);
        auto* title = new QLabel(eye.name + (eye.differential ? QObject::tr(" (differential)") : QString()));
        QFont font = title->font();
        font.setBold(true);
        font.setPointSizeF(font.pointSizeF() * 1.1);
        title->setFont(font);
        title->setWordWrap(true);
        _verdict = new QLabel;
        _verdict->setWordWrap(true);
        _figures = new QLabel;
        _figures->setWordWrap(true);
        _figures->setTextInteractionFlags(Qt::TextSelectableByMouse);
        _chart = new EyeChart(_eye, results);
        layout->addWidget(title);
        layout->addWidget(_verdict);

        if (!eye.noisyTraces.empty()) {
            auto* row = new QHBoxLayout;
            auto* clean = new QPushButton(QObject::tr("Without noise"));
            auto* noisy = new QPushButton(eye.aggressors == 1
                                              ? QObject::tr("With noise from the adversarial excitation")
                                              : QObject::tr("With noise from %1 adversarial excitations").arg(eye.aggressors));
            auto* group = new QButtonGroup(this);
            for (QPushButton* button : {clean, noisy}) {
                button->setCheckable(true);
                group->addButton(button);
                row->addWidget(button);
            }
            row->addStretch();
            clean->setChecked(true);
            layout->addLayout(row);
            QObject::connect(noisy, &QPushButton::toggled, this, [this](bool on) { show(on); });
        }
        layout->addWidget(_chart);
        layout->addWidget(_figures);
        show(false);
    }

private:
    void show(bool noisy) {
        _chart->setNoisy(noisy);
        const auto& verdict = noisy ? _eye.noisyMask : _eye.mask;
        const EyeOpeningData& opening = noisy && _eye.noisyOpening ? *_eye.noisyOpening : _eye.opening;
        _verdict->setText(eyeVerdictText(verdict));
        QPalette palette = _verdict->palette();
        palette.setColor(QPalette::WindowText, verdict && !verdict->passed ? kKeepOut : kPass);
        _verdict->setPalette(palette);
        _verdict->setVisible(verdict.has_value());
        QString text = eyeFigures(opening);
        if (noisy && _eye.noiseLowest && _eye.noiseHighest) {
            text += "\n" + QObject::tr("Across independent sets of draws the height ranged %1 to %2.")
                               .arg(volts(_eye.noiseLowest->heightV), volts(_eye.noiseHighest->heightV));
        }
        _figures->setText(text);
    }

    const EyeEntry& _eye;
    QLabel* _verdict;
    QLabel* _figures;
    EyeChart* _chart;
};

/// Owns the results the panels draw from, for as long as the page lives.
class EyesPage : public QScrollArea {
public:
    explicit EyesPage(const EyeResults& results) : _results(results) {
        auto* content = new QWidget;
        auto* layout = new QVBoxLayout(content);
        auto* header = new QLabel(eyeTestSummary(_results));
        header->setWordWrap(true);
        layout->addWidget(header);
        for (const EyeEntry& eye : _results.eyes) layout->addWidget(new EyePanel(eye, _results));
        layout->addStretch();
        setWidget(content);
        setWidgetResizable(true);
        setFrameShape(QFrame::NoFrame);
    }

private:
    EyeResults _results;
};

} // namespace

EyeResults EyeResults::load(const QString& resultsDir) {
    EyeResults results;
    QFile file(QDir(resultsDir).filePath("eyes.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        results.error = QObject::tr("No eye diagrams yet: they're built when the simulation runs.");
        return results;
    }
    try {
        const nlohmann::json json = nlohmann::json::parse(file.readAll().toStdString());
        const nlohmann::json& test = json.value("test", nlohmann::json::object());
        results.standard = QString::fromStdString(test.value("standard", std::string("custom")));
        results.signalLevel = test.value("signal_level", 1.0);
        results.maskX1 = test.value("mask_x1", 0.0);
        results.maskX2 = test.value("mask_x2", 0.0);
        results.maskY1 = test.value("mask_y1", 0.0);
        results.maskY2 = test.value("mask_y2", 0.0);
        for (const auto& entry : json.value("eyes", nlohmann::json::array())) {
            EyeEntry eye;
            eye.name = QString::fromStdString(entry.value("name", std::string()));
            eye.differential = entry.value("differential", false);
            eye.aggressors = entry.value("aggressors", std::size_t(0));
            eye.bitRate = entry.value("bit_rate", 0.0);
            eye.thresholdV = entry.value("threshold_v", 0.0);
            eye.timeUI = numbers(entry.value("time_ui", nlohmann::json::array()));
            eye.traces = traceList(entry.value("traces", nlohmann::json::array()));
            eye.opening = opening(entry.value("opening", nlohmann::json())).value_or(EyeOpeningData{});
            eye.mask = verdict(entry.value("mask", nlohmann::json()));
            eye.noisyTraces = traceList(entry.value("noisy_traces", nlohmann::json::array()));
            eye.noisyOpening = opening(entry.value("noisy_opening", nlohmann::json()));
            eye.noisyMask = verdict(entry.value("noisy_mask", nlohmann::json()));
            if (const auto& noise = entry.value("noise", nlohmann::json()); noise.is_object()) {
                eye.noiseLowest = opening(noise.value("lowest", nlohmann::json()));
                eye.noiseHighest = opening(noise.value("highest", nlohmann::json()));
            }
            results.eyes.push_back(std::move(eye));
        }
    } catch (const std::exception& exception) {
        results.error = QObject::tr("The eye diagrams couldn't be read: %1").arg(exception.what());
        return results;
    }
    if (results.eyes.empty()) {
        results.error = QObject::tr("This simulation has no eyes: an eye needs a main excitation and a probed pin "
                                    "(or a differential pair) to receive it.");
    }
    return results;
}

QWidget* makeEyesPage(const EyeResults& results) {
    return new EyesPage(results);
}

} // namespace kiems_gui
