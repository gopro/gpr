#!/usr/bin/env python3
"""Run the q8 three-way PREVIEW router as one runtime full-frame receipt.

The router selects one image-level render family from q8 source RGB features:
q8 hard specialist, q8 fallback3 specialist, or the existing v32 full-frame
fallback. Each selected family is rendered by the production-shaped full-frame
evaluator for that family; this wrapper writes one combined receipt/dashboard.
REF is used only inside the child evaluators for scoring.
"""
from __future__ import annotations


import argparse
import html
import json
import os
import resource
import subprocess
import sys
import time
from pathlib import Path
from typing import Any




sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_paths import external_path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/cnn"))
sys.path.insert(0, str(REPO / "tools/cnn"))

from evaluate_preview_runtime_policy import summarize  # noqa: E402
from evaluate_preview_scene_routed import sha256_file  # noqa: E402
from score_preview_q8_threeway_router_union import (  # noqa: E402
    LABEL_FALLBACK,
    LABEL_FALLBACK3,
    LABEL_HARD,
    final_routes,
    frozen_sidecar,
    labeled_features,
    route_summary,
)


POLICY_0606 = external_path('artifacts/preview_runtime_policy_20260606')
POLICY_0613 = external_path('artifacts/preview_runtime_policy_20260613')


def max_rss_mb() -> float:
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    if sys.platform == "darwin":
        return float(rss) / (1024.0 * 1024.0)
    return float(rss) / 1024.0


def selected_image_ids(args: argparse.Namespace, feature_rows: list[dict[str, Any]]) -> list[str]:
    selected = [str(row["image_id"]) for row in feature_rows]
    if args.image_id:
        wanted = set(args.image_id)
        selected = [image_id for image_id in selected if image_id in wanted]
    if args.limit:
        selected = selected[: args.limit]
    if not selected:
        raise RuntimeError("no selected images")
    return selected


def command_env(args: argparse.Namespace) -> dict[str, str]:
    env = dict(os.environ)
    env["TMPDIR"] = str(args.tmp_dir)
    env["GATE_TMPDIR"] = str(args.tmp_dir)
    env["GPR_EXTERNAL_ROOT"] = str(args.external_root)
    return env


def run_command(cmd: list[str], args: argparse.Namespace) -> dict[str, Any]:
    t0 = time.perf_counter()
    print("[threeway-runtime] " + " ".join(cmd), flush=True)
    result = subprocess.run(cmd, cwd=str(REPO), env=command_env(args), text=True, capture_output=True)
    wall_ms = (time.perf_counter() - t0) * 1000.0
    if result.returncode != 0:
        raise RuntimeError(
            "child renderer failed:\n"
            + " ".join(cmd)
            + "\nSTDOUT:\n"
            + result.stdout[-4000:]
            + "\nSTDERR:\n"
            + result.stderr[-4000:]
        )
    if result.stdout.strip():
        print(result.stdout[-1200:], flush=True)
    return {
        "cmd": cmd,
        "wall_ms": wall_ms,
        "stdout_tail": result.stdout[-2000:],
        "stderr_tail": result.stderr[-2000:],
    }


def prefix_rows(rows: list[dict[str, Any]], family: str, subdir: str) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for row in rows:
        updated = dict(row)
        if updated.get("png"):
            updated["png"] = f"{subdir}/{updated['png']}"
        updated["runtime_family"] = family
        updated["variant"] = f"{family}_runtime_fullframe"
        out.append(updated)
    return out


def prefix_images(images: list[dict[str, Any]], family: str, subdir: str) -> list[dict[str, Any]]:
    out = []
    for image in images:
        updated = dict(image)
        updated["runtime_family"] = family
        if updated.get("stitched_png"):
            updated["stitched_png"] = f"{subdir}/{updated['stitched_png']}"
        out.append(updated)
    return out


def receipt_timing(payload: dict[str, Any], family: str) -> dict[str, Any]:
    if family in {LABEL_HARD, LABEL_FALLBACK3}:
        timing = dict(payload.get("timing") or {})
        count = max(1, len(payload.get("images") or []))
        wall = float(timing.get("wall_ms_total", 0.0))
        timing["runtime_no_ref_wall_ms_avg"] = wall / count
        timing["runtime_no_ref_fps_avg"] = 1000.0 / (wall / count) if wall > 0 else 0.0
        return timing
    timing = dict(payload.get("timing_summary") or {})
    timing.update(payload.get("memory") or {})
    return timing


def timing_image_count(payload: dict[str, Any], family: str) -> int:
    if family in {LABEL_HARD, LABEL_FALLBACK3}:
        return len(payload.get("images") or [])
    timing = payload.get("timing_summary") or {}
    return int(timing.get("image_count") or len(payload.get("images") or []))


def fallback_args_from_receipt(args: argparse.Namespace, image_ids: list[str], out_dir: Path, json_path: Path, html_path: Path) -> list[str]:
    payload = json.loads(args.fallback_receipt.read_text())
    contract = payload["runtime_contract"]
    checkpoints = payload["checkpoints"]
    cmd = [
        sys.executable,
        str(REPO / "tools/cnn/evaluate_preview_scene_routed_fullframe.py"),
        "--router-sidecar",
        contract["router_sidecar"],
        "--default-checkpoint",
        checkpoints["default"]["path"],
        "--tile-size",
        str(contract["tile_size"]),
        "--overlap",
        str(contract["overlap"]),
        "--valid-margin",
        str(contract.get("valid_margin", 0)),
        "--route-context-padding",
        str(contract.get("route_context_padding", 0)),
        "--coordinate-mode",
        contract.get("coordinate_mode", "local"),
        "--output-dir",
        str(out_dir),
        "--dashboard-json",
        str(json_path),
        "--dashboard-html",
        str(html_path),
        "--tmp-dir",
        str(args.tmp_dir),
    ]
    for sidecar in contract.get("override_router_sidecar") or []:
        cmd.extend(["--override-router-sidecar", sidecar])
    for key, value in sorted(checkpoints.items()):
        if key == "default" or key == "post_refiner":
            continue
        if key.startswith("cluster_"):
            cmd.extend(["--cluster-checkpoint", f"{key.removeprefix('cluster_')}={value['path']}"])
        elif key.startswith("override_"):
            parts = key.split("_")
            if len(parts) == 4 and parts[2] == "cluster":
                cmd.extend(["--override-cluster-checkpoint", f"{parts[1]}:{parts[3]}={value['path']}"])
    for mode_key, mode in infer_fallback_conditioning(payload).items():
        flag = "--override-cluster-conditioning" if ":" in mode_key else "--cluster-conditioning"
        cmd.extend([flag, f"{mode_key}={mode}"])
    for image_id in image_ids:
        cmd.extend(["--image-id", image_id])
    return cmd


def infer_fallback_conditioning(payload: dict[str, Any]) -> dict[str, str]:
    modes: dict[str, dict[str, int]] = {}
    for image in payload.get("images") or []:
        for tile in image.get("tiles") or []:
            role = str(tile.get("checkpoint_role"))
            mode = str(tile.get("conditioning"))
            if role.startswith("cluster_"):
                key = role.removeprefix("cluster_")
            elif role.startswith("override_"):
                parts = role.split("_")
                if len(parts) == 4 and parts[2] == "cluster":
                    key = f"{parts[1]}:{parts[3]}"
                else:
                    continue
            else:
                continue
            modes.setdefault(key, {})[mode] = modes.setdefault(key, {}).get(mode, 0) + 1
    out = {}
    for key, counts in modes.items():
        mode = sorted(counts.items(), key=lambda item: (-item[1], item[0]))[0][0]
        if mode != "zero":
            out[key] = mode
    return out


def run_q8_family(
    *,
    args: argparse.Namespace,
    family: str,
    checkpoint: Path,
    image_ids: list[str],
    out_dir: Path,
    json_path: Path,
    html_path: Path,
) -> dict[str, Any]:
    cmd = [
        sys.executable,
        str(REPO / "tools/cnn/evaluate_preview_q8_crop_fullframe.py"),
        "--source-fullframe-receipt",
        str(args.source_fullframe_receipt),
        "--checkpoint",
        str(checkpoint),
        "--tile-size",
        str(args.tile_size),
        "--overlap",
        str(args.overlap),
        "--coordinate-mode",
        args.q8_coordinate_mode,
        "--output-dir",
        str(out_dir),
        "--output-json",
        str(json_path),
        "--output-html",
        str(html_path),
        "--tmp-dir",
        str(args.tmp_dir),
    ]
    if args.save_fullframe:
        cmd.append("--save-fullframe")
    for image_id in image_ids:
        cmd.extend(["--image-id", image_id])
    run = run_command(cmd, args)
    payload = json.loads(json_path.read_text())
    payload["child_run"] = run
    payload["runtime_family"] = family
    return payload


def write_html(payload: dict[str, Any], path: Path) -> None:
    summary = payload["summary"]["preview_q8_threeway_runtime_fullframe"]
    route = payload["route_summary"]["final_sidecar"]
    timing = payload["timing_summary"]
    css = """
body { font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; margin:18px; background:#f7f8f9; color:#222; }
.cards { display:grid; grid-template-columns:repeat(5,minmax(150px,1fr)); gap:10px; margin:14px 0; }
.card,.tile { background:#fff; border:1px solid #d4d8de; border-radius:6px; padding:10px; }
table { border-collapse:collapse; background:#fff; width:100%; font-size:12px; margin:14px 0; }
td,th { border:1px solid #ccd2d9; padding:5px 7px; text-align:right; }
th.left,td.left { text-align:left; }
.pass { color:#096b2b; font-weight:700; }
.fail { color:#9b1c1c; font-weight:700; }
.grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(220px,1fr)); gap:10px; }
.tile img { width:100%; display:block; border:1px solid #ddd; }
"""
    parts = [
        "<!doctype html><meta charset='utf-8'><title>q8 Three-way Runtime PREVIEW</title>",
        f"<style>{css}</style><h1>q8 Three-way Runtime PREVIEW</h1>",
        "<p>Image-level router uses q8 source RGB features only. REF is scoring only.</p>",
        "<div class=cards>",
        f"<div class=card><b>Pass</b><br>{summary['pass_count']}/{summary['count']}</div>",
        f"<div class=card><b>Route</b><br>{route['correct']}/{route['count']}</div>",
        f"<div class=card><b>Worst LPIPS</b><br>{summary['worst_lpips']:.4f}</div>",
        f"<div class=card><b>Worst dE2000</b><br>{summary['worst_dE2000_mean']:.2f}</div>",
        f"<div class=card><b>Runtime FPS</b><br>{timing['runtime_no_ref_fps_avg']:.3f}</div>",
        "</div><table><thead><tr><th class=left>image</th><th class=left>crop</th><th class=left>family</th><th>pass</th><th>LPIPS</th><th>MS</th><th>Y</th><th>dE</th></tr></thead><tbody>",
    ]
    for row in sorted(payload["rows"], key=lambda item: (item["preview_pass"], item["image_id"], item["crop"])):
        cls = "pass" if row["preview_pass"] else "fail"
        parts.append(
            f"<tr><td class=left>{html.escape(row['image_id'])}</td><td class=left>{html.escape(row['crop'])}</td>"
            f"<td class=left>{html.escape(row['runtime_family'])}</td><td class={cls}>{'PASS' if row['preview_pass'] else 'FAIL'}</td>"
            f"<td>{row['lpips']:.4f}</td><td>{row['ms_ssim']:.4f}</td><td>{row['y_psnr']:.2f}</td><td>{row['dE2000_mean']:.2f}</td></tr>"
        )
    parts.append("</tbody></table><div class=grid>")
    for row in sorted(payload["rows"], key=lambda item: (-float(item["lpips"]), item["image_id"], item["crop"])):
        cls = "pass" if row["preview_pass"] else "fail"
        parts.append(
            f"<div class=tile><b>{html.escape(row['image_id'])} {html.escape(row['crop'])}</b>"
            f"<br><span class={cls}>{html.escape(row['runtime_family'])} LPIPS {row['lpips']:.4f}, MS {row['ms_ssim']:.4f}, Y {row['y_psnr']:.2f}, dE {row['dE2000_mean']:.2f}</span>"
            f"<img src='{html.escape(row['png'])}'></div>"
        )
    parts.append("</div>")
    path.write_text("".join(parts))


def collect(args: argparse.Namespace) -> dict[str, Any]:
    feature_rows, meta = labeled_features(args)
    sidecar = frozen_sidecar(feature_rows, meta, args.fallback3_max_distance)
    routes = final_routes(feature_rows, sidecar)
    selected = set(selected_image_ids(args, feature_rows))
    selected_routes = [row for row in routes if str(row["image_id"]) in selected]
    by_label: dict[str, list[str]] = {LABEL_HARD: [], LABEL_FALLBACK3: [], LABEL_FALLBACK: []}
    for row in selected_routes:
        by_label[str(row["predicted_label"])].append(str(row["image_id"]))
    args.output_dir.mkdir(parents=True, exist_ok=True)
    child_payloads: dict[str, dict[str, Any]] = {}
    child_runs: dict[str, dict[str, Any]] = {}
    rows: list[dict[str, Any]] = []
    images: list[dict[str, Any]] = []

    family_specs = [
        (LABEL_HARD, args.q8_hard_checkpoint, "hard"),
        (LABEL_FALLBACK3, args.q8_fallback3_checkpoint, "fallback3"),
    ]
    for family, checkpoint, subdir in family_specs:
        image_ids = by_label[family]
        if not image_ids:
            continue
        out_dir = args.output_dir / subdir
        payload = run_q8_family(
            args=args,
            family=family,
            checkpoint=checkpoint,
            image_ids=image_ids,
            out_dir=out_dir,
            json_path=out_dir / "preview_q8_crop_fullframe.json",
            html_path=out_dir / "preview_q8_crop_fullframe.html",
        )
        child_payloads[family] = payload
        child_runs[family] = payload["child_run"]
        rows.extend(prefix_rows(payload.get("rows") or [], family, subdir))
        images.extend(prefix_images(payload.get("images") or [], family, subdir))

    fallback_ids = by_label[LABEL_FALLBACK]
    if fallback_ids:
        subdir = "fallback"
        out_dir = args.output_dir / subdir
        out_dir.mkdir(parents=True, exist_ok=True)
        json_path = out_dir / "preview_scene_routed_fullframe.json"
        html_path = out_dir / "preview_scene_routed_fullframe.html"
        cmd = fallback_args_from_receipt(args, fallback_ids, out_dir, json_path, html_path)
        run = run_command(cmd, args)
        payload = json.loads(json_path.read_text())
        payload["child_run"] = run
        payload["runtime_family"] = LABEL_FALLBACK
        child_payloads[LABEL_FALLBACK] = payload
        child_runs[LABEL_FALLBACK] = run
        rows.extend(prefix_rows(payload.get("rows") or [], LABEL_FALLBACK, subdir))
        images.extend(prefix_images(payload.get("images") or [], LABEL_FALLBACK, subdir))

    runtime_weighted_total = 0.0
    runtime_image_count = 0
    child_timing: dict[str, Any] = {}
    child_max_rss = max_rss_mb()
    for family, payload in child_payloads.items():
        timing = receipt_timing(payload, family)
        child_timing[family] = timing
        child_max_rss = max(child_max_rss, float(timing.get("max_rss_mb", 0.0)))
        image_count = timing_image_count(payload, family)
        if image_count > 0 and timing.get("runtime_no_ref_wall_ms_avg"):
            runtime_weighted_total += float(timing["runtime_no_ref_wall_ms_avg"]) * float(image_count)
            runtime_image_count += image_count
    runtime_avg = runtime_weighted_total / float(runtime_image_count) if runtime_image_count else 0.0
    return {
        "schema": "preview_q8_threeway_runtime_fullframe_receipt.v1",
        "manifest": str(args.manifest),
        "source_fullframe_receipt": str(args.source_fullframe_receipt),
        "fallback_receipt_template": str(args.fallback_receipt),
        "route_mode": "final_sidecar",
        "render_contract": {
            "router_inputs": ["q8_source_fullframe_rgb", "fixed_manifest_crop_rgb_windows"],
            "render_inputs": ["source RGB full frame", "selected expert checkpoint", "source-derived feature planes", "normalized coordinates"],
            "forbidden_inputs": ["ref_rgb", "ref_dng", "gate_metrics", "sample_index", "crop_identity_key_planes"],
            "uses_ref_at_route_time": False,
            "uses_ref_at_render_time": False,
            "uses_ref_for_scoring_only": True,
        },
        "sidecar": sidecar,
        "routes": {"final_sidecar": selected_routes},
        "route_summary": {"final_sidecar": route_summary(selected_routes)},
        "selected_images_by_family": by_label,
        "checkpoints": {
            LABEL_HARD: {"path": str(args.q8_hard_checkpoint), "sha256": sha256_file(args.q8_hard_checkpoint)},
            LABEL_FALLBACK3: {"path": str(args.q8_fallback3_checkpoint), "sha256": sha256_file(args.q8_fallback3_checkpoint)},
            LABEL_FALLBACK: (json.loads(args.fallback_receipt.read_text()).get("checkpoints") or {}),
        },
        "child_runs": child_runs,
        "child_timing": child_timing,
        "timing_summary": {
            "image_count": runtime_image_count,
            "runtime_no_ref_wall_ms_avg": runtime_avg,
            "runtime_no_ref_fps_avg": 1000.0 / runtime_avg if runtime_avg > 0 else 0.0,
            "child_wall_ms_total": float(sum(run["wall_ms"] for run in child_runs.values())),
        },
        "memory": {"max_rss_mb": child_max_rss, "wrapper_max_rss_mb": max_rss_mb()},
        "summary": {"preview_q8_threeway_runtime_fullframe": summarize(rows)},
        "images": images,
        "rows": rows,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", type=Path, default=REPO / "pipelines/preview_holdout_set.json")
    ap.add_argument("--source-fullframe-receipt", type=Path, default=POLICY_0613 / "q8_source_fullframes_holdout28_v1/preview_codec_source_fullframes.json")
    ap.add_argument("--q8-hard-receipt", type=Path, default=POLICY_0613 / "q8_crop_fullframe_hardfit_hard8_t512_v1/preview_q8_crop_fullframe.json")
    ap.add_argument("--q8-fallback3-receipt", type=Path, default=POLICY_0613 / "q8_crop_fullframe_fallback3_allfit_t512_v1/preview_q8_crop_fullframe.json")
    ap.add_argument("--fallback-receipt", type=Path, default=POLICY_0606 / "fullframe_tiled_v32_holdout28_baseline_t512/preview_scene_routed_fullframe.json")
    ap.add_argument("--q8-hard-checkpoint", type=Path, default=POLICY_0613 / "q8_crop_refiner_hardfit_diverseholdout_w40_s300_v1/q8_crop_refiner.pt")
    ap.add_argument("--q8-fallback3-checkpoint", type=Path, default=POLICY_0613 / "q8_crop_refiner_fallback3_allfit_w40_s300_v1/q8_crop_refiner.pt")
    ap.add_argument("--fallback3-max-distance", type=float, default=3.0)
    ap.add_argument("--image-id", action="append", default=[])
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--tile-size", type=int, default=512)
    ap.add_argument("--overlap", type=int, default=0)
    ap.add_argument("--q8-coordinate-mode", choices=["local", "global", "zero"], default="local")
    ap.add_argument("--save-fullframe", action="store_true")
    ap.add_argument("--output-dir", type=Path, required=True)
    ap.add_argument("--output-json", type=Path, required=True)
    ap.add_argument("--output-html", type=Path, required=True)
    ap.add_argument("--tmp-dir", type=Path, default=external_path('tmp'))
    ap.add_argument("--external-root", type=Path, default=external_path(''))
    args = ap.parse_args()
    args.tmp_dir.mkdir(parents=True, exist_ok=True)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    payload = collect(args)
    args.output_json.write_text(json.dumps(payload, indent=2, allow_nan=False) + "\n")
    write_html(payload, args.output_html)
    summary = payload["summary"]["preview_q8_threeway_runtime_fullframe"]
    print(
        f"threeway-runtime {summary['pass_count']}/{summary['count']} "
        f"LPIPS={summary['worst_lpips']:.4f} MS={summary['worst_ms_ssim']:.4f} "
        f"Y={summary['worst_y_psnr']:.2f} dE={summary['worst_dE2000_mean']:.2f}",
        flush=True,
    )
    print(args.output_json)
    print(args.output_html)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
