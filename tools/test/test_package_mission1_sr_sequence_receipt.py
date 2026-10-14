#!/usr/bin/env python3
"""Regression-test deterministic helpers in the Mission 1 SR sequence packer."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/cnn/package_mission1_sr_sequence_receipt.py"


def import_tool():
    spec = importlib.util.spec_from_file_location("package_mission1_sr_sequence_receipt_under_test", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    module = import_tool()
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        external_root = tmp / "external"
        artifacts = external_root / "artifacts" / "receipt_test"
        raw_dir = tmp / "raws"
        artifacts.mkdir(parents=True)
        raw_dir.mkdir()

        (raw_dir / "frame_000002_sr8k.raw").write_bytes(b"two")
        (raw_dir / "frame_000000_sr8k.raw").write_bytes(b"zero")
        (raw_dir / "frame_000001.raw").write_bytes(b"fallback")

        raws = module.sr_raws(raw_dir, frame_count=1)
        if [path.name for path in raws] != ["frame_000000_sr8k.raw"]:
            raise AssertionError(f"unexpected SR raw selection: {[path.name for path in raws]}")

        py_cmd = module.tool_cmd(Path("tools/gvid_pack.py"))
        if py_cmd[0] != sys.executable:
            raise AssertionError(f"Python helper was not invoked with current interpreter: {py_cmd}")

        native_cmd = module.tool_cmd(Path("build-local/source/app/gpr_tools/gpr_tools"))
        if native_cmd != ["build-local/source/app/gpr_tools/gpr_tools"]:
            raise AssertionError(f"native helper command changed unexpectedly: {native_cmd}")

        summary = module.summarize([1.0, 2.0, 10.0])
        if summary["median"] != 2.0 or round(summary["p95"], 3) != 9.2:
            raise AssertionError(f"unexpected summary math: {summary}")

        artifact_file = artifacts / "sample.bin"
        artifact_file.write_bytes(b"abc")
        record = module.file_record(artifact_file, external_root)
        if record["path"] != "artifacts/receipt_test/sample.bin":
            raise AssertionError(f"unexpected relative artifact path: {record}")
        if record["bytes"] != 3:
            raise AssertionError(f"unexpected artifact byte count: {record}")
        if record["sha256"] != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad":
            raise AssertionError(f"unexpected artifact hash: {record}")

    print("test_package_mission1_sr_sequence_receipt: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
