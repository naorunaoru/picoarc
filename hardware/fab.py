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


def run(cmd, **kw):
    print("+ " + " ".join(str(p) for p in cmd), flush=True)
    return subprocess.run(cmd, **kw)


def run_drc(kicad, board, report_path):
    """Return the kicad-cli return code (nonzero => violations)."""
    return run([kicad, "pcb", "drc", "--format", "json", "-o", str(report_path),
                "--severity-all", "--exit-code-violations", "--units", "mm",
                str(board)]).returncode


def summarize_drc(report_path):
    data = json.loads(Path(report_path).read_text())
    items = list(data.get("violations", [])) + list(data.get("unconnected_items", []))
    counts = {}
    for v in items:
        counts[v.get("type", "?")] = counts.get(v.get("type", "?"), 0) + 1
    lines = [f"  {n:4d}  {t}" for t, n in sorted(counts.items(), key=lambda x: -x[1])]
    return len(items), "\n".join(lines)


def export_gerbers(kicad, board, out_dir, git_hash):
    run([kicad, "pcb", "export", "gerbers", "-o", str(out_dir),
         "--layers", GERBER_LAYERS, "--check-zones",
         "-D", f"PCB_GIT_HASH={git_hash}", str(board)], check=True)


def export_drill(kicad, board, out_dir):
    run([kicad, "pcb", "export", "drill", "-o", str(out_dir) + os.sep,
         "--format", "excellon", "--excellon-units", "mm",
         "--drill-origin", "absolute", str(board)], check=True)


def export_pos(kicad, board, csv_path):
    run([kicad, "pcb", "export", "pos", "--format", "csv", "--units", "mm",
         "-o", str(csv_path), str(board)], check=True)


def run_pcb_bom(zen):
    proc = run(["pcb", "bom", "--format", "json", str(zen)],
               capture_output=True, text=True, check=True, cwd=str(HARDWARE))
    return json.loads(proc.stdout)


def write_csv(path, fieldnames, rows):
    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)


def zip_dir(src_dir, zip_path):
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for f in sorted(Path(src_dir).iterdir()):
            if f.is_file():
                zf.write(f, f.name)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="fab", description="Export a JLCPCB assembly package.")
    ap.add_argument("--output", default=str(DEFAULT_OUTPUT), help="output directory")
    ap.add_argument("--skip-drc", action="store_true", help="skip the strict DRC gate (logged)")
    ap.add_argument("--gerbers-only", action="store_true", help="skip BOM + CPL")
    ap.add_argument("--no-zip", action="store_true", help="leave loose gerber/drill files")
    args = ap.parse_args(argv)

    kicad = resolve_kicad_cli()
    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)

    git_hash = git_short_hash(ROOT)
    dirty = tree_dirty(ROOT, [HARDWARE])
    version = read_pcb_version(PROJECT)
    stamp = compute_stamp(version, git_hash, dirty)
    names = output_names(stamp, out_dir)
    if dirty:
        print(f"WARN: working tree dirty — silk hash {git_hash} may not reflect the plotted "
              f"layout; outputs tagged -dirty", flush=True)

    # 0. DRC gate
    if args.skip_drc:
        print("WARN: DRC skipped (--skip-drc)", flush=True)
    else:
        if run_drc(kicad, BOARD, names["drc"]) != 0:
            total, summary = summarize_drc(names["drc"])
            print(f"\nDRC FAILED: {total} violation(s):\n{summary}\n"
                  f"See {names['drc']}. Fix the board or re-run with --skip-drc.", flush=True)
            return 1

    # 1. Gerbers + drill -> zip
    with tempfile.TemporaryDirectory() as tmp:
        export_gerbers(kicad, BOARD, tmp, git_hash)
        export_drill(kicad, BOARD, tmp)
        if args.no_zip:
            for f in Path(tmp).iterdir():
                shutil.copy(f, out_dir / f.name)
        else:
            zip_dir(tmp, names["gerber_zip"])

    if not args.gerbers_only:
        # 2. BOM
        mpn_map, ref_map = load_lcsc_overlay(OVERLAY.read_text())
        bom_rows = run_pcb_bom(ZEN)
        jlc_bom, unmapped = build_bom(bom_rows, mpn_map, ref_map)
        write_csv(names["bom"], ["Comment", "Designator", "Footprint", "LCSC Part #"], jlc_bom)
        placed = {r["designator"] for r in bom_rows}
        if unmapped:
            print(f"WARN: {len(unmapped)} part(s) need an LCSC mapping: "
                  f"{', '.join(unmapped)}", flush=True)

        # 3. CPL (filtered to placed parts)
        with tempfile.TemporaryDirectory() as tmp:
            pos_csv = Path(tmp) / "pos.csv"
            export_pos(kicad, BOARD, pos_csv)
            cpl_rows = remap_cpl(pos_csv.read_text(), placed)
        write_csv(names["cpl"], CPL_FIELDS, cpl_rows)

    print(f"\nFab package written to {out_dir} (stamp {stamp})", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
