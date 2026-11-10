// Tests for the GUI's editing logic -- the unit-aware fields, the document's settings accessors and
// the Tests tab's presets -- driven through the widgets' own signals, without showing a window.
// Every document edited here is also run through libkiems' own parser (Document::validate()).
#include "Document.hpp"
#include "EyeTestPresets.hpp"
#include "QuantityEdit.hpp"
#include "SettingsPanel.hpp"
#include "SimulationEditor.hpp"
#include "TestsPanel.hpp"

#include "kiems/config.hpp"

#include <QAction>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

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
};

QTEST_MAIN(KiemsGuiTests)
#include "kiems_gui_tests.moc"
