#!/usr/bin/env python3
"""No compiler, coverage package, or product artifacts required."""
import contextlib
import gzip
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "audit_feature_coverage", Path(__file__).with_name("audit_feature_coverage.py"))
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.git("init", "-q")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "user.name", "Inventory Test")
        self.write("source/lib/codec/core.c", "int old = 0;\nint keep = 1;\n")
        self.write("source/lib/codec/gone.h", "void gone(void);\n")
        self.write("tools/old name.py", "value = 1\n" * 12)
        self.write("tools/truncate.sh", "echo keep\necho remove\n")
        self.base = self.commit()
        self.write("source/lib/codec/core.c", "int added = 2;\nint keep = 1;\nint end = 3;\n")
        (self.root / "source/lib/codec/gone.h").unlink()
        self.git("mv", "tools/old name.py", "tools/new name.py")
        self.write("tools/truncate.sh", "echo keep\n")
        self.write("tools/new.py", "value = 1\nvalue += 1\n# comment\n")
        self.write("tools/runner", "#!/bin/sh\necho run\n")
        self.write("source/lib/codec/core.h", "void run(void);\n")
        self.write("CMakeLists.txt", "add_library(codec source/lib/codec/core.c)\n")
        self.write("README.md", "not implementation\n")
        self.head = self.commit()
        self.git("branch", "review/product-runtime")
        self.write("tools/test/test_core.py", '# source/lib/codec/core.c\n# tools/new.py\n')
        self.test_ref = self.commit()

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.root), *args], stderr=subprocess.PIPE).decode().strip()

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def commit(self):
        self.git("add", ".")
        self.git("commit", "-qm", "fixture")
        return self.git("rev-parse", "HEAD")

    def run_audit(self, *args):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            self.assertEqual(audit.main(["--root", str(self.root), "--base", self.base, *args]), 0)
        return json.loads(stdout.getvalue())

    def report(self, kind, data, compressed=False):
        path = self.root / (kind + ".json" + (".gz" if compressed else ""))
        raw = json.dumps(data).encode()
        path.write_bytes(gzip.compress(raw) if compressed else raw)
        return str(path)

    def rows(self, result):
        return {r["path"]: r for r in result["files"]}

    def test_inventory_unknown_static_not_execution(self):
        result = self.run_audit()
        rows = self.rows(result)
        self.assertEqual(set(rows), {"source/lib/codec/core.c", "source/lib/codec/core.h",
                                    "source/lib/codec/gone.h", "tools/new name.py", "tools/new.py",
                                    "tools/runner", "tools/truncate.sh", "CMakeLists.txt"})
        core = rows["source/lib/codec/core.c"]
        self.assertEqual(core["changed_lines"], [[1, 1], [3, 3]])
        self.assertEqual(core["feature"], "source/lib/codec")
        self.assertEqual(core["direct_test_references_static_hints"],
                         [{"path": "tools/test/test_core.py", "lines": [[1, 1]]}])
        self.assertTrue(core["build_import_references_static_hints"])
        self.assertEqual(core["build_or_import"], "UNKNOWN")
        self.assertEqual(core["execution"]["status"], "UNKNOWN")
        self.assertIsNone(core["changed_line_coverage"]["percent"])
        self.assertEqual(result["refs"], {"base": self.base, "head": self.head, "tests": self.test_ref})

    def test_rename_delete_and_deletion_only(self):
        rows = self.rows(self.run_audit())
        rename = rows["tools/new name.py"]
        self.assertEqual(rename["previous_path"], "tools/old name.py")
        self.assertEqual(rename["changed_lines"], [])
        self.assertEqual(rows["source/lib/codec/gone.h"]["retention_review"], "REVIEW_DELETION")
        self.assertEqual(rows["tools/truncate.sh"]["removed_line_count"], 1)
        self.assertIsNone(rows["tools/truncate.sh"]["changed_line_coverage"]["percent"])

    def test_python_measured_denominator_and_union(self):
        first = self.report("first", {"files": {"tools/new.py": {
            "executed_lines": [1], "missing_lines": [2]}}})
        second = self.report("second", {"files": {"tools/new.py": {
            "executed_lines": [2], "missing_lines": [1]}}})
        row = self.rows(self.run_audit("--python-json", first))["tools/new.py"]
        self.assertEqual(row["changed_line_coverage"], {"status": "EXECUTED", "measured_lines": 2,
                         "executed_lines": 1, "unmeasured_lines": 1, "percent": 50.0})
        row = self.rows(self.run_audit("--python-json", first, "--python-json", second))["tools/new.py"]
        self.assertEqual(row["changed_line_coverage"]["percent"], 100.0)
        self.assertEqual(row["execution"]["executed_lines"], [[1, 2]])
        self.assertEqual(row["retention_review"], "RETAIN_CANDIDATE")

    def test_gcov_zero_old_execution_and_path_remapping(self):
        report = self.report("gcov", {"current_working_directory": "/ci/checkout/build",
            "files": [{"file": "../source/lib/codec/core.c", "lines": [
                {"line_number": 1, "count": 0}, {"line_number": 2, "count": 9},
                {"line_number": 3, "count": 0}]}]}, compressed=True)
        unknown = self.run_audit("--gcov-json", report)
        self.assertTrue(unknown["coverage_inputs"][0]["unmapped_files"])
        row = self.rows(self.run_audit("--gcov-json", report, "--coverage-root", "/ci/checkout"))[
            "source/lib/codec/core.c"]
        self.assertEqual(row["execution"]["status"], "EXECUTED")
        self.assertEqual(row["changed_line_coverage"]["status"], "NOT_EXECUTED")
        self.assertEqual(row["changed_line_coverage"]["percent"], 0)
        self.assertEqual(row["retention_review"], "REVIEW_TEST_GAP")

    def test_llvm_segments_and_gap_regions(self):
        report = self.report("llvm", {"type": "llvm.coverage.json.export", "data": [{"files": [{
            "filename": str(self.root / "source/lib/codec/core.c"), "segments": [
                [1, 1, 0, True, True, False], [1, 8, 4, True, True, False],
                [2, 1, 0, True, False, True], [3, 1, 0, True, True, False],
                [4, 1, 0, False, False, False]]}]}]})
        row = self.rows(self.run_audit("--llvm-json", report))["source/lib/codec/core.c"]
        self.assertEqual(row["execution"]["executed_lines"], [[1, 1]])
        self.assertEqual(row["changed_line_coverage"]["percent"], 50)

    def test_gcovr_excluded_lines_do_not_count(self):
        report = self.report("gcovr", {"gcovr/format_version": "0.14", "files": [
            {"file": "source/lib/codec/core.c", "lines": [
                {"line_number": 1, "count": 8, "gcovr/excluded": True},
                {"line_number": 2, "count": 2, "gcovr/excluded": False},
                {"line_number": 3, "count": 0}]},
            {"file": "tools/new.py", "lines": [
                {"line_number": 1, "count": 9, "gcovr/excluded": True},
                {"line_number": 2, "count": 0, "gcovr/excluded": True}]}]})
        self.write("tools/new.py", "excluded-only source need not match\n")
        result = self.run_audit("--gcov-json", report, "--verify-checkout")
        rows = self.rows(result)
        core = rows["source/lib/codec/core.c"]
        self.assertEqual(core["execution"]["executed_lines"], [[2, 2]])
        self.assertEqual(core["changed_line_coverage"], {"status": "NOT_EXECUTED",
                         "measured_lines": 1, "executed_lines": 0, "unmeasured_lines": 1, "percent": 0})
        self.assertEqual(core["retention_review"], "REVIEW_TEST_GAP")
        self.assertEqual(rows["tools/new.py"]["execution"]["status"], "UNKNOWN")
        self.assertEqual(result["checkout_verification"]["verified_files"], ["source/lib/codec/core.c"])

    def test_verify_checkout_matches_and_ignores_validation_only_files(self):
        native = self.report("gcovr", {"gcovr/format_version": "0.14", "files": [
            {"file": "source/lib/codec/core.c", "lines": [{"line_number": 1, "count": 0}]},
            {"file": "source/lib/codec/core.h", "lines": [{"line_number": 1, "count": 1}]}]})
        python = self.report("python", {"files": {
            "tools/new.py": {"executed_lines": [1], "missing_lines": [2]},
            "tools/test/test_core.py": {"executed_lines": [1], "missing_lines": []}}})
        (self.root / "tools/test/test_core.py").unlink()
        self.write("CMakeLists.txt", "unmeasured validation build changes\n")
        args = ["--gcov-json", native, "--python-json", python]
        default = self.run_audit(*args)
        self.assertNotIn("checkout_verification", default)
        verified = self.run_audit(*args, "--verify-checkout")
        verification = verified.pop("checkout_verification")
        self.assertEqual(default, verified)
        self.assertEqual(verification["status"], "VERIFIED")
        self.assertEqual(verification["verified_files"], [
            "source/lib/codec/core.c", "source/lib/codec/core.h", "tools/new.py"])

    def test_verify_checkout_rejects_dirty_committed_missing_and_symlink_sources(self):
        source = self.root / "tools/new.py"
        original = source.read_bytes()
        report = self.report("python", {"files": {"tools/new.py": {
            "executed_lines": [], "missing_lines": [1, 2]}}})
        output = self.root / "output.json"
        for case in ("dirty", "committed", "missing", "symlink", "line_endings"):
            with self.subTest(case=case):
                if source.is_symlink():
                    source.unlink()
                source.write_bytes(original)
                if case in {"dirty", "committed"}:
                    source.write_text("different source\n")
                    if case == "committed":
                        self.commit()
                elif case == "missing":
                    source.unlink()
                elif case == "symlink":
                    target = self.write("same_bytes.py", original.decode())
                    source.unlink()
                    source.symlink_to(target)
                else:
                    source.write_bytes(original.replace(b"\n", b"\r\n"))
                error, stdout = io.StringIO(), io.StringIO()
                with contextlib.redirect_stderr(error), contextlib.redirect_stdout(stdout), \
                        self.assertRaises(SystemExit) as raised:
                    audit.main(["--root", str(self.root), "--base", self.base,
                                "--python-json", report, "--verify-checkout", "--output", str(output)])
                self.assertEqual(raised.exception.code, 2)
                self.assertIn("checkout verification failed for tools/new.py", error.getvalue())
                self.assertEqual(stdout.getvalue(), "")
                self.assertFalse(output.exists())
                # Default attribution remains independent of the worktree.
                self.assertNotIn("checkout_verification", self.run_audit("--python-json", report))

    def test_verify_checkout_without_measured_inventory_does_not_claim_verification(self):
        report = self.report("python", {"files": {"tests/validation_only.py": {
            "executed_lines": [1], "missing_lines": []}}})
        for args in ([], ["--python-json", report]):
            with self.subTest(args=args):
                verification = self.run_audit(*args, "--verify-checkout")["checkout_verification"]
                self.assertEqual(verification["status"], "NO_MEASURED_FILES")
                self.assertEqual(verification["verified_files"], [])

    def test_dirty_worktree_and_output_reproducibility(self):
        expected = self.run_audit()
        self.write("source/lib/codec/core.c", "uncommitted edits\n" * 20)
        self.write("tools/test/test_core.py", "uncommitted test edits\n")
        self.assertEqual(expected, self.run_audit())
        output = self.root / "inventory.json"
        audit.main(["--root", str(self.root), "--base", self.base, "--output", str(output)])
        self.assertEqual(expected, json.loads(output.read_text()))

    def test_empty_diff_and_explicit_head(self):
        self.assertEqual(self.run_audit("--head", self.base)["files"], [])

    def test_malformed_coverage_fails_not_silently_unknown(self):
        bad = self.report("bad", {"files": {"tools/new.py": {"executed_lines": [1]}}})
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            self.run_audit("--python-json", bad)
        self.assertEqual(raised.exception.code, 2)

    def test_paths_do_not_match_by_basename(self):
        self.assertIsNone(audit.coverage_path("/elsewhere/core.c", self.root, None))
        self.assertIsNone(audit.coverage_path("../core.c", self.root, None))

    def test_measured_zero_is_not_unknown(self):
        report = self.report("python", {"files": {"tools/new.py": {
            "executed_lines": [], "missing_lines": [1, 2]}}})
        row = self.rows(self.run_audit("--python-json", report))["tools/new.py"]
        self.assertEqual(row["execution"]["status"], "NOT_EXECUTED")
        self.assertEqual(row["build_or_import"], "INSTRUMENTED")
        self.assertEqual(row["changed_line_coverage"]["percent"], 0)

    def test_module_static_hint_is_not_test_name_matching(self):
        refs = audit.static_refs("tools/policy.py", {
            "tests/test_policy.py": "assert True\n",
            "tests/test_import.py": "import policy\nfrom tools.policy import run\n",
            "tests/test_other.py": "import policy_extra\n"})
        self.assertEqual(refs, [{"path": "tests/test_import.py", "lines": [[1, 2]]}])

    def test_unusual_path_and_mode_only_change(self):
        unusual = "tools/odd\tname\nscript.py"
        self.write(unusual, "value = 3\n")
        (self.root / "tools/runner").chmod(0o755)
        head = self.commit()
        rows = self.rows(self.run_audit("--base", self.head, "--head", head))
        self.assertEqual(rows[unusual]["changed_lines"], [[1, 1]])
        self.assertEqual(rows["tools/runner"]["changed_lines"], [])
        self.assertEqual(rows["tools/runner"]["changed_line_coverage"]["status"], "UNKNOWN")


if __name__ == "__main__":
    unittest.main()
