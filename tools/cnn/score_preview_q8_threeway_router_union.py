#!/usr/bin/env python3
"""Runtime model and inference helpers; experiment commands are archived."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import numpy as np


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/cnn"))
sys.path.insert(0, str(REPO / "tools/cnn"))

from score_preview_q8_hard_router_union import build_features, feature_schema, normalize  # noqa: E402


LABEL_FALLBACK = "fallback"
LABEL_FALLBACK3 = "fallback3"
LABEL_HARD = "hard"
LABELS = [LABEL_FALLBACK, LABEL_FALLBACK3, LABEL_HARD]


def receipt_image_ids(path: Path) -> set[str]:
    payload = json.loads(path.read_text())
    return {str(row["image_id"]) for row in payload.get("rows") or []}


def labeled_features(args: argparse.Namespace) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    rows, meta = build_features(args)
    hard_ids = receipt_image_ids(args.q8_hard_receipt)
    fallback3_ids = receipt_image_ids(args.q8_fallback3_receipt)
    overlap = hard_ids & fallback3_ids
    if overlap:
        raise RuntimeError(f"overlapping specialist labels: {sorted(overlap)}")
    for row in rows:
        image_id = str(row["image_id"])
        row["label"] = LABEL_HARD if image_id in hard_ids else LABEL_FALLBACK3 if image_id in fallback3_ids else LABEL_FALLBACK
    meta["hard_image_ids"] = sorted(hard_ids)
    meta["fallback3_image_ids"] = sorted(fallback3_ids)
    meta["feature_schema"] = feature_schema(meta["crop_names"])
    return rows, meta


def guarded_label(distances: dict[str, float], fallback3_max_distance: float) -> str:
    label = min(distances, key=distances.get)
    if label == LABEL_FALLBACK3 and distances[LABEL_FALLBACK3] > fallback3_max_distance:
        return LABEL_FALLBACK
    return label


def build_centers(z_all: np.ndarray, rows: list[dict[str, Any]], train_idx: list[int]) -> dict[str, np.ndarray]:
    centers: dict[str, np.ndarray] = {}
    for label in LABELS:
        idx = [index for index in train_idx if rows[index]["label"] == label]
        if not idx:
            raise RuntimeError(f"no training rows for {label}")
        centers[label] = z_all[idx].mean(axis=0)
    return centers


def route_with_centers(z: np.ndarray, centers: dict[str, np.ndarray], fallback3_max_distance: float) -> tuple[str, dict[str, float], float]:
    distances = {label: float(np.linalg.norm(z - center)) for label, center in centers.items()}
    label = guarded_label(distances, fallback3_max_distance)
    ordered = sorted(distances.items(), key=lambda item: item[1])
    margin = float(ordered[1][1] - ordered[0][1])
    return label, distances, margin


def frozen_sidecar(rows: list[dict[str, Any]], meta: dict[str, Any], fallback3_max_distance: float) -> dict[str, Any]:
    features = np.stack([row["feature"] for row in rows])
    z_all, mean, std = normalize(features)
    centers = build_centers(z_all, rows, list(range(len(rows))))
    return {
        "schema": "preview_q8_threeway_router_sidecar.v1",
        "feature_schema": meta["feature_schema"],
        "normalization_mean": mean.tolist(),
        "normalization_std": std.tolist(),
        "centers": {label: center.tolist() for label, center in centers.items()},
        "fallback3_max_distance": fallback3_max_distance,
        "training_counts": {label: sum(1 for row in rows if row["label"] == label) for label in LABELS},
        "training_image_ids": {label: [row["image_id"] for row in rows if row["label"] == label] for label in LABELS},
    }


def final_routes(rows: list[dict[str, Any]], sidecar: dict[str, Any]) -> list[dict[str, Any]]:
    mean = np.asarray(sidecar["normalization_mean"], dtype=np.float64)
    std = np.asarray(sidecar["normalization_std"], dtype=np.float64)
    centers = {label: np.asarray(center, dtype=np.float64) for label, center in sidecar["centers"].items()}
    routed = []
    for row in rows:
        z = (row["feature"] - mean) / std
        label, distances, margin = route_with_centers(z, centers, float(sidecar["fallback3_max_distance"]))
        routed.append(
            {
                "image_id": row["image_id"],
                "actual_label": row["label"],
                "predicted_label": label,
                "distances": distances,
                "nearest_margin": margin,
            }
        )
    return routed


def route_summary(routes: list[dict[str, Any]]) -> dict[str, Any]:
    out = {
        "count": len(routes),
        "correct": sum(1 for row in routes if row["actual_label"] == row["predicted_label"]),
    }
    for label in LABELS:
        out[f"{label}_correct"] = sum(1 for row in routes if row["actual_label"] == label and row["predicted_label"] == label)
        out[f"{label}_count"] = sum(1 for row in routes if row["actual_label"] == label)
    return out
