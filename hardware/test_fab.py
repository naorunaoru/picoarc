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


class BomIdentityTest(unittest.TestCase):
    """Parts group by orderable identity (MPN), not by cosmetic value/package."""

    def test_distinct_unmapped_parts_with_empty_value_package_do_not_merge(self):
        # The real bug: D2 (ESD), Q1 (MOSFET), SW1 (switch) all have empty value+package
        # and no LCSC, but DIFFERENT mpns — they must NOT collapse into one row.
        rows = [
            {"designator": "D2", "mpn": "PESD5V0L4UF", "value": "", "package": ""},
            {"designator": "Q1", "mpn": "2N7002", "value": "", "package": ""},
            {"designator": "SW1", "mpn": "TS-1088", "value": "", "package": ""},
        ]
        fps = {"D2": "SOT-886", "Q1": "SOT-23", "SW1": "SW-SMD"}
        jlc, unmapped = fab.build_bom(rows, {}, {}, fps)
        self.assertEqual(len(jlc), 3)
        self.assertEqual(sorted(r["Designator"] for r in jlc), ["D2", "Q1", "SW1"])
        self.assertEqual(sorted(unmapped), ["D2", "Q1", "SW1"])

    def test_empty_value_falls_back_to_mpn_and_empty_package_to_pos_footprint(self):
        rows = [{"designator": "U5", "mpn": "RP2040", "value": "", "package": ""}]
        jlc, _ = fab.build_bom(rows, {"RP2040": "C2040"}, {}, {"U5": "QFN-56-1EP"})
        self.assertEqual(jlc[0]["Comment"], "RP2040")        # value empty -> mpn
        self.assertEqual(jlc[0]["Footprint"], "QFN-56-1EP")  # package empty -> pos footprint
        self.assertEqual(jlc[0]["LCSC Part #"], "C2040")

    def test_same_mpn_different_value_strings_merge_into_one_line(self):
        # R4 "1k" and R23 "1k 5%" share one MPN -> the same orderable part -> one line.
        rows = [
            {"designator": "R4", "mpn": "RMPN", "value": "1k", "package": "0402"},
            {"designator": "R23", "mpn": "RMPN", "value": "1k 5%", "package": "0402"},
        ]
        jlc, _ = fab.build_bom(rows, {}, {}, {})
        self.assertEqual(len(jlc), 1)
        self.assertEqual(jlc[0]["Designator"], "R4,R23")
        self.assertIn(jlc[0]["Comment"], ("1k", "1k 5%"))    # a representative value
        self.assertEqual(jlc[0]["Footprint"], "0402")


class PosFootprintsTest(unittest.TestCase):
    def test_maps_designator_to_package_column(self):
        pos = (
            "Ref,Val,Package,PosX,PosY,Rot,Side\n"
            '"U5","RP2040","QFN-56-1EP",1,2,0,top\n'
            '"D2","?","SOT-886",3,4,0,bottom\n'
        )
        self.assertEqual(fab.pos_footprints(pos), {"U5": "QFN-56-1EP", "D2": "SOT-886"})


class RotationCorrectionTest(unittest.TestCase):
    def test_load_splits_fp_and_ref_rules(self):
        text = "# comment\nfp,^SOT-23,-90\nfp,^VSSOP-8_,180\nref,U5,270\n\n"
        fp, ref = fab.load_rotations(text)
        self.assertEqual(fp, [("^SOT-23", -90.0), ("^VSSOP-8_", 180.0)])
        self.assertEqual(ref, {"U5": 270.0})

    def test_load_rejects_unknown_kind(self):
        with self.assertRaises(ValueError):
            fab.load_rotations("xx,foo,90\n")

    def test_no_rule_leaves_rotation_string_untouched(self):
        self.assertEqual(fab.correct_rotation("90.000000", "J9", "Some_FP", [], {}), "90.000000")

    def test_fp_regex_applied_and_normalized_to_0_360(self):
        fp = [("^SOT-23", -90.0)]
        self.assertEqual(fab.correct_rotation("0.0", "U3", "SOT-23-6", fp, {}), "270")    # 0-90 -> 270
        self.assertEqual(fab.correct_rotation("180.0", "U2", "SOT-23-5", fp, {}), "90")   # 180-90 -> 90

    def test_ref_override_beats_fp_rule(self):
        fp = [("^QFN", 90.0)]
        self.assertEqual(fab.correct_rotation("-90", "U5", "QFN-56", fp, {"U5": 270.0}), "180")

    def test_first_matching_fp_rule_wins(self):
        fp = [("^VSSOP-8_3.0x3.0mm", 270.0), ("^VSSOP-8_", 180.0)]
        self.assertEqual(fab.correct_rotation("0", "U1", "VSSOP-8_3.0x3.0mm_P0.65mm", fp, {}), "270")
        self.assertEqual(fab.correct_rotation("0", "U1", "VSSOP-8_2.3x2mm_P0.5mm", fp, {}), "180")

    def test_remap_cpl_applies_correction_using_package_column(self):
        pos = ("Ref,Val,Package,PosX,PosY,Rot,Side\n"
               '"U2","?","SOT-23-5",1,2,180,top\n')
        rows = fab.remap_cpl(pos, {"U2"}, [("^SOT-23", -90.0)], {})
        self.assertEqual(rows[0]["Rotation"], "90")


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


class SummarizeDrcTest(unittest.TestCase):
    """Exercises summarize_drc against a synthetic KiCad-10 DRC JSON fixture."""

    def _write_report(self, tmpdir, violations, unconnected_items):
        p = _Path(tmpdir) / "drc-report.json"
        p.write_text(_json.dumps({
            "violations": violations,
            "unconnected_items": unconnected_items,
        }))
        return p

    def test_mixed_violations_and_unconnected(self):
        """Correct total and per-type counts for a report with multiple violation types."""
        with tempfile.TemporaryDirectory() as d:
            report = self._write_report(d, [
                {"type": "clearance", "description": "Clearance violation"},
                {"type": "clearance", "description": "Another clearance"},
                {"type": "silk_over_copper", "description": "Silk over copper"},
            ], [
                {"type": "unconnected_items", "description": "Net not connected"},
                {"type": "unconnected_items", "description": "Another unconnected"},
            ])
            total, summary = fab.summarize_drc(report)
            self.assertEqual(total, 5)
            # Each type's count must appear in the summary
            self.assertIn("clearance", summary)
            self.assertIn("silk_over_copper", summary)
            self.assertIn("unconnected_items", summary)
            # clearance appears 2 times — its count must be in the summary
            self.assertIn("2", summary)
            # silk_over_copper and unconnected_items appear once / twice
            self.assertIn("1", summary)

    def test_clean_report_returns_zero(self):
        """Empty violations and unconnected_items produce total 0 and empty summary."""
        with tempfile.TemporaryDirectory() as d:
            report = self._write_report(d, [], [])
            total, summary = fab.summarize_drc(report)
            self.assertEqual(total, 0)
            self.assertEqual(summary, "")


import subprocess as _sp
import zipfile as _zip


class ToolFailureTest(unittest.TestCase):
    """main() must return 1 and print a clean message (no traceback) on tool failure."""

    def test_tool_failure_returns_1_and_prints_stderr(self):
        """Simulate a failing kicad-cli call; main must catch it and return 1."""
        import io as _io
        import unittest.mock as _mock

        err_output = _io.StringIO()
        fake_error = _sp.CalledProcessError(
            returncode=1,
            cmd=["kicad-cli", "pcb", "export", "gerbers"],
            stderr="kicad-cli: error: no board file found\n",
        )

        with tempfile.TemporaryDirectory() as d:
            with _mock.patch.object(fab, "export_gerbers", side_effect=fake_error), \
                 _mock.patch("sys.stderr", err_output):
                rc = fab.main(["--skip-drc", "--output", d])

        self.assertEqual(rc, 1)
        msg = err_output.getvalue()
        self.assertIn("kicad-cli", msg)
        self.assertIn("kicad-cli: error:", msg)


class IntegrationSmokeTest(unittest.TestCase):
    """Runs the real pipeline with --skip-drc so an in-progress board still exercises export."""

    def test_make_fab_skip_drc_produces_artifacts(self):
        with tempfile.TemporaryDirectory() as d:
            proc = _sp.run(
                [sys.executable, str(_Path(fab.__file__)), "--skip-drc", "--output", d],
                capture_output=True, text=True, cwd=str(fab.ROOT),
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
