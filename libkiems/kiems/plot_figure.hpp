// Matplot++ figures that gnuplot draws from a script file instead of a pipe.
//
// Matplot++'s own gnuplot backend streams every command, data included, into a long-lived gnuplot
// process's stdin. On Windows gnuplot reads piped input very slowly: an 800x600 image (a DC
// layer's voltage map) took 12 s through the pipe and under 1 s from a file. Each figure also kept
// its gnuplot process alive until the program exited. newPlotFigure()'s backend collects the same
// commands, writes them to a script beside the output when the figure is drawn, and starts gnuplot
// on it in the background (deleting the script once it succeeds) -- so the plots are exactly what
// Matplot++ would have drawn, drawn side by side.
#pragma once

#include <matplot/matplot.h>

namespace kiems {

/// A quiet (non-interactive) figure whose save() starts gnuplot on a script file. The image is only
/// complete once waitForPlots() returns.
matplot::figure_handle newPlotFigure();

/// Waits for every plot newPlotFigure() figures have started; false if any failed (each is logged).
/// The program's exit waits for them too.
bool waitForPlots();

} // namespace kiems
