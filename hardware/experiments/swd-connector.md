# SWD connector alternative — placement unresolved

This draft recovers the earlier JST-SH connector proposal as an alternative
to the compact SWD edge pads merged in PR #6. A suitable position and enough
space for the connector/cable have not been established. This is schematic
work in progress, not a connector-equipped, routed PCB.

## Recovered circuit

- JST `SM03B-SRSS-TB`, 3-pin, 1.00 mm pitch, horizontal/side-entry footprint.
- Target-side pinout: 1 = SWCLK, 2 = GND, 3 = SWDIO.
- A 100 ohm 0402 series resistor in each SWD signal path, matching the
  original proposal.

The circuit is recovered from local reflog commit
`eff6deb5af85c3c3523b524d8bea0335be2712f3`, before subsequent amendments
replaced the connector with a 2.54 mm header and then edge pads. That
connector attempt changed schematic source only; no corresponding placed
and routed connector PCB was found in those commits.

The recovered circuit now lives in the existing `modules/DebugPort.zen`
module, preserving mainline's separate status-LED module and current RP2040
definition. Pin keys use `Pin_1`/`Pin_2`/`Pin_3` to match the actual generic
connector symbol, correcting the old proposal's numeric-key spelling.

## Deliberate draft mismatch

`picoarc.zen` evaluates the connector proposal through `DebugPort.zen`.
The checked-in `layout.kicad_pcb`, `default.net`, and generated layout
snapshot still describe mainline's compact edge-pad version. No placement
or routing has been invented to make the connector appear finished.
The old edge-pad footprint remains available for comparison.

Do not fabricate from this draft or infer connector clearance from the
existing PCB's DRC result. Synchronize the schematic into an isolated layout
candidate before placement review. The DDC reroute is a separate draft.

## Validation — September 21, 2026

`pcb build --offline --locked picoarc.zen` passes with pcbc 0.3.93 and
74 components (mainline has 72). It reports the same six grouped HDMI/DDC
power-pin/net-type warnings as mainline. `git diff --check` passes.
No connector placement, board/netlist parity, or live probe test is claimed.

## Completion criteria

- [ ] Establish a connector position, board side, cable exit, mating access,
      height, and clearance to the enclosure and HDMI/USB connectors.
- [ ] Decide whether the side-entry SM03B is viable, whether a top-entry
      BM03B alternative helps, or whether to retain the edge pads.
- [ ] Review the two series resistors and their placement for the selected
      probe/cable arrangement.
- [ ] Synchronize Zener netlist/layout metadata and place the connector and
      resistors without losing existing manual routing.
- [ ] Route SWCLK/SWDIO/GND, remove obsolete pad routing as appropriate, and
      verify connector orientation and pin numbering.
- [ ] Run full board/netlist comparison and DRC after zone refill.
- [ ] Test attach/reset/program/readback with the intended probe.
- [ ] Update sourcing and fabrication outputs after choosing this option.

If the connector still cannot fit, this draft can be closed while retaining
mainline's already-routed SWD edge pads. No change to master is needed to
reject the alternative.
