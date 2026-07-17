#!/usr/bin/env python3
"""Export the routed KiCad PCB and render the pinned PicoARC camera set."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
DEFAULT_CONFIG = SCRIPT_DIR / "render-config.json"
DEFAULT_KICAD = Path("/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
DEFAULT_BLENDER = Path("/Applications/Blender.app/Contents/MacOS/Blender")


def _resolve_tool(explicit: str | None, env_name: str, command: str, fallback: Path) -> Path:
    requested = explicit or os.environ.get(env_name)
    if requested:
        path = Path(requested).expanduser().resolve()
        if path.is_file():
            return path
        raise FileNotFoundError(f"{env_name} points to a missing executable: {path}")

    on_path = shutil.which(command)
    if on_path:
        return Path(on_path).resolve()
    if fallback.is_file():
        return fallback
    raise FileNotFoundError(
        f"Could not find {command}. Pass --{command.replace('_', '-')} or set {env_name}."
    )


def _repo_path(value: str) -> Path:
    path = Path(value)
    return path.resolve() if path.is_absolute() else (REPO_ROOT / path).resolve()


def _run(command: list[str], *, dry_run: bool, expected_output: Path | None = None) -> None:
    print("+", " ".join(command), flush=True)
    if dry_run:
        return
    completed = subprocess.run(
        command,
        cwd=REPO_ROOT,
        check=False,
        text=True,
        stdout=subprocess.PIPE if expected_output else None,
        stderr=subprocess.STDOUT if expected_output else None,
    )
    if completed.stdout:
        print(completed.stdout, end="" if completed.stdout.endswith("\n") else "\n", flush=True)
    if completed.returncode == 0:
        return
    # KiCad returns 2 when a valid STEP was written but VRML-only component
    # models could not be embedded. Those models are replaced by calibrated
    # native STEP objects in Blender. Never relax failures without an artifact.
    known_vrml_omission = bool(
        completed.stdout
        and "Cannot use VRML models when exporting to non-mesh formats." in completed.stdout
        and "STEP file '" in completed.stdout
        and "created." in completed.stdout
        and "Error" not in completed.stdout
    )
    if (
        known_vrml_omission
        and expected_output
        and expected_output.is_file()
        and expected_output.stat().st_size > 1024
    ):
        print(
            f"warning: command returned {completed.returncode}, but produced "
            f"{expected_output} ({expected_output.stat().st_size} bytes); continuing",
            flush=True,
        )
        return
    raise subprocess.CalledProcessError(completed.returncode, command)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Render the six pinned PicoARC views from hardware/layout/layout.kicad_pcb."
    )
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--pcb", type=Path, help="Override the routed .kicad_pcb input.")
    parser.add_argument("--output-dir", type=Path, help="Override the output directory.")
    parser.add_argument("--samples", type=int, help="Override Cycles samples (default: preset value).")
    parser.add_argument(
        "--angles",
        default="all",
        help="Comma-separated camera ids (for example 1,3,4); default: all six.",
    )
    parser.add_argument(
        "--background",
        choices=("transparent", "charcoal"),
        help="Override the preset background. Transparent is the pinned default.",
    )
    parser.add_argument("--save-blend", type=Path, help="Optionally save the refreshed scene for inspection.")
    parser.add_argument("--kicad-cli", help="Path to kicad-cli (or set KICAD_CLI).")
    parser.add_argument("--blender", help="Path to Blender (or set BLENDER).")
    parser.add_argument("--keep-intermediate", type=Path, help="Copy the generated STEP and placement CSV here.")
    parser.add_argument("--dry-run", action="store_true", help="Print commands without running them.")
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    config_path = args.config.expanduser().resolve()
    config = json.loads(config_path.read_text())

    pcb = args.pcb.expanduser().resolve() if args.pcb else _repo_path(config["pcb"])
    template = _repo_path(config["template"])
    output_dir = (
        args.output_dir.expanduser().resolve()
        if args.output_dir
        else _repo_path(config["output_dir"])
    )
    samples = args.samples or int(config["render"]["samples"])
    background = args.background or config["render"]["background"]
    save_blend = args.save_blend.expanduser().resolve() if args.save_blend else None

    if not pcb.is_file():
        raise FileNotFoundError(f"Routed PCB not found: {pcb}")
    if not template.is_file():
        raise FileNotFoundError(f"Blender template not found: {template}")
    if samples < 1:
        raise ValueError("--samples must be positive")

    kicad = _resolve_tool(args.kicad_cli, "KICAD_CLI", "kicad-cli", DEFAULT_KICAD)
    blender = _resolve_tool(args.blender, "BLENDER", "blender", DEFAULT_BLENDER)
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"PCB source: {pcb}")
    print(f"Preset: Neutral Shadow / {background} / Cycles {samples} samples")
    print(f"Output: {output_dir}")

    # KiCad 9 on macOS can create a STEP in /var/folders and still return 2
    # while querying extended file attributes. /private/tmp avoids that bug.
    temp_parent = Path("/private/tmp") if Path("/private/tmp").is_dir() else None
    with tempfile.TemporaryDirectory(prefix="picoarc-render-", dir=temp_parent) as temp_name:
        temp_dir = Path(temp_name)
        step_path = temp_dir / "picoarc-layout.step"
        positions_path = temp_dir / "picoarc-positions.csv"
        success_path = temp_dir / "blender-success.json"
        # Work around KiCad's macOS attribute lookup on a not-yet-existing
        # output file. --force then replaces this empty placeholder normally.
        if not args.dry_run:
            step_path.touch()

        export_step = [
            str(kicad),
            "pcb",
            "export",
            "step",
            "--force",
            "--subst-models",
            "--include-tracks",
            "--include-pads",
            "--include-zones",
            "--include-inner-copper",
            "--include-silkscreen",
            "--include-soldermask",
            "--output",
            str(step_path),
            str(pcb),
        ]
        export_positions = [
            str(kicad),
            "pcb",
            "export",
            "pos",
            "--format",
            "csv",
            "--units",
            "mm",
            "--side",
            "both",
            "--output",
            str(positions_path),
            str(pcb),
        ]
        _run(export_step, dry_run=args.dry_run, expected_output=step_path)
        _run(export_positions, dry_run=args.dry_run)

        if args.keep_intermediate and not args.dry_run:
            keep = args.keep_intermediate.expanduser().resolve()
            keep.mkdir(parents=True, exist_ok=True)
            shutil.copy2(step_path, keep / step_path.name)
            shutil.copy2(positions_path, keep / positions_path.name)

        blender_command = [
            str(blender),
            "--background",
            str(template),
            "--python",
            str(SCRIPT_DIR / "blender_render.py"),
            "--",
            "--step",
            str(step_path),
            "--positions",
            str(positions_path),
            "--pcb",
            str(pcb),
            "--config",
            str(config_path),
            "--repo-root",
            str(REPO_ROOT),
            "--output-dir",
            str(output_dir),
            "--samples",
            str(samples),
            "--background",
            background,
            "--angles",
            args.angles,
            "--success-file",
            str(success_path),
        ]
        if save_blend:
            save_blend.parent.mkdir(parents=True, exist_ok=True)
            blender_command.extend(("--save-blend", str(save_blend)))
        _run(blender_command, dry_run=args.dry_run)
        if not args.dry_run and not success_path.is_file():
            raise RuntimeError(
                "Blender exited without completing the render pipeline; inspect the traceback above"
            )

    if not args.dry_run:
        print(f"Rendered PicoARC camera set to {output_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
