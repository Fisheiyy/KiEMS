// A line edit for one physical quantity, shown with its unit and accepting typed units: lengths in
// mm, µm, mil, inch or cm (stored in micrometres, the unit of every length in simulation.json),
// frequencies, times and voltages with SI prefixes ("2.4 GHz", "500 ps", "150 mV"), and plain or
// whole numbers. A length remembers the unit last typed into it and keeps showing that one, as the
// macOS app's fields do.
#pragma once

#include <QLineEdit>

#include <optional>

namespace kiems_gui {

class QuantityEdit : public QLineEdit {
    Q_OBJECT

public:
    enum class Kind {
        Length,    ///< stored in µm
        Frequency, ///< Hz
        Time,      ///< s
        Voltage,   ///< V
        BitRate,   ///< bits per second
        Resistance, ///< ohms
        Capacitance, ///< farads
        Angle,     ///< degrees
        UnitInterval, ///< a fraction of a bit period, shown as "UI"
        Number,
        Integer,
    };

    explicit QuantityEdit(Kind kind, QWidget* parent = nullptr);

    /// Empty input then means "none" (valueEdited(nullopt)) and shows `placeholder`.
    void setAllowEmpty(bool allow, const QString& placeholder = {});
    void setMinimum(double minimum) { _minimum = minimum; }
    void setMaximum(double maximum) { _maximum = maximum; }

    std::optional<double> value() const { return _value; }
    /// Shows `value` without emitting valueEdited().
    void setValue(std::optional<double> value);
    /// setValue(), unless the user is part-way through typing a new value here.
    void refreshValue(std::optional<double> value);

    /// The text `value` is shown as in a field of `kind` (lengths in mm).
    static QString format(Kind kind, double value);

signals:
    /// The user finished editing and entered a valid, changed value.
    void valueEdited(std::optional<double> value);

private:
    void commit();
    std::optional<double> parse(const QString& text, QString* lengthUnit) const;
    void showValue();

    Kind _kind;
    std::optional<double> _value;
    bool _allowEmpty = false;
    double _minimum = -1e300;
    double _maximum = 1e300;
    QString _lengthUnit = QStringLiteral("mm");
};

} // namespace kiems_gui
