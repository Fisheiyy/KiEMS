# Simulating a signal

This guide walks through simulating one signal on your board, such as a USB data pair, from setup to
reading the results. You can open it any time from **Help › Simulating a Signal**.

## The idea

KiEMS sends a sharp pulse into your trace from the chip that drives it, and records what arrives at
each chip or connector that receives it. The simulation covers every millimetre of copper in
between, including vias, the planes underneath and series resistors.

From those recordings you get:

- **How cleanly bits arrive.** The eye diagram, tested against your standard's mask.
- **How well the trace is matched.** Its impedance along its length, and where it rings.
- **How much leaks and reflects.** The S-parameters.

## Words you'll see

| Word | What it means |
|---|---|
| **Driver** (*Excited*) | The pin that sends the signal, usually the chip's output. In a pair, its partner pin is driven in opposite phase. |
| **Receiver** (*Probed*) | A pin where the signal is measured. |
| **Absorbing** | The pin is terminated in the line's impedance, as a real receiver or driver loads it. Receivers absorb. |
| **Differential pair** | Two traces carrying one signal as a difference, such as USB D+ and D−. Set up one half and KiEMS mirrors it onto the other. |
| **Impedance** | What the trace should present, per line. USB 2.0 is 90 Ω differential, which is **45 Ω per line**. |
| **Eye / mask** | Many bits laid over each other. The mask is the keep-out area the standard says the eye must stay clear of. |
| **Sweep** | The range of frequencies simulated. Its top sets how sharp the test pulse is. |
| **Max timesteps** | How long the simulation runs. It must outlast the test pulse, plus the time to travel down the trace and settle. |

## Setting up a USB pair, the quick way

1. **Simulation › New Signal Simulation…**, or **+ › New Signal Simulation…** under the sidebar.
2. **The signal.**
   - Keep *A differential pair*, and choose either half of the pair, for example `/D+`.
   - KiEMS finds its partner (`/D−`) by name.
   - It also follows the pair through series resistors to the connector, so the whole path is
     simulated. Untick *Follow it through series parts* to stop at the resistors.
3. **Who sends it, and who receives it.**
   - **Driven by** is KiEMS's guess at the driving pin: an IC's output or bidirectional pin. For a
     USB device, pick the USB chip or microcontroller pin.
   - **Received at** lists every other pin on the signal. Untick any that shouldn't be measured,
     such as test points.
4. **What it has to meet.**
   - Pick the standard, for example *USB Full Speed (12 Mb/s)* or *USB High Speed (480 Mb/s)*.
     This sets the eye mask, the bit rate, and 45 Ω per line.
   - If the document's sweep is too low to show the trace (below 1 GHz), KiEMS offers to sweep to
     3 GHz. Keep that ticked unless you have a reason not to.
5. Read **This will**, the plain summary of everything that will be set, then **Create Simulation**.

The new simulation appears in the sidebar. Select its row to see the board with the signal
highlighted.

## Running it

- **Run** (the play button, or Ctrl+R) queues the simulation. The toolbar shows what it's doing,
  how far along it is, and about how long is left.
- A board the size of a USB stick takes a minute or two on a GPU. Large boards and fine meshes take
  longer.
- To change the engine (GPU or CPU), click **Engine** at the right of the toolbar.

## Reading the results

| Where | What to look for |
|---|---|
| **Simulation Results › Eyes** | The eye should stay clear of the red mask. A green verdict in the sidebar means it passed; a red stop sign means it failed the test. |
| **Simulation Results › S-Parameters** | *Received at …* should stay near 0 dB up to your signal's frequencies; *Returned to source* should stay well below (−15 dB or better). |
| **Trace** | The trace coloured by impedance (blue too low, red too high) or by ringing. The findings list points out each stretch more than 10 % off the target. |

## When something looks wrong

| You see | Why, and what to do |
|---|---|
| *"This run stopped before its excitation had played"* | The run was shorter than the test pulse. Raise **Max timesteps** (Settings tab), or the top of the sweep (**Frequency range**). The message says how many timesteps it needs. |
| The Trace profile stops partway along the trace | A step has to reach a point and come back before the run ends. Raise **Max timesteps**. |
| *"probes left out, with no plane under them"* | Part of the trace has no copper under it on its reference layer, such as a bottom-layer trace with the top layer bare. Its ringing can't be measured there; the impedance still can. |
| The eye fails | Look at the Trace page first: a red or blue stretch, or heavy ringing, shows where the mismatch is. Common fixes are trace width, a gap in the plane under the trace, or a stub. |
| *"Stops at U7: /_D+, /_D− look like the same signal past it"* | The signal passes through a chip, such as an ESD array or a USB switch. A simulation can't carry a signal through a chip. Simulate each side on its own, for example the connector side driven from the connector. |
| The driver guess is wrong | Re-run *New Signal Simulation* and pick the right pin, or click the pin on the board and tick **Excited** in the Selection tab. |

## Doing it by hand

Every choice the wizard makes can also be made, or changed, by hand:

1. **Simulation › Add Simulation…**, then tick **Differential pair** in the Simulation tab for a pair.
2. **Click a pad on the board.** The Selection tab shows that pin.
   - Tick **Excited** for the driver.
   - Tick **Probed** for each receiver.
   - Leave **Absorbing** on for receivers.
   - In a pair, each change is copied to the partner pin.
3. **Click a trace** to include its net, or to set its impedance and how far around it is simulated
   (**Hull padding**).
4. Click a series resistor and tick **Included in simulation** to model it.
5. **Tests tab:** pick the eye standard.
