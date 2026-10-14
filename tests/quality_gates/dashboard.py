#!/usr/bin/env python3
"""Build a comparison dashboard from gate run logs.

Reads every `runs/<hash>/run.json` in tests/quality_gates/runs/ and
emits a static HTML dashboard with:

  - Per-pipeline / per-image / per-metric grid (worst-first)
  - Full-image PNG row (downsampled for browser)
  - 100% crop rows (A_detail, B_center, C_lowerleft)
  - Side-by-side worst-image visual diff for each pipeline
  - PASS/FAIL verdicts shown per cell

This script never writes verdicts. It only renders what the gate
already decided. If you want a different verdict, change `gates.json`
and re-run the gate — not the dashboard.

Usage:
  python3 tests/quality_gates/dashboard.py [--out DIR]

Default output: tests/quality_gates/runs/dashboard/index.html
"""
from __future__ import annotations
import argparse
import json
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
from validation_registry import load_validation_registry
RUNS_DIR = REPO / "tests/quality_gates/runs"
REGISTRY = REPO / "pipelines/registry.json"
GATES = REPO / "tests/quality_gates/gates.json"
TEST_SET = REPO / "tests/quality_gates/test_set.json"


def load_runs():
    runs = []
    for run_dir in sorted(RUNS_DIR.iterdir()):
        if not run_dir.is_dir():
            continue
        run_json = run_dir / "run.json"
        if not run_json.exists():
            continue
        runs.append({"dir": run_dir, "json": json.loads(run_json.read_text())})
    return runs


def metric_class(metric: str, value: float, gate_rule: dict) -> str:
    if value is None or gate_rule is None:
        return "neutral"
    if "max" in gate_rule and value > gate_rule["max"]:
        return "fail"
    if "min" in gate_rule and value < gate_rule["min"]:
        return "fail"
    # OK against gate; show how close to threshold
    if "max" in gate_rule and value > gate_rule["max"] * 0.5:
        return "warn"
    if "min" in gate_rule and value < gate_rule["min"] + (1.0 - gate_rule["min"]) * 0.5:
        return "warn"
    return "good"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(RUNS_DIR / "dashboard"))
    args = ap.parse_args()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    runs = load_runs()
    if not runs:
        print("No runs found. Execute run_gate.py first.")
        return
    registry = load_validation_registry(REGISTRY)
    gates = json.loads(GATES.read_text())
    test_set = json.loads(TEST_SET.read_text())

    # Collect images present across all runs (should be the same 4)
    all_images = []
    for r in runs:
        for img_id in r["json"]["images"]:
            if img_id not in all_images:
                all_images.append(img_id)

    # Build a dict of role -> ship class for nicer column labels
    pipeline_meta = {}
    for name, p in registry["pipelines"].items():
        if name.startswith("$"):
            continue
        pipeline_meta[name] = {
            "role": p.get("$role", ""),
            "ship_class": p["ship_class"],
            "codec": p["codec"],
            "cnn": p["cnn"],
        }

    # Copy crop assets next to the dashboard
    img_dir = out_dir / "imgs"
    img_dir.mkdir(exist_ok=True)
    crop_map = {}        # (run_hash, img_id, kind) -> filename in dashboard
    visual_diff_map = {} # run_hash -> visual diff filename
    ref_crop_for_img = {}  # img_id -> filename (shared REF crop, one per image)

    for r in runs:
        run_hash = r["json"]["run_hash"]
        for img_id, row in r["json"]["images"].items():
            # REF crop is content-identical across runs of the same image,
            # so dedupe to one copy per image_id.
            ref_src = row.get("ref_crop")
            if ref_src:
                ref_src = REPO / ref_src
            if ref_src and Path(ref_src).exists() and img_id not in ref_crop_for_img:
                dst_name = f"REF_{img_id}_crop_A.png"
                dst = img_dir / dst_name
                if not dst.exists():
                    shutil.copy(ref_src, dst)
                ref_crop_for_img[img_id] = dst_name
            crop_map[(run_hash, img_id, "REF_A")] = ref_crop_for_img.get(img_id)
            # PIPELINE crop per-run
            pipe_src = row.get("pipeline_crop")
            if pipe_src:
                pipe_src = REPO / pipe_src
            if pipe_src and Path(pipe_src).exists():
                dst_name = f"{run_hash}_{img_id}_PIPE_A.png"
                dst = img_dir / dst_name
                if not dst.exists():
                    shutil.copy(pipe_src, dst)
                crop_map[(run_hash, img_id, "PIPE_A")] = dst_name
        # visual diff
        vd = r["json"].get("worst_image", {}).get("visual_diff_png")
        if vd:
            vd = REPO / vd
            if vd.exists():
                dst_name = f"{run_hash}_VISUAL_DIFF.png"
                dst = img_dir / dst_name
                if not dst.exists():
                    shutil.copy(vd, dst)
                visual_diff_map[run_hash] = dst_name
    # NOTE: we do NOT copy full 8K REF/PIPELINE PNGs into the dashboard
    # (~180 MB each, no value when the dashboard is already showing crops).

    # Build per-pipeline summary rows. Dedupe by pipeline name — keep only
    # the latest run for each pipeline (most-recent run.json timestamp).
    # Older intermediate-checkpoint runs for the same pipeline are still on
    # disk for audit but the dashboard shows the current state per pipeline.
    latest_per_pipe = {}
    for r in runs:
        j = r["json"]
        pipe = j["pipeline"]
        finished_at = j.get("finished_at", "")
        prev = latest_per_pipe.get(pipe)
        if prev is None or finished_at > prev["json"].get("finished_at", ""):
            latest_per_pipe[pipe] = r

    rows = []
    for pipe, r in latest_per_pipe.items():
        j = r["json"]
        meta = pipeline_meta.get(pipe, {"role": "", "ship_class": j["ship_class"]})
        rows.append({
            "pipeline": pipe,
            "role": meta["role"],
            "ship_class": j["ship_class"],
            "verdict": j["verdict"],
            "run_hash": j["run_hash"],
            "worst_id": j["worst_image"]["id"],
            "worst_lpips": j["worst_image"]["lpips"],
            "images": j["images"],
            "worst_first": j["worst_first"],
        })
    # Sort pipelines: PASS first, then by worst LPIPS ascending
    rows.sort(key=lambda r: (r["verdict"] != "PASS", r["worst_lpips"] or 1.0))

    # Render HTML
    def fmt(v, p=3):
        if v is None: return "—"
        try:
            return f"{float(v):.{p}f}"
        except Exception:
            return "—"

    css = """
    body{font-family:system-ui,-apple-system,sans-serif;background:#0e0e10;color:#ddd;margin:0;padding:24px 36px 64px}
    h1{color:#fff;margin:0 0 6px}
    .sub{color:#888;font-size:13px;margin-bottom:24px}
    h2{color:#fff;border-bottom:1px solid #2a2a2a;padding-bottom:6px;margin-top:36px}
    h3{color:#bbb;margin:14px 0 6px}
    table{border-collapse:collapse;background:#1a1a1c;margin:8px 0;width:100%}
    th,td{border:1px solid #2c2c2e;padding:6px 10px;text-align:right;font-family:'Menlo','Monaco',monospace;font-size:11px;white-space:nowrap}
    th{background:#222;color:#fff;font-weight:600}
    th.left,td.left{text-align:left;font-family:system-ui,sans-serif}
    tr.pass td.verdict{background:#16361b;color:#7fc596;font-weight:600}
    tr.fail td.verdict{background:#3a1b1b;color:#f76;font-weight:600}
    .good{color:#7fc596}
    .warn{color:#e0c878}
    .fail{color:#f76}
    .neutral{color:#aaa}
    .row{display:flex;gap:6px;margin:8px 0;overflow-x:auto;padding:4px 0}
    .col{flex:0 0 auto;text-align:center}
    .col img.crop{width:180px;height:180px;border:1px solid #2c2c2e;image-rendering:pixelated;background:#000;display:block}
    .col img.full{width:360px;border:1px solid #2c2c2e;background:#000;display:block}
    .col img.diff{width:540px;border:1px solid #2c2c2e;background:#000;display:block}
    .col .lab{font-size:10px;color:#888;padding:3px 0;max-width:180px;overflow:hidden;text-overflow:ellipsis}
    .col .lab.wide{max-width:360px}
    .pill{display:inline-block;padding:1px 7px;border-radius:8px;font-size:10px;letter-spacing:0.5px;font-family:'Menlo',monospace}
    .pill.still{background:#1c3559;color:#9bc1e8}
    .pill.video{background:#3b2c1c;color:#e6c98b}
    .pill.preview{background:#3a1f3a;color:#d99fd9}
    .legend{background:#161618;padding:10px 14px;border-radius:6px;font-size:12px;margin-bottom:16px;border:1px solid #232325}
    code{background:#222;color:#fc6;padding:1px 5px;border-radius:3px;font-size:11px}
    a{color:#7fbfff;text-decoration:none}
    a:hover{text-decoration:underline}
    """

    html = ['<!DOCTYPE html><html><head><meta charset="utf-8">',
            '<title>Quality-gate dashboard</title>',
            f'<style>{css}</style></head><body>',
            '<h1>Quality-gate dashboard</h1>',
            '<div class="sub">All results read from <code>tests/quality_gates/runs/*/run.json</code>. '
            'Dashboard never writes verdicts — only renders what the gate decided. '
            'To change a verdict: edit <code>gates.json</code> in an isolated PR, then re-run.</div>']

    # Gate-class legend
    html.append('<div class="legend">')
    for cls, body in gates["ship_classes"].items():
        if cls.startswith("$"): continue
        per = body["per_image"]
        html.append(f'<span class="pill {cls.lower().split("_")[0]}">{cls}</span> ')
        parts = []
        for m, rule in per.items():
            if m.startswith("$"): continue
            bound = "≤" + str(rule["max"]) if "max" in rule else "≥" + str(rule["min"])
            parts.append(f"{m} {bound}")
        html.append(" · ".join(parts))
        html.append('<br>')
    html.append('</div>')

    # ---- Master summary table ----
    html.append('<h2>Pipelines — worst-image summary</h2>')
    html.append('<table><tr><th class="left">Pipeline</th><th class="left">Role</th>'
                '<th>Class</th><th>Verdict</th><th>Worst img</th><th>Worst LPIPS</th>'
                '<th class="left">Run hash</th></tr>')
    for r in rows:
        cls_class = r["ship_class"].lower().split("_")[0]
        cls_str = f'<span class="pill {cls_class}">{r["ship_class"]}</span>'
        v_cls = r["verdict"].lower()
        html.append(f'<tr class="{v_cls}">'
                    f'<td class="left"><code>{r["pipeline"]}</code></td>'
                    f'<td class="left">{r["role"]}</td>'
                    f'<td>{cls_str}</td>'
                    f'<td class="verdict">{r["verdict"]}</td>'
                    f'<td class="left">{r["worst_id"]}</td>'
                    f'<td>{fmt(r["worst_lpips"], 4)}</td>'
                    f'<td class="left">{r["run_hash"]}</td></tr>')
    html.append('</table>')

    # ---- Per-pipeline detail ----
    for r in rows:
        pipeline = r["pipeline"]
        run_hash = r["run_hash"]
        gate_rules = gates["ship_classes"][r["ship_class"]]["per_image"]
        cls_class = r["ship_class"].lower().split("_")[0]
        html.append(f'<h2><span class="pill {cls_class}">{r["ship_class"]}</span> '
                    f'&nbsp;<code>{pipeline}</code></h2>')
        html.append(f'<div class="sub">role: <code>{r["role"]}</code> · '
                    f'verdict: <strong class="{r["verdict"].lower()}">{r["verdict"]}</strong> · '
                    f'run hash: <code>{run_hash}</code></div>')

        # Worst-image visual diff
        if run_hash in visual_diff_map:
            html.append('<h3>Worst-image side-by-side (REF | PIPELINE)</h3>')
            html.append('<div class="row"><div class="col">'
                        f'<img class="diff" src="imgs/{visual_diff_map[run_hash]}">'
                        f'<div class="lab wide">{r["worst_id"]}'
                        f' · LPIPS={fmt(r["worst_lpips"], 4)}</div></div></div>')

        # Per-image metrics
        html.append('<h3>Per-image metrics (worst-first)</h3>')
        html.append('<table><tr><th class="left">Image</th><th class="left">Verdict</th>'
                    '<th>LPIPS</th><th>Y-PSNR</th><th>MS-SSIM</th><th>ΔE2000</th>'
                    '<th>bayer-PSNR codec</th><th>bayer-PSNR final</th>'
                    '<th>enc KB</th><th>enc ms</th></tr>')
        for img_id in r["worst_first"]:
            row = r["images"][img_id]
            v_cls = row["verdict"].lower()
            lc = metric_class("lpips", row.get("lpips"), gate_rules.get("lpips"))
            yc = metric_class("y_psnr", row.get("y_psnr"), gate_rules.get("y_psnr"))
            sc = metric_class("ms_ssim", row.get("ms_ssim"), gate_rules.get("ms_ssim"))
            dc = metric_class("dE2000_mean", row.get("dE2000_mean"), gate_rules.get("dE2000_mean"))
            html.append(f'<tr class="{v_cls}">'
                        f'<td class="left">{img_id}</td>'
                        f'<td class="left verdict">{row["verdict"]}</td>'
                        f'<td class="{lc}">{fmt(row.get("lpips"), 4)}</td>'
                        f'<td class="{yc}">{fmt(row.get("y_psnr"), 2)}</td>'
                        f'<td class="{sc}">{fmt(row.get("ms_ssim"), 4)}</td>'
                        f'<td class="{dc}">{fmt(row.get("dE2000_mean"), 2)}</td>'
                        f'<td>{fmt(row.get("bayer_psnr_codec"), 2)}</td>'
                        f'<td>{fmt(row.get("bayer_psnr_final"), 2)}</td>'
                        f'<td>{fmt((row.get("enc_bytes") or 0)/1024, 0)}</td>'
                        f'<td>{fmt(row.get("enc_ms"), 1)}</td></tr>')
        html.append('</table>')

        # Crops (A_detail) for each image
        html.append('<h3>A_detail crops (100% pixels) — REF on the left of each pair</h3>')
        for img_id in r["worst_first"]:
            ref_crop = crop_map.get((run_hash, img_id, "REF_A"))
            pipe_crop = crop_map.get((run_hash, img_id, "PIPE_A"))
            if not (ref_crop and pipe_crop): continue
            html.append(f'<div class="row">'
                        f'<div class="col"><img class="crop" src="imgs/{ref_crop}">'
                        f'<div class="lab">{img_id} REF</div></div>'
                        f'<div class="col"><img class="crop" src="imgs/{pipe_crop}">'
                        f'<div class="lab">{img_id} PIPELINE · LPIPS={fmt(r["images"][img_id].get("lpips"), 4)}</div></div>'
                        f'</div>')

    # ---- Image-major view: same image across all pipelines ----
    html.append('<h2>Cross-pipeline by image (A_detail crops)</h2>')
    for img_id in all_images:
        html.append(f'<h3>{img_id}</h3>')
        html.append('<div class="row">')
        # REF first (shared crop)
        ref_name = ref_crop_for_img.get(img_id)
        if ref_name:
            html.append(f'<div class="col"><img class="crop" src="imgs/{ref_name}">'
                        f'<div class="lab"><strong>REF</strong></div></div>')
        for r in rows:
            run_hash = r["run_hash"]
            pipe_crop = crop_map.get((run_hash, img_id, "PIPE_A"))
            if not pipe_crop:
                continue
            row = r["images"].get(img_id, {})
            v_cls = row.get("verdict", "").lower() or "neutral"
            short = r["role"] or r["pipeline"].split("+")[0].replace("codec=", "")
            html.append(f'<div class="col"><img class="crop" src="imgs/{pipe_crop}">'
                        f'<div class="lab {v_cls}">{short}<br>LPIPS={fmt(row.get("lpips"), 3)}</div></div>')
        html.append('</div>')

    html.append('</body></html>')
    out = out_dir / "index.html"
    out.write_text("\n".join(html))
    print(f"dashboard: {out}")


if __name__ == "__main__":
    main()
