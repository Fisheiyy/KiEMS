# Trace impedance and ringing

kiems-gui's **Trace** page, under each simulation in the sidebar, shows signal integrity along the
simulated net that a run drives. It's meant for tuning a trace's width, stubs and transitions until
the trace is matched and quiet.

## What it shows

- **Map.** The simulated nets' tracks over the board. The driven net is coloured either by the
  impedance a step meets along it (blue below the target, red above) or by how much it rings once
  the step has passed. Hover for the distance, impedance and ringing; click to pick a point.
- **Impedance.** The impedance profile against distance from the driver, with the target (the
  driving port's impedance) and a ±10 % band.
- **Step Response.** The voltage at the picked point, at the driver and at the net's other ports,
  for a step of one volt.
- **Ringing.** At each point along the trace: the most the voltage strays from where it settles
  once the edge has passed, and the overshoot.
- **Reflections.** Distance along the trace against time, coloured by how far the voltage is from
  where it settles. The step's front is the edge where the blue ends. Each reflection is a line
  leaving the spot that caused it.
- **Findings.** Each stretch more than 10 % off the target, the worst ringing, and each port's
  overshoot, ringback and settling time.

The step's **rise time** sets the resolution. A faster edge separates features that are closer
together, down to what the simulation's frequency sweep can carry (its spectrum is down to 1 % at
the top of the sweep). A real driver's edge shows what that driver will see.

## How it's computed

The numerics are in `libkiems/kiems/trace_profile.hpp`, checked against exact cases by
`tests/libkiems/trace_profile_tests.cpp`. The page's loader is `kiems-gui/TraceAnalysis.hpp`.

- **Impedance: time-domain reflectometry.**
  - The method:
    - From the driving port's voltage and current, the incident and reflected waves are
      u_inc = (u + Z0 i)/2 and u_ref = (u − Z0 i)/2.
    - The step response of u_ref/u_inc, for a step with Gaussian edges, is ρ(t), the reflection
      seen at round-trip time t. Then Z(t) = Z0 (1 + ρ)/(1 − ρ).
  - **Placing it on the trace:** each round-trip time is mapped to the distance a step reaches in
    half that time. That one-way delay against distance comes from when the step arrives at each
    trace probe, at the far-end ports, and at the driver itself (0, 0).
- **Ringing: trace probes.**
  - Port resolution places voltage probes along every simulated net's tracks:
    `SimulationConfig::traceProbes()`, at most 400 per simulation and at least 0.3 mm apart.
  - Each probe is a vertical line from the track to its reference plane. Copper samples it about
    ten times per period of the top of the sweep, through the probe box's `copper_sample_period`
    attribute, so it costs neither run time nor much disk.
  - Each run writes them as `trace_ut_<n>`, listed in `trace_probes.txt`.
  - Each probe's transfer from the incident wave gives its step response, and from that its
    overshoot, ringback, settling and largest deviation.
- **Distance along the copper:** a graph of the net's tracks, with vias and T-junctions, measured
  from the driving pad.

## Limits

- **Probes with no plane under them** are left out, and the page says how many. This happens where
  a trace returns through copper beside it rather than below it, such as a bottom-layer trace on a
  two-layer board whose top layer is bare there. Such a probe settles somewhere other than the
  driver does, and that is how it's found.
- **The ports are resistive terminations** at their configured impedance, so what rings is the
  trace's own mismatch rather than a real receiver's. A high-impedance receiver would add its own
  reflection.
- **Runs from before trace probes** still get the impedance profile. It is placed along the trace at
  an assumed microstrip speed (effective permittivity 3), and those runs have no ringing.
