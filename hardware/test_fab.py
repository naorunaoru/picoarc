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


if __name__ == "__main__":
    unittest.main()
