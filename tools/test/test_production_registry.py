"""Production selection is explicit; historical lookup is validation-only."""
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from validation_registry import load_validation_registry


class RegistryIsolationTests(unittest.TestCase):
    def test_selections_and_references(self):
        production = json.loads((ROOT / "pipelines/registry.json").read_text())
        research = json.loads((ROOT / "pipelines/research_registry.json").read_text())
        merged = load_validation_registry()
        self.assertEqual(len(merged["pipelines"]),
                         len(production["pipelines"]) + len(research["pipelines"]))
        for entry in production["pipelines"].values():
            self.assertTrue(entry["release_selection"] in
                            ("ship_root", "production", "existing_offline") or
                            entry.get("visual_review_status") == "approved")
            for field, section in (("codec", "codecs"), ("cnn", "cnns"),
                                   ("demosaic", "demosaicers")):
                if entry[field] != "none":
                    self.assertIn(entry[field], production[section])
        self.assertTrue(research["pipelines"])

    def test_production_renderer_does_not_merge_research(self):
        spec = importlib.util.spec_from_file_location(
            "render_gvid_sr_receipt", ROOT / "tools/cnn/render_gvid_sr_receipt.py")
        sys.path.insert(0, str(ROOT / "tools/cnn"))
        renderer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(renderer)
        loaded = renderer.load_registry(ROOT / "pipelines/registry.json")
        research = json.loads((ROOT / "pipelines/research_registry.json").read_text())
        self.assertFalse(set(loaded["pipelines"]) & set(research["pipelines"]))

    def test_duplicate_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ("production.json", "research.json"):
                (root / name).write_text(json.dumps({"pipelines": {"same": {}}}))
            with self.assertRaisesRegex(ValueError, "duplicate"):
                load_validation_registry(root / "production.json", root / "research.json")


if __name__ == "__main__":
    unittest.main()
