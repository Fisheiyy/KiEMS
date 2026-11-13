// Matplot++ figures that gnuplot draws from a script file instead of a pipe.
//
// Matplot++'s own gnuplot backend streams every command, data included, into a long-lived gnuplot
// process's stdin. On Windows gnuplot reads piped input very slowly: an 800x600 image (a DC
// layer's voltage map) took 12 s through the pipe and under 1 s from a file. Each figure also kept
// its gnuplot process alive until the program exited. newPlotFigure()'s backend collects the same
// commands, writes them to a script beside the output when the figure is drawn, runs gnuplot on it
// to completion and deletes it -- so the plots are exactly what Matplot++ would have drawn.
#pragma once

#include <matplot/matplot.h>

namespace kiems {

/// A quiet (non-interactive) figure whose save() runs gnuplot on a script file.
matplot::figure_handle newPlotFigure();

} // namespace kiems
