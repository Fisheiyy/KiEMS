#include "SignalSetup.hpp"

#include "EyeTestPresets.hpp"
#include "QuantityEdit.hpp"
#include "SimulationEditor.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace kiems_gui {

namespace {

constexpr double kSweepStop = 3e9;

QString qstr(const std::string& s) { return QString::fromStdString(s); }

bool isGroundOrPower(const QString& net) { return SimulationEditor::isPowerNet(net); }

/// A part that sits in series on a signal: a resistor, inductor, capacitor or ferrite bead with
/// two pins, each on a different net.
bool isSeriesPart(const libkicad::FootprintInfo& footprint) {
    const QString reference = qstr(footprint.reference).toUpper();
    const bool kind = SimulationEditor::isPassive(reference) || reference.startsWith("FB");
    return kind && footprint.pins.size() == 2 && footprint.pins[0].netName != footprint.pins[1].netName &&
           !footprint.pins[0].netName.empty() && !footprint.pins[1].netName.empty();
}

QString partnerOn(const BoardData& board, const QString& net) {
    for (const QString& candidate : SimulationEditor::differentialPartnerCandidates(net)) {
        if (board.nets.contains(candidate)) return candidate;
    }
    return {};
}

/// The pin that most likely drives the signal: an IC's output, then any IC pin, then any pin.
std::optional<SignalPin> guessDriver(const QList<SignalPin>& pins) {
    const auto ic = [](const SignalPin& p) { return p.footprint.startsWith('U', Qt::CaseInsensitive); };
    const auto drives = [](const SignalPin& p) {
        return p.pinType == "output" || p.pinType == "bidirectional" || p.pinType == "tri_state";
    };
    for (const auto& test : {std::function<bool(const SignalPin&)>([&](const SignalPin& p) { return ic(p) && drives(p); }),
                             std::function<bool(const SignalPin&)>(drives), std::function<bool(const SignalPin&)>(ic)}) {
        const auto it = std::find_if(pins.begin(), pins.end(), test);
        if (it != pins.end()) return *it;
    }
    if (!pins.isEmpty()) return pins.front();
    return std::nullopt;
}

/// The nets a signal runs through from `start`, on through series parts (each one found after
/// the net it was reached from), and those parts. `avoid` are nets not to step into: the pair's
/// other half, across a termination.
void follow(const BoardData& board, const QString& start, const QStringList& avoid, QStringList& nets, QStringList& parts) {
    nets << start;
    for (int n = static_cast<int>(nets.size()) - 1; n < nets.size(); ++n) {
        for (const libkicad::FootprintInfo& footprint : board.footprints) {
            if (!isSeriesPart(footprint)) continue;
            const QString a = qstr(footprint.pins[0].netName), b = qstr(footprint.pins[1].netName);
            const QString other = a == nets[n] ? b : b == nets[n] ? a : QString();
            if (other.isEmpty() || isGroundOrPower(other) || nets.contains(other) || avoid.contains(other)) continue;
            if (SimulationEditor::differentialPartnerCandidates(nets[n]).contains(other)) continue;
            nets << other;
            parts << qstr(footprint.reference);
        }
    }
}

/// A net's name without its sheet path and decoration: "/USB/_D+" -> "D+".
QString plainName(const QString& net) {
    QString leaf = net.section('/', -1).toUpper();
    while (!leaf.isEmpty() && (leaf.front() == '_' || leaf.front() == '~')) leaf.remove(0, 1);
    return leaf;
}

/// The standard a pair's net names suggest: USB's D+/D-.
QString guessStandard(const QString& net, double bitRate) {
    const QString leaf = net.section('/', -1).toUpper();
    if (leaf.contains("USB") || leaf.contains("D+") || leaf.contains("D-") || leaf == "DP" || leaf == "DM") {
        return bitRate >= 100e6 ? "usb_high_speed" : "usb_full_speed";
    }
    return {};
}

} // namespace

QString SignalPin::label() const {
    QStringList parts{QObject::tr("%1 pin %2").arg(footprint, pin)};
    if (!value.isEmpty()) parts << value;
    if (!pinType.isEmpty()) parts << pinType;
    return parts.join(QString::fromUtf8(" · "));
}

QStringList signalNets(const BoardData& board) {
    QStringList nets;
    for (const QString& net : board.nets) {
        if (net.isEmpty() || net.startsWith("unconnected-") || isGroundOrPower(net)) continue;
        nets << net;
    }
    return nets;
}

std::optional<double> recommendedSweepStop(const Document& document) {
    const nlohmann::json stop = document.documentValue("/frequency/stop");
    if (stop.is_number() && stop.get<double>() >= 1e9) return std::nullopt;
    return kSweepStop;
}

SignalPlan suggestSignal(const BoardData& board, const Document& document, const QString& net, bool differential) {
    SignalPlan plan;
    const QString partner = differential ? partnerOn(board, net) : QString();
    plan.differential = !partner.isEmpty();
    // The nets the signal runs through: the chosen one, then on through series parts -- and the
    // pair's other half, followed the same way on its own, as its nets beyond a resistor needn't be
    // named as a pair (a chip's Net-(U1-IO26) and Net-(U1-IO27)).
    follow(board, net, plan.differential ? QStringList{partner} : QStringList(), plan.nets, plan.seriesParts);
    if (plan.differential) {
        QStringList partnerParts;
        follow(board, partner, plan.nets, plan.partners, partnerParts);
        plan.seriesParts += partnerParts;
    }
    // Every other part joining two of the signal's nets: the partner side's series parts, and a
    // termination across the pair. They're modelled too, never receivers.
    QStringList all = plan.nets;
    for (const QString& partner : plan.partners) {
        if (!partner.isEmpty()) all << partner;
    }
    for (const libkicad::FootprintInfo& footprint : board.footprints) {
        if (!isSeriesPart(footprint) || plan.seriesParts.contains(qstr(footprint.reference))) continue;
        if (all.contains(qstr(footprint.pins[0].netName)) && all.contains(qstr(footprint.pins[1].netName))) {
            plan.seriesParts << qstr(footprint.reference);
        }
    }
    // The pins on the signal's nets (the pair's partner pins follow their own).
    std::map<std::string, const libkicad::FootprintInfo*> footprints;
    for (const auto& footprint : board.footprints) footprints[footprint.reference] = &footprint;
    for (const libkicad::PadPosition& pad : board.pads) {
        const QString padNet = qstr(pad.netName);
        // Passive parts are modelled or left as they are, but they don't receive the signal.
        const QString reference = qstr(pad.footprintRef).toUpper();
        if (!plan.nets.contains(padNet) || SimulationEditor::isPassive(reference) || reference.startsWith("FB")) continue;
        SignalPin pin{qstr(pad.footprintRef), qstr(pad.padNumber), padNet, {}, qstr(pad.pinType)};
        if (const auto it = footprints.find(pad.footprintRef); it != footprints.end()) pin.value = qstr(it->second->value);
        if (!plan.pins.contains(pin)) plan.pins << pin;
    }
    // Nets that look like the same signal (/_D+ beside /D+) but are only reached through a chip --
    // an ESD array, a switch -- which a simulation can't pass a signal through.
    QStringList ours = plan.nets + plan.partners;
    QStringList plainNames;
    for (const QString& n : ours) plainNames << plainName(n);
    for (const QString& other : board.nets) {
        if (ours.contains(other) || !plainNames.contains(plainName(other)) || isGroundOrPower(other)) continue;
        plan.beyondChips << other;
        for (const libkicad::FootprintInfo& footprint : board.footprints) {
            bool touchesOther = false, touchesOurs = false;
            for (const auto& pin : footprint.pins) {
                touchesOther = touchesOther || qstr(pin.netName) == other;
                touchesOurs = touchesOurs || ours.contains(qstr(pin.netName));
            }
            if (touchesOther && touchesOurs && !plan.throughChips.contains(qstr(footprint.reference))) {
                plan.throughChips << qstr(footprint.reference);
            }
        }
    }
    std::sort(plan.pins.begin(), plan.pins.end(), [](const SignalPin& x, const SignalPin& y) {
        return x.footprint == y.footprint ? x.pin < y.pin : x.footprint < y.footprint;
    });
    plan.driver = guessDriver(plan.pins);
    for (const SignalPin& pin : plan.pins) {
        if (!plan.driver || !(pin == *plan.driver)) plan.receivers << pin;
    }
    // The standard and impedance: the names' suggestion, Board Setup's bit rate and impedance.
    const kiems::BoardSetupConfig setup = SimulationEditor::boardSetup(document);
    const double boardRate = setup.bitRate();
    plan.standard = plan.differential ? guessStandard(net, boardRate) : QString();
    plan.impedance = plan.standard.startsWith("usb") ? 45 : setup.signalImpedance();
    plan.bitRate = boardRate;
    plan.sweepStop = recommendedSweepStop(document);
    plan.name = plan.differential ? QString("%1 / %2").arg(net, plan.partners.front()) : net;
    return plan;
}

std::optional<SignalPin> signalPartnerPin(const BoardData& board, const SignalPlan& plan, const SignalPin& pin) {
    // The partner side's net in the same place along the signal, else any of the partner side's.
    const qsizetype at = plan.nets.indexOf(pin.net);
    const QStringList targets = at >= 0 && at < plan.partners.size() ? QStringList{plan.partners[at]} : plan.partners;
    const libkicad::PadPosition* source = nullptr;
    for (const libkicad::PadPosition& pad : board.pads) {
        if (qstr(pad.footprintRef) == pin.footprint && qstr(pad.padNumber) == pin.pin) source = &pad;
    }
    std::optional<SignalPin> best;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const libkicad::PadPosition& pad : board.pads) {
        if (qstr(pad.footprintRef) != pin.footprint || !targets.contains(qstr(pad.netName))) continue;
        // The nearest of the part's pins on it: the one beside this one.
        const double distance = source ? std::hypot(pad.xMm - source->xMm, pad.yMm - source->yMm) : 0;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = SignalPin{pin.footprint, qstr(pad.padNumber), qstr(pad.netName), pin.value, qstr(pad.pinType)};
        }
    }
    return best;
}

void applySignalPlan(Document& document, const BoardData* board, int simulation, const SignalPlan& plan) {
    const Document::ChangeBatch batch(document);
    SimulationEditor editor(document, board, simulation);
    editor.applyBoardSetup();
    if (plan.differential) document.setSimulationValue(simulation, "/is_differential_pair", true);
    QStringList nets = plan.nets;
    for (const QString& partner : plan.partners) {
        if (!partner.isEmpty() && !nets.contains(partner)) nets << partner;
    }
    for (const QString& net : nets) {
        editor.setNetSimulated(net, true);
        editor.setNetImpedance(net, plan.impedance);
    }
    for (const QString& part : plan.seriesParts) editor.setComponentIncluded(part, true);
    // The driver and the receivers. A pair's partner pins follow by themselves where the nets are
    // named as a pair (SimulationEditor mirrors them); elsewhere they're set here, the driver's
    // partner in antiphase.
    const auto mirrorsItself = [&](const SignalPin& pin) { return !board || !partnerOn(*board, pin.net).isEmpty(); };
    if (plan.driver) {
        editor.setPinExcited(plan.driver->footprint, plan.driver->pin, true);
        if (plan.differential && !mirrorsItself(*plan.driver)) {
            if (const auto partner = signalPartnerPin(*board, plan, *plan.driver)) {
                editor.setPinExcited(partner->footprint, partner->pin, true);
                editor.updateExcitation(partner->footprint, partner->pin, [&](kiems::ExcitationConfig& excitation) {
                    excitation.setAmplitude(-1.0);
                });
            }
        }
    }
    for (const SignalPin& pin : plan.receivers) {
        editor.setPinProbed(pin.footprint, pin.pin, true);
        if (plan.differential && !mirrorsItself(pin)) {
            if (const auto partner = signalPartnerPin(*board, plan, pin)) editor.setPinProbed(partner->footprint, partner->pin, true);
        }
    }
    // The eye: the standard's mask and bit rate, as the Tests tab sets them.
    if (const EyeTestPreset* preset = EyeTestPreset::find(plan.standard)) {
        document.setSimulationValue(simulation, "/eye_test/standard", plan.standard.toStdString());
        if (preset->bitRate) document.setSimulationValue(simulation, "/eye_bit_rate", *preset->bitRate);
        document.setSimulationValue(simulation, "/eye_test/signal_level", preset->signalLevel);
        document.setSimulationValue(simulation, "/eye_test/mask_x1", preset->x1);
        document.setSimulationValue(simulation, "/eye_test/mask_x2", preset->x2);
        document.setSimulationValue(simulation, "/eye_test/mask_y1", preset->y1);
        document.setSimulationValue(simulation, "/eye_test/mask_y2", preset->y2);
    } else if (plan.bitRate > 0) {
        document.setSimulationValue(simulation, "/eye_bit_rate", plan.bitRate);
    }
    if (plan.sweepStop) document.setDocumentValue("/frequency/stop", *plan.sweepStop);
}

QString describeSignalPlan(const SignalPlan& plan) {
    if (plan.nets.isEmpty()) return QObject::tr("Pick the signal's net.");
    QStringList lines;
    const QString bullet = QString::fromUtf8("• ");
    QStringList nets = plan.nets;
    for (const QString& partner : plan.partners) {
        if (!partner.isEmpty()) nets << partner;
    }
    lines << bullet + QObject::tr("Simulates %1 at %2 Ω per line%3.")
                          .arg(nets.join(", "))
                          .arg(plan.impedance, 0, 'g', 4)
                          .arg(plan.differential ? QObject::tr(" (%1 Ω differential)").arg(2 * plan.impedance, 0, 'g', 4)
                                                 : QString());
    if (!plan.seriesParts.isEmpty()) {
        lines << bullet + QObject::tr("Models %1, the parts along and across the signal, so the whole path is included.")
                              .arg(plan.seriesParts.join(", "));
    }
    if (plan.driver) {
        lines << bullet + (plan.differential ? QObject::tr("Drives the pair from %1 and its partner pin, in opposite phase.")
                                             : QObject::tr("Drives the signal from %1."))
                              .arg(QObject::tr("%1 pin %2").arg(plan.driver->footprint, plan.driver->pin));
    } else {
        lines << bullet + QObject::tr("Nothing drives it yet: choose a driving pin.");
    }
    if (!plan.receivers.isEmpty()) {
        QStringList names;
        for (const SignalPin& pin : plan.receivers) names << QObject::tr("%1 pin %2").arg(pin.footprint, pin.pin);
        lines << bullet + QObject::tr("Measures what arrives at %1%2, each terminated in %3 Ω.")
                              .arg(names.join(", "))
                              .arg(plan.differential ? QObject::tr(" (and their partner pins)") : QString())
                              .arg(plan.impedance, 0, 'g', 4);
    }
    if (!plan.beyondChips.isEmpty()) {
        lines << bullet + QObject::tr("Stops at %1: %2 look like the same signal past it, but a simulation can't pass a "
                                      "signal through a chip. To see that side, simulate it on its own.")
                              .arg(plan.throughChips.join(", "), plan.beyondChips.join(", "));
    }
    if (const EyeTestPreset* preset = EyeTestPreset::find(plan.standard)) {
        lines << bullet + QObject::tr("Tests the eye against the %1 mask.").arg(preset->title);
    }
    if (plan.sweepStop) {
        lines << bullet + QObject::tr("Sets the frequency sweep to end at %1 GHz (for every simulation in the document).")
                              .arg(*plan.sweepStop / 1e9, 0, 'g', 3);
    }
    return lines.join("\n");
}

// ---------------------------------------------------------------------------------------------
// The dialog

SignalWizard::SignalWizard(const Document& document, std::shared_ptr<const BoardData> board, QWidget* parent)
    : QDialog(parent), _document(document), _board(std::move(board)) {
    setWindowTitle(tr("New Signal Simulation"));
    setMinimumWidth(620);
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Choose a signal and KiEMS works out the rest from the board: the pair's other half, "
                                "the parts in series with it, which pin drives it and which receive it. Change "
                                "anything that isn't right."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    // 1. What
    auto* what = new QGroupBox(tr("1. The signal"));
    auto* whatForm = new QFormLayout(what);
    _pair = new QRadioButton(tr("A differential pair (USB, Ethernet, HDMI, LVDS...)"));
    _single = new QRadioButton(tr("A single-ended trace"));
    _pair->setChecked(true);
    auto* kind = new QVBoxLayout;
    kind->addWidget(_pair);
    kind->addWidget(_single);
    whatForm->addRow(tr("Kind:"), kind);
    _net = new QComboBox;
    _net->setEditable(true);
    _net->setInsertPolicy(QComboBox::NoInsert);
    _net->setToolTip(tr("One of the signal's nets -- for a pair, either half."));
    whatForm->addRow(tr("Net:"), _net);
    _found = new QLabel;
    _found->setWordWrap(true);
    _found->setEnabled(false);
    whatForm->addRow(QString(), _found);
    _followSeries = new QCheckBox(tr("Follow it through series parts"));
    _followSeries->setChecked(true);
    _followSeries->setToolTip(tr("Include the nets beyond series resistors, capacitors and ferrites, with those parts, "
                                 "so the simulation covers the whole path from the driver to the connector."));
    whatForm->addRow(QString(), _followSeries);
    layout->addWidget(what);

    // 2. Pins
    auto* pins = new QGroupBox(tr("2. Who sends it, and who receives it"));
    auto* pinsForm = new QFormLayout(pins);
    _driver = new QComboBox;
    _driver->setToolTip(tr("The pin that drives the signal -- usually the chip's output. In a pair, its partner pin is "
                           "driven in opposite phase."));
    pinsForm->addRow(tr("Driven by:"), _driver);
    _receivers = new QListWidget;
    _receivers->setMaximumHeight(130);
    _receivers->setToolTip(tr("Where the signal is measured: each ticked pin is probed and terminated at the line's "
                              "impedance, as a receiver would. In a pair, the partner pins are included too."));
    pinsForm->addRow(tr("Received at:"), _receivers);
    layout->addWidget(pins);

    // 3. Standard
    auto* standard = new QGroupBox(tr("3. What it has to meet"));
    auto* standardForm = new QFormLayout(standard);
    _standard = new QComboBox;
    _standard->addItem(tr("No eye test"), QString());
    for (const EyeTestPreset& preset : EyeTestPreset::all()) {
        _standard->addItem(QString("%1 %2").arg(preset.family, preset.menuTitle), QString::fromLatin1(preset.standard));
    }
    _standard->setToolTip(tr("The standard's eye mask the received signal is tested against, and its bit rate."));
    standardForm->addRow(tr("Standard:"), _standard);
    _impedance = new QuantityEdit(QuantityEdit::Kind::Resistance);
    _impedance->setMinimum(1);
    _impedance->setToolTip(tr("Each line's impedance, which the driver and receivers are terminated in. A pair's "
                              "differential impedance is twice this: USB 2.0's 90 Ω is 45 per line."));
    standardForm->addRow(tr("Impedance per line:"), _impedance);
    _bitRate = new QuantityEdit(QuantityEdit::Kind::BitRate);
    _bitRate->setAllowEmpty(true, tr("Automatic"));
    _bitRate->setToolTip(tr("The bit rate the eye is drawn at, when no standard sets it."));
    standardForm->addRow(tr("Bit rate:"), _bitRate);
    _sweep = new QCheckBox;
    standardForm->addRow(QString(), _sweep);
    layout->addWidget(standard);

    auto* summaryBox = new QGroupBox(tr("This will"));
    auto* summaryLayout = new QVBoxLayout(summaryBox);
    _summary = new QLabel;
    _summary->setWordWrap(true);
    summaryLayout->addWidget(_summary);
    layout->addWidget(summaryBox);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create Simulation"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // The nets to choose from: a pair's halves first when it's a pair.
    if (_board) {
        QStringList nets = signalNets(*_board), paired, single;
        for (const QString& net : nets) {
            (SimulationEditor::differentialPartnerCandidates(net).isEmpty() ? single : paired) << net;
        }
        _net->addItems(paired + single);
    }
    connect(_net, &QComboBox::currentTextChanged, this, &SignalWizard::netChosen);
    connect(_pair, &QRadioButton::toggled, this, &SignalWizard::netChosen);
    connect(_followSeries, &QCheckBox::toggled, this, &SignalWizard::netChosen);
    connect(_driver, &QComboBox::currentIndexChanged, this, &SignalWizard::choicesChanged);
    connect(_receivers, &QListWidget::itemChanged, this, &SignalWizard::choicesChanged);
    connect(_standard, &QComboBox::currentIndexChanged, this, [this] {
        // A USB standard means 90 ohm differential.
        if (!_updating && _standard->currentData().toString().startsWith("usb")) _impedance->setValue(45);
        choicesChanged();
    });
    connect(_impedance, &QuantityEdit::valueEdited, this, &SignalWizard::choicesChanged);
    connect(_bitRate, &QuantityEdit::valueEdited, this, &SignalWizard::choicesChanged);
    connect(_sweep, &QCheckBox::toggled, this, &SignalWizard::choicesChanged);
    netChosen();
}

void SignalWizard::netChosen() {
    if (!_board) return;
    _updating = true;
    _plan = suggestSignal(*_board, _document, _net->currentText().trimmed(), _pair->isChecked());
    if (!_followSeries->isChecked() && _plan.nets.size() > 1) {
        // Only the chosen net (and its partner) without what lies past series parts.
        const SignalPlan full = _plan;
        _plan.nets = {full.nets.front()};
        _plan.partners = full.partners.isEmpty() ? QStringList() : QStringList{full.partners.front()};
        _plan.beyondChips.clear();
        _plan.throughChips.clear();
        _plan.seriesParts.clear();
        _plan.pins.clear();
        for (const SignalPin& pin : full.pins) {
            if (pin.net == _plan.nets.front()) _plan.pins << pin;
        }
        if (_plan.driver && _plan.driver->net != _plan.nets.front()) _plan.driver.reset();
        if (!_plan.driver && !_plan.pins.isEmpty()) _plan.driver = _plan.pins.front();
    }
    QStringList found;
    if (_pair->isChecked()) {
        found << (_plan.differential ? tr("Paired with %1.").arg(_plan.partners.front())
                                     : tr("No partner net found by name (D+ / D-, _P / _N): it'll be simulated alone."));
    }
    if (!_plan.beyondChips.isEmpty()) {
        found << tr("%1 continues past %2 as %3, which can't be simulated through a chip.")
                     .arg(_plan.nets.front(), _plan.throughChips.join(", "), _plan.beyondChips.join(", "));
    }
    if (!_plan.seriesParts.isEmpty()) {
        // The parts on the chosen net's own path: one found for each net it reached.
        const QStringList path = _plan.seriesParts.mid(0, _plan.nets.size() - 1);
        if (!path.isEmpty()) found << tr("Through %1 to %2.").arg(path.join(", "), _plan.nets.mid(1).join(", "));
    }
    _found->setText(found.join(' '));
    _driver->clear();
    for (const SignalPin& pin : _plan.pins) _driver->addItem(pin.label());
    if (_plan.driver) _driver->setCurrentIndex(static_cast<int>(_plan.pins.indexOf(*_plan.driver)));
    _receivers->clear();
    for (const SignalPin& pin : _plan.pins) {
        auto* item = new QListWidgetItem(pin.label(), _receivers);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(_plan.driver && pin == *_plan.driver ? Qt::Unchecked : Qt::Checked);
    }
    const int standard = _standard->findData(_plan.standard);
    _standard->setCurrentIndex(std::max(0, standard));
    _impedance->setValue(_plan.impedance);
    _bitRate->setValue(_plan.bitRate > 0 ? std::optional<double>(_plan.bitRate) : std::nullopt);
    const nlohmann::json start = _document.documentValue("/frequency/start"), stop = _document.documentValue("/frequency/stop");
    _sweep->setVisible(_plan.sweepStop.has_value());
    _sweep->setChecked(_plan.sweepStop.has_value());
    if (_plan.sweepStop) {
        _sweep->setText(tr("Sweep to %1 GHz instead of %2 MHz (for every simulation): sharp enough edges to see along "
                           "the trace, and an excitation short enough for the run")
                            .arg(*_plan.sweepStop / 1e9, 0, 'g', 3)
                            .arg(stop.is_number() ? stop.get<double>() / 1e6 : 0, 0, 'g', 4));
    }
    _updating = false;
    choicesChanged();
}

void SignalWizard::choicesChanged() {
    if (_updating) return;
    const int driver = _driver->currentIndex();
    _plan.driver = driver >= 0 && driver < _plan.pins.size() ? std::optional<SignalPin>(_plan.pins[driver]) : std::nullopt;
    _plan.receivers.clear();
    for (int i = 0; i < _receivers->count() && i < _plan.pins.size(); ++i) {
        // The driver can't also be a receiver.
        const bool isDriver = _plan.driver && _plan.pins[i] == *_plan.driver;
        if (_receivers->item(i)->checkState() == Qt::Checked && !isDriver) _plan.receivers << _plan.pins[i];
    }
    _plan.standard = _standard->currentData().toString();
    _plan.impedance = _impedance->value().value_or(45);
    _plan.bitRate = _bitRate->value().value_or(0);
    _plan.sweepStop = !_sweep->isHidden() && _sweep->isChecked() ? std::optional<double>(kSweepStop) : std::nullopt;
    _summary->setText(describeSignalPlan(_plan));
}

} // namespace kiems_gui
