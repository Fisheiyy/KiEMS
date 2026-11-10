#include "EyeTestPresets.hpp"

#include <QObject>

namespace kiems_gui {

namespace {

const QString kEnDash = QString(QChar(0x2013));

} // namespace

const QStringList& EyeTestPreset::families() {
    static const QStringList families = {"USB",         "DRAM",     "PCI Express", "FireWire",
                                         "Thunderbolt", "HDMI",     "DisplayPort", "Ethernet"};
    return families;
}

const std::vector<EyeTestPreset>& EyeTestPreset::all() {
    // Numbers from the macOS app's EyeTestPresets.swift, which cites each one's source.
    static const std::vector<EyeTestPreset> presets = {
        // USB
        {"usb_high_speed", "USB", "High Speed (480 Mb/s)", "USB High Speed", 480e6, 0.4, 0.075, 0.375, 0.3, 0.525},
        {"usb_full_speed", "USB", "Full Speed (12 Mb/s)", "USB Full Speed", 12e6, 1.65, 0.22, 0.22, 0.2, 0},
        {"usb_5gbps", "USB", "5 Gb/s", "USB 5 Gb/s", 5e9, 0.5, 0.33, 0.5, 0.05, 0.6},
        {"usb_10gbps", "USB", "10 Gb/s", "USB 10 Gb/s", 10e9, 0.5, 0.357, 0.5, 0.035, 0.6},
        {"usb_20gbps", "USB", "20 Gb/s", "USB 20 Gb/s", 10e9, 0.5, 0.357, 0.5, 0.035, 0.6},

        // DRAM: data receivers, then command/address, then LPDDR.
        {"ddr", "DRAM", "DDR", "DDR", std::nullopt, 0.625, 0.35, 0.35, 0.31, 0},
        {"ddr2", "DRAM", "DDR2", "DDR2", std::nullopt, 0.45, 0.35, 0.35, 0.2, 0},
        {"ddr3", "DRAM", "DDR3 Data", "DDR3 Data", std::nullopt, 0.375, 0.35, 0.35, 0.15, 0},
        {"ddr4", "DRAM", "DDR4 Data", "DDR4 Data", std::nullopt, 0.3, 0.4, 0.4, 0.068, 0},
        {"ddr5", "DRAM", "DDR5 Data", "DDR5 Data", std::nullopt, 0.275, 0.4, 0.4, 0.065, 0},
        {"ddr3_ca", "DRAM", "DDR3 Address/Command", "DDR3 Address/Command", std::nullopt, 0.375, 0.35, 0.35, 0.175, 0,
         true},
        {"ddr4_ca", "DRAM", "DDR4 Address/Command", "DDR4 Address/Command", std::nullopt, 0.3, 0.35, 0.35, 0.1, 0},
        {"ddr5_ca", "DRAM", "DDR5 Address/Command", "DDR5 Address/Command", std::nullopt, 0.275, 0.4, 0.4, 0.065, 0},
        {"lpddr4", "DRAM", "LPDDR4", "LPDDR4", std::nullopt, 0.275, 0.39, 0.39, 0.07, 0, true},
        {"lpddr4x", "DRAM", "LPDDR4X", "LPDDR4X", std::nullopt, 0.15, 0.375, 0.375, 0.07, 0},
        {"lpddr5", "DRAM", "LPDDR5", "LPDDR5", std::nullopt, 0.15, 0.325, 0.41, 0.035, 0},

        // PCI Express
        {"pcie_1", "PCI Express", "Gen 1 (2.5 GT/s)", "PCI Express Gen 1", 2.5e9, 0.5, 0.3, 0.5, 0.0875, 0.6},
        {"pcie_2", "PCI Express", "Gen 2 (5 GT/s)", "PCI Express Gen 2", 5e9, 0.5, 0.3, 0.5, 0.06, 0.6},
        {"pcie_3", "PCI Express", "Gen 3 (8 GT/s)", "PCI Express Gen 3", 8e9, 0.5, 0.35, 0.5, 0.0125, 0},
        {"pcie_4", "PCI Express", "Gen 4 (16 GT/s)", "PCI Express Gen 4", 16e9, 0.5, 0.35, 0.5, 0.0075, 0},
        {"pcie_5", "PCI Express", "Gen 5 (32 GT/s)", "PCI Express Gen 5", 32e9, 0.5, 0.35, 0.5, 0.0075, 0},

        // FireWire
        {"firewire_s400", "FireWire", "S400 (IEEE 1394a)", "FireWire S400", 393.216e6, 0.22, 0.3, 0.3, 0.118, 0},
        {"firewire_s800", "FireWire", "S800 (IEEE 1394b)", "FireWire S800", 983.04e6, 0.25, 0.35, 0.5, 0.05, 0},
        {"firewire_s1600", "FireWire", "S1600 (IEEE 1394b)", "FireWire S1600", 1966.08e6, 0.3, 0.35, 0.5, 0.05, 0},

        // Thunderbolt
        {"thunderbolt_1", "Thunderbolt", "Thunderbolt 1 (10.3125 Gb/s)", "Thunderbolt 1", 10.3125e9, 0.5, 0.357, 0.5,
         0.035, 0},
        {"thunderbolt_2", "Thunderbolt", "Thunderbolt 2 (10.3125 Gb/s)", "Thunderbolt 2", 10.3125e9, 0.5, 0.357, 0.5,
         0.035, 0},
        {"thunderbolt_3", "Thunderbolt", "Thunderbolt 3 (20.625 Gb/s)", "Thunderbolt 3", 20.625e9, 0.5, 0.35, 0.5,
         0.0245, 0},
        {"thunderbolt_4", "Thunderbolt", "Thunderbolt 4 (20 Gb/s)", "Thunderbolt 4", 20e9, 0.5, 0.35, 0.5, 0.0245, 0},

        // HDMI
        {"hdmi_1_0", "HDMI", "HDMI 1.0" + kEnDash + "1.2 (1.65 Gb/s)", "HDMI 1.0" + kEnDash + "1.2", 1.65e9, 0.5, 0.2,
         0.375, 0.075, 0},
        {"hdmi_1_4", "HDMI", "HDMI 1.3" + kEnDash + "1.4 (3.4 Gb/s)", "HDMI 1.3" + kEnDash + "1.4", 3.4e9, 0.5, 0.2,
         0.375, 0.075, 0},
        {"hdmi_2_0", "HDMI", "HDMI 2.0 (6 Gb/s)", "HDMI 2.0", 6e9, 0.5, 0.3, 0.375, 0.075, 0},

        // DisplayPort
        {"displayport_rbr", "DisplayPort", "RBR (1.62 Gb/s)", "DisplayPort RBR", 1.62e9, 0.3, 0.375, 0.375, 0.023, 0},
        {"displayport_hbr", "DisplayPort", "HBR (2.7 Gb/s)", "DisplayPort HBR", 2.7e9, 0.3, 0.2455, 0.375, 0.075, 0},
        {"displayport_hbr2", "DisplayPort", "HBR2 (5.4 Gb/s)", "DisplayPort HBR2", 5.4e9, 0.3, 0.31, 0.375, 0.035, 0},
        {"displayport_hbr3", "DisplayPort", "HBR3 (8.1 Gb/s)", "DisplayPort HBR3", 8.1e9, 0.3, 0.325, 0.375, 0.0375,
         0},

        // Ethernet
        {"ethernet_sgmii", "Ethernet", "SGMII / 1000BASE-X (1.25 Gb/s)", "Ethernet SGMII", 1.25e9, 0.35, 0.3, 0.5, 0.05,
         0.45},
        {"ethernet_xaui", "Ethernet", "XAUI (3.125 Gb/s)", "Ethernet XAUI", 3.125e9, 0.5, 0.275, 0.5, 0.1, 0.8},
        {"ethernet_sfi", "Ethernet", "SFI / SFP+ (10.3125 Gb/s)", "Ethernet SFI", 10.3125e9, 0.3, 0.12, 0.33, 0.095,
         0.35},
    };
    return presets;
}

const EyeTestPreset* EyeTestPreset::find(const QString& standard) {
    for (const EyeTestPreset& preset : all()) {
        if (standard == preset.standard) return &preset;
    }
    return nullptr;
}

QString EyeTestPreset::titleFor(const QString& standard) {
    const EyeTestPreset* preset = find(standard);
    return preset ? preset->title : QObject::tr("Custom");
}

} // namespace kiems_gui
