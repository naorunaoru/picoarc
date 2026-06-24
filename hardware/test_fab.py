import unittest
from pathlib import Path

import fab


class StampTest(unittest.TestCase):
    def test_clean_stamp(self):
        self.assertEqual(fab.compute_stamp("v0.0.0", "cc0bb70", False), "v0.0.0-cc0bb70")

    def test_dirty_stamp_appends_suffix(self):
        self.assertEqual(fab.compute_stamp("v0.0.0", "cc0bb70", True), "v0.0.0-cc0bb70-dirty")

    def test_output_names(self):
        names = fab.output_names("v0.0.0-cc0bb70", Path("/out"))
        self.assertEqual(names["gerber_zip"], Path("/out/picoarc-v0.0.0-cc0bb70-gerbers.zip"))
        self.assertEqual(names["bom"], Path("/out/picoarc-v0.0.0-cc0bb70-bom.csv"))
        self.assertEqual(names["cpl"], Path("/out/picoarc-v0.0.0-cc0bb70-cpl.csv"))
        self.assertEqual(names["drc"], Path("/out/drc-report.json"))


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


if __name__ == "__main__":
    unittest.main()
