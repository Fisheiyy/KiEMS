# DC power analysis (IR drop and current density): research notes

Status: step 1 of the [order of work](#order-of-work) is built: the libkiems solver and
`kiems --dc` (see [What's built](#whats-built)). The research notes and the Python prototype in
[`Scripts/dc_prototype/`](../Scripts/dc_prototype/) follow.

The goal is a DC power-integrity check like Altium's *Power Analyzer by Keysight*. You name a
power rail, where its power comes from, and what draws from it. KiEMS then shows how far the
voltage sags at every load, where the copper is overloaded, and which vias carry too much
current.

## What's built

- **The GUI: `kiems-gui`'s Power page** ([`PowerView.hpp`](../kiems-gui/PowerView.hpp)).
  - **Setup.** Simulation › Add Power Analysis... picks a net and fills in its sources and loads
    from KiCad's pin types. The page edits the pins, the solver's options, and the limits:
    current density on outer and inner layers, current per via, and each load's minimum voltage.
  - **Results.** A run takes well under a second for a rail like StickHub's, in-process on a
    worker thread. It shows:
    - a voltage or current-density map per layer (log scale optional), with hover probes;
    - the pins' voltages, and the vias' currents, largest first;
    - the limits broken. Selecting one rings it on the map.

- **The solver: [`libkiems/kiems/dc_analysis.hpp`](../libkiems/kiems/dc_analysis.hpp).**
  - **Mesh.** Each layer's copper for the net is unioned, then meshed with CGAL `Mesh_2`
    (minimum angle about 20.7°, a maximum element size, 0.5 mm by default), with linear
    elements.
  - **Outline clean-up.** Outlines are snapped to a 1 µm grid before meshing. GEOS unions leave
    nanometre edges where pieces meet, and refinement can't split those; it spins forever.
  - **Pads and rings.** Pads and annular rings are seeded as mesh points, not as constrained
    outlines, because a pad's outline mostly runs along the copper's own and nearly coincident
    constraints have the same effect.
  - **Equipotential nodes.** A source's pad is one node held at its voltage. Each barrel's ring is
    one node per layer, joined by `ρL/(π(d+p)p)`.
  - **Load pads.** By default a load's pad is one node: the soldered lead ties it together.
    `"load_pads": "distributed"` draws the load evenly from the pad's copper instead.
  - **Solve.** The system is solved by conjugate gradients with an IC(0) preconditioner. This
    avoids an Eigen dependency, which the Xcode build has no include path for.
  - **Islands.** Copper with no source is found first and reported, not solved.
- **From a board: [`dc_board.hpp`](../libkiems/kiems/dc_board.hpp).**
  - Analyses live in the document's `dc_analyses` array.
  - `buildProblem()` takes the net's copper, plated holes and pads, and the stackup, from libkicad.
  - `automaticAnalysis()` starts from the pin types.
- **The CLI.** `kiems --dc [NAME...]` writes `ems/dc/<name>/result.json` plus voltage and
  current-density maps per layer.
- **Checks:** [`tests/libkiems/dc_analysis_tests.cpp`](../tests/libkiems/dc_analysis_tests.cpp)
  (CTest `dc_analysis_tests`).
  - **Strip:** exact away from the pads, and the drop within 0.1 %.
  - **Distributed-load strip:** within 0.3 %.
  - **Disc spreading resistance:** within 0.5 %.
  - **Via chain:** exact barrel current and resistance.
  - **Islands:** detected and reported.

**Against the prototype on StickHub +5V** (U2 pin 5 at 5 V, each USB port's pin 1 drawing
0.5 A; the worst port is J8):

| | FEM, equipotential pads | FEM, distributed pads | Raster, distributed pads |
|---|---|---|---|
| J8 sag | 67.6 mV | 75.2 mV | 79.2 / 77.5 / 76.5 mV at h = 0.05 / 0.025 / 0.0125, extrapolating to about 75.0 |
| J2 sag | 11.2 mV | 12.1 mV | 12.8 / 12.7 / 12.4 mV, about 12.1 |
| Busiest via | 3.5 A | 3.5 A | 3.5 A |
| Unknowns, time | 2 k, 0.03 s | 2 k, 0.03 s | 340 k, 2.8 s at h = 0.0125 |

- **Agreement.** With the same pad model the two methods agree within 1 %. The FEM sag changes
  by under 1 % from a 0.5 mm to a 0.025 mm element size.
- **The pad model matters.** The +5V trace runs through each connector's pad on its way to the
  next. Tying each pad together, as its soldered pin does, takes about 10 % off the far ports'
  sag.
- **Ground return.** It takes 18 k unknowns and 0.4 s, and gives 3–5 mV at the ports, as the
  prototype did.

## What the reference tool does

From Altium's documentation for Power Analyzer by Keysight (AD 22.10 and later). Altium doesn't
document its solver, mesh or accuracy.

- **Setup**
  - *Power nets*: each has a *Reference Net* (its ground). They can be picked by hand, or
    "Define Automatically" finds them.
  - *Sources*: a source's voltage comes from a power port, a net property or the net's name.
  - *Loads*:
    - Fields: "Load Type", "Total Load Current", "Min Voltage", "Max Voltage".
    - A load's current is split equally across its power pins, unless edited per pin.
  - *VRMs*: a VRM is a load on its input net and a source for a child output net.
  - *Series parts*: resistors, inductors and fuses joining nets ("Extend Nets"), each with a
    resistance and a maximum current.
  - *Board-wide limits*:
    - "Max Current Density" for surface and for internal layers.
    - A via-current limit by hole diameter.
    - "Copper Type", "Work Temperature" (which sets resistivity) and "Via Plating Thickness"
      (from the stackup).
    - An optional fixed "Simulation Mesh Size".
- **Results**
  - A per-layer heatmap of current density or voltage, with current-direction arrows and a log
    scale.
  - Probes: a single point, or the difference between two points.
  - A violation list (voltage drop, current density, via current) that cross-probes to the board.
  - An HTML report: stackup, heatmaps per layer, power consumption, margins, failed vias, and a
    summary of every via.

## The physics

At DC, a copper layer of thickness `t` carries sheet current `K = -(t/ρ) ∇V`. Conservation of
charge gives the 2-D Laplace/Poisson problem `∇·((t/ρ) ∇V) = -s` on each layer's copper for one
net, where `s` is the current drawn per unit area at load pads.

- Layers are coupled only through barrels:
  - Via and PTH walls are resistors `R = ρ L / (π (d + p) p)`, where `d` is the drill, `p` the
    plating thickness and `L` the height between the two copper layers (from the stackup).
- Boundary conditions:
  - Source pads are held at their set voltage (Dirichlet).
  - Load pads draw their current (Neumann).
  - Everywhere else, the copper's edges carry no current.
- The return is a second, independent problem on the reference net. It has the same loads with
  their sign flipped, and the source's return pins are held at 0 V. The voltage a load sees is
  `V_rail(load) − V_ground(load)`.
- Copper resistivity is 1.72e-8 Ω·m at 20 °C, scaled by `1 + 0.00393 (T − 20)`.
  - A single working temperature, like Altium's, is enough to start.
  - A coupled electro-thermal solve (Joule heating, then ρ(T), repeated) can come later. It
    needs a board thermal model, which KiEMS doesn't have.

This is quasi-2-D and ignores current crowding through the copper's thickness. Commercial tools
(SIwave DC, PowerDC, Keysight PIPro) make the same 2.5-D approximation for planes and traces.

## The prototype

[`Scripts/dc_prototype/`](../Scripts/dc_prototype/) solves one net on a raster: a finite-volume
method on square cells.

- **`export_net.py`** runs under KiCad's own Python. It writes each copper layer's filled copper
  for the chosen nets, plus their vias, PTH pads and pads (with pin types), to JSON.
- **`solve.py`** needs numpy, scipy and matplotlib (`python -m venv v; v/Scripts/pip install
  numpy scipy matplotlib`).
  - It joins neighbouring copper cells with the sheet conductance `t/ρ`.
  - A barrel ties to every cell under its annular ring at each end.
  - Source pads are fixed, and each load spreads its current evenly over its pad's cells.
  - It solves the reduced SPD system with a sparse direct solver.
  - Load pads that aren't connected to the source are reported, not solved.

**Checks** (`solve.py --selftest`):

| Case | Analytic | Prototype |
|---|---|---|
| 10 × 1 mm strip, 35 µm copper, 1 A, pads at both ends | 4.767 mV | 4.791 / 4.779 / 4.773 mV at h = 0.1 / 0.05 / 0.025 mm (+0.52 / +0.26 / +0.13 %: first order in h, as expected from pads snapped to cells) |
| Same strip, current density | 28.6 A/mm² | 28.6 A/mm² |
| Two layers joined by one 0.3 mm via, 1 A | via carries 1 A, barrel 1.08 mΩ | 1.0000 A |

**On KiCad's StickHub demo** (h = 0.05 mm):
- **+5V:** sourced at U2 pin 5 at 5.000 V, with every USB port's pin 1 drawing 0.5 A.
  - 21 k unknowns, solved in 0.14 s.
  - The ports at the end of the daisy-chained +5V trace sag by 80 mV.
  - The trace reaches 276 A/mm².
  - The via next to U2 carries 3.55 A, about four times what a 0.3 mm via is usually allowed.
- **GND return:** 291 k unknowns, solved in 3.5 s.
  - It adds 3–5 mV at the ports.
  - Current crowds into U2's ground pin on B.Cu.

All three would be flagged as violations: the 80 mV sag, the 276 A/mm² trace and the 3.55 A via.
The demo isn't meant to deliver 3.5 A, so those load currents are arbitrary.

**What the raster can't do.** A thin trace needs several cells across its width, but a pour only
needs a few per millimetre. At one global pitch, a 100 × 100 mm, four-layer board at h = 0.025 mm
is 64 M unknowns. That is why production should use a mesh that follows the geometry.

## Recommended design for libkiems

### Discretisation: linear (P1) finite elements on a refined, constrained triangulation

- **Mesher.**
  - Each layer's copper for the net is meshed as a single polygon set, with refinement
    (CGAL `Mesh_2` / `refine_Delaunay_mesh_2`, with angle and size criteria).
  - CGAL is already a dependency through CSXCAD on every platform. GEOS's constrained Delaunay,
    which slicing uses, has no refinement.
  - Barrels and pad outlines are added as constrained loops, so their copper falls exactly on
    mesh edges and nodes.
  - Element size grades from about 1/4 of the trace width near narrow features up to a few
    millimetres in pours.
  - Expected scale: around 10⁵–10⁶ unknowns even for large boards.
- **Element matrix.**
  - The standard P1 Laplacian scaled by `t/ρ`: cotangent weights, assembled as a symmetric
    sparse matrix.
  - Current density is constant per element: `J = -(1/ρ) ∇V`.
  - For arrows and probes, average it to the nodes.
- **Barrels.** A two-terminal resistor between "ring" nodes:
  - At each end, the annular ring's nodes are tied to one barrel node, either by elimination
    (an equipotential ring) or by a stiff conductance as in the prototype.
  - Blind and buried vias span only their own layers.
  - Via-in-pad joins the pad's copper.
- **Pins.**
  - **Load model.** Altium splits a load's current equally across its pins. A better default
    ties a part's pins on the net to one node (an equipotential die/package) and lets the copper
    decide the split. That gives per-pin currents as a result. Offer both.
  - **Source model.** The source pins are fixed at the source voltage. A "Sense" option would
    hold a remote point's voltage instead, by fitting the source voltage in an outer 1-D solve.
- **Series parts and VRMs.** Each becomes one extra conductance, or a current/voltage
  relationship between nets, solved in a single system.
  - A VRM's input load is `Vout · Iout / (η · Vin)`, iterated because `Vin` depends on the
    solve.
  - KiEMS already parses component values (`component_value.hpp`) for resistor values.
- **Linear algebra.**
  - Eigen: `SimplicialLDLT` up to about 1 M unknowns, and `ConjugateGradient` with
    `IncompleteCholesky` beyond that.
  - Eigen is header-only and already present on Windows through vcpkg (pulled in by CGAL). On
    macOS it is a Homebrew formula; on Nix, `eigen`.
  - On macOS, Accelerate's sparse Cholesky could be an alternative, but one portable path is
    simpler to test.
- **Islands.** Find connected components first. Copper that isn't connected to a source is
  reported (for example "load J5 isn't connected to +5V's source") instead of making the matrix
  singular. This is the DC version of `warnAboutDisconnectedPortReturns()`.

### Inputs from libkicad (all exist today)

- `boardGeometry().copper`: per-net, per-layer filled copper. It has to be the *filled* zone
  copper, not `zones()`'s outline.
- `throughHoles()`: vias and PTH pads, with drill and pad size.
- `stackup()`: copper thickness per layer, and the dielectric thicknesses that set barrel
  lengths.
- `padsOnNet()`: positions and shapes, and `pinType` ("power_out" for sources and "power_in" for
  loads). This gives "Define Automatically" with no extra metadata.
- `footprints()` values: the series resistors, ferrites and fuses between rails.

Not available: via plating thickness (KiCad doesn't store it; the default should be 25 µm,
IPC-6012 class 3) and Altium-style per-component current parameters. A KiCad symbol field such
as `Current` could be read later, through the netlist path that the simulation models use.

### Where it lives

- **`libkiems/kiems/dc_analysis.{hpp,cpp}`**
  - `DCAnalysisConfig`:
    - rails: net, reference net, sources (pins, V, optional sense point), loads (pins, current,
      min/max voltage, split mode), series parts;
    - limits: J for surface and inner layers, a via-current table by drill;
    - temperature and plating.
  - `DCAnalysisResult`:
    - per layer: the mesh plus nodal V and element J;
    - via currents, per-load voltages, per-pin currents, power loss;
    - violations.
  - Both serialise to JSON like the other `kiems::*Config` types.
- **The document.** A `dc_analyses` array in the `.g2esim` beside `simulations`. A DC run takes
  seconds, so it runs in-process, with no FDTD worker or grid.
- **CLI.** `kiems --dc NAME` writes the result JSON and PNG heatmaps (gnuplot or matplot++ like
  the other plots).
- **GUI.**
  - A *Power* inspector tab for setup. It reuses Board Setup's pin picking: pick a pad, then
    "Source" or "Load".
  - The result appears as a board overlay:
    - a voltage or current-density heatmap with layer selection, arrows and a log scale;
    - click probes, a violation list that selects the via or pad, and a via-current table.
  - It belongs in the PDF report once that exists.

### Validation plan

1. Analytic cases as unit tests:
   - the strip, and a via chain;
   - the disc spreading resistance between two concentric rings, `R = ρ ln(b/a) / (2π t)`;
   - a square plate between two edge electrodes, which is exactly one sheet resistance.
2. FEM against the raster prototype on StickHub +5V/GND and a multi-layer demo board (cm5_minima,
   6 copper layers; jetson-agx-thor-baseboard, 10). The two should agree within a few percent at fine raster pitch.
3. Against an independent solver: FEMM (free, 2-D current flow) on one plane exported as DXF, as
   was done for the FDTD side with openEMS.

## Order of work

1. *(Done; see [What's built](#whats-built).)* libkiems:
   - mesher plus P1 assembly plus a sparse solve (preconditioned CG rather than Eigen) for one
     rail with fixed sources and per-pin loads;
   - the analytic tests;
   - CLI output (JSON plus PNG).
2. Island diagnostics, violations against limits and via currents are done. Still to do: the
   reference net, solved together with the rail so a load sees `V_rail − V_ground`. Today the
   return is a second analysis of the ground net, with the loads' currents negated.
3. *(Done.)* GUI (`kiems-gui` PowerView):
   - Power page setup with automatic definition from pin types;
   - heatmap, probes and violation list.
   The page is its own map rather than an overlay on the Board page. It doesn't yet pick pins
   on the board or add a page to the PDF report.
4. Series parts and VRMs, so multi-rail trees work; then the equipotential-package load model;
   then remote sense.
5. Later: electro-thermal coupling. AC PDN impedance is a separate problem, closer to the
   existing FDTD path than to this solver.
