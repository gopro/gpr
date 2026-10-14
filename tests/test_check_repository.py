import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "check_repository", Path(__file__).with_name("check_repository.py")
)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class RepositoryChecks(unittest.TestCase):
    def test_links_and_examples(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "image.png").touch()
            (root / "README.md").write_text(
                "![Photo](image.png)\n[Web](https://example.com)\n"
                "`[example](not-real)`\n<!-- [old](not-real) -->\n"
            )
            self.assertEqual(checker.inspect(root, ["README.md"]), [])
            (root / "image.png").unlink()
            self.assertEqual(len(checker.inspect(root, ["README.md"])), 1)

    def test_personal_paths_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "config.txt").write_text("/" + "Users" + "/example/data")
            self.assertEqual(len(checker.inspect(root, ["config.txt"])), 1)

    def test_missing_registered_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "pipelines").mkdir()
            registry = {"cnns": {"model": {"runtime_entrypoint": "render.py"}}}
            (root / "pipelines/registry.json").write_text(json.dumps(registry))
            self.assertEqual(len(checker.inspect(root, [])), 1)
            (root / "render.py").touch()
            self.assertEqual(checker.inspect(root, []), [])

    def test_generated_media_is_not_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "capture.dng").touch()
            self.assertEqual(len(checker.inspect(root, ["capture.dng"])), 1)
            (root / "docs/img").mkdir(parents=True)
            (root / "docs/img/photo.png").touch()
            self.assertEqual(checker.inspect(root, ["docs/img/photo.png"]), [])


if __name__ == "__main__":
    unittest.main()
