"""Portable holdout fixture resolution without running an image model."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/cnn"))
from build_preview_holdout_runtime_receipt import resolve_ref


class FixturePaths(unittest.TestCase):
    def test_external_manifest_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.dict(os.environ, {"GPR_EXTERNAL_ROOT": str(root)}):
                self.assertEqual(
                    resolve_ref({"id": "frame", "path": "fixtures/frame.dng"}, []),
                    root / "fixtures/frame.dng",
                )
                absolute = root / "elsewhere/frame.dng"
                self.assertEqual(resolve_ref({"id": "frame", "path": str(absolute)}, []), absolute)

    def test_explicit_search_root_precedes_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "frame.dng").touch()
            self.assertEqual(
                resolve_ref({"id": "frame", "path": "missing.dng"}, [root]),
                root / "frame.dng",
            )


if __name__ == "__main__":
    unittest.main()
