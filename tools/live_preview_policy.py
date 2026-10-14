#!/usr/bin/env python3
"""Runtime display policy for bounded live PREVIEW paths."""
from __future__ import annotations

import argparse
import json
from copy import deepcopy
from typing import Any


DEFAULT_POLICY_ID = "preview_live_mission1_1024_v1"

POLICIES: dict[str, dict[str, Any]] = {
    DEFAULT_POLICY_ID: {
        "schema": "gpr_live_preview_policy.v1",
        "production_path_id": "preview_live_mission1_1024",
        "raw_target": "mission1_preview_1024",
        "source_codec": "mission1_native12_gvid",
        "display_mode": "full_frame_downsample",
        "source_width": 4096,
        "source_height": 3072,
        "input_width": 1024,
        "input_height": 768,
        "edge_inset_px": 0,
        "target_fps": 20.0,
        "p95_ms_budget": 50.0,
        "forbids_ref_content": True,
        "quality_receipt": (
            "artifacts/mission1_current_goal_sync_20260625/"
            "run_1440f_direct_gvid/preview_ui_receipt.json"
        ),
        "timing_receipt": (
            "artifacts/mission1_current_goal_sync_20260625/"
            "run_1440f_direct_gvid/preview_decode_1024x768/receipt.json"
        ),
    },
    "preview_live_2k_l2hh_edge_safe_v1": {
        "schema": "gpr_live_preview_policy.v1",
        "production_path_id": "preview_live_2k_l2hh_edge_safe",
        "raw_target": "2k_raw_0p5x_l2hh",
        "source_codec": "ml2_q3_dec2",
        "display_mode": "edge_safe_viewport",
        "input_width": 2070,
        "input_height": 1380,
        "edge_inset_px": 16,
        "target_fps": 24.0,
        "p95_ms_budget": 41.7,
        "forbids_ref_content": True,
        "quality_receipt": "artifacts/raw_resolution_targets_20260614_analysis/visual_2k_l2hh_edgeinset16_28f/raw_resolution_targets_visual_dashboard.json",
        "timing_receipt": "artifacts/raw_resolution_targets_20260614_alias_v4/pi5_2k_l2hh_alias_120f/raw_resolution_targets_pi5_120f.json",
    }
}


def viewport(width: int, height: int, edge_inset_px: int) -> dict[str, int]:
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be positive")
    if edge_inset_px < 0:
        raise ValueError("edge inset must be non-negative")
    if edge_inset_px * 2 >= width or edge_inset_px * 2 >= height:
        raise ValueError("edge inset consumes the full display viewport")
    return {
        "x": edge_inset_px,
        "y": edge_inset_px,
        "width": width - edge_inset_px * 2,
        "height": height - edge_inset_px * 2,
    }


def materialize_policy(policy_id: str = DEFAULT_POLICY_ID) -> dict[str, Any]:
    if policy_id not in POLICIES:
        raise KeyError(f"unknown live PREVIEW policy: {policy_id}")
    policy = deepcopy(POLICIES[policy_id])
    policy["id"] = policy_id
    policy["display_viewport"] = viewport(
        int(policy["input_width"]),
        int(policy["input_height"]),
        int(policy["edge_inset_px"]),
    )
    return policy


def main() -> int:
    parser = argparse.ArgumentParser(description="Print a live PREVIEW display policy as JSON.")
    parser.add_argument("--policy", default=DEFAULT_POLICY_ID, choices=sorted(POLICIES))
    args = parser.parse_args()
    print(json.dumps(materialize_policy(args.policy), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
