# USB-powered DDC — routing complete

This change removes the power dependency on HDMI pin 18 from the PCA9306
translator and its high-side pull-ups. The intended extra use is a bespoke
USB–DDC master for flashing a monitor with a Realtek scaler, using separate
firmware, without the previous soldered resistor modification.

The layout is complete and passes the existing design rules. The circuit is
reasonable for this powered-target bench use, subject to the pull-up loading
and power-state limits below. No monitor flashing or ARC hardware test was
performed during this review. Fabrication outputs have **not** been regenerated.

## Circuit review

- R10.2 and R11.2: 2.2 kohm SCL/SDA high-side pull-ups to USB `VDD_5V`.
- R12.1: USB `VDD_5V` feeds the EN/VREF2 node through 200 kohm.
- VREF1 and R8/R9 remain on 3.3 V. C2 remains 100 pF on EN/VREF2.
- HDMI pin 18, its TVS, HPD pull-up, and sense divider stay on the separate
  `HDMI._HDMI_5V` net. USB does **not** supply pin 18 or power the monitor.

The EN/VREF2 connection, 200 kohm bias resistor, and 100 pF filter follow
[TI PCA9306 sections 9.2.2.1 and 10](https://www.ti.com/lit/ds/symlink/pca9306.pdf).
Both sides of the DDC connection must use open-drain signalling. The bias feed
must not be replaced by a direct connection to 5 V.

There are two distinct roles:

| Role | Assessment |
|---|---|
| USB–DDC master talking to a monitor | Local 5 V pull-ups and USB-derived translator bias allow transactions without incoming HDMI +5 V. The monitor/scaler still needs its normal power and the appropriate programming mode. Targets that need source-provided pin-18 power are not covered by this change. |
| ARC endpoint acting as an HDMI sink | Existing firmware still serves EDID as an I2C slave. HPD is pulled up from HDMI pin 18, and ARC discovery still checks HDMI +5 V. This change does not enable ARC without that supply. |

`firmware/src/main.c` initializes the EDID slave independently of HDMI sense;
`firmware/src/arc.c:arc_task()` gates active ARC discovery on that sense. A
flashing application must take ownership of GP6/SDA and GP7/SCL as an I2C
master instead of running the EDID slave. No flashing firmware is added here.

### Pull-up loading and power states

PCA9306 is a pass-FET translator: a device pulling a line low sinks current
from both pull-ups. At 5.0 V / 3.3 V and a 0.4 V low level, the existing
2.2 kohm resistors require approximately
`(5.0 - 0.4)/2200 + (3.3 - 0.4)/2200 = 3.41 mA` per line, before any pull-up
inside the target. That exceeds a generic 3 mA I2C sink budget; the actual
scaler and GPIO low levels need measurement. The pull-up strengths predate
this power reroute; their values have been retained.

For normal HDMI sink operation, strong local pull-ups are also a compromise:
[NXP PCA9507 section 6.3.1, Table 3](https://www.nxp.com/docs/en/data-sheet/PCA9507.pdf)
describes source pull-ups of 1.5–2 kohm on both lines, versus a sink's 47 kohm
on SCL and no SDA pull-up. With those source pull-ups connected, the present
board's low-level current is about 5.71–6.48 mA per line at nominal supplies.
This is not a claim of HDMI electrical compliance.

The two 5 V power nets are not directly joined, but resistor-mediated
back-power paths still exist. USB can feed an unpowered target through DDC
input clamps; a powered external source can feed an unpowered USB rail
through R10/R11. Turning off the PCA9306 alone does not disconnect those
high-side resistors. Keep the monitor normally powered during programming,
and measure these states before claiming unrestricted hot-plug behavior.

A useful future dual-role revision would make R10/R11 switchable as a pair
with a small switch or properly isolated load switch, and consider weaker
R8/R9. For example, 10 kohm low-side pull-ups would reduce the standalone
nominal low current to about 2.38 mA, but must be checked against low-side
capacitance and rise time. Use the target's actual sink rating and
[TI's pull-up sizing method](https://www.ti.com/lit/an/slva689/slva689.pdf);
slowing the clock does not fix excess DC sink current.

## Completed layout

- R8 moves 0.10 mm left to separate its courtyard from R9.
- R11 rotates to a horizontal placement above the DDC traces; R13 moves
  0.30 mm right to make room. This removes the SCL-to-power-pad clearance
  violation without narrowing traces or relaxing clearances.
- A 0.25 mm front-side connection and one new 0.50/0.30 mm via join
  R8/R9/R13 to the existing 3.3 V plane.
- R10/R12 share a local 5 V connection; R11 has its own short feed and via.
  A 0.30 mm B.Cu trunk joins both to the existing USB 5 V via near U3.
- Three vias added in total, all 0.50 mm copper / 0.30 mm drill. The inner
  ground layer receives no new signal or power traces. All zones are refilled.
- The recovered removal of the HDMI 5 V In2.Cu zone is retained. The compact
  SWD edge pads, board outline, and unrelated component placements are retained.

## Validation — September 21, 2026

| Snapshot | DRC violations | Unconnected items |
|---|---:|---:|
| Mainline baseline (recovery audit) | 0 | 0 |
| Initial stash (recovery audit) | 9 | 2 |
| Recovered PR head before completion | 2 | 6 |
| Completed routing | 0 | 0 |

- KiCad 10.0.3: zone refill and DRC with all configured severities. The six
  existing ignored checks remain; project rules and exclusions are unchanged.
- `pcb build --offline --locked picoarc.zen`, pcbc 0.3.93: passes with 72
  components and the same six grouped power-pin/net-type warnings as mainline.
- `pcb layout --offline --no-open picoarc.zen` on an isolated copy: passes;
  generated `default.net` is byte-identical. Only group membership changes
  in the PCB; the synchronized board and `snapshot.layout.json` are retained.
- Every component reference, value, footprint ID, schematic UUID, and
  numbered pad net matches the regenerated netlist: 72 components, 77 nets,
  240 distinct component pins / 246 physical pads (including duplicates).
- Visual review of the changed front and back copper; `git diff --check`.

The recorded DRC report and pin-parity check are in
[`usb-powered-ddc-validation/`](usb-powered-ddc-validation/).
Run the checker with KiCad's Python interpreter, which provides `pcbnew`:

```sh
python3 experiments/usb-powered-ddc-validation/check_parity.py layout
kicad-cli pcb drc --refill-zones --format json --severity-all \
  --exit-code-violations --output /tmp/ddc-drc.json layout/layout.kicad_pcb
```

Before fabrication acceptance, verify powered-monitor programming/readback,
DDC high/low levels and rise times, USB-only/HDMI-only/both-power leakage,
and normal soundbar EDID/HPD/ARC startup and reconnect behavior. Existing
Gerbers, drill files, BOM, CPL, and renders are historical artifacts.

## Other routing improvements to consider

1. **Ground continuity and flash:** In1.Cu carries all six QSPI signals,
   approximately 6.2–9.2 mm each, plus ARC, CEC, DDC, and short USB sections.
   A future placement pass should bring flash closer to U5 and move these
   routes to outer copper where practical, reducing interruptions in the
   intended ground plane. Do not sacrifice the reference plane for cosmetic
   trace-length matching.
2. **USB:** R21/R22 currently have about 3.95/4.25 mm of copper between the
   RP2040 and their near pads. Move the series resistors closer to U5 when
   revisiting this area, and aim for a continuous reference and fewer layer
   transitions through U3. The existing values are 33 ohm; Raspberry Pi's
   [hardware guide, section 2.4.1](https://datasheets.raspberrypi.com/rp2040/hardware-design-with-rp2040.pdf)
   specifies 27 ohm near the chip, so revisit the value with signal-integrity
   measurements rather than changing it as part of this power route.
3. **HDMI protection:** Retain D2/D3 close to J1 and keep connector-to-TVS
   paths and TVS ground returns short when repacking the area. Moving these
   parts inward merely to simplify DDC routing would be a poor trade.

## Recovery provenance

The first recovery commit copies the schematic, netlist, and initial PCB
from stash `6a9e3dd08a8a8170080cb1d2e411c54989a77f09` (August 1).
The second restores the KiCad local-history placement/routing attempt from
`a29ee2f586882c4d5f9e4652a2317bdf1001efea` (July 29, 18:50:51), PCB blob
`d3bf02c5cddb06c78cc2443d2c6395162593c705`. Completion builds on that recovered
placement; the earlier stages remain available in PR history.
