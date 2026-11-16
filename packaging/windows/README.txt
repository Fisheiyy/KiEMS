KiEMS for Windows (x64)
=======================

KiEMS is an electromagnetic simulation tool for KiCad PCB designs.

This folder holds:

  kiems-gui.exe           the desktop app: start here
  kiems.exe               the command-line tool (the app runs it for each job)
  kiems_fdtd_worker.exe   CPU simulation workers the command-line tool starts
  copper_fdtd_worker.exe
  *.dll and folders       the libraries they need

Keep everything together in one folder; the programs look for each other and for
their libraries next to themselves. Nothing needs installing: unzip anywhere and
run kiems-gui.exe.

Before you start
----------------

1. KiCad 10 or newer (https://www.kicad.org/download/windows/). KiEMS reads your
   boards through KiCad itself and finds the newest version installed under
   C:\Program Files\KiCad. To use another installation, set LIBKICAD_KICAD_PYTHON
   to its python.exe and LIBKICAD_KICAD_CLI to its kicad-cli.exe.

2. gnuplot (http://www.gnuplot.info/), for the plots the command-line tool
   writes. KiEMS finds it on PATH or in gnuplot's standard install folder.
   Running "winget install gnuplot.gnuplot" installs it.

3. For GPU simulation, a graphics driver with Vulkan 1.2 or newer. Current NVIDIA,
   AMD and Intel drivers all include it. Without one, simulations run on the CPU
   (slower) and the log says so. "kiems --list-gpus" shows which GPUs KiEMS can use.

Windows 10 version 1903 or newer is required.

More
----

Documentation and source: https://github.com/beelsebob/KiEMS
