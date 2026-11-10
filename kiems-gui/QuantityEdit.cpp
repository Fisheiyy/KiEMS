#include "QuantityEdit.hpp"

#include <QRegularExpression>

#include <cmath>
#include <utility>
#include <vector>

namespace kiems_gui {

namespace {

const QString kMicro = QString(QChar(0x00B5));

/// Accepted length units, first spelling of each being how it's shown.
const std::vector<std::pair<QStringList, double>>& lengthUnits() {
    static const std::vector<std::pair<QStringList, double>> units = {
        {{"mm", "millimeter", "millimeters", "millimetre", "millimetres"}, 1000.0},
        {{kMicro + "m", "um", "micron", "microns", "micrometer", "micrometers"}, 1.0},
        {{"mil", "mils", "thou"}, 25.4},
        {{"in", "inch", "inches", "\""}, 25400.0},
        {{"cm"}, 10000.0},
        {{"m"}, 1e6},
    };
    return units;
}

/// SI prefixes, case-sensitive ("M" mega, "m" milli); "u" stands in for µ on input.
double prefixScale(const QString& prefix, bool* ok) {
    *ok = true;
    if (prefix.isEmpty()) return 1;
    if (prefix == "T") return 1e12;
    if (prefix == "G") return 1e9;
    if (prefix == "M") return 1e6;
    if (prefix == "k" || prefix == "K") return 1e3;
    if (prefix == "m") return 1e-3;
    if (prefix == "u" || prefix == kMicro || prefix == QString(QChar(0x03BC))) return 1e-6;
    if (prefix == "n") return 1e-9;
    if (prefix == "p") return 1e-12;
    if (prefix == "f") return 1e-15;
    *ok = false;
    return 1;
}

QString siUnit(QuantityEdit::Kind kind) {
    switch (kind) {
    case QuantityEdit::Kind::Frequency: return "Hz";
    case QuantityEdit::Kind::Time: return "s";
    case QuantityEdit::Kind::Voltage: return "V";
    case QuantityEdit::Kind::BitRate: return "b/s";
    case QuantityEdit::Kind::Resistance: return QString(QChar(0x03A9));
    case QuantityEdit::Kind::Capacitance: return "F";
    default: return {};
    }
}

QStringList siUnitSpellings(QuantityEdit::Kind kind) {
    switch (kind) {
    case QuantityEdit::Kind::Frequency: return {"hz"};
    case QuantityEdit::Kind::Time: return {"s", "sec", "secs", "second", "seconds"};
    case QuantityEdit::Kind::Voltage: return {"v", "volt", "volts"};
    case QuantityEdit::Kind::BitRate: return {"b/s", "bit/s", "bps"};
    case QuantityEdit::Kind::Resistance: return {"ohms", "ohm", QString(QChar(0x03A9)), QString(QChar(0x2126))};
    case QuantityEdit::Kind::Capacitance: return {"farads", "farad", "f"};
    default: return {};
    }
}

QString number(double value) {
    QString text = QString::number(value, 'g', 6);
    if (text.contains('e')) text = QString::number(value, 'g', 4);
    return text;
}

QString formatSI(double value, const QString& unit) {
    if (value == 0 || !std::isfinite(value)) return number(value) + " " + unit;
    static const std::pair<double, QString> prefixes[] = {
        {1e12, "T"}, {1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1, ""}, {1e-3, "m"}, {1e-6, kMicro}, {1e-9, "n"}, {1e-12, "p"},
    };
    const double magnitude = std::fabs(value);
    for (const auto& [scale, prefix] : prefixes) {
        // A little below 1 still counts, so 999.9999999 MHz reads as 1 GHz rather than 1000 MHz.
        if (magnitude >= scale * 0.9999995) return number(value / scale) + " " + prefix + unit;
    }
    return number(value / 1e-15) + " f" + unit;
}

} // namespace

QuantityEdit::QuantityEdit(Kind kind, QWidget* parent) : QLineEdit(parent), _kind(kind) {
    setAlignment(Qt::AlignRight);
    connect(this, &QLineEdit::editingFinished, this, &QuantityEdit::commit);
}

void QuantityEdit::setAllowEmpty(bool allow, const QString& placeholder) {
    _allowEmpty = allow;
    setPlaceholderText(placeholder);
}

void QuantityEdit::setValue(std::optional<double> value) {
    _value = value;
    showValue();
}

void QuantityEdit::refreshValue(std::optional<double> value) {
    if (hasFocus() && isModified()) return;
    setValue(value);
}

QString QuantityEdit::format(Kind kind, double value) {
    switch (kind) {
    case Kind::Length: return number(value / 1000.0) + " mm";
    case Kind::Frequency:
    case Kind::Time:
    case Kind::Voltage:
    case Kind::BitRate:
    case Kind::Resistance:
    case Kind::Capacitance: return formatSI(value, siUnit(kind));
    case Kind::Angle: return number(value) + QString(QChar(0x00B0));
    case Kind::UnitInterval: return number(value) + " UI";
    case Kind::Integer: return QString::number(std::llround(value));
    case Kind::Number: return number(value);
    }
    return number(value);
}

void QuantityEdit::showValue() {
    if (!_value) {
        clear();
        return;
    }
    if (_kind == Kind::Length) {
        for (const auto& [names, micrometres] : lengthUnits()) {
            if (names.first() == _lengthUnit) {
                setText(number(*_value / micrometres) + " " + _lengthUnit);
                return;
            }
        }
    }
    setText(format(_kind, *_value));
}

std::optional<double> QuantityEdit::parse(const QString& input, QString* lengthUnit) const {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^\s*([+-]?(?:\d+(?:[.,]\d*)?|[.,]\d+)(?:[eE][+-]?\d+)?)\s*(.*?)\s*$)"));
    const auto match = pattern.match(input);
    if (!match.hasMatch()) return std::nullopt;
    QString digits = match.captured(1);
    digits.replace(',', '.');
    bool ok = false;
    double value = digits.toDouble(&ok);
    if (!ok) return std::nullopt;
    const QString suffix = match.captured(2);

    switch (_kind) {
    case Kind::Length: {
        if (suffix.isEmpty()) {
            for (const auto& [names, micrometres] : lengthUnits()) {
                if (names.first() == _lengthUnit) return value * micrometres;
            }
            return value * 1000.0;
        }
        for (const auto& [names, micrometres] : lengthUnits()) {
            for (const QString& name : names) {
                if (suffix.compare(name, Qt::CaseInsensitive) == 0 && (name != "m" || suffix == "m")) {
                    if (lengthUnit) *lengthUnit = names.first();
                    return value * micrometres;
                }
            }
        }
        return std::nullopt;
    }
    case Kind::Frequency:
    case Kind::Time:
    case Kind::Voltage:
    case Kind::BitRate:
    case Kind::Resistance:
    case Kind::Capacitance: {
        // An optional prefix, then optionally the unit itself ("2.4G", "2.4 GHz", "2400 MHz").
        QString rest = suffix;
        for (const QString& unit : siUnitSpellings(_kind)) {
            if (rest.endsWith(unit, Qt::CaseInsensitive) && rest.size() <= unit.size() + 1) {
                rest.chop(unit.size());
                break;
            }
        }
        rest = rest.trimmed();
        bool prefixOk = false;
        const double scale = prefixScale(rest, &prefixOk);
        if (!prefixOk) return std::nullopt;
        return value * scale;
    }
    case Kind::UnitInterval:
        if (!suffix.isEmpty() && suffix.compare("ui", Qt::CaseInsensitive) != 0) return std::nullopt;
        return value;
    case Kind::Angle: {
        static const QStringList degrees = {QString(QChar(0x00B0)), "deg", "degree", "degrees"};
        if (!suffix.isEmpty() && !degrees.contains(suffix, Qt::CaseInsensitive)) return std::nullopt;
        return value;
    }
    case Kind::Integer:
        if (!suffix.isEmpty() || value != std::floor(value)) return std::nullopt;
        return value;
    case Kind::Number:
        if (!suffix.isEmpty()) return std::nullopt;
        return value;
    }
    return std::nullopt;
}

void QuantityEdit::commit() {
    const QString text = this->text().trimmed();
    if (text.isEmpty()) {
        if (_allowEmpty) {
            if (_value) {
                _value.reset();
                emit valueEdited(std::nullopt);
            }
        }
        showValue();
        return;
    }
    QString unit = _lengthUnit;
    const std::optional<double> parsed = parse(text, &unit);
    if (!parsed || *parsed < _minimum || *parsed > _maximum) {
        showValue(); // reverts what can't be used
        return;
    }
    _lengthUnit = unit;
    const bool changed = !_value || std::fabs(*_value - *parsed) > 1e-12 * std::max(1.0, std::fabs(*parsed));
    _value = parsed;
    showValue();
    if (changed) emit valueEdited(_value);
}

} // namespace kiems_gui
