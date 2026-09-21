# USB-powered DDC — unfinished routing

This draft moves the DDC high-side pull-ups and PCA9306 bias supply from
HDMI pin 18 to USB VBUS (`VDD_5V`). It preserves the recovered work for
continuation; the PCB is not ready for fabrication.

## Circuit and recovery

- R10.2 and R11.2: DDC SCL/SDA high-side pull-ups to USB `VDD_5V`.
- R12.1: 200 kohm EN/VREF2 bias feed to USB `VDD_5V`.
- HDMI pin 18, its protection, HPD pull-up, and voltage-sense divider remain
  on the independent `HDMI._HDMI_5V` net.

The first recovery commit copies the schematic, netlist, and initial PCB
from stash `6a9e3dd08a8a8170080cb1d2e411c54989a77f09` (August 1).
The following commit restores the placement/routing attempt from the
separate KiCad local-history repository, commit
`a29ee2f586882c4d5f9e4652a2317bdf1001efea` (July 29, 18:50:51).
The recovered PCB blob is `d3bf02c5cddb06c78cc2443d2c6395162593c705`.
The stash-only PCB remains accessible in the first recovery commit.

The local-history candidate moves ten HDMI/DDC-area components and removes
the HDMI 5 V zone on In2.Cu. It is a candidate for further routing, not an
approved placement. The existing compact SWD edge pads are retained; the
JST-SH connector experiment is a separate branch.

## Recorded validation — September 21, 2026

KiCad 10.0.3 DRC on exact snapshot copies, with zones refilled and all
configured severities included:

| Snapshot | DRC violations | Unconnected items |
|---|---:|---:|
| Mainline baseline | 0 | 0 |
| Initial stash | 9 | 2 |
| Recovered PCB in this draft | 2 | 6 |

The two remaining violations are:

- DDC SCL track to R11.2 clearance is 0.16 mm; Power requires 0.20 mm.
- R8 and R9 courtyards overlap.

The six missing connections are three links in the 3V3 pull-up/gate-bias
cluster and three links joining R10/R11/R12 to USB 5 V. No USB/HDMI power
short is reported in this candidate. The initial stash reported three
shorting-item violations at R12.1, plus associated mask bridges.

`pcb build --offline --locked picoarc.zen` with pcbc 0.3.93 passes with
72 components and the same six grouped power-pin/net-type warnings as
mainline. Board and netlist pin assignments for the three reassigned
resistor pads were checked. This is not a complete schematic-parity sign-off.
The project retains its existing ignored DRC checks; none were relaxed for
this recovery. No live hardware test was performed.

## Remaining work

- [ ] Review the recovered placement against the stash-only starting point.
- [ ] Resolve both DRC violations and all six missing connections.
- [ ] Reconcile Zener-generated layout metadata (`snapshot.layout.json`) with
      the chosen manual layout and verify full board/netlist parity.
- [ ] Refill zones and rerun DRC without introducing new exclusions.
- [ ] Verify USB-only, HDMI-only, and combined-power behavior, EDID reads,
      HPD sequencing, ARC startup, and reconnect behavior on hardware.
- [ ] Regenerate Gerbers, drill files, BOM, CPL, and renders only after the
      board is accepted. Existing manufacturing outputs predate this work.

USB-powered DDC alone does not enable complete ARC operation without HDMI
+5 V. Firmware still gates ARC progression on HDMI +5 V sense, and HPD's
pull-up remains supplied from HDMI. Any change to that behavior needs an
explicit follow-up design decision.
