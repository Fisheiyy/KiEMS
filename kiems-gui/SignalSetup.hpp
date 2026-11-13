// "New Signal Simulation": a simulation of one signal set up from a few plain choices, instead of
// net by net and pin by pin. Pick a net (and say whether it's half of a differential pair), and it
// works out the rest from the board:
//
//  - the pair's other net, by name (D+ / D-, _P / _N, P / N);
//  - the nets beyond series parts (a USB pair's 22-27 ohm resistors, an AC-coupling capacitor, a
//    ferrite), with those parts modelled, so the whole channel from driver to connector is in it;
//  - which pin drives the signal (an IC's output or bidirectional pin, by its KiCad pin type) and
//    which receive it (every other pin on those nets) -- each pair partner's pin following its own;
//  - the impedance and eye test a standard calls for (USB 2.0 is 90 ohm differential: 45 ohm per
//    line), and a frequency sweep that gives the trace a sharp enough edge and a short enough pulse.
//
// suggestSignal() makes that plan, applySignalPlan() turns it into the simulation's settings
// through SimulationEditor (so they're exactly what the pin-by-pin editing would have made), and
// SignalWizard is the dialog that shows the plan and lets each choice be changed.
#pragma once

#include "BoardModel.hpp"
#include "Document.hpp"

#include <QDialog>

#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QRadioButton;

namespace kiems_gui {

class QuantityEdit;

/// A pin on the board, as the wizard lists it.
struct SignalPin {
    QString footprint, pin, net;
    QString value;   // the footprint's value, e.g. "USBLC6-2SC6"
    QString pinType; // KiCad's electrical type, e.g. "bidirectional"
    /// "U7 pin 7 · USBLC6-2SC6 · bidirectional"
    QString label() const;
    bool operator==(const SignalPin& other) const { return footprint == other.footprint && pin == other.pin; }
};

struct SignalPlan {
    QString name;
    bool differential = false;
    /// The signal's nets, the chosen one first, then those beyond series parts.
    QStringList nets;
    /// The pair's other half: its nets, likewise followed through series parts (the first the chosen
    /// net's partner by name); only in a differential pair.
    QStringList partners;
    /// Nets that look like the same signal (/_D+ beside /D+) but lie past a chip, which a simulation
    /// can't pass the signal through, and those chips; left out, and said so.
    QStringList beyondChips, throughChips;
    /// The parts joining those nets -- in series along them, or across a pair -- modelled as lumped
    /// components.
    QStringList seriesParts;
    /// Every pin on the nets but passive parts' (which don't receive the signal), the driver among them.
    QList<SignalPin> pins;
    std::optional<SignalPin> driver;
    /// The pins that receive the signal: probed, and terminated at the impedance.
    QList<SignalPin> receivers;
    double impedance = 45; // per line, ohms
    /// An EyeTestPreset's standard; empty for no eye test.
    QString standard;
    /// The eye's bit rate when no standard sets it; 0 for none.
    double bitRate = 0;
    /// Set the document's sweep to end here (Hz) when it should; nullopt to leave it.
    std::optional<double> sweepStop;
};

/// The signal nets a user would pick from: everything but power, ground and unconnected nets.
QStringList signalNets(const BoardData& board);
/// Works out a plan for simulating `net` on `board` -- as half of a pair if `differential` (and
/// the pair's other net is on the board).
SignalPlan suggestSignal(const BoardData& board, const Document& document, const QString& net, bool differential);
/// The sweep a signal simulation of `plan` wants, if the document's isn't suitable: tops under
/// 1 GHz leave a trace's edges too slow to see and its excitation pulse long (16 ns at 350 MHz).
std::optional<double> recommendedSweepStop(const Document& document);
/// `pin`'s partner in the pair: the same part's pin on the other half's net at the same place along
/// the signal -- the nearest, if it has several there.
std::optional<SignalPin> signalPartnerPin(const BoardData& board, const SignalPlan& plan, const SignalPin& pin);
/// Fills `simulation` (just added) from `plan`.
void applySignalPlan(Document& document, const BoardData* board, int simulation, const SignalPlan& plan);
/// One line saying what `plan` will set up.
QString describeSignalPlan(const SignalPlan& plan);

class SignalWizard : public QDialog {
    Q_OBJECT

public:
    SignalWizard(const Document& document, std::shared_ptr<const BoardData> board, QWidget* parent = nullptr);

    /// The plan as the dialog's choices leave it.
    SignalPlan plan() const { return _plan; }

private:
    void netChosen();
    void choicesChanged();

    const Document& _document;
    std::shared_ptr<const BoardData> _board;
    SignalPlan _plan;
    bool _updating = false;

    QRadioButton* _pair;
    QRadioButton* _single;
    QComboBox* _net;
    QLabel* _found;
    QCheckBox* _followSeries;
    QComboBox* _driver;
    QListWidget* _receivers;
    QComboBox* _standard;
    QuantityEdit* _impedance;
    QuantityEdit* _bitRate;
    QCheckBox* _sweep;
    QLabel* _summary;
};

} // namespace kiems_gui
