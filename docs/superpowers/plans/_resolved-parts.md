# Resolved parts reference (Task 0)

Research-only output. Resolves concrete LCSC part numbers, manufacturers, footprints,
and pin mappings for the picoarc BOM so later tasks can pin parts to JLCPCB **Basic**
where possible. **No layout/source files were edited.**

- Date: 2026-05-31
- JLC library-type field (`componentLibraryType`): `base`/`basic`/`preferred` = **Basic** (no setup fee); `expand` = **Extended**.
- Method: JLCPCB `selectSmtComponentList` API. The API field was **calibrated** against the task's confirmed anchors (C11702, C25905, C25744 all return `base`) — so `base`/`expand` reported here is trustworthy. Where keyword searches returned 0 Basic, that was cross-checked by direct LCSC-code lookup and by exact-MPN lookup; both methods agree.
- KiCad 9.0.3 libs:
  - footprints: `/Users/naoru/.pcb/cache/gitlab.com/kicad/libraries/kicad-footprints/9.0.3/`
  - symbols: `/Users/naoru/.pcb/cache/gitlab.com/kicad/libraries/kicad-symbols/9.0.3/`
  - In `.zen`, reference these via the un-versioned path prefix `gitlab.com/kicad/libraries/kicad-footprints/<lib>.pretty/<fp>.kicad_mod` and `…/kicad-symbols/<lib>.kicad_sym` (matches existing modules), or `@kicad-footprints/…` / `@kicad-symbols/…` (matches the vendored AP2112K module).

---

## A) Passive Basic parts

### Confirmed anchors (recorded as-is; manufacturer added)
| Value | Pkg | MPN | Manufacturer | LCSC# | Lib |
|---|---|---|---|---|---|
| 100nF | 0402 | CL05B104KB54PNC | Samsung Electro-Mechanics | C307331 | Basic |
| 10nF  | 0402 | CL05B103KB5NNNC | Samsung Electro-Mechanics | C15195  | Basic |
| 15pF C0G | 0402 | 0402CG150J500NT | FH (Fenghua) | C1548 | Basic |
| 10µF  | 0603 | CL10A106MA8NRNC | Samsung Electro-Mechanics | C96446  | Basic |
| 1k    | 0402 1% | 0402WGF1001TCE | UNI-ROYAL (Uniroyal Elec) | C11702 | Basic |
| 2.2k  | 0402 1% | 0402WGF2201TCE | UNI-ROYAL | C25879 | Basic |
| 5.1k  | 0402 1% | 0402WGF5101TCE | UNI-ROYAL | C25905 | Basic |
| 10k   | 0402 1% | 0402WGF1002TCE | UNI-ROYAL | C25744 | Basic |
| 100k  | 0402 1% | 0402WGF1003TCE | UNI-ROYAL | C25741 | Basic |
| 220R  | 0402 1% | 0402WGF2200TCE | UNI-ROYAL | C25091 | Basic |
| 330R  | 0402 1% | 0402WGF3300TCE | UNI-ROYAL | C25104 | Basic |
| 12MHz xtal | 3225-4pin | X322512MSB4SI | YXC (Yangxing Tech) | C9002 | Basic |

### Newly resolved

| Value | Pkg | MPN | Manufacturer | LCSC# | Lib | Stock | Notes |
|---|---|---|---|---|---|---|---|
| **1µF** | 0402 16V X5R | **CL05A105KA5NQNC** | Samsung Electro-Mechanics | **C52923** | **Basic** | ~3.7M | Clean Basic. (Note: the very common C15849/GRM155 1µF 0402 is Extended; use the Samsung Basic part.) |

### Resistors 27R / 56R / 270R / 27k / 68k (0402 1%) — **NOT available as Basic** ⚠️

These five values are **not** in JLC's Basic library. The UNI-ROYAL `0402WGF…` series (same family as the anchors) has only an **Extended** SKU for each of these values, and no other manufacturer offers a Basic 0402 1% part at these values either (verified by exact-MPN lookup and deep paged value searches — all return `expand` only).

| Value | UNI-ROYAL MPN | LCSC# | Lib | Stock |
|---|---|---|---|---|
| 27R   | 0402WGF270JTCE *(see note)* | C25100 | Extended | ~90k |
| 27R   | 0402WGF2700TCE *(if 270×10⁰ reading)* | C25099 | Extended | ~9 (low) |
| 56R   | 0402WGF560JTCE | C25127 | Extended | ~294k |
| 270R  | 0402WGF2700TCE | C25099 | Extended | ~9 (low — avoid) |
| 270R  | 0402WGF2701TCE *(270×10¹)* | C25885 | Extended | ~866k |
| 27k   | 0402WGF2702TCE | C25771 | Extended | ~679k |
| 68k   | 0402WGF6802TCE | C36871 | Extended | ~437k |

> ⚠️ **DECISION NEEDED in a later task.** Options:
> 1. Accept these 5 as Extended (each is one-time JLC setup fee per unique Extended SKU, but stock is plentiful for the high-stock SKUs above).
> 2. Re-spin the affected resistor *values* to E24 values that DO exist as Basic (UNI-ROYAL Basic 0402 1% confirmed-present set includes: 0Ω/jumper, 10, 100, 120, 150, 200, 220, 330, 470, 499, 510, 1k, 1.2k, 1.5k, 2k, 2.2k, 3.3k, 4.7k, 5.1k, 10k, 12k, 15k, 20k, 22k, 33k, 47k, 51k, 100k, 200k…). For example 56R→47R, 270R→220R or 330R, 27k→22k or 33k, 68k→47k+… — **but these are functional analog values (ARC attenuator R4=56Ω, CEC R1=27k, divider R6=68k), so changing them changes circuit behavior and must be re-derived, not blindly swapped.**
> Resolving the exact value-encoding ambiguity for 27R/270R (the JLC MPN suffix `J` vs trailing digit): when this matters, confirm the resistance from the LCSC product page before committing.

---

## B) Explicit components — symbol + footprint + pin map

### 1) ESD — PESD5V0S2BT (Nexperia, SOT-23, 2-line **bidirectional**)

- **LCSC#: C49338** — MPN `PESD5V0S2BT,215`, Nexperia, package SOT-23, **Extended** (`expand`), stock ~26.3k.
  - This is the **allowed Extended exception** per the task.
  - (Cheaper Basic-priced non-Nexperia clones exist as Extended too: BORN C343998, UMW C2687130 — all `expand`. Keep the genuine Nexperia C49338 unless cost-down is wanted.)
- **CRITICAL — exact pinout (from official Nexperia datasheet Table 2, "Pinning information"):**

  | Pin | Symbol | Description |
  |---|---|---|
  | 1 | **K1** | cathode 1 (protected line A) |
  | 2 | **K2** | cathode 2 (protected line B) |
  | 3 | **K** | "double cathode" = **common** node |

  Datasheet limiting-values footnote: clamp paths are **"pin 1 to 3 or pin 2 to 3"**, i.e. each protected line (pin 1, pin 2) clamps bidirectionally against the **common pin 3**.

- **Application mapping (per task intent):**
  - Pin 1 (K1) → **ARC** line
  - Pin 2 (K2) → **HDMI_5V** line
  - Pin 3 (K, common) → **GND**
- **Recommended KiCad symbol:** `SP0502BAHT` in
  `gitlab.com/kicad/libraries/kicad-symbols/Power_Protection.kicad_sym`
  — same SOT-23 3-pin 2-channel ESD topology; its pins are **1=K, 2=K, 3=A**. Map symbol pin 3 (its "A") to the common net → GND, pins 1 & 2 to the two protected lines.
  - (No exact `PESD5V0S2BT` symbol exists in the cached KiCad libs. `SP0502BAHT` is the closest correct 2-line bidirectional SOT-23 ESD symbol. A generic `Device:D_TVS_x2_AAC`/`Diode:` alternative would also work but `SP0502BAHT` is purpose-built.)
- **Footprint:** `gitlab.com/kicad/libraries/kicad-footprints/Package_TO_SOT_SMD.pretty/SOT-23.kicad_mod`
  (3-pad SOT-23; pads numbered 1,2,3. Pad geometry: 1=top-left, 2=bottom-left, 3=right single. `SOT-23-3.kicad_mod` is identical and also acceptable.)
- **Suggested `.zen` pins block** (using SP0502BAHT symbol pin names; both pin-1 and pin-2 are named "K", so reference by **number** to disambiguate):
  ```
  pins={ "1": <line_A/ARC>, "2": <line_B/HDMI_5V>, "3": GND }
  ```

### 2) LDO — AP2112K-3.3TRG1 (Diodes Inc, SOT-23-5, 600mA, enable)

- **LCSC#: C51118** — MPN `AP2112K-3.3TRG1`, Diodes Incorporated, SOT-23-5, stock ~102k.
  - ⚠️ **Library type per API: `expand` (Extended), NOT Basic.** The task expected Basic. Direct code lookup of C51118 confirms `expand`. (Diodes-brand C51118 is the genuine part; brand clones e.g. C23380830 "TECH PUBLIC" are also Extended.) JLC's web UI has historically shown C51118 under "Basic Parts" but the order API reports `expand` today — **flag for confirmation in the assembly step.** No Basic-flagged AP2112K SKU was found.
- **Symbol:** `gitlab.com/kicad/libraries/kicad-symbols/Regulator_Linear.kicad_sym`, name **`AP2112K-3.3`**
  (this is the exact symbol the existing `@kicad/Regulator_Linear/AP2112K-3.3.zen` module already uses — it `(extends "AP2204K-1.5")`).
- **Footprint:** `gitlab.com/kicad/libraries/kicad-footprints/Package_TO_SOT_SMD.pretty/SOT-23-5.kicad_mod`
  (this is the standard SOT-23-5 — **matches the existing U1 footprint**; the vendored module already declares `@kicad-footprints/Package_TO_SOT_SMD.pretty/SOT-23-5.kicad_mod`).
- **Pin function map (verified from symbol; matches Diodes AP2112 datasheet):**

  | Pin | Function |
  |---|---|
  | 1 | **VIN** |
  | 2 | **GND** |
  | 3 | **EN** (active-high enable; tie to VIN for always-on) |
  | 4 | **NC** (no-connect) |
  | 5 | **VOUT** |

  Symbol uses named pins VIN/EN/GND/VOUT (NC unconnected). The existing module ties EN→VIN (AlwaysOn).

### 3) Status / power LEDs (0603)

- ⚠️ **Blue and Green 0603 are NOT available as Basic.** In the JLC Basic LED line (Hubei KENTO `KT-0603` family), only **Red** and **White** are `base`. Blue and Green KENTO parts are Extended, and no other-brand Basic blue/green 0603 surfaced.

  | Color | MPN | Manufacturer | LCSC# | Lib | Stock |
  |---|---|---|---|---|---|
  | **Blue** (status LED, D_STATUS) | KT-0603B | Hubei KENTO Elec | **C2288** | **Extended** | ~43k |
  | **Green** (power-good LED, LED_PWR) | KT-0603G | Hubei KENTO Elec | **C12624** | **Extended** | ~80k |
  | Red *(Basic, for reference)* | KT-0603R | Hubei KENTO | C2286 | Basic | ~7.1M |
  | White *(Basic, for reference)* | KT-0603W | Hubei KENTO | C2290 | Basic | ~2.8M |

  > **DECISION NEEDED:** either (a) keep blue/green as Extended (2 extra Extended SKUs, well-stocked), or (b) switch the two LEDs to **Red** (C2286, Basic) — generics keep the same 0603 footprint, only `color=`/MPN change. Functionally either works for status/power indication.
- Footprint is the standard generic 0603 LED footprint produced by `@stdlib/generics/Led.zen` (`package="0603"`); no special footprint needed — only `mpn`/`manufacturer` pinning matters.

### 4) USB-C 2.0 receptacle, 16-pin

- ⚠️ **No Basic 16P USB-C receptacle found.** Every common 16P USB-C 2.0 receptacle is **Extended** per the API, including:

  | MPN | Manufacturer | LCSC# | Lib | Stock | KiCad footprint match |
  |---|---|---|---|---|---|
  | TYPE-C 16PIN 2MD(073) | SHOU HAN | C2765186 | Extended | ~977k | XKB/GCT-style 16P top-mount (geometry-compatible) |
  | TYPE-C-31-M-12 | Korean Hroparts (HRO) | C165948 | Extended | ~304k | HRO 16P |
  | U262-161N-4BVC11 | XKB Connection | C319148 | Extended | ~38k | `USB_C_Receptacle_XKB_U262-16XN-4BVC11.kicad_mod` (note JLC SKU is **161N** vs footprint **16XN** — verify pad/pin compatibility) |
  | USB4105-GF-A-060 *(current J2)* | GCT | C3025063 | Extended | ~2.5k | `USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal.kicad_mod` |

- **CONCLUSION: could NOT find a Basic USB-C 16P with a clean standard-KiCad footprint.** Per the task instruction, the later task should **keep the current Extended J2** (GCT USB4105 / `USB_C_Receptacle_USB2.0_16P` symbol + `USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal` footprint).
  - If a higher-stock Extended swap is desired (still Extended, not Basic), **SHOU HAN C2765186** is the JLC-popular high-stock choice and is mechanically compatible with the GCT 16P top-mount footprint.
- Reference USB2 pin map for the existing GCT footprint/symbol (verified from `Connector.kicad_sym` `USB_C_Receptacle_USB2.0_16P` and the GCT footprint pads A1/A4/A5/A6/A7/A8/A9/A12/B1/B4/B5/B6/B7/B8/B9/B12/S1):

  | Signal | Symbol pin(s) |
  |---|---|
  | VBUS | A4, A9, B4, B9 |
  | GND | A1, A12, B1, B12 |
  | CC1 | A5 |
  | CC2 | B5 |
  | D+ | A6, B6 (tied) |
  | D− | A7, B7 (tied) |
  | SBU1/SBU2 | A8 / B8 (no-connect for USB2) |
  | SHIELD | S1 |

### 5) SMD tact switch (2-terminal) — ✅ CLEAN BASIC FOUND

- **LCSC#: C318884** — MPN **`TS-1187A-B-A-B`**, **XKB Connection**, package SMD-4P 5.1×5.1mm, **Basic** (`base`), stock **~1.22M**.
- **KiCad footprint:** `gitlab.com/kicad/libraries/kicad-footprints/Button_Switch_SMD.pretty/SW_Push_1P1T_XKB_TS-1187A.kicad_mod`
  — this footprint is **purpose-named for this exact part**. 4 pads = 2 electrical terminals (`1 1 2 2`), a single-pole momentary NO switch.
- **Symbol:** `gitlab.com/kicad/libraries/kicad-symbols/Switch.kicad_sym`, name **`SW_Push`** (same symbol the current SW_BOOT already uses).
- **Pin map (2 nets):** `pins={ "1": <signal e.g. QSPI_CS>, "2": GND }` — identical wiring to the existing `SW_SPST_B3U-1000P` footprint, so it is a drop-in swap (Omron B3U-1000P → XKB TS-1187A-B-A-B) that moves the switch from Extended to **Basic** with a clean standard-KiCad footprint.
- **CONCLUSION: found a clean Basic 2-terminal SMD tact switch with a standard KiCad footprint = YES.**

---

## Summary of Basic/Extended outcomes

| Item | Result |
|---|---|
| 1µF 0402 | **Basic** — C52923 (Samsung CL05A105KA5NQNC) |
| 27R / 56R / 270R / 27k / 68k 0402 1% | **No Basic** — Extended only (decision needed: accept Extended or change values) |
| ESD PESD5V0S2BT | Extended (allowed exception) — C49338; pinout K1/K2/common-K = pin1/pin2/pin3 |
| LDO AP2112K-3.3TRG1 | **Extended per API** (C51118) — expected Basic; flag for confirmation |
| LED blue / green 0603 | **No Basic** — Extended (C2288 / C12624); Red/White are the only Basic |
| USB-C 16P (J2) | **No clean Basic** — keep current Extended J2 |
| SMD tact switch (SW1) | **Basic, clean footprint** — C318884 XKB TS-1187A-B-A-B |
