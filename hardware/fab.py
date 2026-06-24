#!/usr/bin/env python3
"""Export a JLCPCB assembly package (Gerbers+drill, BOM, CPL) from the picoarc board."""
import argparse
import csv
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HARDWARE = ROOT / "hardware"
BOARD = HARDWARE / "layout" / "layout.kicad_pcb"
PROJECT = HARDWARE / "layout" / "layout.kicad_pro"
ZEN = HARDWARE / "picoarc.zen"
OVERLAY = HARDWARE / "jlcpcb-lcsc.csv"
DEFAULT_OUTPUT = HARDWARE / "fab"

BUNDLED_KICAD_CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
GERBER_LAYERS = "F.Cu,In1.Cu,In2.Cu,B.Cu,F.SilkS,B.SilkS,F.Mask,B.Mask,F.Paste,B.Paste,Edge.Cuts"


def resolve_kicad_cli(environ=os.environ, which=shutil.which, exists=os.path.exists):
    if environ.get("KICAD_CLI"):
        return environ["KICAD_CLI"]
    if exists(BUNDLED_KICAD_CLI):
        return BUNDLED_KICAD_CLI
    found = which("kicad-cli")
    if found:
        return found
    raise SystemExit(
        "kicad-cli not found. Set $KICAD_CLI, install KiCad 10, "
        f"or ensure {BUNDLED_KICAD_CLI} exists."
    )


def read_pcb_version(pro_path):
    data = json.loads(Path(pro_path).read_text())
    return data.get("text_variables", {}).get("PCB_VERSION", "v0.0.0")


def git_short_hash(repo):
    return subprocess.run(
        ["git", "-C", str(repo), "rev-parse", "--short", "HEAD"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()


def tree_dirty(repo, paths):
    out = subprocess.run(
        ["git", "-C", str(repo), "status", "--porcelain", "--", *map(str, paths)],
        capture_output=True, text=True, check=True,
    ).stdout
    return bool(out.strip())


def compute_stamp(version, short_hash, dirty):
    stamp = f"{version}-{short_hash}"
    if dirty:
        stamp += "-dirty"
    return stamp


def output_names(stamp, out_dir):
    base = f"picoarc-{stamp}"
    return {
        "gerber_zip": out_dir / f"{base}-gerbers.zip",
        "bom": out_dir / f"{base}-bom.csv",
        "cpl": out_dir / f"{base}-cpl.csv",
        "drc": out_dir / "drc-report.json",
    }


def load_lcsc_overlay(text):
    mpn_map, ref_map = {}, {}
    for row in csv.reader(io.StringIO(text)):
        if not row or row[0].lstrip().startswith("#"):
            continue
        if len(row) < 3:
            raise ValueError(f"bad overlay row (need kind,key,lcsc): {row!r}")
        kind, key, lcsc = row[0].strip(), row[1].strip(), row[2].strip()
        if kind == "mpn":
            mpn_map[key] = lcsc
        elif kind == "ref":
            ref_map[key] = lcsc
        else:
            raise ValueError(f"unknown kind {kind!r} (expected 'mpn' or 'ref'): {row!r}")
    return mpn_map, ref_map


def resolve_lcsc(designator, mpn, mpn_map, ref_map):
    if designator in ref_map:
        return ref_map[designator]
    if mpn and mpn in mpn_map:
        return mpn_map[mpn]
    return ""


def natkey(ref):
    m = re.match(r"([A-Za-z]+)(\d+)$", ref)
    return (m.group(1), int(m.group(2))) if m else (ref, 0)


CPL_FIELDS = ["Designator", "Mid X", "Mid Y", "Layer", "Rotation"]


def remap_cpl(pos_text, placed):
    out = []
    for row in csv.DictReader(io.StringIO(pos_text)):
        ref = row["Ref"]
        if ref not in placed:
            continue
        layer = "Top" if row["Side"].strip().lower() == "top" else "Bottom"
        out.append({
            "Designator": ref,
            "Mid X": row["PosX"],
            "Mid Y": row["PosY"],
            "Layer": layer,
            "Rotation": row["Rot"],
        })
    out.sort(key=lambda x: natkey(x["Designator"]))
    return out


def build_bom(bom_rows, mpn_map, ref_map):
    groups = {}
    unmapped = []
    for r in bom_rows:
        ref = r["designator"]
        lcsc = resolve_lcsc(ref, r.get("mpn", ""), mpn_map, ref_map)
        if not lcsc:
            unmapped.append(ref)
        groups.setdefault((r.get("value", ""), r.get("package", ""), lcsc), []).append(ref)
    jlc_rows = []
    for (value, package, lcsc), refs in groups.items():
        jlc_rows.append({
            "Comment": value,
            "Designator": ",".join(sorted(refs, key=natkey)),
            "Footprint": package,
            "LCSC Part #": lcsc,
        })
    jlc_rows.sort(key=lambda x: natkey(x["Designator"].split(",")[0]))
    return jlc_rows, sorted(unmapped, key=natkey)
