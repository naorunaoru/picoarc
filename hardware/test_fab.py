import csv
import sys
import unittest
import tempfile
import json as _json
from pathlib import Path as _Path

import fab


class StampTest(unittest.TestCase):
    def test_clean_stamp(self):
        self.assertEqual(fab.compute_stamp("v0.0.0", "cc0bb70", False), "v0.0.0-cc0bb70")

    def test_dirty_stamp_appends_suffix(self):
        self.assertEqual(fab.compute_stamp("v0.0.0", "cc0bb70", True), "v0.0.0-cc0bb70-dirty")

    def test_output_names(self):
        names = fab.output_names("v0.0.0-cc0bb70", _Path("/out"))
        self.assertEqual(names["gerber_zip"], _Path("/out/picoarc-v0.0.0-cc0bb70-gerbers.zip"))
        self.assertEqual(names["bom"], _Path("/out/picoarc-v0.0.0-cc0bb70-bom.csv"))
        self.assertEqual(names["cpl"], _Path("/out/picoarc-v0.0.0-cc0bb70-cpl.csv"))
        self.assertEqual(names["drc"], _Path("/out/drc-report.json"))


class OverlayTest(unittest.TestCase):
    SAMPLE = (
        "# comment line\n"
        "mpn,RP2040,C2040\n"
        'mpn,"PESD5V0S2BT,215",C49338\n'
        "ref,R5,C9999\n"
        "\n"
    )

    def test_load_splits_mpn_and_ref(self):
        mpn_map, ref_map = fab.load_lcsc_overlay(self.SAMPLE)
        self.assertEqual(mpn_map, {"RP2040": "C2040", "PESD5V0S2BT,215": "C49338"})
        self.assertEqual(ref_map, {"R5": "C9999"})

    def test_load_rejects_unknown_kind(self):
        with self.assertRaises(ValueError):
            fab.load_lcsc_overlay("widget,foo,C1\n")

    def test_resolve_ref_override_beats_mpn(self):
        mpn_map, ref_map = fab.load_lcsc_overlay(self.SAMPLE)
        self.assertEqual(fab.resolve_lcsc("R5", "RP2040", mpn_map, ref_map), "C9999")

    def test_resolve_by_mpn(self):
        mpn_map, ref_map = fab.load_lcsc_overlay(self.SAMPLE)
        self.assertEqual(fab.resolve_lcsc("U5", "RP2040", mpn_map, ref_map), "C2040")

    def test_resolve_unmapped_returns_empty(self):
        self.assertEqual(fab.resolve_lcsc("R1", "NOPE", {}, {}), "")


class BomTest(unittest.TestCase):
    ROWS = [
        {"designator": "R10", "mpn": "RES1", "package": "0402", "value": "100"},
        {"designator": "R2", "mpn": "RES1", "package": "0402", "value": "100"},
        {"designator": "U5", "mpn": "RP2040", "package": "QFN-56", "value": "RP2040"},
        {"designator": "C1", "mpn": "NOPART", "package": "0402", "value": "10nF"},
    ]
    MAP = {"RES1": "C100", "RP2040": "C2040"}

    def test_groups_identical_parts_and_natsorts_designators(self):
        rows, unmapped = fab.build_bom(self.ROWS, self.MAP, {})
        res = next(r for r in rows if r["LCSC Part #"] == "C100")
        self.assertEqual(res["Designator"], "R2,R10")   # natural order, not R10,R2
        self.assertEqual(res["Comment"], "100")
        self.assertEqual(res["Footprint"], "0402")

    def test_unmapped_part_listed_not_dropped(self):
        rows, unmapped = fab.build_bom(self.ROWS, self.MAP, {})
        self.assertIn("C1", unmapped)
        c1 = next(r for r in rows if "C1" in r["Designator"])
        self.assertEqual(c1["LCSC Part #"], "")        # present, blank LCSC

    def test_columns_exact(self):
        rows, _ = fab.build_bom(self.ROWS, self.MAP, {})
        self.assertEqual(list(rows[0].keys()), ["Comment", "Designator", "Footprint", "LCSC Part #"])


class CplTest(unittest.TestCase):
    POS = (
        "Ref,Val,Package,PosX,PosY,Rot,Side\n"
        '"TP1","?","TestPad",134.0,-114.2,180.0,bottom\n'
        '"U5","RP2040","QFN",120.0,-110.0,90.0,top\n'
        '"R2","100","0402",100.5,-90.0,0.0,bottom\n'
    )

    def test_filters_to_placed_set(self):
        rows = fab.remap_cpl(self.POS, {"U5", "R2"})
        refs = [r["Designator"] for r in rows]
        self.assertEqual(refs, ["R2", "U5"])           # TP1 dropped, natural-sorted
        self.assertNotIn("TP1", refs)

    def test_column_mapping_and_side_normalization(self):
        rows = fab.remap_cpl(self.POS, {"U5"})
        self.assertEqual(rows[0], {
            "Designator": "U5", "Mid X": "120.0", "Mid Y": "-110.0",
            "Layer": "Top", "Rotation": "90.0",
        })

    def test_bottom_maps_to_capitalized(self):
        rows = fab.remap_cpl(self.POS, {"R2"})
        self.assertEqual(rows[0]["Layer"], "Bottom")


class ResolveCliTest(unittest.TestCase):
    def test_env_override_wins(self):
        got = fab.resolve_kicad_cli(environ={"KICAD_CLI": "/custom/kc"},
                                    which=lambda _: None, exists=lambda _: False)
        self.assertEqual(got, "/custom/kc")

    def test_bundled_path_when_present(self):
        got = fab.resolve_kicad_cli(environ={}, which=lambda _: None,
                                    exists=lambda p: p == fab.BUNDLED_KICAD_CLI)
        self.assertEqual(got, fab.BUNDLED_KICAD_CLI)

    def test_path_fallback(self):
        got = fab.resolve_kicad_cli(environ={}, which=lambda _: "/usr/bin/kicad-cli",
                                    exists=lambda _: False)
        self.assertEqual(got, "/usr/bin/kicad-cli")

    def test_missing_raises(self):
        with self.assertRaises(SystemExit):
            fab.resolve_kicad_cli(environ={}, which=lambda _: None, exists=lambda _: False)


class PcbVersionTest(unittest.TestCase):
    def test_reads_text_variable(self):
        with tempfile.TemporaryDirectory() as d:
            p = _Path(d) / "x.kicad_pro"
            p.write_text(_json.dumps({"text_variables": {"PCB_VERSION": "v1.2.3"}}))
            self.assertEqual(fab.read_pcb_version(p), "v1.2.3")

    def test_defaults_when_absent(self):
        with tempfile.TemporaryDirectory() as d:
            p = _Path(d) / "x.kicad_pro"
            p.write_text("{}")
            self.assertEqual(fab.read_pcb_version(p), "v0.0.0")


import subprocess as _sp
import zipfile as _zip


class IntegrationSmokeTest(unittest.TestCase):
    """Runs the real pipeline with --skip-drc so an in-progress board still exercises export."""

    def test_make_fab_skip_drc_produces_artifacts(self):
        with tempfile.TemporaryDirectory() as d:
            proc = _sp.run(
                [sys.executable, str(_Path(fab.__file__)), "--skip-drc", "--output", d],
                capture_output=True, text=True,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            out = _Path(d)
            zips = list(out.glob("picoarc-*-gerbers.zip"))
            boms = list(out.glob("picoarc-*-bom.csv"))
            cpls = list(out.glob("picoarc-*-cpl.csv"))
            self.assertEqual(len(zips), 1, "exactly one gerber zip")
            self.assertEqual(len(boms), 1)
            self.assertEqual(len(cpls), 1)
            # zip carries copper + paste + edge + drill
            with _zip.ZipFile(zips[0]) as zf:
                names = zf.namelist()
            self.assertTrue(any(n.endswith((".gtl", "-F_Cu.gtl")) or "F_Cu" in n for n in names), names)
            self.assertTrue(any(n.lower().endswith(".drl") for n in names), names)
            # CPL is filtered to placed parts (51), not the 65 pos rows
            with cpls[0].open() as fh:
                cpl_rows = list(csv.DictReader(fh))
            self.assertEqual(len(cpl_rows), 51)
            self.assertNotIn("TP1", {r["Designator"] for r in cpl_rows})
            self.assertEqual(list(cpl_rows[0].keys()), fab.CPL_FIELDS)


if __name__ == "__main__":
    unittest.main()
