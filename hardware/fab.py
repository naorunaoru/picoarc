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
