// The eye-mask presets the Tests tab offers, grouped by family: the same standards, with the same
// numbers, as the macOS app's EyeTestPresets.swift (which documents where each one comes from and
// which are approximations). A simulation keeps the numbers themselves in its eye_test; `standard`
// only records which preset they came from.
//
// Masks sit on the eye's decision threshold across the unit interval: a keep-out hexagon through
// (x1, 0), (x2, +-y1), (1 - x2, +-y1), (1 - x1, 0), and an outer limit of +-y2 (0 for none).
#pragma once

#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace kiems_gui {

struct EyeTestPreset {
    const char* standard; ///< its eye_test "standard" value in simulation.json
    QString family;       ///< the submenu it's listed in
    QString menuTitle;    ///< its item in that submenu
    QString title;        ///< its name on its own
    std::optional<double> bitRate; ///< none for a standard spanning many speed grades (DRAM)
    double signalLevel;
    double x1, x2, y1, y2;
    bool startsGroup = false; ///< a separator comes before it in its submenu

    static const std::vector<EyeTestPreset>& all();
    static const QStringList& families();
    static const EyeTestPreset* find(const QString& standard);
    /// "Custom", or the preset's title.
    static QString titleFor(const QString& standard);
};

} // namespace kiems_gui
