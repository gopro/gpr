#!/usr/bin/env python3
"""Regression tests for raw resolution target helpers."""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/cnn"))

from bench_raw_resolution_targets import downsample_bayer_0p5x  # noqa: E402

sys.path.insert(0, str(REPO / "tools/test"))
from run_pi_raw_resolution_bench import TARGET_2K_CHILD_DECODE_POLICY, TARGET_2K_POLICY  # noqa: E402

sys.path.insert(0, str(REPO / "tools"))
from live_preview_policy import DEFAULT_POLICY_ID, materialize_policy, viewport  # noqa: E402


def assemble_rggb(r: np.ndarray, g1: np.ndarray, g2: np.ndarray, b: np.ndarray) -> np.ndarray:
    out = np.zeros((r.shape[0] * 2, r.shape[1] * 2), dtype=np.uint16)
    out[0::2, 0::2] = r
    out[0::2, 1::2] = g1
    out[1::2, 0::2] = g2
    out[1::2, 1::2] = b
    return out


def area_down2_expected(plane: np.ndarray) -> np.ndarray:
    p = plane.astype(np.uint32)
    return (
        (p[0::2, 0::2] + p[0::2, 1::2] + p[1::2, 0::2] + p[1::2, 1::2] + 2) >> 2
    ).astype(np.uint16)


def test_downsample_bayer_0p5x_preserves_cfa_planes() -> None:
    r = np.array(
        [
            [10, 20, 30, 40],
            [50, 60, 70, 80],
            [90, 100, 110, 120],
            [130, 140, 150, 160],
        ],
        dtype=np.uint16,
    )
    g1 = r + 1000
    g2 = r + 2000
    b = r + 3000

    candidate = downsample_bayer_0p5x(assemble_rggb(r, g1, g2, b))
    expected = assemble_rggb(
        area_down2_expected(r),
        area_down2_expected(g1),
        area_down2_expected(g2),
        area_down2_expected(b),
    )

    assert candidate.dtype == np.uint16
    assert candidate.shape == (4, 4)
    np.testing.assert_array_equal(candidate, expected)


def test_named_2k_target_policies_are_distinct() -> None:
    fast = TARGET_2K_CHILD_DECODE_POLICY["2k_raw_0p5x_fast"]
    l2hh = TARGET_2K_CHILD_DECODE_POLICY["2k_raw_0p5x_l2hh"]

    assert TARGET_2K_POLICY["2k_raw_0p5x_fast"] == "named target: drop L2 highpass"
    assert TARGET_2K_POLICY["2k_raw_0p5x_l2hh"] == "named target: restore selective L2 HH"
    assert fast["source"] == "fused_decode_cli named target"
    assert l2hh["source"] == "fused_decode_cli named target"
    assert fast["halfres_drop_l2_hp"] is True
    assert fast["halfres_l2_mask"] is None
    assert l2hh["halfres_drop_l2_hp"] is False
    assert l2hh["halfres_l2_mask"] == 4
    assert fast["stream_strips"] == 2
    assert l2hh["stream_strips"] == 2


def test_live_preview_edge_safe_policy_contract() -> None:
    policy = materialize_policy(DEFAULT_POLICY_ID)

    assert policy["production_path_id"] == "preview_live_mission1_1024"
    assert policy["raw_target"] == "mission1_preview_1024"
    assert policy["source_codec"] == "mission1_native12_gvid"
    assert policy["display_mode"] == "full_frame_downsample"
    assert policy["forbids_ref_content"] is True
    assert policy["edge_inset_px"] == 0
    assert policy["target_fps"] == 20.0
    assert policy["p95_ms_budget"] == 50.0
    assert policy["display_viewport"] == {
        "x": 0,
        "y": 0,
        "width": 1024,
        "height": 768,
    }
    assert "preview_ui_receipt.json" in policy["quality_receipt"]
    assert "preview_decode_1024x768" in policy["timing_receipt"]


def test_live_preview_viewport_rejects_invalid_inset() -> None:
    try:
        viewport(32, 32, 16)
    except ValueError as exc:
        assert "consumes" in str(exc)
    else:
        raise AssertionError("expected invalid edge inset to fail")


if __name__ == "__main__":
    test_downsample_bayer_0p5x_preserves_cfa_planes()
    test_named_2k_target_policies_are_distinct()
    test_live_preview_edge_safe_policy_contract()
    test_live_preview_viewport_rejects_invalid_inset()
