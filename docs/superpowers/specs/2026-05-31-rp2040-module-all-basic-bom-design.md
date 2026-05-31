# Custom RP2040 module + full all-Basic BOM — design

Date: 2026-05-31
Board: picoarc (RP2040 USB-Audio→HDMI-ARC bridge), diodeinc Zen flow, JLCPCB assembly.

## Goal

1. Replace the vendored `@kicad/MCU_RaspberryPi/RP2040.zen` with a **local, owned** RP2040
   module so every support passive can be pinned to a JLCPCB **Basic** part.
2. Drop the unused GPIO breakouts / DNP pin-header ("header rows").
3. Achieve a **fully source-level all-Basic BOM** (no JLC-side remap), except the few
   functional parts that have no Basic equivalent.
4. Add ESD protection to the previously-unprotected ARC + HDMI-5V lines.

## Why this is needed (constraints discovered)

- Every part in the current BOM is JLCPCB **Extended** (~$3 feeder fee each; ~28 parts).
- The diodeinc generic matcher auto-picks parts (Murata/Panasonic) and has **no JLC-Basic
  preference knob**.
- Generics accept an `mpn=` override, but **~14 passives live inside the vendored RP2040
  module** and can't be overridden from `picoarc.zen`. Partial overrides create duplicate
  BOM lines (a Basic 100nF *and* the Murata 100nF). Owning the module removes this — every
  instance of a value uses one Basic MPN.

## Design

### 1. `hardware/components/RP2040.zen` — clone-and-trim of the vendored module
Keep the **same child component names, footprints, values, and topology** so `pcb layout`
re-sync matches the existing placement/routing of U5's support cluster.
- Chip: same QFN-56 symbol/footprint; VREG_VIN→3V3, VREG_VOUT/DVDD→1V1, IOVDD/USB_VDD/
  ADC_AVDD→3V3, GND, TESTEN→GND.
- Children (unchanged names): `c_bulk_3v3/1v1` (1µF), `c_bypass_3v3_1..4` + `c_bypass_1v1_1..2`
  (100nF), `crystal`(12MHz 3225-4pin)+`c_xin`/`c_xout`(15pF)+`r_xout`(1k)+`_R_OSC`,
  `r_usb_dp`/`r_usb_dm` (27Ω), `r_run_pullup`/`r_qss_pullup` (10k), `tp_swd_io`/`tp_swd_clk`.
- **Each passive pinned to its Basic MPN** (table below).
- **Dropped:** the `PinHeader` (pin_socket_gpio) and the disabled QSPI-flash / BOOTSEL blocks.
- **GPIOs:** expose only the 6 used (gpio2/3/4/6/7/25 → ARC_TX, CEC_BUS, HDMI_5V_SENSE,
  DDC_SDA, DDC_SCL, STATUS_LED); unused chip pins → internal floating NC nets.

### 2. `picoarc.zen`
Point `RP2040 = Module("./components/RP2040.zen")`; drop the 30-line GPIO enumeration and
dead config flags; keep `name="U_MCU"` (hierarchical paths match → placement preserved).

### 3. All-Basic across the other modules (same footprints → zero layout change)
Pin Basic `mpn=`/`manufacturer=` on generics in HdmiArc/Power/UsbC/Debug/Flash (R1–R15,
C1–C4). Resistors → UNI-ROYAL `0402WGF` Basic series; caps → Samsung CL Basic series.

### 4. Explicit-part swaps
| Ref | From (Extended) | To (Basic) | Layout impact |
|---|---|---|---|
| U1 LDO (no MPN) | — | AP2112K-3.3 (SOT-23-5) | none (same footprint); fills missing MPN |
| D1/D3 LEDs | generic | Basic 0603 LED | none |
| J2 USB-C | GCT USB4105 | Basic 16P Type-C | footprint change → re-place + re-route |
| SW1 | Omron B3U-1000P | Basic SMD tact | footprint change → re-place + re-route |

### 5. ESD add
`PESD5V0S2BT` (SOT-23, bidirectional, LCSC C49338) on `HDMI._ARC14` + `HDMI._HDMI_5V` + GND,
added in HdmiArc.zen (D2 4-ch kept). New part → place at J1.14/18 + route. Stays Extended
(the one allowed exception, for correct bidirectional ARC clamping).

## Confirmed Basic part mapping (anchors; remaining resolved at implementation)
| Value | Basic LCSC# | Series |
|---|---|---|
| R 0402 1%: 1k C11702, 2.2k C25879, 5.1k C25905, 10k C25744, 100k C25741, 220R C25091, 330R C25104 | (27R/56R/270R/27k/68k = same) | UNI-ROYAL 0402WGF |
| 100nF C307331, 10nF C15195, 15pF C1548, 10µF C96446 | (1µF = same) | Samsung CL |
| 12MHz crystal C9002 | | YXC X322512 |

## Layout-preservation strategy
- Same child names + footprints ⇒ re-sync keeps placement/routing for U5 cluster + all
  same-footprint passives (the bulk of the board).
- Re-place + re-route only: **J2, SW1** (footprint change) and **the new ESD** (new part).
- The DNP pin-header was already excluded from the layout, so dropping it changes nothing there.

## Verification
1. `pcb layout picoarc.zen` re-sync.
2. Re-fill the 4 zones; confirm net classes + `layout.kicad_dru` rules survive.
3. `kicad-cli pcb drc` → 0 error-severity violations; account for any new ratsnest (J2/SW1/ESD).
4. Re-query JLCPCB parts API → BOM is all-Basic except RP2040, PCA9306, W25Q16, USBLC6,
   HDMI connector, ESD (the unavoidable functional Extended parts).
5. Render top/bottom/inner.

## Risks
- Footprint sourcing for the new ESD / Basic USB-C / Basic tact (need KiCad-compatible
  footprints; mind the **pad double-rotation trap**).
- Confirm `pcb layout` preserves zones/tracks (not just placement) on re-sync.
- The J2/SW1/ESD re-placement + routing is genuine layout work, intertwined with the
  in-progress signal routing.

## Out of scope (follow-ups)
- Finishing the unfinished on-board signal routing and test-point placement.
- Verifying the ARC node's DC bias (whether a unidirectional clamp would have sufficed).
