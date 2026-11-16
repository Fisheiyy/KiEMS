## Windows (x64)

Download `KiEMS-<version>-windows-x64.zip`, unzip it anywhere and run `kiems-gui.exe`. Nothing needs installing, but KiEMS needs:

- **KiCad 10 or newer**, which it reads boards through. It finds the newest one under `C:\Program Files\KiCad`.
- **gnuplot**, for the plots the command-line tool writes (`winget install gnuplot.gnuplot`).
- **A Vulkan 1.2 graphics driver** for GPU simulation. Without one, simulations run on the CPU.

Windows 10 version 1903 or newer is required. `README.txt` in the zip has the details.
