// The app's small line icons, drawn rather than shipped: the toolbar's run buttons and the sidebar's
// rows, in the spirit of the SF Symbols the macOS app uses (play.fill, cube, chart.bar, waveform,
// slider.horizontal.3, square.stack.3d.up...). Each is drawn on a 16-unit square at twice the
// device pixel ratio asked for, so it stays crisp on high-DPI screens.
#pragma once

#include <QColor>
#include <QIcon>

namespace kiems_gui::icons {

enum class Glyph {
    Play,      // run
    PlayAll,   // run all
    Stop,
    Folder,
    Sliders,   // Board Setup
    Board,     // a simulation's board
    Cube,      // Geometry
    Chart,     // Simulation Results
    Waveform,  // Field Viewer
    Trace,     // Trace
    Stack,     // a Simulation Set
    Bolt,      // a power analysis
    Engine,    // the engine (a chip)
};

/// `glyph` in `color` at `size` logical pixels.
QIcon icon(Glyph glyph, const QColor& color, int size = 16);

} // namespace kiems_gui::icons
