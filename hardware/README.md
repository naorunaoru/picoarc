# PicoARC Hardware

The fab-ready PicoARC PCB design lives in this directory. The circuit is
authored with the [Zener](https://docs.pcb.new/pages/spec) toolchain from
[Diode](https://github.com/diodeinc/pcb), and the physical board layout is kept
as a generated/synced [KiCad](https://kicad.org/) project.

## Source of Truth

Zener is a Starlark-based hardware description language for PCB schematics. The
`pcb` CLI evaluates the `.zen` design, manages dependencies from `pcb.toml`,
assigns reference designators, validates the netlist, and generates/syncs KiCad
layout files.

In this repo, the `.zen` files are the schematic source of truth. There is no
separate `.kicad_sch` to edit:

- `picoarc.zen` is the top-level board schematic/netlist.
- `modules/` contains reusable schematic subcircuits for USB-C, power, HDMI
  ARC, and debug/status hardware.
- `components/` contains local RP2040 and flash component definitions.
- `footprints/` contains local symbols, footprints, and 3D models not taken
  directly from the upstream KiCad libraries.

Edit the Zen module tree when changing the circuit, then regenerate/sync the
KiCad layout from that source.

## Layout and Production Files

- `pcb.toml` is the Zener workspace/board manifest. It pins the board entry
  point, repository metadata, `pcb-version`, and KiCad library dependencies.
- `layout/layout.kicad_pro` is the KiCad project to open for board review.
- `layout/layout.kicad_pcb` is the checked-in PCB layout.
- `layout/layout.kicad_dru` contains KiCad design rules.
- `layout/jlcpcb/gerber/` contains the plotted Gerber/drill outputs.
- `layout/jlcpcb/production_files/` contains the current JLCPCB upload bundle:
  `GERBER-layout.zip`, `BOM-layout.csv`, and `CPL-layout.csv`.
- `jlcpcb-lcsc.csv` records JLCPCB/LCSC sourcing data used by the board.

## Zener/KiCad Workflow

Install the `pcb` shim using the
[upstream instructions](https://docs.pcb.new/pages/quickstart). The shim selects
the `pcbc` toolchain lane requested by `pcb.toml`; this repo currently requests
the `0.3` lane.

After changing the Zen schematic, run from this directory:

```sh
pcb sync
pcb build picoarc.zen
pcb layout picoarc.zen
```

`pcb sync` reconciles imports and dependency manifests. `pcb build` evaluates
and validates the Zener design. `pcb layout` regenerates/syncs the KiCad layout
project from the Zen netlist.

After that, open `layout/layout.kicad_pro` in KiCad for placement/routing
review, DRC, and fabrication exports. KiCad remains the tool for inspecting and
finishing the physical layout; Zener remains the tool for changing the circuit.

## Prototype Wiring

If you want to reproduce the old hand-wired Pico dev-board prototype instead,
the minimal wiring is:

- Pi Pico dev board
- HDMI breakout
- Few passives you probably have laying around
- 3.3 V <-> 5 V I2C level shifter for HDMI DDC/EDID (or not, if you're brave)

```text
Net CEC_BUS
  Pico 3V3 (pin 36)  -- R1 (27 kOhm) -- node CEC_BUS
  Pico GP3 (pin 5)   -- R2 (220 Ohm) -- node CEC_BUS
  node CEC_BUS                         -- HDMI pin 13

Net ARC_TX
  Pico GP2 (pin 4)   -- R3 (330 Ohm) -- node ATT_A
  node ATT_A         -- R4 (56 Ohm)  -- Pico GND
  node ATT_A         -- C1 (10 nF)   -- HDMI pin 14

Net HDMI_5V_FROM_SOUNDBAR
  HDMI pin 18        -- R5 (1 kOhm)   -- HDMI pin 19
  HDMI pin 18        -- R6 (68 kOhm)  -- node HDMI_5V_SENSE
  node HDMI_5V_SENSE                   -- Pico GP4 (pin 6)
  node HDMI_5V_SENSE -- R7 (100 kOhm) -- Pico GND

Net DDC_EDID
  HDMI pin 15 (SCL)  -- level shifter -- Pico GP7 (pin 10)
  HDMI pin 16 (SDA)  -- level shifter -- Pico GP6 (pin 9)

Net GND
  Pico GND (pin 38)                    -- HDMI pin 17
```

Note about DDC_EDID: while omitting a level shifter works as a bench-only direct
GPIO bodge, keep in mind that HDMI DDC is a 5 V bus and RP2040 IO is not exactly
5 V tolerant.

## License

The PicoARC hardware design materials in this directory are licensed under the
CERN Open Hardware Licence Version 2 - Weakly Reciprocal (`CERN-OHL-W-2.0`).
See [../LICENSE](../LICENSE) for the repository license notice and full license
text locations.
