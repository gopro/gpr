#!/usr/bin/env python3
"""Regression tests for Labs extension manifest validation."""
from __future__ import annotations

import copy
import importlib.util
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/validate_labs_extension_manifest.py"
SAMPLE = ROOT / "examples/labs/raw_media_extension_gvid.json"


def import_tool():
    spec = importlib.util.spec_from_file_location("validate_labs_extension_manifest_under_test", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def load_sample() -> dict:
    import json

    with SAMPLE.open("r", encoding="utf-8") as f:
        data = json.load(f)
    assert isinstance(data, dict)
    return data


def expect_failure(module, data: dict, text: str) -> None:
    failures = module.validate_manifest(data)
    if not any(text in failure for failure in failures):
        raise AssertionError(f"expected failure containing {text!r}, got {failures!r}")


def main() -> int:
    module = import_tool()
    sample = load_sample()
    failures = module.validate_manifest(sample)
    if failures:
        print(f"sample manifest unexpectedly failed: {failures}", file=sys.stderr)
        return 1

    no_signature = copy.deepcopy(sample)
    no_signature["security"]["signature_required"] = False
    expect_failure(module, no_signature, "signature_required")

    native_code = copy.deepcopy(sample)
    native_code["security"]["native_code_allowed"] = True
    expect_failure(module, native_code, "native_code_allowed")

    missing_capability = copy.deepcopy(sample)
    missing_capability["requires"].remove("preview_surface")
    expect_failure(module, missing_capability, "missing capabilities")

    oversized_preview = copy.deepcopy(sample)
    oversized_preview["capture_modes"][0]["preview"]["width"] = 9999
    expect_failure(module, oversized_preview, "preview.width")

    mismatched_bit_depth = copy.deepcopy(sample)
    mismatched_bit_depth["capture_modes"][0]["bit_depth"] = 14
    expect_failure(module, mismatched_bit_depth, "pixel_format must match bit_depth")

    print("test_validate_labs_extension_manifest: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
