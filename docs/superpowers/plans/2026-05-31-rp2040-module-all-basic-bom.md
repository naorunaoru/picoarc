# Custom RP2040 module + all-Basic BOM — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Hardware/EDA flow — "tests" are `kicad-cli pcb drc`, pcbnew connectivity, and the JLCPCB parts API; commits happen at verified checkpoints. Steps use `- [ ]`.

**Goal:** Replace the vendored RP2040 module with a local clone-and-trim, pin every passive to a JLCPCB Basic part, drop the unused GPIO header/breakouts, and add bidirectional ESD on ARC + HDMI-5V — yielding a source-level all-Basic BOM (minus the unavoidable functional parts) without disturbing the relaxed layout.

**Architecture:** diodeinc Zen flow (`pcb layout picoarc.zen` syncs netlist→`hardware/layout/layout.kicad_pcb`, preserving manual placement). Clone-and-trim keeps the RP2040 support children's names/footprints identical so the re-sync matches existing placement/routing. All other generics get explicit `mpn=`/`manufacturer=` Basic pins.

**Tech Stack:** diodeinc `pcb` CLI; bundled KiCad 10 (`/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`, `…/Frameworks/Python.framework/Versions/3.9/bin/python3` for pcbnew); JLC parts API (`POST https://jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood/selectSmtComponentList`, body `{"keyword":"…","currentPage":1,"pageSize":25}`).

**Safety net:** checkpoint commit `6ed3f1d`. Any step that wipes the layout → `git checkout 6ed3f1d -- hardware/layout/` and stop.

**Gotchas (project memory):** pad double-rotation trap (never edit `(at …)` rotation in raw .kicad_pcb; use pcbnew `SetOrientationDegrees`); `bd.GetTracks()` non-iterable after `bd.Remove()` → snapshot+reload; `GetFootprints()`+`GraphicalItems()` nested iteration → materialize outer with `list()`; QFN paste sub-pads have empty net (skip in collision scans).

---

### Task 0: Resolve concrete parts (subagent, parallel-friendly)

**Files:** none (produces a reference table written to `docs/superpowers/plans/_resolved-parts.md`).

- [ ] **Step 1: Query JLC for Basic MPN+manufacturer for every passive value.** Values: caps 100nF/10nF/15pF/1µF/10µF; resistors 1k/2.2k/5.1k/10k/100k/220R/330R/27R/56R/270R/27k/68k. Confirmed anchors: 100nF=C307331(CL05B104KB54PNC), 10nF=C15195(CL05B103KB5NNNC), 15pF=C1548(0402CG150J500NT), 10µF=C96446(CL10A106MA8NRNC), 1k=C11702(0402WGF1001TCE), 2.2k=C25879(0402WGF2201TCE), 5.1k=C25905, 10k=C25744(0402WGF1002TCE), 100k=C25741, 220R=C25091, 330R=C25104, 12MHz=C9002(X322512MSB4SI). Resolve the rest from UNI-ROYAL `0402WGF` (R) / Samsung `CL05` (C) series. Record MPN + manufacturer string + LCSC# + confirm `componentLibraryType` ∈ {base,basic,preferred}.
- [ ] **Step 2: Resolve explicit parts + footprints/symbols/pinouts:**
  - ESD `PESD5V0S2BT`: confirm LCSC# (C49338), package, and **pin→net mapping** (which SOT-23 pins are I/O vs common GND) from datasheet/symbol. Identify a KiCad symbol + footprint (`@kicad-symbols`/`@kicad-footprints`) usable in a diodeinc `Component(...)`.
  - LDO `AP2112K-3.3TRG1`: confirm Basic + LCSC#; KiCad symbol+footprint for SOT-23-5 (must match U1's existing SOT-23-5 footprint to preserve placement).
  - Basic 0603 LED (blue, green): confirm Basic LCSC#s; generics keep the 0603 footprint.
  - Basic 16P USB-C receptacle (USB2.0): find one with a KiCad-library footprint; record LCSC# + footprint path. **[risky — if none with a clean footprint, flag and keep J2 Extended.]**
  - Basic SMD tact switch (2-pin): find one with a KiCad footprint; record LCSC# + footprint. **[risky — if none clean, keep SW1 Extended.]**
- [ ] **Step 3: Write `_resolved-parts.md`** with every MPN/manufacturer/LCSC#/footprint. This file is the single source the later tasks read from. No commit (planning artifact).

---

### Task 1: Custom RP2040 module (source only — no re-sync yet)

**Files:** Create `hardware/components/RP2040.zen`; Modify `hardware/picoarc.zen:21,24-43`.

- [ ] **Step 1:** Copy the vendored module body from `/Users/naoru/.pcb/cache/github.com/diodeinc/kicad/0.2.2/MCU_RaspberryPi/RP2040.zen` into `hardware/components/RP2040.zen`. Keep the `Component(name="RP2040", …)` chip and ALL support children with identical `name=`, footprint, value, topology (`r_run_pullup`, `r_qss_pullup`, `c_bulk_3v3/1v1`, `c_bypass_3v3_1..4`, `c_bypass_1v1_1..2`, `crystal`+`c_xin`+`c_xout`+`r_xout`+`_R_OSC`, `r_usb_dp/dm`, `tp_swd_io/clk`).
- [ ] **Step 2:** Add `mpn=`/`manufacturer=` (from `_resolved-parts.md`) to every Resistor/Capacitor/Crystal child. Keep values/packages unchanged.
- [ ] **Step 3:** DELETE the `PinHeader(name="pin_socket_gpio", …)` block, the `add_qspi_flash`/`W25Q128JV` block, and the `add_bootsel_button`/`BOOTSEL` block. Hardcode this board's config (qspi external, no flash/bootsel/header, crystal on, swd test points on).
- [ ] **Step 4:** Reduce GPIO io to the 6 used (`gpio2,3,4,6,7,25`); map the chip's other GPIO pins to internal `Net("NC_GPIOxx")` (do NOT expose them). Keep `usb,swd,qspi,RUN,VDD_3V3,VDD_1V1,GND,oscpair` ios.
- [ ] **Step 5:** Edit `picoarc.zen`: `RP2040 = Module("./components/RP2040.zen")`; collapse the instantiation to the kept ios (name="U_MCU", power/gnd, usb/swd/qspi/RUN, and gpio2=ARC_TX, gpio3=CEC_BUS, gpio4=HDMI_5V_SENSE, gpio6=DDC_SDA, gpio7=DDC_SCL, gpio25=STATUS_LED).
- [ ] **Step 6 (verify build):** Run `cd hardware && pcb build picoarc.zen` (or the flow's check command) — expect it to elaborate with no errors. If `pcb build` n/a, `pcb layout --check picoarc.zen` netlist-only. Expected: no missing-pin / unconnected-mandatory errors. **Do NOT commit or re-sync yet.**

---

### Task 2: All-Basic on other modules + LDO/LED swaps (source only)

**Files:** Modify `hardware/modules/HdmiArc.zen`, `Power.zen`, `UsbC.zen`, `Debug.zen`, `hardware/components/W25Q16JVSS.zen`.

- [ ] **Step 1:** Add `mpn=`/`manufacturer=` (Basic, from `_resolved-parts.md`) to every Resistor/Capacitor generic: HdmiArc R1–R11 + C1; Power C2/C3 (+ R13); UsbC R14/R15; Debug R12; W25Q16 C_DEC (C4). Values/packages unchanged.
- [ ] **Step 2:** U1 LDO (in Power.zen): set its `Part(mpn="AP2112K-3.3TRG1", manufacturer="Diodes Incorporated")` (or generic LDO mpn) — **same SOT-23-5 footprint**. Confirm the footprint path is unchanged.
- [ ] **Step 3:** D1/D3 LEDs (Debug.zen, Power.zen): set Basic LED `mpn=`/`manufacturer=`; keep 0603.
- [ ] **Step 4 (verify):** `pcb build picoarc.zen` elaborates clean. No commit yet.

---

### Task 3: Add ARC/HDMI-5V ESD (source only)

**Files:** Modify `hardware/modules/HdmiArc.zen`.

- [ ] **Step 1:** Add a `Component(name="ESD_ARC", symbol=…, footprint=…, pins={…}, part=Part(mpn="PESD5V0S2BT…", manufacturer="Nexperia"))` using the SOT-23 footprint + the verified pin→net mapping from `_resolved-parts.md`: the two I/O channels → `_ARC14` (ARC, connector side, i.e. the J1.14 net) and `_HDMI_5V`; common pin → GND. Keep D2 (PESD5V0L4UF) unchanged.
- [ ] **Step 2 (verify):** `pcb build picoarc.zen` elaborates clean. No commit yet.

---

### Task 4: ★ CRITICAL re-sync + layout-preservation gate

**Files:** regenerates `hardware/layout/layout.kicad_pcb` (+ snapshot/default.net).

- [ ] **Step 1: Record baseline from checkpoint.** `git show 6ed3f1d:hardware/layout/layout.kicad_pcb > /tmp/picoarc/base.kicad_pcb`; via pcbnew count: footprints, tracks, vias, zones, and filled zone area. (Known: 59 fp, ~196 trk, 70 via, 4 zones.)
- [ ] **Step 2: Re-sync.** `cd hardware && pcb layout picoarc.zen`.
- [ ] **Step 3: Compare.** pcbnew-count the new `layout.kicad_pcb`. PASS criteria: zones still 4 (GND/3V3 planes + pours), tracks/vias within ~5% of baseline (existing routing preserved), U_MCU.* footprints still placed at their prior positions, +1 new footprint (ESD_ARC, unplaced/origin). The dropped PinHeader must be absent (was already excluded). J2/SW1 unchanged (their swap is Task 6).
- [ ] **Step 4: GATE.** If zones==0 or tracks collapsed (layout wiped / not preserved): `git checkout 6ed3f1d -- hardware/layout/`, **STOP, write a report** explaining the re-sync did not preserve placement, and do not proceed. If preserved: continue.
- [ ] **Step 5: Re-fill zones + DRC.** pcbnew `ZONE_FILLER(bd).Fill(bd.Zones())`, save; `kicad-cli pcb drc --severity-error`. Expect 0 errors (plus the new ESD_ARC unconnected ratsnest). Confirm `layout.kicad_dru` rules + net classes survived (`VDD_5V`→Power, `USB_D_P`→USB).
- [ ] **Step 6: Commit checkpoint.** `git add -A hardware && git commit -m "hardware: own RP2040 support circuit + all-Basic passives; add ARC/5V ESD (pre-placement)"`.

---

### Task 5: Place + route the new ESD

**Files:** `hardware/layout/layout.kicad_pcb` (pcbnew).

- [ ] **Step 1:** Place `ESD_ARC` near J1 pins 14/18 (≈ x110.5, y106–107), clear of existing copper (use the collision-scan util; skip empty-net pads). `SetPosition`; if rotation needed use `SetOrientationDegrees` (never raw `(at)`).
- [ ] **Step 2:** Route the 3 short connections: I/O→`HDMI._ARC14` (at/near J1.14), I/O→`HDMI._HDMI_5V`, common→GND (via to In1). Keep stubs short; verify each segment with the clearance util before adding (`seg_clear`).
- [ ] **Step 3 (verify):** re-fill zones; `kicad-cli pcb drc --severity-all`. Expect 0 errors; ESD_ARC no longer unconnected. Render top to eyeball.
- [ ] **Step 4: Commit.** `git commit -am "hardware: place + route ARC/HDMI-5V ESD"`.

---

### Task 6: J2/SW1 → Basic (risky; only if Task 0 found clean footprints)

**Files:** `hardware/modules/UsbC.zen`, `hardware/picoarc.zen` (SW_BOOT), then layout.

- [ ] **Step 1: Precondition.** If Task 0 did NOT find Basic J2/SW1 parts with clean KiCad footprints → SKIP this task, keep them Extended (cost ~$6), note in final report. Else continue.
- [ ] **Step 2:** Swap `Part(mpn=…)` + `footprint=File(…)` for J2 (UsbC.zen) and SW1 (picoarc.zen SW_BOOT) to the Basic parts/footprints.
- [ ] **Step 3:** `pcb layout picoarc.zen` re-sync. The two footprints change → they (and their routing) reset.
- [ ] **Step 4:** Re-place J2 at the right board edge (its prior location) and SW1 near its prior spot; re-route their broken nets (VBUS/CC/GND/USB for J2; QSPI_CS/GND for SW1). Re-fill zones.
- [ ] **Step 5 (verify):** `kicad-cli pcb drc --severity-all` → 0 errors. Render. Commit `"hardware: swap J2/SW1 to JLCPCB Basic parts"`.

---

### Task 7: Final verification + report

- [ ] **Step 1: BOM Basic re-audit.** Re-query the JLC API for every distinct MPN now in the design; confirm all are Basic/Preferred except the expected functional Extended set (RP2040 C2040, PCA9306 C123752, W25Q16 C82317, USBLC6 C7519, HDMI connector C916313, ESD PESD5V0S2BT). Report the final Extended count + estimated feeder-fee delta.
- [ ] **Step 2: Full DRC** (`--severity-all`) = 0 errors; report remaining unconnected (TP stubs + unfinished routing).
- [ ] **Step 3: Render** top/bottom/In1/In2; spot-check planes intact, U5 cluster undisturbed, ESD placed.
- [ ] **Step 4:** Update project memory if the re-sync/footprint behavior taught anything new. Write the final summary for the user (what changed, BOM Extended count before/after, any deferred items).
- [ ] **Step 5: Commit** any remaining changes.

---

## Self-review notes
- **Spec coverage:** custom module (T1), picoarc.zen (T1.5), all-Basic other modules (T2), explicit swaps LDO/LED (T2), J2/SW1 (T6), ESD (T3,T5), re-sync+gate (T4), verify/JLC/render (T7). ✓
- **Risk gates:** layout-wipe revert (T4.4); footprint-sourcing skip for J2/SW1 (T6.1). ✓
- **Sequencing:** all source edits (T1–T3) before the single re-sync (T4); risky J2/SW1 (T6) isolated after the core win is committed (T4.6, T5.4) so it can be safely skipped. ✓
- **Detail deferred by design:** exact gap-value MPNs + ESD/USB-C/switch footprints resolved in T0 (live JLC/KiCad lookups) and written to `_resolved-parts.md` — concrete inputs for T1–T6.
