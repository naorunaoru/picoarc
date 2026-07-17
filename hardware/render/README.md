# PicoARC product renders

The render pipeline treats `hardware/layout/layout.kicad_pcb` as the physical
source of truth. This is intentional: Zener owns the schematic/netlist, while
the checked-in KiCad PCB contains the hand-routed placement, tracks, zones,
board outline, and silkscreen used in the images.

Run from the repository root:

```sh
./hardware/render/render_pcb.py
```

The command exports the current routed PCB to a temporary STEP assembly,
refreshes the board inside `picoarc-hero.blend`, reapplies the pinned materials
and exact component models, and renders all six camera views to:

```text
assets/camera-studies/neutral-shadow-transparent/
```

The adjacent `render-manifest.json` uses repository-relative source paths and
output paths relative to the manifest. Generated PNGs and optionally saved
Blender scenes are stripped of host-specific path and runtime metadata.

The pinned default is:

- transparent RGBA background
- Neutral Shadow solder-mask/copper treatment
- neutral-loop product lighting
- Cycles with 64 samples and denoising
- 1400 x 788 PNG output
- the six named camera angles stored in the Blender template

Useful development overrides:

```sh
# Fast check of the first and HDMI views
./hardware/render/render_pcb.py --angles 1,3 --samples 16

# Render the optional charcoal-background variant
./hardware/render/render_pcb.py --background charcoal

# Save the refreshed Blender scene for inspection
./hardware/render/render_pcb.py --save-blend /tmp/picoarc-refreshed.blend
```

`render-config.json` pins the visual preset, output names, camera set, and the
baseline KiCad poses used to move the calibrated exact STEP replacements. If an
exact component is moved or rotated in KiCad, no Blender edit is necessary. If
one is moved to the opposite PCB side or its footprint/model origin changes,
the command stops and asks for that model to be recalibrated instead of
silently producing a wrong render.

Tool discovery uses `PATH` first and then the standard macOS application paths.
Use `--kicad-cli`, `--blender`, `KICAD_CLI`, or `BLENDER` to override them.
