// Tests for the GUI's editing logic -- the unit-aware fields, the document's settings accessors and
// the Tests tab's presets -- driven through the widgets' own signals, without showing a window.
// Every document edited here is also run through libkiems' own parser (Document::validate()).
#include "BoardSetupPanel.hpp"
#include "Document.hpp"
#include "EyeTestPresets.hpp"
#include "EyeView.hpp"
#include "GeometryScene.hpp"
#include "GeometryView.hpp"
#include "JobQueue.hpp"
#include "PowerAnalysis.hpp"
#include "PowerView.hpp"
#include "RunEstimate.hpp"
#include "SignalSetup.hpp"
#include "TraceView.hpp"
#include "QuantityEdit.hpp"
#include "Report.hpp"
#include "ResultsView.hpp"
#include "SettingsPanel.hpp"
#include "SimulationEditor.hpp"
#include "SimulationSet.hpp"
#include "SimulationSetPanel.hpp"
#include "TestsPanel.hpp"

#include "kiems/config.hpp"
#include "kiems/excitation_postprocess.hpp"
#include "kiems/path_utf8.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <complex>
#include <map>
#include <numbers>
#include <set>

using kiems_gui::Document;
using kiems_gui::EyeTestPreset;
using kiems_gui::PinRef;
using kiems_gui::NetLevel;
using kiems_gui::QuantityEdit;
using kiems_gui::SimulationEditor;

namespace {

/// Types `text` into `edit` and finishes editing, as Return or leaving the field does.
void type(QuantityEdit* edit, const QString& text) {
    edit->setText(text);
    emit edit->editingFinished();
}

double number(const nlohmann::json& value) {
    return value.get<double>();
}

/// A small board: U1 drives SIG into U2 (an input) past R1; U1 also drives the pair USB_P/USB_N
/// into U3; GND is the ground. U1 pin 3 is noise-generating in Board Setup.
std::shared_ptr<kiems_gui::BoardData> fakeBoard() {
    auto board = std::make_shared<kiems_gui::BoardData>();
    auto footprint = [&](const char* reference, const char* value,
                         std::vector<libkicad::FootprintPin> pins) {
        board->footprints.push_back({reference, value, std::move(pins)});
    };
    footprint("U1", "MCU", {{"1", "OUT", "SIG", "output"}, {"2", "GND", "GND", "power_in"},
                            {"3", "DP", "USB_P", "bidirectional"}, {"4", "DN", "USB_N", "bidirectional"}});
    footprint("U2", "BUF", {{"1", "IN", "SIG", "input"}, {"2", "GND", "GND", "power_in"}, {"3", "NC", "NC1", "input"}});
    footprint("U3", "PHY", {{"1", "DP", "USB_P", "input"}, {"2", "DN", "USB_N", "input"}});
    footprint("R1", "10k", {{"1", "", "SIG", ""}, {"2", "", "GND", ""}});
    board->nets = {"SIG", "GND", "USB_P", "USB_N", "NC1"};
    board->copperLayers = {"F.Cu", "In1.Cu", "In2.Cu", "B.Cu"};
    board->netClasses = {{"USB_P", "USB"}, {"USB_N", "USB"}};
    return board;
}

/// The QuantityEdits inside `panel`, in the order they were created.
QList<QuantityEdit*> fields(QWidget* panel) {
    return panel->findChildren<QuantityEdit*>();
}

} // namespace

class KiemsGuiTests : public QObject {
    Q_OBJECT

private:
    QTemporaryDir _directory;

    /// A new document with one runnable simulation.
    bool makeDocument(Document& document) {
        QString error;
        const QString package = _directory.filePath(QString("doc%1.g2esim").arg(++_documents));
        if (!document.create(package, _directory.filePath("board.kicad_pcb"), &error)) {
            qWarning() << error;
            return false;
        }
        document.addSimulation("Trace", "GND");
        document.setNetIncluded(0, "SIG", true, 1);
        document.setMainExcitation(0, PinRef{"U1", "5"});
        return true;
    }
    int _documents = 0;

private slots:
    void lengthUnits() {
        QuantityEdit edit(QuantityEdit::Kind::Length);
        QSignalSpy edited(&edit, &QuantityEdit::valueEdited);
        type(&edit, "2.5");
        QCOMPARE(*edit.value(), 2500.0); // millimetres by default, stored in micrometres
        QCOMPARE(edit.text(), QString("2.5 mm"));
        type(&edit, "100 mil");
        QCOMPARE(*edit.value(), 2540.0);
        QCOMPARE(edit.text(), QString("100 mil"));
        type(&edit, "3"); // a bare number is in the unit last typed
        QVERIFY(std::fabs(*edit.value() - 76.2) < 1e-9);
        type(&edit, "50um");
        QCOMPARE(*edit.value(), 50.0);
        type(&edit, "0.1 in");
        QCOMPARE(*edit.value(), 2540.0);
        type(&edit, "12 furlongs");
        QCOMPARE(*edit.value(), 2540.0); // rejected, and the text reverts
        QCOMPARE(edit.text(), QString("0.1 in"));
        QCOMPARE(edited.count(), 5); // one per changed value; the rejected entry sends none
    }

    void siUnits() {
        QuantityEdit frequency(QuantityEdit::Kind::Frequency);
        type(&frequency, "2.4 GHz");
        QCOMPARE(*frequency.value(), 2.4e9);
        QCOMPARE(frequency.text(), QString("2.4 GHz"));
        type(&frequency, "500M");
        QCOMPARE(*frequency.value(), 5e8);
        type(&frequency, "10 kHz");
        QCOMPARE(*frequency.value(), 1e4);
        type(&frequency, "fast");
        QCOMPARE(*frequency.value(), 1e4);

        QuantityEdit time(QuantityEdit::Kind::Time);
        type(&time, "500 ps");
        QVERIFY(std::fabs(*time.value() - 5e-10) < 1e-22);
        QCOMPARE(time.text(), QString("500 ps"));
        type(&time, "2ns");
        QVERIFY(std::fabs(*time.value() - 2e-9) < 1e-21);
        type(&time, "1.5 seconds");
        QCOMPARE(*time.value(), 1.5);

        QuantityEdit bitRate(QuantityEdit::Kind::BitRate);
        type(&bitRate, "5 Gb/s");
        QCOMPARE(*bitRate.value(), 5e9);
        type(&bitRate, "480 Mbps");
        QCOMPARE(*bitRate.value(), 480e6);
        QCOMPARE(bitRate.text(), QString("480 Mb/s"));

        QuantityEdit voltage(QuantityEdit::Kind::Voltage);
        type(&voltage, "150 mV");
        QVERIFY(std::fabs(*voltage.value() - 0.15) < 1e-12);
    }

    void limitsAndEmptiness() {
        QuantityEdit count(QuantityEdit::Kind::Integer);
        count.setMinimum(1);
        type(&count, "64");
        QCOMPARE(*count.value(), 64.0);
        type(&count, "6.5");
        QCOMPARE(*count.value(), 64.0);
        type(&count, "0");
        QCOMPARE(*count.value(), 64.0);

        QuantityEdit optional(QuantityEdit::Kind::Voltage);
        optional.setAllowEmpty(true, "None");
        optional.setValue(0.3);
        QSignalSpy edited(&optional, &QuantityEdit::valueEdited);
        type(&optional, "");
        QCOMPARE(edited.count(), 1);
        QVERIFY(!edited.at(0).at(0).value<std::optional<double>>().has_value());
        QVERIFY(!optional.value().has_value());
    }

    void formatting() {
        QCOMPARE(QuantityEdit::format(QuantityEdit::Kind::Frequency, 6e9), QString("6 GHz"));
        QCOMPARE(QuantityEdit::format(QuantityEdit::Kind::Frequency, 1e6), QString("1 MHz"));
        QCOMPARE(QuantityEdit::format(QuantityEdit::Kind::Length, 1500), QString("1.5 mm"));
        QCOMPARE(QuantityEdit::format(QuantityEdit::Kind::Time, 0), QString("0 s"));
        QCOMPARE(QuantityEdit::format(QuantityEdit::Kind::Integer, 20000), QString("20000"));
    }

    void documentSettings() {
        Document document;
        QVERIFY(makeDocument(document));
        // Defaults come from libkiems.
        QCOMPARE(number(document.documentValue("/grid/optimal")), 50.0);
        QCOMPARE(number(document.documentValue("/frequency/stop")), 6e9);
        QCOMPARE(number(document.simulationValue(0, "/via_spacing")), 1500.0);
        // A new simulation's eye is built at the top of the frequency range.
        QCOMPARE(number(document.simulationValue(0, "/eye_bit_rate")), 6e9);

        document.setDocumentValue("/grid/optimal", 25.0);
        document.setDocumentValue("/grid/margin/xy", 3000.0);
        QCOMPARE(number(document.documentValue("/grid/optimal")), 25.0);
        QCOMPARE(number(document.documentValue("/grid/margin/xy")), 3000.0);
        QCOMPARE(number(document.documentValue("/grid/margin/z")), 2000.0); // sibling untouched

        // Settings libkiems omits at their default are removed again when set back to it.
        document.setSimulationValue(0, "/eye_draw_count", 32);
        QCOMPARE(number(document.simulationValue(0, "/eye_draw_count")), 32.0);
        document.setSimulationValue(0, "/eye_draw_count", 64);
        QSignalSpy changed(&document, &Document::changed);
        document.setSimulationValue(0, "/eye_draw_count", 64); // no change, no signal
        QCOMPARE(changed.count(), 0);

        document.setSimulationValue(0, "/eye_test/mask_x1", 0.3);
        QCOMPARE(QString::fromStdString(document.simulationValue(0, "/eye_test/standard").get<std::string>()),
                 QString("custom"));
        document.setSimulationValue(0, "/eye_test/mask_x1", 0.0);

        document.setSimulationValue(0, "/edge_terminated_nets", nlohmann::json::array({"CLK"}));
        document.setSimulationValue(0, "/is_differential_pair", true);
        document.setSimulationValue(0, "/terminate_ground_at_cut", true);

        // The main excitation's duration, kept when another pin is picked.
        document.setMainExcitationDuration(0, 2e-9);
        QCOMPARE(*document.mainExcitationDuration(0), 2e-9);
        document.setMainExcitation(0, PinRef{"U2", "3"});
        QCOMPARE(*document.mainExcitationDuration(0), 2e-9);
        QCOMPARE(document.mainExcitation(0)->footprint, QString("U2"));

        QString error;
        QVERIFY(document.save(&error));
        QCOMPARE(document.validate(), QString());

        // What libkiems would write itself: defaulted keys absent.
        Document reopened;
        QVERIFY(reopened.open(document.configPath(), &error));
        QCOMPARE(number(reopened.documentValue("/grid/optimal")), 25.0);
        QCOMPARE(number(reopened.simulationValue(0, "/eye_draw_count")), 64.0);
        QFile file(document.configPath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto json = nlohmann::json::parse(file.readAll().toStdString());
        const auto& simulation = json["simulations"][0];
        QVERIFY(!simulation.contains("eye_draw_count"));
        QVERIFY(!simulation.contains("eye_test"));
        QVERIFY(simulation.contains("edge_terminated_nets"));
        QVERIFY(simulation["is_differential_pair"].get<bool>());
        QVERIFY(simulation["terminate_ground_at_cut"].get<bool>());
        QVERIFY(reopened.simulationValue(0, "/terminate_ground_at_cut").get<bool>());
    }

    void eyePresets() {
        Document document;
        QVERIFY(makeDocument(document));
        kiems_gui::TestsPanel panel(&document);
        panel.setSimulation(0);
        auto* standard = panel.findChild<QPushButton*>();
        QVERIFY(standard && standard->menu());
        QAction* usb = nullptr;
        for (QAction* action : standard->menu()->findChildren<QAction*>()) {
            if (action->data().toString() == "usb_high_speed") usb = action;
        }
        QVERIFY(usb);
        usb->trigger();
        auto value = [&](const char* key) { return number(document.simulationValue(0, key)); };
        QCOMPARE(QString::fromStdString(document.simulationValue(0, "/eye_test/standard").get<std::string>()),
                 QString("usb_high_speed"));
        QCOMPARE(value("/eye_bit_rate"), 480e6);
        QCOMPARE(value("/eye_test/mask_x1"), 0.075);
        QCOMPARE(value("/eye_test/mask_y2"), 0.525);
        QCOMPARE(standard->text(), QString("USB High Speed"));

        const QList<QuantityEdit*> edits = fields(&panel); // bit rate, level, x1, x2, y1, y2
        QCOMPARE(edits.size(), 6);
        type(edits[2], "0.075 UI"); // the preset's own value: still the preset
        QCOMPARE(QString::fromStdString(document.simulationValue(0, "/eye_test/standard").get<std::string>()),
                 QString("usb_high_speed"));
        type(edits[2], "0.1");
        QCOMPARE(QString::fromStdString(document.simulationValue(0, "/eye_test/standard").get<std::string>()),
                 QString("custom"));
        QCOMPARE(value("/eye_test/mask_x1"), 0.1);
        type(edits[5], ""); // no outer limit
        QCOMPARE(value("/eye_test/mask_y2"), 0.0);

        // A DRAM preset spans speed grades, so it leaves the bit rate alone.
        for (QAction* action : standard->menu()->findChildren<QAction*>()) {
            if (action->data().toString() == "ddr4") action->trigger();
        }
        QCOMPARE(value("/eye_bit_rate"), 480e6);
        QCOMPARE(value("/eye_test/mask_x1"), 0.4);
        QCOMPARE(document.validate(), QString());
    }

    void settingsPanel() {
        Document document;
        QVERIFY(makeDocument(document));
        kiems_gui::SettingsPanel panel(&document);
        panel.setSimulation(0);
        QuantityEdit* start = nullptr;
        QuantityEdit* stop = nullptr;
        QuantityEdit* spacing = nullptr;
        QuantityEdit* boundary = nullptr;
        for (QuantityEdit* edit : fields(&panel)) {
            if (edit->toolTip().startsWith("The band")) (start ? stop : start) = edit;
            if (edit->toolTip().startsWith("The spacing between ground stitching")) spacing = edit;
            if (edit->toolTip().startsWith("How many cells deep")) boundary = edit;
        }
        QVERIFY(start && stop && spacing && boundary);
        QCOMPARE(start->text(), QString("1 MHz"));
        type(stop, "10 GHz");
        QCOMPARE(number(document.documentValue("/frequency/stop")), 1e10);
        type(start, "20 GHz"); // above the stop frequency: undone
        QCOMPARE(number(document.documentValue("/frequency/start")), 1e6);
        QCOMPARE(start->text(), QString("1 MHz"));
        type(spacing, "2 mm");
        QCOMPARE(number(document.simulationValue(0, "/via_spacing")), 2000.0);
        type(boundary, "12");
        QVERIFY(document.documentValue("/grid/absorbing_boundary_cells").is_number_integer());
        QCOMPARE(number(document.documentValue("/grid/absorbing_boundary_cells")), 12.0);
        type(boundary, "99"); // past the 64-cell limit
        QCOMPARE(number(document.documentValue("/grid/absorbing_boundary_cells")), 12.0);
        QCOMPARE(document.validate(), QString());

        // The short pulse: off by default (and absent from the file), the sweep's own pulse.
        type(stop, "350 MHz");
        QCheckBox* shortPulse = nullptr;
        for (QCheckBox* box : panel.findChildren<QCheckBox*>()) {
            if (box->text().startsWith("Short excitation pulse")) shortPulse = box;
        }
        QVERIFY(shortPulse && !shortPulse->isChecked());
        QVERIFY(document.documentValue("/frequency/pulse").is_null());
        const double sweepPulse = 9 / (std::numbers::pi * (350e6 - 1e6) / 2);
        QVERIFY(std::abs(document.pulseLength() - sweepPulse) < 1e-15);
        shortPulse->setChecked(true);
        QCOMPARE(document.documentValue("/frequency/pulse").get<std::string>(), std::string("short"));
        // DC to 6 GHz: f0 = fc = 3 GHz, 0.95 ns -- whatever the sweep's top below that.
        QVERIFY(std::abs(document.pulseLength() - 9 / (std::numbers::pi * 3e9)) < 1e-15);
        kiems::Frequency parsed = document.documentValue("/frequency").get<kiems::Frequency>();
        QCOMPARE(parsed.pulse(), kiems::ExcitationPulse::Short);
        QCOMPARE(parsed.pulseF0(), 3e9);
        QCOMPARE(parsed.pulseFc(), 3e9);
        // Above 6 GHz the pulse follows the sweep; a coarse mesh lowers the top.
        parsed.setStop(10e9);
        QCOMPARE(parsed.pulseFc(), 5e9);
        QVERIFY(kiems::Frequency::shortPulseTopForGrid(5000) < 6e9);
        QCOMPARE(kiems::Frequency::shortPulseTopForGrid(500), 6e9);
        nlohmann::json written = parsed;
        QCOMPARE(written["pulse"].get<std::string>(), std::string("short"));
        parsed.setPulse(kiems::ExcitationPulse::Sweep);
        written = parsed;
        QVERIFY(!written.contains("pulse"));
        shortPulse->setChecked(false);
        QVERIFY(std::abs(document.pulseLength() - sweepPulse) < 1e-15);
        QCOMPARE(document.validate(), QString());
    }

    void heuristics() {
        QVERIFY(SimulationEditor::isPowerNet("+3V3"));
        QVERIFY(SimulationEditor::isPowerNet("/power/VDD_IO"));
        QVERIFY(SimulationEditor::isPowerNet("AGND"));
        QVERIFY(!SimulationEditor::isPowerNet("USB_DP"));
        QCOMPARE(SimulationEditor::differentialPartnerCandidates("USB_P"), QStringList({"USB_N", "USB_N"}));
        QCOMPARE(SimulationEditor::differentialPartnerCandidates("D+"), QStringList({"D-"}));
        QVERIFY(SimulationEditor::isPassive("R12"));
        QVERIFY(SimulationEditor::isPassive("c3"));
        QVERIFY(!SimulationEditor::isPassive("RN1"));
        QVERIFY(!SimulationEditor::isPassive("U1"));
    }

    void pinPorts() {
        Document document;
        QVERIFY(makeDocument(document));
        const auto board = fakeBoard();
        const int sim = document.addSimulation("Pins", "GND");
        SimulationEditor editor(document, board.get(), sim, 1);

        // The resolver's default termination: an input on a multi-pad, non-ground net.
        QVERIFY(editor.pinState("U2", "1").absorbing);
        QVERIFY(!editor.pinState("U1", "1").absorbing); // an output
        QVERIFY(!editor.pinState("U2", "2").absorbing); // on the ground net
        QVERIFY(!editor.pinState("U2", "3").absorbing); // alone on its net

        editor.setPinExcited("U1", "1", true);
        QCOMPARE(editor.netLevel("SIG"), NetLevel::Simulated);
        auto state = editor.pinState("U1", "1");
        QVERIFY(state.excited && state.absorbing);
        const auto excitation = editor.excitation("U1", "1");
        QVERIFY(excitation && excitation->isMain() && excitation->amplitude() == 1.0);

        editor.setPinProbed("U2", "1", true);
        state = editor.pinState("U2", "1");
        QVERIFY(state.probed && state.absorbing); // the default carries into the explicit choice
        editor.setPinAbsorbing("U2", "1", false);
        state = editor.pinState("U2", "1");
        QVERIFY(state.probed && !state.absorbing);
        editor.setPinProbed("U2", "1", false);
        state = editor.pinState("U2", "1");
        QVERIFY(!state.probed && !state.absorbing); // the explicit "no port" outlives the default

        editor.setPinImpedance("U2", "1", 50.0);
        editor.setPinLoadCapacitance("U2", "1", 1.2e-12);
        state = editor.pinState("U2", "1");
        QCOMPARE(*state.impedance, 50.0);
        QCOMPARE(*state.loadCapacitance, 1.2e-12);
        QCOMPARE(state.netImpedance, 45.0);
        editor.setPinImpedance("U2", "1", std::nullopt);
        QVERIFY(!editor.pinState("U2", "1").impedance);

        // Adversarial: narrowband at the bottom of the sweep, running continuously.
        editor.setExcitationMain("U1", "1", false);
        auto adversarial = editor.excitation("U1", "1");
        QVERIFY(adversarial && !adversarial->isMain());
        QCOMPARE(*adversarial->frequency(), 1e6);
        QVERIFY(adversarial->durationMode() == kiems::ExcitationDurationMode::Continuous);
        editor.updateExcitation("U1", "1", [](kiems::ExcitationConfig& e) { e.setFrequency(250e6); });
        QCOMPARE(*editor.excitation("U1", "1")->frequency(), 250e6);
        editor.setExcitationMain("U1", "1", true);

        // The new entry is explicit: only chosen pins get ports, not every pad on the net.
        const auto entry = document.simulationJson(sim)->at("involved_nets");
        bool sigExplicit = false;
        for (const auto& e : entry) {
            if (e.value("net", std::string()) == "SIG") sigExplicit = e.contains("probed_pins");
        }
        QVERIFY(sigExplicit);

        editor.setPinExcited("U1", "1", false);
        QVERIFY(!editor.excitation("U1", "1"));
        editor.setPinExcited("U1", "1", true);
        QCOMPARE(document.validate(), QString());
    }

    void netsAndComponents() {
        Document document;
        QVERIFY(makeDocument(document));
        const auto board = fakeBoard();
        const int sim = document.addSimulation("Pins", "GND");
        SimulationEditor editor(document, board.get(), sim, 1);

        editor.setNetIncluded("USB_P", true);
        QCOMPARE(editor.netLevel("USB_P"), NetLevel::GeometryOnly);
        editor.setNetSimulated("USB_P", true);
        QCOMPARE(editor.netLevel("USB_P"), NetLevel::Simulated);
        editor.setHullPadding("USB_P", 2500);
        QCOMPARE(editor.hullPadding("USB_P"), 2500.0);
        editor.setImpedanceProbed("USB_P", true);
        QVERIFY(editor.impedanceProbed("USB_P"));
        editor.setNetImpedance("USB_P", 90);
        QCOMPARE(editor.netImpedance("USB_P"), 90.0);
        editor.setNetSimulated("USB_P", false); // demoted, never removed
        QCOMPARE(editor.netLevel("USB_P"), NetLevel::GeometryOnly);
        editor.setNetIncluded("USB_P", false);
        QCOMPARE(editor.netLevel("USB_P"), NetLevel::None);

        editor.setComponentIncluded("R1", true);
        QVERIFY(editor.componentIncluded("R1"));
        QVERIFY(!editor.componentContributesToHull("R1"));
        editor.setComponentContributesToHull("R1", true);
        editor.setComponentHullPadding("R1", 1000);
        QCOMPARE(editor.componentHullPadding("R1"), 1000.0);
        editor.setComponentIncluded("R1", false);
        QVERIFY(!document.simulationJson(sim)->contains("included_components"));

        // An untouched entry keeps its exact text through another net's edit.
        editor.setNetIncluded("SIG", true);
        QCOMPARE(document.simulationJson(sim)->at("involved_nets").size(), std::size_t(1));
        const nlohmann::json before = document.simulationJson(sim)->at("involved_nets")[0];
        editor.setNetIncluded("USB_N", true);
        QVERIFY(document.simulationJson(sim)->at("involved_nets")[0] == before);
        QCOMPARE(document.validate(), QString());
    }

    void boardSetupDefaults() {
        Document document;
        QVERIFY(makeDocument(document));
        const auto board = fakeBoard();
        document.setDocumentValue("/board_setup", nlohmann::json{
            {"signal_impedance", 45}, {"power_impedance", 1},
            {"net_class_impedances", {{{"name", "USB"}, {"impedance", 90}}}},
            {"net_impedances", nlohmann::json::array()},
            {"pins", {{{"footprint", "U1"}, {"pin", "3"}, {"noise_generating", true},
                       {"noise_frequency", 300e6}, {"noise_amplitude", 0.5}},
                      {{"footprint", "U3"}, {"pin", "1"}, {"impedance", 75}}}}});
        const int sim = document.addSimulation("Pins", "GND");
        SimulationEditor editor(document, board.get(), sim, 1);
        editor.setNetIncluded("USB_P", true);
        QCOMPARE(editor.netImpedance("USB_P"), 90.0);                  // its class's
        QCOMPARE(*editor.pinState("U3", "1").impedance, 75.0);         // the pin's own
        QVERIFY(!editor.excitation("U1", "3"));                        // geometry only: no noise yet
        editor.setNetSimulated("USB_P", true);
        const auto noise = editor.excitation("U1", "3");
        QVERIFY(noise && !noise->isMain());
        QCOMPARE(*noise->frequency(), 300e6);
        QCOMPARE(*noise->amplitude(), 0.5);
        QCOMPARE(document.validate(), QString());
    }

    void boardSetupPanel() {
        Document document;
        QVERIFY(makeDocument(document));
        const auto board = fakeBoard();
        kiems_gui::BoardSetupPanel panel(&document);
        panel.setBoard(board);
        auto setup = [&] { return SimulationEditor::boardSetup(document); };
        auto field = [&](const QString& toolTipStart) -> QuantityEdit* {
            for (QuantityEdit* edit : fields(&panel)) {
                if (edit->toolTip().startsWith(toolTipStart)) return edit;
            }
            return nullptr;
        };

        // Board-wide defaults.
        QuantityEdit* signal = field("The impedance a signal net");
        QuantityEdit* bitRate = field("The bit rate new");
        QuantityEdit* usb = field("The impedance of the USB class");
        QVERIFY(signal && bitRate && usb);
        type(signal, "50");
        QCOMPARE(setup().signalImpedance(), 50.0);
        type(bitRate, "480 Mb/s");
        QCOMPARE(setup().bitRate(), 480e6);
        type(bitRate, "");
        QCOMPARE(setup().bitRate(), 0.0);
        type(usb, "90");
        QCOMPARE(*setup().netClassImpedance("USB"), 90.0);
        auto* ground = panel.findChild<QComboBox*>("boardGround");
        QVERIFY(ground);
        const int gnd = ground->findText("GND");
        QVERIFY(gnd > 0);
        ground->setCurrentIndex(gnd);
        emit ground->activated(gnd);
        QVERIFY(setup().groundName() == "GND");
        QVERIFY(setup().groundKind() == kiems::GroundSelectorKind::Net);

        // A picked input pin absorbs by default; unticking records the choice, ticking again
        // hands it back to the rule and leaves no entry behind.
        panel.setPick({"SIG", "F.Cu", "U2", "1"});
        auto* absorbing = panel.findChild<QCheckBox*>("boardPinAbsorbing");
        auto* noise = panel.findChild<QCheckBox*>("boardPinNoise");
        QVERIFY(absorbing && noise);
        QVERIFY(absorbing->isChecked());
        absorbing->click();
        QVERIFY(setup().pin("U2", "1")->absorbing == std::optional(false));
        absorbing->click();
        QVERIFY(setup().pin("U2", "1") == nullptr);
        noise->click();
        QVERIFY(setup().pin("U2", "1")->noiseGenerating);
        QCOMPARE(setup().pin("U2", "1")->noiseFrequency, 100e6);

        // The picked net: included, then edge terminated; leaving it out drops the termination.
        auto* included = panel.findChild<QCheckBox*>("boardNetIncluded");
        auto* terminated = panel.findChild<QCheckBox*>("boardNetTerminated");
        QVERIFY(included && terminated);
        QVERIFY(!terminated->isEnabled());
        included->click();
        QVERIFY(terminated->isEnabled());
        terminated->click();
        QVERIFY(setup().edgeTerminatedNets() == std::vector<std::string>{"SIG"});
        QCOMPARE(panel.findChild<QListWidget*>("boardEdgeNets")->count(), 1);
        included->click();
        QVERIFY(setup().includedNets().empty() && setup().edgeTerminatedNets().empty());
        included->click();
        terminated->click();

        // A net in a class, and a passive part.
        panel.setPick({"USB_P", "F.Cu", "", ""});
        panel.findChild<QCheckBox*>("boardNetClassIncluded")->click();
        QVERIFY(setup().includedNetClasses() == std::vector<std::string>{"USB"});
        panel.setPick({"SIG", "F.Cu", "R1", "1"});
        panel.findChild<QCheckBox*>("boardComponentIncluded")->click();
        QVERIFY(setup().includedComponents() == std::vector<std::string>{"R1"});

        // A new simulation starts from all of it.
        const int sim = document.addSimulation("New", "SIG");
        SimulationEditor editor(document, board.get(), sim);
        editor.applyBoardSetup();
        const nlohmann::json& json = *document.simulationJson(sim);
        QVERIFY(json["ground_net"] == nlohmann::json({{"net", "GND"}}));
        QVERIFY(json["edge_terminated_nets"] == nlohmann::json::array({"SIG"}));
        QCOMPARE(editor.netLevel("SIG"), NetLevel::GeometryOnly);
        QCOMPARE(editor.netImpedance("SIG"), 50.0);
        bool usbClass = false;
        for (const auto& entry : json["involved_nets"]) {
            if (entry.value("net_class", std::string()) == "USB") {
                usbClass = true;
                QCOMPARE(entry["impedance"].get<double>(), 90.0);
            }
        }
        QVERIFY(usbClass);
        QVERIFY(editor.componentIncluded("R1"));
        QCOMPARE(document.validate(), QString());
    }

    void differentialPairs() {
        Document document;
        QVERIFY(makeDocument(document));
        const auto board = fakeBoard();
        const int sim = document.addSimulation("Pins", "GND");
        SimulationEditor editor(document, board.get(), sim, 1);
        // Set up one half first, then tick Differential pair: the partner half follows.
        editor.setPinExcited("U1", "3", true);
        editor.setPinProbed("U3", "1", true);
        document.setSimulationValue(sim, "/is_differential_pair", true);
        editor.reconcileDifferentialPairs();
        QCOMPARE(editor.netLevel("USB_N"), NetLevel::Simulated);
        const auto partner = editor.excitation("U1", "4");
        QVERIFY(partner && partner->isMain());
        QCOMPARE(*partner->amplitude(), -1.0); // antiphase
        QVERIFY(editor.pinState("U3", "2").probed);

        // Changes now mirror as they're made.
        editor.updateExcitation("U1", "3", [](kiems::ExcitationConfig& e) { e.setPhaseDegrees(30); });
        QCOMPARE(editor.excitation("U1", "4")->phaseDegrees(), 30.0);
        editor.setPinProbed("U3", "1", false);
        QVERIFY(!editor.pinState("U3", "2").probed);
        editor.setPinExcited("U1", "3", false);
        QVERIFY(!editor.excitation("U1", "4"));

        const auto& entries = document.simulationJson(sim)->at("involved_nets");
        int paired = 0;
        for (const auto& e : entries) {
            if (e.contains("differential_pair_partner")) ++paired;
        }
        QCOMPARE(paired, 2);
        QCOMPARE(document.validate(), QString());
    }

    void presetTable() {
        // Every preset names a standard libkiems knows -- it reads an unknown one as Custom, so a
        // misspelt name would otherwise pass unnoticed.
        for (const EyeTestPreset& preset : EyeTestPreset::all()) {
            const nlohmann::json written = {{"standard", preset.standard}, {"mask_x1", preset.x1}};
            const auto parsed = written.get<kiems::EyeTestConfig>();
            const nlohmann::json roundTripped = parsed;
            QCOMPARE(QString::fromStdString(roundTripped["standard"].get<std::string>()), QString(preset.standard));
            QVERIFY(preset.x1 <= preset.x2 && preset.x2 <= 0.5);
        }
    }

    void eyes() {
        // Two known channels through the whole path the Eyes tab takes: libkiems builds the eyes and
        // writes eyes.json as kiems does, the GUI reads it back and draws it. A single-pole low-pass
        // with a delay: at 2 GHz a 480 Mb/s eye is wide open, at 150 MHz the bits smear together.
        std::vector<double> frequencies;
        for (int f = 1; f <= 6000; ++f) frequencies.push_back(f * 1e6);
        const auto channel = [&](double cornerHz, double delaySeconds) {
            std::vector<std::complex<double>> transfer;
            for (const double f : frequencies) {
                transfer.push_back(std::polar(1.0, -2 * std::numbers::pi * f * delaySeconds) /
                                   std::complex<double>(1.0, f / cornerHz));
            }
            return transfer;
        };
        kiems::EyeTestConfig test;
        test.standard = kiems::EyeMaskStandard::UsbHighSpeed;
        test.signalLevel = 0.4;
        test.mask = {0.075, 0.375, 0.3, 0.525};
        std::vector<kiems::SimulationEye> eyes;
        for (const auto& [name, corner] : {std::pair{"Fast channel", 2e9}, std::pair{"Slow channel", 150e6}}) {
            auto eye = kiems::computeEyeDiagram(frequencies, channel(corner, 0.7e-9), 480e6, {}, {}, test.signalLevel,
                                                test.mask);
            QVERIFY(eye.has_value());
            eyes.push_back({name, false, 0, std::move(*eye)});
        }
        // Launched at +-0.4 V into a lossless-at-DC channel, the open eye is nearly 0.8 V tall.
        QVERIFY(eyes[0].eye.opening.heightV > 0.7 && eyes[0].eye.opening.heightV < 0.81);
        QVERIFY(eyes[0].eye.maskResult && eyes[0].eye.maskResult->passed());
        QVERIFY(eyes[1].eye.opening.heightV < eyes[0].eye.opening.heightV - 0.2);
        QVERIFY(eyes[1].eye.maskResult && !eyes[1].eye.maskResult->passed());
        // Both are centred on their threshold crossings, so the mask sits on the eye: the traces
        // switching at 0 UI cross there on average -- not where the slow channel's edges start.
        for (const auto& simulated : eyes) {
            const kiems::EyeDiagramData& eye = simulated.eye;
            double crossings = 0;
            int count = 0;
            for (const auto& trace : eye.traces) {
                for (std::size_t i = 0; i + 1 < trace.size() && eye.timeUI[i] < 0.5; ++i) {
                    const double a = trace[i] - eye.thresholdV, b = trace[i + 1] - eye.thresholdV;
                    if (eye.timeUI[i] < -0.5 || (a < 0) == (b < 0)) continue;
                    crossings += eye.timeUI[i] + (eye.timeUI[i + 1] - eye.timeUI[i]) * a / (a - b);
                    ++count;
                    break;
                }
            }
            QVERIFY(count > 20);
            QVERIFY2(std::fabs(crossings / count) < 0.05, qPrintable(QString::number(crossings / count)));
        }

        QTemporaryDir dir;
        kiems::saveEyeDiagrams(eyes, test, std::filesystem::path(dir.path().toStdWString()) / "eyes.json");
        const kiems_gui::EyeResults results = kiems_gui::EyeResults::load(dir.path());
        QCOMPARE(results.error, QString());
        QCOMPARE(results.standard, QString("usb_high_speed"));
        QCOMPARE(results.eyes.size(), std::size_t(2));
        QCOMPARE(results.maskY1, 0.3);
        const kiems_gui::EyeEntry& open = results.eyes[0];
        QCOMPARE(open.name, QString("Fast channel"));
        QCOMPARE(open.traces.size(), eyes[0].eye.traces.size());
        QVERIFY(open.mask && open.mask->passed);
        QVERIFY(results.eyes[1].mask && !results.eyes[1].mask->passed);
        QVERIFY(std::fabs(open.opening.heightV - eyes[0].eye.opening.heightV) < 1e-6);

        // The bits sit where the chart puts them: at 0.5 UI every trace is a settled one or zero,
        // and at 0 and 1 UI the transitions cross the threshold.
        const auto at = [&](double ui) {
            const auto nearest = std::min_element(open.timeUI.begin(), open.timeUI.end(), [&](double a, double b) {
                return std::fabs(a - ui) < std::fabs(b - ui);
            });
            return static_cast<std::size_t>(nearest - open.timeUI.begin());
        };
        int ones = 0, zeros = 0;
        for (const auto& trace : open.traces) {
            const double middle = trace[at(0.5)] - open.thresholdV;
            QVERIFY(std::fabs(middle) > 0.3);
            (middle > 0 ? ones : zeros)++;
        }
        QVERIFY(ones > 30 && zeros > 30);
        for (const double edge : {0.0, 1.0}) {
            double nearest = INFINITY;
            for (const auto& trace : open.traces) nearest = std::min(nearest, std::fabs(trace[at(edge)] - open.thresholdV));
            QVERIFY(nearest < 0.15);
        }

        // Drawn, for a look: KIEMS_GUI_SNAPSHOTS names a folder to save it in.
        std::unique_ptr<QWidget> page(kiems_gui::makeEyesPage(results));
        page->resize(1000, 1500);
        const QPixmap pixmap = page->grab();
        QVERIFY(!pixmap.isNull());
        if (const QString snapshots = qEnvironmentVariable("KIEMS_GUI_SNAPSHOTS"); !snapshots.isEmpty()) {
            QVERIFY(pixmap.save(QDir(snapshots).filePath("eyes.png")));
        }
    }

    // The run queue: one job at a time, in order, no duplicates, failures and cancels move on.
    void simulationSets() {
        // A memory bus: U1 drives DQ0-2 and DQ10 with their strobes DQS0/1; U2 receives them.
        auto board = std::make_shared<kiems_gui::BoardData>();
        board->footprints.push_back({"U1", "MCU", {{"1", "", "DQ0", "output"}, {"2", "", "DQ1", "output"},
                                                   {"3", "", "DQ2", "output"}, {"4", "", "DQS0", "output"},
                                                   {"5", "", "DQS1", "output"}, {"6", "", "GND", "power_in"},
                                                   {"7", "", "DQ10", "output"}}});
        board->footprints.push_back({"U2", "DRAM", {{"1", "", "DQ0", "input"}, {"2", "", "DQ1", "input"},
                                                    {"3", "", "DQS0", "input"}, {"4", "", "DQS1", "input"},
                                                    {"5", "", "GND", "power_in"}, {"6", "", "DQ10", "input"}}});
        board->nets = {"DQ0", "DQ1", "DQ2", "DQ10", "DQS0", "DQS1", "GND"};
        board->copperLayers = {"F.Cu", "In1.Cu", "In2.Cu", "B.Cu"};

        Document document;
        QString error;
        QVERIFY(document.create(_directory.filePath("sets.g2esim"), _directory.filePath("board.kicad_pcb"), &error));
        document.addSimulation("Lone", "GND");
        document.setNetIncluded(0, "DQ0", true, 1);
        const int set = document.addSimulationSet("Bus", "GND");
        QVERIFY(document.isSimulationSet(set));
        QVERIFY(!document.isSimulationSet(0));
        const QString id = document.simulationSetId(set);
        QCOMPARE(id.size(), 36);
        QVERIFY(document.validate().isEmpty()); // a template with no nets parses

        auto config = *kiems_gui::simulationSet(document, set);
        QCOMPARE(kiems_gui::describeSimulationSet(config, board.get()).join('\n'),
                 QString("Choose an excitation IC and a net name pattern."));
        config.excitationFootprint = "U1";
        config.netNamePattern = R"(DQ(\d+))";
        config.additionalNetPatterns = {"DQS$1"};
        config.phaseDegrees = 90;
        config.amplitude = 0.4;
        config.driveImpedance = 40;
        config.probes = {kiems::SimulationSetProbe{"U2", true, 50, std::nullopt}, kiems::SimulationSetProbe{"U9"}};
        kiems_gui::setSimulationSet(document, set, config);
        QCOMPARE(kiems_gui::describeSimulationSet(config, board.get()),
                 QStringList({"Generates 4 simulation(s).", "U9 has no pins on the matching nets."}));

        const auto first = kiems_gui::regenerateSimulationSet(document, board.get(), set);
        QVERIFY(first);
        QCOMPARE(first->added, 4);
        QList<int> generated = kiems_gui::generatedSimulations(document, set);
        QStringList names;
        for (int index : generated) names << document.simulationName(index);
        QCOMPARE(names, QStringList({"Bus DQ0", "Bus DQ1", "Bus DQ2", "Bus DQ10"})); // numbers sort as numbers
        QVERIFY2(document.validate().isEmpty(), qPrintable(document.validate()));

        // Bus DQ0 drives U1's DQ0 pin, includes its strobe, and probes U2 on both.
        const int dq0 = document.simulationIndex("Bus DQ0");
        QCOMPARE(document.generatedBySet(dq0), id);
        QCOMPARE(document.generatingTemplate(dq0), set);
        QVERIFY(!document.isSimulationSet(dq0));
        QVERIFY(!document.simulationJson(dq0)->contains("simulation_set"));
        QVERIFY(*document.mainExcitation(dq0) == (PinRef{"U1", "1"}));
        SimulationEditor editor(document, board.get(), dq0);
        const auto excitation = *editor.excitation("U1", "1");
        QCOMPARE(excitation.phaseDegrees(), 90.0);
        QCOMPARE(*excitation.amplitude(), 0.4);
        QCOMPARE(*editor.pinState("U1", "1").impedance, 40.0);
        QCOMPARE(editor.netLevel("DQ0"), NetLevel::Simulated);
        QCOMPARE(editor.netLevel("DQS0"), NetLevel::Simulated);
        QCOMPARE(editor.netLevel("DQ1"), NetLevel::None);
        for (const char* pin : {"1", "3"}) {
            const auto state = editor.pinState("U2", pin);
            QVERIFY(state.probed);
            QVERIFY(state.absorbing);
            QCOMPARE(*state.impedance, 50.0);
        }
        QVERIFY(!editor.pinState("U2", "2").probed);
        // DQ2 has no strobe and DQ10 none either; each still drives its own pin.
        QVERIFY(*document.mainExcitation(document.simulationIndex("Bus DQ10")) == (PinRef{"U1", "7"}));

        // Regenerating an unchanged set changes nothing, and leaves a saved document saved.
        QVERIFY(document.save(&error));
        QSignalSpy changed(&document, &Document::changed);
        const auto again = kiems_gui::regenerateSimulationSet(document, board.get(), set);
        QCOMPARE(again->added + again->changed + again->removed, 0);
        QCOMPARE(changed.count(), 0);
        QVERIFY(!document.isModified());

        // A narrower pattern drops what no longer matches and keeps the rest where they were.
        config.netNamePattern = "DQ[01]";
        config.additionalNetPatterns.clear();
        kiems_gui::setSimulationSet(document, set, config);
        changed.clear();
        const auto narrower = kiems_gui::regenerateSimulationSet(document, board.get(), set);
        QCOMPARE(narrower->removed, 2);
        QCOMPARE(narrower->added, 0);
        QCOMPARE(narrower->changed, 2); // no strobes now
        QCOMPARE(changed.count(), 1);  // one change for the whole regeneration
        QCOMPARE(kiems_gui::generatedSimulations(document, set).size(), 2);
        QCOMPARE(SimulationEditor(document, board.get(), document.simulationIndex("Bus DQ0")).netLevel("DQS0"),
                 NetLevel::None);

        // Listed nets, duplicates and unknown names ignored.
        config.netSelectionMode = kiems::SimulationSetNetSelectionMode::List;
        config.netNames = {"DQ2", "DQ2", "NOPE"};
        config.excitationDuration = 2e-9;
        kiems_gui::setSimulationSet(document, set, config);
        kiems_gui::regenerateSimulationSet(document, board.get(), set);
        generated = kiems_gui::generatedSimulations(document, set);
        QCOMPARE(generated.size(), 1);
        QCOMPARE(document.simulationName(generated[0]), QString("Bus DQ2"));
        QCOMPARE(*document.mainExcitationDuration(generated[0]), 2e-9);

        // A bad pattern says why, and generates nothing.
        config.netSelectionMode = kiems::SimulationSetNetSelectionMode::RegularExpression;
        config.netNamePattern = "DQ(";
        QVERIFY(kiems_gui::describeSimulationSet(config, board.get()).first().startsWith("Invalid net name pattern"));
        QVERIFY(kiems_gui::expandSimulationSet(config, *board).isEmpty());

        // Without a board there's nothing to match, so nothing changes.
        QVERIFY(!kiems_gui::regenerateSimulationSet(document, nullptr, set));

        // The panel edits the template's set.
        // Inside a parent that's never shown, so showing the panel doesn't open a window.
        QWidget host;
        kiems_gui::SimulationSetPanel panel(&document, &host);
        panel.setBoard(board);
        panel.setSimulation(0);
        QVERIFY(panel.isHidden()); // not a set
        panel.setSimulation(set);
        QVERIFY(!panel.isHidden());
        panel.findChild<QComboBox*>("selectionMode")->setCurrentIndex(0);
        QCOMPARE(kiems_gui::simulationSet(document, set)->netSelectionMode,
                 kiems::SimulationSetNetSelectionMode::RegularExpression);
        auto* pattern = panel.findChild<QLineEdit*>("pattern");
        pattern->setText("DQ1|DQ10");
        emit pattern->editingFinished();
        QCOMPARE(kiems_gui::simulationSet(document, set)->netNamePattern, std::string("DQ1|DQ10"));
        QCOMPARE(panel.findChild<QLabel*>("summary")->text(), QString("Generates 2 simulation(s).\nU9 has no pins on the matching nets."));
        panel.findChild<QPushButton*>("addProbe")->click();
        QCOMPARE(kiems_gui::simulationSet(document, set)->probes.size(), std::size_t(3));
        QCOMPARE(panel.findChildren<QGroupBox*>().size(), 1 + 3); // the set's own box and one per probe

        // A generated name holds the net's, often a KiCad hierarchical "/sheet/NET": its output folder
        // must still be one folder, which on Windows takes escaping.
        const auto folder = [](const std::string& name) {
            return QString::fromStdU16String(kiems::simulationDirectoryName(name).u16string());
        };
#ifdef _WIN32
        QCOMPARE(folder("USB /U1D+"), QString("USB %2FU1D+"));
        QCOMPARE(folder("a: b?"), QString("a%3A b%3F"));
        QCOMPARE(folder("50%"), QString("50%25"));
        QCOMPARE(folder("tail. "), QString("tail%2E%20"));
        QCOMPARE(folder("con.txt"), QString("%63on.txt"));
        QCOMPARE(folder("COM3"), QString("%43OM3"));
        QCOMPARE(folder("COM10"), QString("COM10"));
        for (const char* name : {"USB /U1D+", "a: b?", "50%", "tail. ", "con.txt", "plain"}) {
            QCOMPARE(kiems::simulationNameFromDirectory(kiems::simulationDirectoryName(name)), std::string(name));
        }
        QVERIFY(document.resultsDir(generated[0]).endsWith("ems/results/Bus DQ2"));
        const int hierarchical = document.addSimulation("Set /sheet/NET", "GND");
        QVERIFY(document.simulationDir(hierarchical).endsWith("ems/simulation/Set %2Fsheet%2FNET"));
        document.removeSimulation(hierarchical);
#else
        QCOMPARE(folder("USB /U1D+"), QString("USB /U1D+"));
#endif

        // Renaming the template renames what it generates.
        document.renameSimulation(set, "Data");
        kiems_gui::regenerateSimulationSet(document, board.get(), set);
        names.clear();
        for (int index : kiems_gui::generatedSimulations(document, set)) names << document.simulationName(index);
        QCOMPARE(names, QStringList({"Data DQ1", "Data DQ10"}));
        QVERIFY2(document.validate().isEmpty(), qPrintable(document.validate()));
    }

    void geometryScene() {
        // A 10 mm square cut of a 2-layer, 1.51 mm board, as the geometry stage writes it (0.1 um
        // units): a triangle of copper on each layer, a stitching via, a rejected one, top solder
        // mask, and a small mesh.
        const auto tri = [](double x0, double y0, double x1, double y1, double x2, double y2) {
            return nlohmann::json{{"a", {{"x", x0}, {"y", y0}}}, {"b", {{"x", x1}, {"y", y1}}}, {"c", {{"x", x2}, {"y", y2}}}};
        };
        const auto pt = [](double x, double y) { return nlohmann::json{{"x", x}, {"y", y}}; };
        const nlohmann::json square = {pt(0, 0), pt(100000, 0), pt(100000, 100000), pt(0, 100000)};
        nlohmann::json json = {
            {"geometry",
             {{"slicedBoard",
               {{"previewLayerTriangles",
                 {{tri(0, 0, 100000, 0, 0, 100000)}, {tri(0, 0, 100000, 0, 100000, 100000)}}},
                {"topMaskTriangles", {tri(0, 0, 100000, 0, 0, 100000)}},
                {"bottomMaskTriangles", nlohmann::json::array()},
                {"cutoutLoops", {square}},
                {"outline", square},
                {"npthHoleLoops", nlohmann::json::array()},
                {"stitchingVias", {{{"x", 90000.0}, {"y", 90000.0}, {"diameter", 3000.0}, {"annularRingDiameter", 6000.0}}}},
                {"failedStitchingViaAttempts", {pt(10000, 90000)}},
                {"bounds", {{"xMin", 0.0}, {"yMin", 0.0}, {"xMax", 100000.0}, {"yMax", 100000.0}}}}}}},
            {"grid",
             {{"gridLines",
               {{"x", {-20000.0, 0.0, 50000.0, 100000.0, 120000.0}},
                {"y", {-20000.0, 0.0, 50000.0, 100000.0, 120000.0}},
                {"z", {-35100.0, -15100.0, 0.0, 20000.0}},
                {"pmlInnerXMin", -10000.0}, {"pmlInnerXMax", 110000.0},
                {"pmlInnerYMin", -10000.0}, {"pmlInnerYMax", 110000.0},
                {"pmlInnerZMin", -30000.0}, {"pmlInnerZMax", 15000.0}}}}}};
        const QString path = _directory.filePath("geometry.json");
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray::fromStdString(json.dump()));
        }
        const kiems_gui::SimulationGeometry geometry = kiems_gui::SimulationGeometry::load(path);
        QVERIFY2(geometry.isValid(), qPrintable(geometry.error));
        QCOMPARE(geometry.layerTriangles.size(), std::size_t(2));
        QCOMPARE(geometry.gridZ.size(), std::size_t(4));
        QVERIFY(!kiems_gui::SimulationGeometry::load(_directory.filePath("missing.json")).isValid());

        // The board: its stackup, outline from (0, 0), one via inside the cut and one far outside,
        // and U1 driving SIG into U2.
        auto board = fakeBoard();
        using Kind = libkicad::StackupLayerKind;
        board->stackup = {{Kind::SolderMaskTop, "F.Mask", 0.01, 3.3, 0.02}, {Kind::Copper, "F.Cu", 0.035, 0, 0},
                          {Kind::Core, "Dielectric 1", 1.51, 4.5, 0.02}, {Kind::Copper, "B.Cu", 0.035, 0, 0},
                          {Kind::SolderMaskBottom, "B.Mask", 0.01, 3.3, 0.02}};
        board->copperLayers = {"F.Cu", "B.Cu"};
        board->geometry.outline = {{false, {{0, 0}, {10, 0}, {10, 10}, {0, 10}}}};
        libkicad::ThroughHole inside;
        inside.xMm = 5;
        inside.yMm = 5;
        inside.drillWidthMm = inside.drillHeightMm = 0.3;
        inside.padWidthMm = inside.padHeightMm = 0.6;
        libkicad::ThroughHole outside = inside;
        outside.xMm = 40;
        board->throughHoles = {inside, outside};
        board->pads = {{"U1", "1", "SIG", "output", 2, 2, 0, "F.Cu", 1, 0.5},
                       {"U2", "1", "SIG", "input", 8, 8, 90, "F.Cu", 1, 0.5}};
        board->layerColors = {{"F.Cu", "#C83434FF"}};

        Document document;
        QVERIFY(makeDocument(document));
        document.setMainExcitation(0, PinRef{"U1", "1"});
        const auto context = kiems_gui::GeometryContext::of(*board, document, 0);
        QCOMPARE(context.metalNames, QStringList({"F.Cu", "B.Cu"}));
        QCOMPARE(context.metalZ, (std::vector<double>{0, -15100}));
        QCOMPARE(context.topMaskZ, 100.0);
        QCOMPARE(context.bottomMaskZ, -15200.0);
        QCOMPARE(context.boardVias.size(), std::size_t(2));
        QCOMPARE(context.boardVias[0].x, 50000.0);
        QCOMPARE(context.ports.size(), std::size_t(2));
        QCOMPARE(context.ports[0].name, QString("U1.1"));
        QVERIFY(context.ports[0].kind == kiems_gui::PortKind::Excited);
        QVERIFY(context.ports[1].kind == kiems_gui::PortKind::Load); // U2's input absorbs by default

        const kiems_gui::GeometryScene scene = kiems_gui::buildGeometryScene(geometry, context);
        QStringList names;
        for (const auto& group : scene.groups) names << group.name;
        QCOMPARE(names, QStringList({"F.Cu", "B.Cu", "Vias", "Stitching vias", "Rejected stitching vias", "Ports",
                                     "Outline", "Top solder mask", "Mesh"}));
        // Each layer at its height (scene mm, centred in X and Y), in its KiCad color.
        const auto& top = scene.groups[0].triangles;
        QCOMPARE(top.size(), std::size_t(3));
        QCOMPARE(top[0].x, -5.0f);
        QCOMPARE(top[0].z, 0.0f);
        QVERIFY(std::abs(scene.groups[1].triangles[0].z + 1.51f) < 1e-6f);
        QVERIFY(std::abs(top[0].r - 200 / 255.0f) < 1e-6f);
        // Only the via inside the cut: its barrel, and its ring on both layers.
        QCOMPARE(scene.groups[2].triangles.size(), std::size_t(24 * 6 * 3));
        QVERIFY(!scene.groups[7].visibleByDefault); // the mask hides the copper until asked for
        QVERIFY(scene.groups[7].translucent);
        QVERIFY(std::abs(scene.groups[7].triangles[0].z - 0.01f) < 1e-6f);
        QCOMPARE(scene.gridSummary, QString("Mesh: 5 x 5 x 4 lines, 0.00 M cells"));
        QVERIFY(std::abs(scene.boundsMin.z() + 1.52f) < 1e-5f);

        // The page loads it in the background and draws it, without opening a window.
        kiems_gui::GeometryView view;
        view.setAttribute(Qt::WA_DontShowOnScreen);
        view.resize(900, 650);
        view.show();
        QSignalSpy loaded(&view, &kiems_gui::GeometryView::sceneLoaded);
        view.showSimulation("Trace", path, context);
        QVERIFY(loaded.wait(10000));
        QVERIFY(view.scene());
        QCOMPARE(view.findChild<QListWidget*>()->count(), 9);
        const QImage image = view.canvas()->grabFramebuffer();
        QVERIFY(!image.isNull());
        // Something other than the background was drawn.
        const QRgb background = image.pixel(2, 2);
        bool drawn = false;
        for (int y = 0; y < image.height() && !drawn; y += 4) {
            for (int x = 0; x < image.width() && !drawn; x += 4) drawn = image.pixel(x, y) != background;
        }
        QVERIFY(drawn);
        if (const QString snapshots = qEnvironmentVariable("KIEMS_GUI_SNAPSHOTS"); !snapshots.isEmpty()) {
            image.save(QDir(snapshots).filePath("geometry.png"));
            view.grab().save(QDir(snapshots).filePath("geometry_page.png"));
        }
    }

    void powerAnalysis() {
        // A 10 x 1 mm +5V strip on F.Cu: U1's power output pin at one end, J1's power input at the other.
        auto board = std::make_shared<kiems_gui::BoardData>();
        using Kind = libkicad::StackupLayerKind;
        board->stackup = {{Kind::Copper, "F.Cu", 0.035, 0, 0}, {Kind::Core, "Dielectric 1", 1.51, 4.5, 0.02},
                          {Kind::Copper, "B.Cu", 0.035, 0, 0}};
        board->copperLayers = {"F.Cu", "B.Cu"};
        board->nets = {"GND", "+5V", "SIG"};
        const auto rect = [](double x0, double y0, double x1, double y1) {
            return libkicad::PolygonLoop{false, {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}};
        };
        board->geometry.outline = {rect(-1, -1, 11, 2)};
        // And its return: a matching GND strip from J1's ground pin back to U1's.
        board->geometry.copper = {{"+5V", "F.Cu", false, "", "", rect(0, 0, 10, 1)},
                                  {"+5V", "F.Cu", false, "U1", "1", rect(0, 0, 0.15, 1)},
                                  {"+5V", "F.Cu", false, "J1", "1", rect(9.85, 0, 10, 1)},
                                  {"GND", "F.Cu", false, "", "", rect(0, 2, 10, 3)},
                                  {"GND", "F.Cu", false, "U1", "2", rect(0, 2, 0.15, 3)},
                                  {"GND", "F.Cu", false, "J1", "2", rect(9.85, 2, 10, 3)}};
        board->pads = {{"U1", "1", "+5V", "power_out", 0.075, 0.5, 0, "F.Cu", 0.15, 1},
                       {"J1", "1", "+5V", "power_in", 9.925, 0.5, 0, "F.Cu", 0.15, 1},
                       {"U1", "2", "GND", "power_in", 0.075, 2.5, 0, "F.Cu", 0.15, 1},
                       {"J1", "2", "GND", "passive", 9.925, 2.5, 0, "F.Cu", 0.15, 1}};
        QCOMPARE(kiems_gui::powerNetCandidates(*board).first(), QString("GND"));
        QVERIFY(kiems_gui::powerNetCandidates(*board).indexOf("SIG") > kiems_gui::powerNetCandidates(*board).indexOf("+5V"));

        Document document;
        QVERIFY(makeDocument(document));
        auto analysis = kiems_gui::newPowerAnalysis(*board, "+5V", 2.0);
        QCOMPARE(analysis.sources.size(), std::size_t(1));
        QCOMPARE(analysis.sources[0].value, 5.0); // from the net's name
        QCOMPARE(analysis.loads.size(), std::size_t(1));
        QCOMPARE(analysis.loads[0].value, 2.0);
        QCOMPARE(analysis.referenceNet, std::string("GND")); // returning on the board's ground
        QCOMPARE(kiems_gui::addPowerAnalysis(document, analysis), 0);
        QCOMPARE(kiems_gui::addPowerAnalysis(document, analysis), 1);
        QCOMPARE(kiems_gui::powerAnalysis(document, 1)->name, std::string("+5V 2"));
        kiems_gui::removePowerAnalysis(document, 1);
        QCOMPARE(kiems_gui::powerAnalysisCount(document), 1);
        QVERIFY2(document.validate().isEmpty(), qPrintable(document.validate())); // libkiems still reads it

        // Solved here, in-process: 2 A over 9.7 squares of 35 um copper.
        const double sheet = 1.72e-8 / 35e-6;
        const auto run = kiems_gui::runPowerAnalysis(*board, *kiems_gui::powerAnalysis(document, 0));
        QVERIFY2(run->ok(), qPrintable(run->error));
        QVERIFY(std::abs((5.0 - run->result.terminals[1].voltage) - 2.0 * 9.7 * sheet) < 2e-3 * 2.0 * 9.7 * sheet);
        QVERIFY(run->violations.empty());
        // The return drops as much again: J1 sees twice the rail's drop across it.
        QVERIFY(run->returnResult);
        const double across = run->result.terminals[1].voltage - kiems::dc::partVoltages(*run->returnResult).at("J1");
        QVERIFY2(std::abs((5.0 - across) - 2 * 2.0 * 9.7 * sheet) < 3e-3 * 2 * 2.0 * 9.7 * sheet,
                 qPrintable(QString::number(5.0 - across)));

        // Limits it breaks: the current density, and the load's minimum voltage.
        analysis = *kiems_gui::powerAnalysis(document, 0);
        analysis.limits.outerCurrentDensity = 10.0;
        analysis.loads[0].minimumVoltage = 4.999;
        kiems_gui::setPowerAnalysis(document, 0, analysis);
        const auto limited = kiems_gui::runPowerAnalysis(*board, *kiems_gui::powerAnalysis(document, 0));
        // The voltage across J1, the rail's density and the return's.
        QCOMPARE(limited->violations.size(), std::size_t(3));
        QVERIFY(limited->violations[0].kind == kiems::dc::Violation::Kind::Voltage);
        QVERIFY(limited->violations[1].kind == kiems::dc::Violation::Kind::CurrentDensity);
        QVERIFY(limited->violations[2].onReturn);
        QCOMPARE(document.documentValue("/dc_analyses")[0]["max_current_density_outer"].get<double>(), 10.0);

        // The page runs it in the background and lists what it found.
        QWidget host;
        kiems_gui::PowerView view(&document, &host);
        view.resize(1300, 850);
        view.setBoard(board);
        view.showAnalysis(0);
        QVERIFY(view.canRun());
        // Picking pins on the map: J1 pin 1 is already a load; U1 pin 1 a source.
        const auto config = [&] { return *kiems_gui::powerAnalysis(document, 0); };
        view.setPinRole("J1", "1", kiems_gui::PowerCanvas::Role::Load); // no change
        QCOMPARE(config().loads.size(), std::size_t(1));
        QCOMPARE(config().loads[0].value, 2.0);
        view.setPinRole("J1", "1", kiems_gui::PowerCanvas::Role::None);
        QVERIFY(config().loads.empty());
        view.setPinRole("J1", "1", kiems_gui::PowerCanvas::Role::Load);
        QCOMPARE(config().loads[0].value, 0.1); // a new load's default
        auto restored = config();
        restored.loads[0].value = 2.0;
        restored.loads[0].minimumVoltage = 4.999;
        kiems_gui::setPowerAnalysis(document, 0, restored);
        view.setPinRole("J1", "1", kiems_gui::PowerCanvas::Role::Source); // from load to source
        QCOMPARE(config().sources.size(), std::size_t(2));
        QCOMPARE(config().sources[1].value, 5.0);
        QVERIFY(config().loads.empty());
        // Sensing at J1: it's held at 5 V across its part, the source rising to make it so.
        kiems_gui::setPowerAnalysis(document, 0, restored);
        view.setPinRole("J1", "1", kiems_gui::PowerCanvas::Role::Sense);
        QVERIFY(config().sense && config().sense->footprint == "J1" && config().sense->value == 5.0);
        const auto sensed = kiems_gui::runPowerAnalysis(*board, config());
        QVERIFY2(sensed->ok(), qPrintable(sensed->error));
        QVERIFY(sensed->senseShift);
        const double sensedAcross = sensed->result.terminals[1].voltage - kiems::dc::partVoltages(*sensed->returnResult).at("J1");
        QVERIFY(std::abs(sensedAcross - 5.0) < 1e-9);
        QVERIFY(std::abs(sensed->result.terminals[0].voltage - (5.0 + 2 * 2.0 * 9.7 * sheet)) < 3e-3 * 2 * 2.0 * 9.7 * sheet);
        kiems_gui::setPowerAnalysis(document, 0, restored);
        if (const QString snapshots = qEnvironmentVariable("KIEMS_GUI_SNAPSHOTS"); !snapshots.isEmpty()) {
            view.grab().save(QDir(snapshots).filePath("power_preview.png")); // the board's copper, before a run
        }
        QSignalSpy finished(&view, &kiems_gui::PowerView::runFinished);
        view.run();
        QVERIFY(finished.wait(20000));
        QVERIFY(finished.first().at(1).toBool());
        QVERIFY(view.currentRun() && view.currentRun()->ok());
        QCOMPARE(view.findChild<QTreeWidget*>("pins")->topLevelItemCount(), 4); // with the return's two
        QCOMPARE(view.findChild<QListWidget*>("problems")->count(), 3); // the three limits broken
        auto* meshes = view.findChild<QTreeWidget*>("study");
        QCOMPARE(meshes->topLevelItemCount(), 2); // the rail's mesh and the return's, unrefined
        QCOMPARE(meshes->topLevelItem(0)->childCount(), 0);
        if (const QString snapshots = qEnvironmentVariable("KIEMS_GUI_SNAPSHOTS"); !snapshots.isEmpty()) {
            view.setAttribute(Qt::WA_DontShowOnScreen);
            view.show();
            view.grab().save(QDir(snapshots).filePath("power.png"));
        }

        // A refinement study: kept in the document, and each mesh listed with the extrapolation.
        auto studied = config();
        studied.options.refinementStudy = true;
        studied.options.studyTolerance = 0.001;
        studied.options.studyMaxRefinements = 2;
        kiems_gui::setPowerAnalysis(document, 0, studied);
        QCOMPARE(document.documentValue("/dc_analyses")[0]["refinement_study"]["max_refinements"].get<int>(), 2);
        QVERIFY(config().options == studied.options);
        view.run();
        QVERIFY(finished.wait(60000));
        QVERIFY(view.currentRun()->result.study && view.currentRun()->returnResult->study);
        const auto& levels = view.currentRun()->result.study->levels;
        QVERIFY(levels.size() >= 2 && levels.size() <= 3);
        QCOMPARE(levels[1].elements, 4 * levels[0].elements);
        QCOMPARE(meshes->topLevelItemCount(), 2);
        // Each level, and the extrapolation once there are two.
        QCOMPARE(meshes->topLevelItem(0)->childCount(), static_cast<int>(levels.size()) + 1);
        if (const QString snapshots = qEnvironmentVariable("KIEMS_GUI_SNAPSHOTS"); !snapshots.isEmpty()) {
            view.findChild<QTabWidget*>("results")->setCurrentWidget(meshes);
            view.findChild<QCheckBox*>("mesh")->setChecked(true);
            view.grab().save(QDir(snapshots).filePath("power_study.png"));
            view.findChild<QCheckBox*>("mesh")->setChecked(false);
        }
        kiems_gui::setPowerAnalysis(document, 0, restored);

        // The report runs it again on the board, and fails it for the limits it breaks.
        QCOMPARE(kiems_gui::judgePowerAnalysis(nullptr).kind, kiems_gui::SimulationVerdict::Kind::NotRun);
        QVERIFY(kiems_gui::judgePowerAnalysis(limited.get()).kind == kiems_gui::SimulationVerdict::Kind::Fail);
        QVERIFY(kiems_gui::judgePowerAnalysis(run.get()).kind == kiems_gui::SimulationVerdict::Kind::Pass);
        const QString pdf = _directory.filePath("power_report.pdf");
        QString reportError;
        QVERIFY2(kiems_gui::writeReport(document, pdf, &reportError, board.get()), qPrintable(reportError));
        QVERIFY(QFileInfo(pdf).size() > 10000);
        if (const QString snapshots = qEnvironmentVariable("KIEMS_GUI_SNAPSHOTS"); !snapshots.isEmpty()) {
            QFile::remove(QDir(snapshots).filePath("power_report.pdf"));
            QFile::copy(pdf, QDir(snapshots).filePath("power_report.pdf"));
        }
    }

    void runEstimate() {
        using kiems_gui::RunEstimator;
        const auto near = [](double actual, double expected, double tolerance) {
            return std::abs(actual - expected) <= tolerance;
        };
        // One port, running to its step limit at 1625 steps/s. The in-process engine prints each
        // tick twice; the copy doesn't count as a sample.
        RunEstimator run;
        run.start(0);
        QCOMPARE(run.timing(1).remaining, -1.0); // nothing to go on yet
        run.feed("[INFO ] Running simulation", 1);
        run.feed("[INFO ] [USB] Simulating with excitation on port #1", 1);
        run.feed("[INFO ] [Copper] Setup: starting...", 2);
        run.feed("[INFO ] [Copper] FDTD run: step 0/100000 (0.0%) | energy decay 0.00/60.00dB (0.0%)", 5);
        run.feed("Copper: [@     4.0s] timestep 6500/100000 -- GPU allocated: 0.127 GB", 9);
        run.feed("[INFO ] [Copper] FDTD run: step 6500/100000 (6.5%) | energy decay 0.00/60.00dB (0.0%)", 9);
        run.feed("[INFO ] [Copper] FDTD run: step 13000/100000 (13.0%) | energy decay 1.00/60.00dB (1.7%)", 13);
        auto timing = run.timing(13);
        QCOMPARE(timing.port, 1);
        QCOMPARE(timing.ports, 1);
        QVERIFY2(near(timing.remaining, 87000.0 / 1625, 0.01), qPrintable(QString::number(timing.remaining)));
        QVERIFY(!timing.decayLimited);
        QVERIFY(near(run.timing(15).remaining, 87000.0 / 1625 - 2, 0.01)); // counting down between lines
        QVERIFY(near(run.timing(13).simulatingFraction, 12.0 / (12.0 + 87000.0 / 1625), 1e-6));
        // Then the energy starts falling 10 dB every 6500 steps. Three samples in the latter half of
        // the run aren't enough to go on; with a fourth, at 40 dB, it'll reach 60 in 13000 more.
        run.feed("[INFO ] [Copper] FDTD run: step 19500/100000 (19.5%) | energy decay 10.00/60.00dB (16.7%)", 17);
        run.feed("[INFO ] [Copper] FDTD run: step 26000/100000 (26.0%) | energy decay 20.00/60.00dB (33.3%)", 21);
        run.feed("[INFO ] [Copper] FDTD run: step 32500/100000 (32.5%) | energy decay 30.00/60.00dB (50.0%)", 25);
        QVERIFY(!run.timing(25).decayLimited);
        run.feed("[INFO ] [Copper] FDTD run: step 39000/100000 (39.0%) | energy decay 40.00/60.00dB (66.7%)", 29);
        timing = run.timing(29);
        QVERIFY(timing.decayLimited);
        QVERIFY2(near(timing.remaining, 13000.0 / 1625, 0.01), qPrintable(QString::number(timing.remaining)));
        run.feed("[INFO ] Processing all data from simulation. Calculating S-parameters and impedance", 40);
        QCOMPARE(run.timing(41).remaining, 0.0);

        // Two ports through the CPU worker, whose line carries the decay but no setup line: the
        // second port is found by its steps starting again, and expected to take as long as the first.
        RunEstimator worker;
        worker.start(0);
        worker.feed("[INFO ] Running simulation", 0);
        worker.feed("[INFO ] [LED] Simulating with excitation on port #0", 0);
        worker.feed("[INFO ] [LED] Simulating with excitation on port #1", 0);
        worker.feed("Copper: [@     4.0s] timestep 4000/20000 || Speed: 0.0010 s/step || Energy: ~1e-03 (-0.50dB)", 4);
        worker.feed("Copper: [@     8.0s] timestep 8000/20000 || Speed: 0.0010 s/step || Energy: ~1e-03 (-0.60dB)", 8);
        timing = worker.timing(8);
        QCOMPARE(timing.ports, 2);
        QCOMPARE(timing.port, 1);
        // 12000 steps of this port, then 20000 of the next plus the 4 s this one took to start.
        QVERIFY2(near(timing.remaining, 12.0 + 20.0 + 4.0, 0.01), qPrintable(QString::number(timing.remaining)));
        worker.feed("Copper: [@    20.0s] timestep 20000/20000 || Speed: 0.0010 s/step || Energy: ~1e-03 (-0.70dB)", 20);
        worker.feed("Copper: [@     4.0s] timestep 4000/20000 || Speed: 0.0010 s/step || Energy: ~1e-03 (-0.50dB)", 26);
        worker.feed("Copper: [@     8.0s] timestep 8000/20000 || Speed: 0.0010 s/step || Energy: ~1e-03 (-0.60dB)", 30);
        timing = worker.timing(30);
        QCOMPARE(timing.port, 2);
        QVERIFY2(near(timing.remaining, 12.0, 0.01), qPrintable(QString::number(timing.remaining)));

        // Before any steps, the last run of the same simulation is the estimate.
        RunEstimator again;
        again.start(100, 60);
        QVERIFY(near(again.timing(110).remaining, 50, 1e-9));
        QCOMPARE(again.timing(200).remaining, 0.0);

        QCOMPARE(kiems_gui::formatDuration(42.4), QString("42 s"));
        QCOMPARE(kiems_gui::formatDuration(187), QString("3:07"));
        QCOMPARE(kiems_gui::formatDuration(3727), QString("1:02:07"));
    }

    void traceAnalysis() {
        // A straight 10 mm, 45 ohm trace from U1 pin 1 to J1 pin 1, driven at U1 and matched at J1:
        // a run's files written from ideal waveforms -- the incident pulse at the driver, delayed
        // copies along the trace at 150 mm/ns, and one probe over no plane that reads nothing.
        auto board = std::make_shared<kiems_gui::BoardData>();
        board->copperLayers = {"F.Cu", "B.Cu"};
        board->pads = {{"U1", "1", "SIG", "output", 0, 0, 0, "F.Cu", 0.3, 0.3},
                       {"J1", "1", "SIG", "input", 10, 0, 0, "F.Cu", 0.3, 0.3}};
        libkicad::TrackSegment segment;
        segment.endXMm = 10;
        segment.widthMm = 0.2;
        segment.copperLayerName = "F.Cu";
        segment.uuid = "track";
        board->tracks = {{"SIG", segment}};

        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("0"));
        const QDir run(dir.filePath("0"));
        const auto write = [&](const QString& name, const QString& text) {
            QFile file(run.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            file.write(text.toUtf8());
        };
        write("ports.txt", "# ports\n0\tU1 pin 1 (SIG)\tnet=SIG\texcited\tZ=45\tCload=0\tat=(0, 0)\tlayer=0->1\n"
                           "1\tJ1 pin 1 (SIG)\tnet=SIG\tabsorbing\tZ=45\tCload=0\tat=(10, 0)\tlayer=0->1\n");
        const double fc = 3e9, t0 = 9 / (2 * std::numbers::pi * fc), velocity = 150e9; // mm/s
        const auto pulse = [&](double t) {
            return std::cos(2 * std::numbers::pi * 3e9 * (t - t0)) * std::exp(-std::pow(2 * std::numbers::pi * fc * t / 3 - 3, 2));
        };
        const auto waveform = [&](double delay, double scale, int every, double offset) {
            QString text = "% probe\n";
            for (int n = 0; n < 4000; n += every) {
                const double t = (n + offset) * 1e-12;
                text += QString("%1\t%2\n").arg(t, 0, 'g', 12).arg(scale * pulse(t - delay), 0, 'g', 12);
            }
            return text;
        };
        write("port_ut_0", waveform(0, 1, 1, 0));
        write("port_it_0", waveform(0, 1.0 / 45, 1, 0.5)); // matched: nothing comes back
        write("port_ut_1", waveform(10 / velocity, 1, 1, 0));
        QString probes = "# probes\n";
        for (int k = 0; k < 9; ++k) {
            probes += QString("%1\tSIG\t%2\t0\t0->1\t0.2\ttrack\n").arg(k).arg(k + 1);
            write(QString("trace_ut_%1").arg(k), waveform((k + 1) / velocity, 1, 8, 0));
        }
        probes += "9\tSIG\t5.5\t0\t0->1\t0.2\ttrack\n";
        write("trace_ut_9", waveform(0, 0, 8, 0));
        write("trace_probes.txt", probes);

        // A sweep whose pulse (fc = 3 GHz) is the one these waveforms carry: 0.95 ns, inside the 4 ns run.
        const double pulseSeconds = 2 * 9 / (2 * std::numbers::pi * 3e9);
        const auto a = kiems_gui::analyseTrace(*board, dir.path(), 0, 6e9, 0, pulseSeconds);
        QVERIFY2(a->ok(), qPrintable(a->error));
        QCOMPARE(a->driver.name, QString("U1 pin 1 (SIG)"));
        QCOMPARE(a->targetImpedance, 45.0);
        QVERIFY(std::abs(a->netLength - 10) < 1e-9);
        QVERIFY(a->delayMeasured);
        QVERIFY2(std::abs(a->velocity / velocity - 1) < 0.03, qPrintable(QString::number(a->velocity)));
        for (double d : {2.0, 5.0, 8.0}) QVERIFY2(std::abs(a->impedanceAt(d) - 45) < 1, qPrintable(QString::number(a->impedanceAt(d))));
        QCOMPARE(a->unreferencedProbes, 1); // the one reading nothing
        for (const auto& probe : a->probes) {
            if (!probe.referenced) continue;
            QVERIFY(std::abs(probe.ringing.finalValue - 1) < 0.02);
            QVERIFY(probe.ringing.deviation < 0.02);
            // Each arrives when the trace's delay says it should.
            QVERIFY2(std::abs(probe.ringing.arrival - probe.distanceMm / velocity) < 5e-12,
                     qPrintable(QString::number(probe.ringing.arrival)));
        }
        QCOMPARE(a->portSteps.size(), std::size_t(1));

        // The page shows it, and finds nothing wrong with it.
        auto boardData = std::shared_ptr<const kiems_gui::BoardData>(board);
        kiems_gui::TraceView view;
        view.resize(1200, 800);
        QSignalSpy analysed(&view, &kiems_gui::TraceView::analysed);
        view.setBoard(boardData);
        view.showSimulation("Line", dir.path(), 6e9, pulseSeconds);
        QVERIFY(analysed.wait(30000));
        QVERIFY(analysed.first().at(0).toBool());
        auto* findings = view.findChild<QTreeWidget*>("findings");
        bool matched = false, blurred = false;
        for (int i = 0; i < findings->topLevelItemCount(); ++i) {
            matched = matched || findings->topLevelItem(i)->text(0).startsWith("Within 10 %");
            blurred = blurred || findings->topLevelItem(i)->text(0).contains("blurs the impedance");
        }
        QVERIFY(matched);
        QVERIFY(blurred); // a 0.2 ns edge spans more than this 10 mm line, there and back
        // Nothing about the run being short: it outlasts its pulse, and the profile reaches the end.
        QVERIFY(a->recordSeconds > a->pulseSeconds);
        QVERIFY(a->profileReachMm >= a->netLength);
        if (const QString snapshots = qEnvironmentVariable("KIEMS_GUI_SNAPSHOTS"); !snapshots.isEmpty()) {
            view.setAttribute(Qt::WA_DontShowOnScreen);
            view.show();
            view.grab().save(QDir(snapshots).filePath("trace.png"));
        }
    }

    void signalSetup() {
        // A USB channel: U1 drives /D+ and /D-, which run through 27 ohm resistors R1 and R2 to
        // /_D+ and /_D- at connector J1. R9 terminates across the pair and C5 decouples a line to
        // GND: neither is in series with the signal.
        auto board = std::make_shared<kiems_gui::BoardData>();
        board->copperLayers = {"F.Cu", "B.Cu"};
        board->nets = {"GND", "+3V3", "/D+", "/D-", "/_D+", "/_D-", "/OTHER"};
        using Pin = libkicad::FootprintPin;
        board->footprints = {
            {"U1", "CH340", {Pin{"1", "", "/D+", "bidirectional"}, Pin{"2", "", "/D-", "bidirectional"}, Pin{"3", "", "GND", "power_in"}}},
            {"R1", "27", {Pin{"1", "", "/D+", "passive"}, Pin{"2", "", "/_D+", "passive"}}},
            {"R2", "27", {Pin{"1", "", "/D-", "passive"}, Pin{"2", "", "/_D-", "passive"}}},
            {"R9", "90", {Pin{"1", "", "/D+", "passive"}, Pin{"2", "", "/D-", "passive"}}},
            {"C5", "100n", {Pin{"1", "", "/OTHER", "passive"}, Pin{"2", "", "GND", "passive"}}},
            {"J1", "USB_C", {Pin{"A6", "", "/_D+", "passive"}, Pin{"A7", "", "/_D-", "passive"},
                             Pin{"B6", "", "/_D+", "passive"}, Pin{"B7", "", "/_D-", "passive"}}}};
        for (const auto& footprint : board->footprints) {
            for (const auto& pin : footprint.pins) {
                board->pads.push_back({footprint.reference, pin.number, pin.netName, pin.pinType, 0, 0, 0, "F.Cu", 0.3, 0.3});
            }
        }
        Document document;
        QVERIFY(makeDocument(document));
        document.setDocumentValue("/frequency/stop", 350e6);

        const auto nets = kiems_gui::signalNets(*board);
        QVERIFY(nets.contains("/D+") && !nets.contains("GND") && !nets.contains("+3V3"));
        const kiems_gui::SignalPlan plan = kiems_gui::suggestSignal(*board, document, "/D+", true);
        QVERIFY(plan.differential);
        QCOMPARE(plan.nets, QStringList({"/D+", "/_D+"}));
        QCOMPARE(plan.partners, QStringList({"/D-", "/_D-"}));
        QCOMPARE(plan.seriesParts, QStringList({"R1", "R2", "R9"})); // R9 across the pair, but not C5 to ground
        QVERIFY(plan.driver && plan.driver->footprint == "U1" && plan.driver->pin == "1");
        QCOMPARE(plan.receivers.size(), 2); // J1 A6 and B6 (not R9's pin): the partner pins follow
        QCOMPARE(plan.standard, QString("usb_full_speed"));
        QCOMPARE(plan.impedance, 45.0);
        QVERIFY(plan.sweepStop && *plan.sweepStop == 3e9); // 350 MHz is too low for a trace
        const QString summary = kiems_gui::describeSignalPlan(plan);
        QVERIFY2(summary.contains("90 Ω differential") && summary.contains("R1, R2, R9") && summary.contains("U1 pin 1"),
                 qPrintable(summary));

        // Applied: the pair, its four nets at 45 ohm, the resistors, the driver in antiphase, the
        // receivers and their partners probed, the USB eye test and the wider sweep.
        const int index = document.addSimulation(document.uniqueSimulationName(plan.name), "GND");
        kiems_gui::applySignalPlan(document, board.get(), index, plan);
        const nlohmann::json sim = *document.simulationJson(index);
        QCOMPARE(sim["is_differential_pair"].get<bool>(), true);
        QCOMPARE(sim["involved_nets"].size(), std::size_t(4));
        for (const auto& net : sim["involved_nets"]) QCOMPARE(net["impedance"].get<double>(), 45.0);
        QCOMPARE(sim["included_components"].size(), std::size_t(3));
        QCOMPARE(sim["excitations"].size(), std::size_t(2));
        double amplitudes = 0;
        for (const auto& excitation : sim["excitations"]) {
            QCOMPARE(QString::fromStdString(excitation["footprint"].get<std::string>()), QString("U1"));
            amplitudes += excitation["amplitude"].get<double>();
        }
        QCOMPARE(amplitudes, 0.0); // +1 and -1
        int probed = 0;
        for (const auto& net : sim["involved_nets"]) {
            for (const auto& pin : net.value("probed_pins", nlohmann::json::array())) {
                if (pin["footprint"] == "J1") ++probed;
            }
        }
        QCOMPARE(probed, 4); // A6, B6 and their partners A7, B7
        QCOMPARE(sim["eye_test"]["standard"].get<std::string>(), std::string("usb_full_speed"));
        QCOMPARE(sim["eye_bit_rate"].get<double>(), 12e6);
        QCOMPARE(document.documentValue("/frequency/stop").get<double>(), 3e9);
        QVERIFY2(document.validate().isEmpty(), qPrintable(document.validate()));

        // The dialog shows the same plan, and a single trace has no partner or antiphase.
        kiems_gui::SignalWizard wizard(document, board);
        QCOMPARE(wizard.plan().nets.isEmpty(), false);
        const kiems_gui::SignalPlan single = kiems_gui::suggestSignal(*board, document, "/OTHER", false);
        QVERIFY(!single.differential && single.partners.isEmpty() && single.seriesParts.isEmpty());
        QVERIFY(!single.sweepStop); // the sweep is 3 GHz now
        if (const QString snapshots = qEnvironmentVariable("KIEMS_GUI_SNAPSHOTS"); !snapshots.isEmpty()) {
            document.setDocumentValue("/frequency/stop", 350e6);
            kiems_gui::SignalWizard shown(document, board);
            shown.setAttribute(Qt::WA_DontShowOnScreen);
            shown.show();
            shown.findChild<QComboBox*>()->setCurrentText("/D+");
            QApplication::processEvents();
            shown.grab().save(QDir(snapshots).filePath("signal_wizard.png"));
        }

        // As the remote board has it: the MCU's pins are on nets named for the pins (no D+/D-), behind
        // R16 and R17; the charger U2 and ESD array U7 sit on /D+ and /D-; the connector is on /_D+
        // and /_D-, which only U7 joins to the rest.
        auto remote = std::make_shared<kiems_gui::BoardData>();
        remote->copperLayers = {"F.Cu", "B.Cu"};
        remote->nets = {"GND", "Net-(U1-IO27)", "Net-(U1-IO26)", "/D+", "/D-", "/_D+", "/_D-"};
        remote->footprints = {
            {"U1", "ESP32-H2", {Pin{"27", "", "Net-(U1-IO27)", "bidirectional"}, Pin{"26", "", "Net-(U1-IO26)", "bidirectional"}}},
            {"R16", "22", {Pin{"1", "", "Net-(U1-IO27)", "passive"}, Pin{"2", "", "/D+", "passive"}}},
            {"R17", "22", {Pin{"1", "", "Net-(U1-IO26)", "passive"}, Pin{"2", "", "/D-", "passive"}}},
            {"U2", "BQ25640", {Pin{"21", "", "/D+", "unspecified"}, Pin{"20", "", "/D-", "unspecified"}}},
            {"U7", "TPD4EUSB30", {Pin{"7", "", "/D+", "free"}, Pin{"9", "", "/D-", "free"}, Pin{"4", "", "/_D+", "passive"},
                                  Pin{"2", "", "/_D-", "passive"}}},
            {"J1", "USB_C", {Pin{"A6", "", "/_D+", "bidirectional"}, Pin{"A7", "", "/_D-", "bidirectional"}}}};
        double y = 0;
        for (const auto& footprint : remote->footprints) {
            for (const auto& pin : footprint.pins) {
                remote->pads.push_back({footprint.reference, pin.number, pin.netName, pin.pinType, 0, y, 0, "F.Cu", 0.3, 0.3});
                y += 1;
            }
        }
        Document remoteDocument;
        QVERIFY(makeDocument(remoteDocument));
        const kiems_gui::SignalPlan usb = kiems_gui::suggestSignal(*remote, remoteDocument, "/D+", true);
        QCOMPARE(usb.nets, QStringList({"/D+", "Net-(U1-IO27)"}));
        QCOMPARE(usb.partners, QStringList({"/D-", "Net-(U1-IO26)"})); // followed past R17 on its own
        QCOMPARE(usb.seriesParts, QStringList({"R16", "R17"}));
        QVERIFY(usb.driver && usb.driver->footprint == "U1" && usb.driver->pin == "27");
        QCOMPARE(usb.receivers.size(), 2); // U2 pin 21 and U7 pin 7
        QCOMPARE(usb.beyondChips, QStringList({"/_D+", "/_D-"}));
        QCOMPARE(usb.throughChips, QStringList({"U7"}));
        QVERIFY(kiems_gui::describeSignalPlan(usb).contains("Stops at U7"));
        const int usbIndex = remoteDocument.addSimulation("USB", "GND");
        kiems_gui::applySignalPlan(remoteDocument, remote.get(), usbIndex, usb);
        const nlohmann::json usbSim = *remoteDocument.simulationJson(usbIndex);
        // Both legs driven in antiphase, though no name pairs the MCU's nets.
        std::map<std::string, double> drive;
        for (const auto& excitation : usbSim["excitations"]) drive[excitation["pin"].get<std::string>()] = excitation["amplitude"].get<double>();
        QCOMPARE(drive.size(), std::size_t(2));
        QCOMPARE(drive["27"], 1.0);
        QCOMPARE(drive["26"], -1.0);
        std::set<std::string> probedPins;
        for (const auto& net : usbSim["involved_nets"]) {
            for (const auto& pin : net.value("probed_pins", nlohmann::json::array())) {
                probedPins.insert(pin["footprint"].get<std::string>() + " " + pin["pin"].get<std::string>());
            }
        }
        for (const char* expected : {"U2 21", "U2 20", "U7 7", "U7 9"}) QVERIFY2(probedPins.contains(expected), expected);
        QVERIFY2(remoteDocument.validate().isEmpty(), qPrintable(remoteDocument.validate()));
    }

    void jobQueue() {
        using kiems_gui::Job;
        kiems_gui::JobQueue queue;
        QStringList started;
        int stops = 0;
        QStringList refuse; // simulations whose start hook fails
        queue.setHooks({[&](const Job& job, QString* error) {
                            started << job.simulation + (job.storeFields ? "+fields" : "");
                            if (refuse.contains(job.simulation)) {
                                *error = "refused";
                                return false;
                            }
                            return true;
                        },
                        [&] { ++stops; }});
        QSignalSpy drained(&queue, &kiems_gui::JobQueue::drained);
        QSignalSpy finished(&queue, &kiems_gui::JobQueue::jobFinished);

        // The first starts at once; the rest wait, and a repeat isn't queued twice -- but asking
        // for fields upgrades a waiting job.
        QVERIFY(queue.enqueue("A", false));
        QVERIFY(queue.enqueue("B", false));
        QVERIFY(queue.enqueue("C", false));
        QVERIFY(!queue.enqueue("B", true));
        QVERIFY(!queue.enqueue("A", false));
        QVERIFY(started == QStringList{"A"});
        QVERIFY(queue.running() && queue.running()->simulation == "A");
        QCOMPARE(queue.queuedCount(), 2);
        QVERIFY(queue.contains("B") && queue.isBusy());

        // Progress lands on the running job; a rename follows a queued job, not the running one.
        queue.reportProgress("FDTD", 0.25);
        QCOMPARE(queue.running()->stage, QString("FDTD"));
        QCOMPARE(queue.running()->fraction, 0.25);
        queue.renameSimulation("C", "C2");
        queue.renameSimulation("A", "A2");
        QVERIFY(queue.contains("C2") && !queue.contains("C") && queue.running()->simulation == "A");

        // A finishes; B (now with fields) starts. B fails; C2 starts.
        queue.reportFinished(true, QString());
        QVERIFY(started == (QStringList{"A", "B+fields"}));
        queue.reportFinished(false, "kiems exited with status 1.");
        QVERIFY(started == (QStringList{"A", "B+fields", "C2"}));
        QCOMPARE(finished.count(), 2);
        QVERIFY(finished.at(1).at(0).value<Job>().status == Job::Status::Failed);

        // Cancelling the running job asks the runner to stop; its end is reported as stopped, and
        // the queue drains.
        const int c2 = queue.running()->id;
        queue.cancel(c2);
        QCOMPARE(stops, 1);
        QCOMPARE(drained.count(), 0);
        queue.reportFinished(false, "Stopped.");
        QCOMPARE(drained.count(), 1);
        QVERIFY(!queue.isBusy());
        const QList<Job>& jobs = queue.jobs();
        QCOMPARE(jobs.size(), 3);
        QVERIFY(jobs[0].status == Job::Status::Succeeded && jobs[0].fraction == 1);
        QVERIFY(jobs[1].status == Job::Status::Failed && jobs[1].message == "kiems exited with status 1.");
        QVERIFY(jobs[2].status == Job::Status::Stopped);

        // A job whose start fails is marked failed, and the next one starts in its place.
        refuse = {"D"};
        QVERIFY(queue.enqueue("D", false));
        QVERIFY(queue.jobs().back().status == Job::Status::Failed);
        QCOMPARE(queue.jobs().back().message, QString("refused"));
        QCOMPARE(drained.count(), 2);
        QVERIFY(queue.enqueue("E", false));
        QVERIFY(queue.enqueue("F", false));
        QVERIFY(queue.running() && queue.running()->simulation == "E");

        // Cancelling a waiting job removes it; stopping everything empties the queue too.
        const int f = queue.jobs().back().id;
        queue.cancel(f);
        QVERIFY(!queue.contains("F"));
        QVERIFY(queue.enqueue("G", false));
        queue.stopAll();
        QCOMPARE(stops, 2);
        QCOMPARE(queue.queuedCount(), 0);
        queue.reportFinished(false, "Stopped.");
        QVERIFY(!queue.isBusy());

        // A hook that finishes its run before returning still lets the queue move on.
        refuse.clear();
        queue.setHooks({[&](const Job& job, QString*) {
                            started << job.simulation;
                            if (job.simulation == "H") queue.reportFinished(true, QString());
                            return true;
                        },
                        [] {}});
        queue.enqueue("H", false);
        QVERIFY(queue.enqueue("I", false));
        QVERIFY(queue.running() && queue.running()->simulation == "I");
        queue.reportFinished(true, QString());

        queue.clearFinished();
        QVERIFY(queue.jobs().isEmpty());
    }

    // Probe<n>.csv as the pipeline writes it: excitations that never reached the probe, and Z for
    // a probe that measures none, are NaN and left out; the Results view gets a Probes tab.
    void probeResults() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString simulation = dir.filePath("simulation");
        const QString results = dir.filePath("results");
        QVERIFY(QDir().mkpath(simulation + "/0") && QDir().mkpath(results));
        auto write = [](const QString& path, const QByteArray& text) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            file.write(text);
        };
        write(simulation + "/0/ports.txt", "# header\n0\tU1 pin 5 (/LED3)\tnet=/LED3\texcited\n"
                                           "1\tR9 pin 2 (/LED3)\tnet=/LED3\tpassive-probe\n");
        write(simulation + "/Probe1.csv",
              "Frequency [MHz], re(V-0), im(V-0), re(I-0), im(I-0), re(Z-0), im(Z-0), re(T-0), im(T-0), "
              "re(V-1), im(V-1), re(I-1), im(I-1), re(Z-1), im(Z-1), re(T-1), im(T-1), \n"
              "1, 1e-15, 0, 0, 0, nan, nan, 0.6, 0.8, nan, nan, nan, nan, nan, nan, nan, nan\n"
              "10, 1e-15, 0, 0, 0, nan, nan, 0, 0.5, nan, nan, nan, nan, nan, nan, -nan, -nan\n");
        write(results + "/Port_0_data.csv", "Frequency [MHz], |S0-0|, Arg(S0-0)\n1, 0.5, 0\n10, 0.5, 0\n");

        const kiems_gui::SimulationResults loaded = kiems_gui::SimulationResults::load(results, simulation);
        QVERIFY(loaded.error.isEmpty());
        QCOMPARE(loaded.probes.size(), std::size_t(1));
        const kiems_gui::ProbeResults& probe = loaded.probes.front();
        QCOMPARE(probe.port, 1);
        QVERIFY(probe.frequencyHz == (std::vector<double>{1e6, 10e6}));
        QCOMPARE(probe.transfer.size(), std::size_t(1));
        QVERIFY(probe.transfer.contains(0));
        QVERIFY(std::abs(probe.transfer.at(0)[0] - std::complex<double>(0.6, 0.8)) < 1e-12);
        QVERIFY(std::abs(probe.transfer.at(0)[1] - std::complex<double>(0, 0.5)) < 1e-12);
        QVERIFY(probe.impedance.empty());

        kiems_gui::ResultsView view;
        view.showResults("LED3 probe", loaded);
        auto* tabs = view.findChild<QTabWidget*>();
        QVERIFY(tabs);
        int probesTab = -1;
        for (int i = 0; i < tabs->count(); ++i) {
            if (tabs->tabText(i) == "Probes") probesTab = i;
        }
        QVERIFY(probesTab >= 0);
        QVERIFY(!qobject_cast<QLabel*>(tabs->widget(probesTab))); // charts, not the "no probes" message
        if (const QString snapshots = qEnvironmentVariable("KIEMS_GUI_SNAPSHOTS"); !snapshots.isEmpty()) {
            view.resize(1000, 700);
            tabs->setCurrentIndex(probesTab);
            QVERIFY(view.grab().save(QDir(snapshots).filePath("probes.png")));
        }
    }

    // The report's verdicts follow the macOS app's rule, and the PDF comes out with every
    // simulation's pages: one with results (its eyes, one failing its mask; its charts; a probe),
    // one never run.
    void report() {
        using kiems_gui::SimulationVerdict;
        kiems_gui::EyeResults none;
        QVERIFY(kiems_gui::judgeSimulation(false, none).kind == SimulationVerdict::Kind::NotRun);
        QVERIFY(kiems_gui::judgeSimulation(true, none).kind == SimulationVerdict::Kind::Warning);
        kiems_gui::EyeResults unmasked;
        unmasked.eyes.push_back({});
        QVERIFY(kiems_gui::judgeSimulation(true, unmasked).kind == SimulationVerdict::Kind::Warning);
        kiems_gui::EyeResults passing = unmasked;
        passing.eyes[0].name = "D+";
        passing.eyes[0].mask = kiems_gui::EyeMaskVerdict{100, 0, 0, true};
        SimulationVerdict pass = kiems_gui::judgeSimulation(true, passing);
        QVERIFY(pass.kind == SimulationVerdict::Kind::Pass);
        QCOMPARE(pass.notes, QStringList{"D+ passes the eye mask."});
        // A noisy result that hasn't settled across draws is a warning, not a pass.
        kiems_gui::EyeResults unsettled = passing;
        unsettled.eyes[0].opening.heightV = 0.5;
        unsettled.eyes[0].opening.widthUI = 0.8;
        unsettled.eyes[0].noisyOpening = kiems_gui::EyeOpeningData{0.4, 0.7};
        unsettled.eyes[0].noiseLowest = kiems_gui::EyeOpeningData{0.3, 0.7};
        unsettled.eyes[0].noiseHighest = kiems_gui::EyeOpeningData{0.45, 0.7};
        QVERIFY(kiems_gui::judgeSimulation(true, unsettled).kind == SimulationVerdict::Kind::Warning);
        kiems_gui::EyeResults failing = passing;
        failing.eyes[0].noisyMask = kiems_gui::EyeMaskVerdict{100, 3, 0, false};
        SimulationVerdict fail = kiems_gui::judgeSimulation(true, failing);
        QVERIFY(fail.kind == SimulationVerdict::Kind::Fail);
        QCOMPARE(fail.notes, QStringList{"D+ leaves the eye mask (with adversarial signals)."});

        Document document;
        QVERIFY(makeDocument(document));
        document.addSimulation("Never run", "GND");
        const QString simulation = document.simulationDir(0);
        const QString results = document.resultsDir(0);
        QVERIFY(QDir().mkpath(simulation + "/0") && QDir().mkpath(results + "/excitations"));
        auto write = [](const QString& path, const QByteArray& text) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            file.write(text);
        };
        write(simulation + "/0/ports.txt", "0\tU1 pin 5 (SIG)\tnet=SIG\texcited\n1\tR9 pin 2 (SIG)\tnet=SIG\tpassive-probe\n");
        QByteArray port = "Frequency [MHz], |S0-0|, Arg(S0-0), |Z0|, Arg(Z0)\n";
        QByteArray probe = "Frequency [MHz], re(V-0), im(V-0), re(I-0), im(I-0), re(Z-0), im(Z-0), re(T-0), im(T-0)\n";
        for (int f = 1; f <= 100; ++f) {
            const double mhz = f * 60.0;
            port += QByteArray::number(mhz) + ", " + QByteArray::number(0.1 + f * 0.004) + ", " +
                    QByteArray::number(-f * 0.03) + ", " + QByteArray::number(50 + f * 0.2) + ", 0.1\n";
            probe += QByteArray::number(mhz) + ", 0, 0, 0, 0, nan, nan, " + QByteArray::number(0.9 - f * 0.003) + ", 0\n";
        }
        write(results + "/Port_0_data.csv", port);
        write(simulation + "/Probe1.csv", probe);
        QByteArray response = "time_s,voltage_v\n";
        for (int i = 0; i < 400; ++i) {
            response += QByteArray::number(i * 25e-12) + "," + QByteArray::number(i < 40 ? 0.0 : 0.4 * (1 - std::exp(-(i - 40) / 30.0))) + "\n";
        }
        write(results + "/excitations/U1 pin 5 (SIG)_response.csv", response);

        // Eyes, as kiems writes them: one fast channel that passes, one slow one that fails.
        std::vector<double> frequencies;
        for (int f = 1; f <= 6000; ++f) frequencies.push_back(f * 1e6);
        kiems::EyeTestConfig test;
        test.standard = kiems::EyeMaskStandard::UsbHighSpeed;
        test.signalLevel = 0.4;
        test.mask = {0.075, 0.375, 0.3, 0.525};
        std::vector<kiems::SimulationEye> eyes;
        for (const auto& [name, corner] : {std::pair{"Fast channel", 2e9}, std::pair{"Slow channel", 150e6}}) {
            std::vector<std::complex<double>> transfer;
            for (const double f : frequencies) {
                transfer.push_back(std::polar(1.0, -2 * std::numbers::pi * f * 0.7e-9) / std::complex<double>(1.0, f / corner));
            }
            auto eye = kiems::computeEyeDiagram(frequencies, transfer, 480e6, {}, {}, test.signalLevel, test.mask);
            QVERIFY(eye.has_value());
            eyes.push_back({name, false, 0, std::move(*eye)});
        }
        kiems::saveEyeDiagrams(eyes, test, std::filesystem::path(results.toStdWString()) / "eyes.json");

        QTemporaryDir out;
        const QString pdf = out.filePath("report.pdf");
        QString error;
        QVERIFY2(kiems_gui::writeReport(document, pdf, &error), qPrintable(error));
        QFile written(pdf);
        QVERIFY(written.open(QIODevice::ReadOnly));
        const QByteArray bytes = written.readAll();
        QVERIFY(bytes.startsWith("%PDF"));
        // A summary page, then Trace's pages (eyes and charts) and Never run's.
        const qsizetype pageCount = bytes.count("/Type /Page") - bytes.count("/Type /Pages");
        QVERIFY2(pageCount >= 4, qPrintable(QString::number(pageCount)));
        if (const QString snapshots = qEnvironmentVariable("KIEMS_GUI_SNAPSHOTS"); !snapshots.isEmpty()) {
            QFile::remove(QDir(snapshots).filePath("report.pdf"));
            QVERIFY(QFile::copy(pdf, QDir(snapshots).filePath("report.pdf")));
        }
    }
};

QTEST_MAIN(KiemsGuiTests)
#include "kiems_gui_tests.moc"
