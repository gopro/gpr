#!/usr/bin/env python3
"""Quality-gate runner — the SINGLE source of truth for ship verdicts.

This script is the only thing allowed to produce a "PASS" / "FAIL" verdict
for any pipeline. If you're claiming a pipeline ships, you must reference
a run-hash this script emitted.

Design constraints (don't undo these without reading docs/quality_gates.md):
  - Per-image evaluation, never aggregate-only. Worst image governs.
  - Crop positions, eval resolution, and thresholds are file-fixed in
    test_set.json and gates.json. The script never picks them.
  - Pipelines are looked up by name in pipelines/registry.json. No
    inline overrides.
  - Visual diff PNG is written. If it isn't, verdict is INDETERMINATE.
  - The script writes a JSON run log to tests/quality_gates/runs/. The
    run-hash is deterministic from inputs — same inputs = same hash.
  - A failing image is reported with its filename AND the metric values
    that failed. No silent failures.

Usage:
  python3 tests/quality_gates/run_gate.py PIPELINE_NAME [--claim]

Example:
  python3 tests/quality_gates/run_gate.py \
      'codec=ml2_q3+cnn=none+demosaic=sips_via_gpr_tools'

Exit codes:
  0   PASS — all per-image metrics under their gate-class thresholds
  1   FAIL — at least one image failed at least one metric
  2   INDETERMINATE — visual diff wasn't produced, source missing, etc.
  3   USAGE_ERROR — bad invocation, unknown pipeline, etc.
"""
from __future__ import annotations
import argparse
import concurrent.futures as _cf
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
from validation_registry import load_validation_registry
GATES_PATH = REPO / "tests/quality_gates/gates.json"
TEST_SET_PATH = REPO / "tests/quality_gates/test_set.json"
REGISTRY_PATH = REPO / "pipelines/registry.json"
RUNS_DIR = REPO / "tests/quality_gates/runs"
CLAIMS_LOG = REPO / "docs/claims_log.md"

sys.path.insert(0, str(REPO / "tools/test"))
from metrics import compute_visual_metrics, bayer_psnr  # noqa: E402


# --------------------------------------------------------------------- helpers


def die(code: int, msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(code)


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()


def artifact_candidates(path_value: str) -> list[Path]:
    """Candidate locations for checkpoint/model artifacts.

    Registry paths stay portable (`models/foo.pt`), while production binaries
    live off-main under the external drive. Environment roots may contain
    os.pathsep-separated entries for staging/release testing.
    """
    path = Path(path_value)
    if path.is_absolute():
        return [path]

    roots: list[tuple[Path, bool]] = [(REPO, False)]
    for key in ("GPR_MODEL_ROOT", "GPR_CHECKPOINT_ROOT"):
        for item in os.environ.get(key, "").split(os.pathsep):
            if item:
                roots.append((Path(item), True))
    external_root = Path(os.environ.get("GPR_EXTERNAL_ROOT", REPO))
    roots.extend([
        (external_root, False),
        (external_root / "models", True),
        (external_root / "checkpoints", True),
    ])

    candidates: list[Path] = []
    for root, prefer_stripped in roots:
        stripped = None
        if path.parts and path.parts[0] in {"models", "checkpoints"}:
            stripped = root / Path(*path.parts[1:])
        if prefer_stripped and stripped is not None:
            candidates.append(stripped)
            candidates.append(root / path)
        else:
            candidates.append(root / path)
            if stripped is not None:
                candidates.append(stripped)

    unique = []
    seen = set()
    for candidate in candidates:
        key = str(candidate)
        if key not in seen:
            seen.add(key)
            unique.append(candidate)
    return unique


def resolve_artifact_path(path_value: str, label: str = "artifact") -> Path:
    candidates = artifact_candidates(path_value)
    for candidate in candidates:
        if candidate.exists():
            return candidate
    searched = ", ".join(str(p) for p in candidates[:6])
    die(2, f"{label} missing: {path_value}. Searched: {searched}")


def canonical_env(env: dict) -> str:
    return ";".join(f"{k}={env[k]}" for k in sorted(env))


def load_json(p: Path) -> dict:
    if not p.exists():
        die(3, f"missing: {p}")
    return json.loads(p.read_text())


def parse_source_path_map(raw: str | None) -> list[tuple[str, str]]:
    """Parse FROM=TO mappings for running the frozen gate on another host.

    Example:
      GATE_SOURCE_PATH_MAP='artifacts/visual_compare_20260525/source_dngs=fixtures'

    Multiple mappings are separated by semicolons. Longest source prefix wins.
    """
    if not raw:
        return []
    mappings: list[tuple[str, str]] = []
    for item in raw.split(";"):
        item = item.strip()
        if not item:
            continue
        if "=" not in item:
            die(3, f"invalid GATE_SOURCE_PATH_MAP entry {item!r}; expected FROM=TO")
        src, dst = item.split("=", 1)
        src = src.rstrip("/")
        dst = dst.rstrip("/")
        if not src or not dst:
            die(3, f"invalid GATE_SOURCE_PATH_MAP entry {item!r}; FROM and TO are required")
        mappings.append((src, dst))
    mappings.sort(key=lambda pair: len(pair[0]), reverse=True)
    return mappings


def apply_source_path_map(test_set: dict, mappings: list[tuple[str, str]]) -> list[dict]:
    images = [dict(im) for im in test_set["images"]]
    if not mappings:
        return images
    for im in images:
        original = str(im["path"])
        for src, dst in mappings:
            if original == src or original.startswith(src + "/"):
                im["path_original"] = original
                im["path"] = dst + original[len(src):]
                break
    return images


def _path_is_system_tmp(path: Path) -> bool:
    try:
        resolved = path.expanduser().resolve()
    except OSError:
        resolved = path.expanduser().absolute()
    system_roots = (
        Path("/tmp"),
        Path("/private/tmp"),
        Path("/var/tmp"),
        Path("/private/var/folders"),
    )
    return any(resolved == root or root in resolved.parents for root in system_roots)


def enforce_large_gate_scratch_policy(images: list[dict], gate_tmpdir: str | None) -> None:
    """Keep full-image quality gates from accidentally using the boot disk.

    The ship gate renders 50MP REF/PIPELINE PNGs and may load PyTorch/LPIPS
    inside multiple worker processes. Losing an interrupted run to local tmp is
    recoverable; exhausting the boot volume is not. Small synthetic tests are
    unaffected, but large real-image gates must opt into an explicit scratch
    location or set GATE_ALLOW_LOCAL_TMP=1.
    """
    if os.environ.get("GATE_ALLOW_LOCAL_TMP", "") not in ("", "0"):
        return

    max_pixels = 0
    total_pixels = 0
    for im in images:
        try:
            import tifffile
            with tifffile.TiffFile(im["path"]) as tf:
                page_pixels = 0
                for page in tf.pages:
                    candidates = [page]
                    try:
                        candidates.extend(list(getattr(page, "pages", None) or []))
                    except Exception:
                        pass
                    for candidate in candidates:
                        shape = getattr(candidate, "shape", None)
                        if shape is not None and len(shape) == 2:
                            page_pixels = max(page_pixels, int(shape[0]) * int(shape[1]))
        except Exception:
            continue
        max_pixels = max(max_pixels, page_pixels)
        total_pixels += page_pixels

    # Z8 gate images are ~46MP each. This threshold keeps tiny fixtures and
    # quick unit-style runs frictionless while protecting production gates.
    if max_pixels < 20_000_000 and total_pixels < 80_000_000:
        return

    if not gate_tmpdir:
        die(
            3,
            "large quality gate requires GATE_TMPDIR on external scratch. "
            "Use: tools/dev/external_drive_env.sh python3 tests/quality_gates/run_gate.py ... "
            "or set GATE_ALLOW_LOCAL_TMP=1 intentionally.",
        )

    scratch = Path(gate_tmpdir).expanduser()
    if _path_is_system_tmp(scratch):
        die(
            3,
            f"large quality gate refuses system temp scratch: {scratch}. "
            "Point GATE_TMPDIR at an artifact drive or set "
            "GATE_ALLOW_LOCAL_TMP=1 intentionally.",
        )

    scratch.mkdir(parents=True, exist_ok=True)
    min_free_gb = float(os.environ.get("GATE_MIN_FREE_GB", "25"))
    free_gb = shutil.disk_usage(scratch).free / (1024 ** 3)
    if free_gb < min_free_gb:
        die(
            3,
            f"large quality gate scratch has only {free_gb:.1f} GiB free at {scratch}; "
            f"requires at least {min_free_gb:.1f} GiB. Override with GATE_MIN_FREE_GB "
            "only after cleaning space.",
        )


# --------------------------------------------------------------------- core


def read_source_bayer(dng_path: str) -> tuple[np.ndarray, int, int]:
    import tifffile
    with tifffile.TiffFile(dng_path) as tf:
        best = None
        best_size = 0
        for page in tf.pages:
            candidates = [page]
            try:
                candidates.extend(list(getattr(page, "pages", None) or []))
            except Exception:
                pass
            for candidate in candidates:
                try:
                    shape = candidate.shape
                except Exception:
                    continue
                if len(shape) != 2:
                    continue
                size = int(shape[0]) * int(shape[1])
                if size > best_size:
                    best = candidate
                    best_size = size
        if best is None:
            die(2, f"source DNG has no 2D bayer page: {dng_path}")
        raw = best.asarray()
    h, w = raw.shape
    return raw.astype("<u2"), w, h


def _encode_decode_legacy_gpr_tools(codec: dict, bayer: np.ndarray,
                                    w: int, h: int, workdir: Path,
                                    src_dng: str) -> tuple[np.ndarray, int, float]:
    """Encode via legacy gpr_tools. Different CLI than test_fused_roundtrip
    AND different encode mode: gpr_tools' raw-input path produces .gpr files
    that the decoder can't always reconstruct (missing tags). Instead we
    encode the source DNG directly (matches build_dataset_gpr_tools_q3.py).
    Bayer argument is unused — we go from src_dng to .gpr to bayer."""
    binary = REPO / codec["binary"]
    if not binary.exists():
        die(2, f"codec binary not built: {binary}")
    quality = codec.get("quality", 3)
    gpr_path = workdir / "encoded.gpr"
    t0 = time.time()
    r = subprocess.run([str(binary), "-i", str(src_dng), "-q", str(quality),
                        "-o", str(gpr_path)], capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        die(2, f"legacy encode failed: {r.stderr[-200:]}")
    enc_bytes = gpr_path.stat().st_size
    dec_dng = workdir / "decoded.dng"
    r = subprocess.run([str(binary), "-i", str(gpr_path), "-o", str(dec_dng)],
                       capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        die(2, f"legacy decode failed: {r.stderr[-200:]}")
    enc_ms = (time.time() - t0) * 1000.0
    import tifffile
    def _find_bayer(pages):
        best = None; best_size = 0
        for pg in pages:
            try:
                sh = pg.shape
                if len(sh) == 2 and sh[0]*sh[1] > best_size:
                    best, best_size = pg, sh[0]*sh[1]
            except Exception: pass
            try:
                for sp in getattr(pg, "pages", None) or []:
                    try:
                        sh = sp.shape
                        if len(sh) == 2 and sh[0]*sh[1] > best_size:
                            best, best_size = sp, sh[0]*sh[1]
                    except Exception: pass
            except Exception: pass
        return best
    with tifffile.TiffFile(dec_dng) as tf:
        page = _find_bayer(tf.pages)
        if page is None:
            die(2, "legacy decode: no bayer page")
        dec_bayer = page.asarray().astype("<u2")
    return dec_bayer, enc_bytes, enc_ms


def encode_decode(codec: dict, bayer: np.ndarray, w: int, h: int,
                  workdir: Path, src_dng: str = None) -> tuple[np.ndarray, int, float]:
    """Returns (decoded_bayer, enc_bytes, enc_ms). Decoded dims may be
    half of (w, h) when GPR_ROW_DECIMATE=2 and GPR_COL_DECIMATE=2 are set
    in the codec env — the codec emits a half-res bayer. Caller is
    expected to detect this and apply a super-res CNN to restore to
    (w, h) before metric comparison against the full-res REF."""
    if codec.get("encoder_kind") == "legacy_gpr_tools":
        if src_dng is None:
            die(3, "legacy_gpr_tools codec requires src_dng to be passed to encode_decode")
        return _encode_decode_legacy_gpr_tools(codec, bayer, w, h, workdir, src_dng)
    binary = REPO / codec["binary"]
    if not binary.exists():
        die(2, f"codec binary not built: {binary}")
    env = os.environ.copy()
    env.update({k: str(v) for k, v in codec.get("env", {}).items()})
    if "quality" in codec and "FUSED_QUALITY" not in env:
        env["FUSED_QUALITY"] = str(codec["quality"])
    in_raw = workdir / "in.raw"
    out_raw = workdir / "out.raw"
    if not in_raw.exists():
        bayer.tofile(in_raw)
    t0 = time.time()
    r = subprocess.run(
        [str(binary), str(in_raw), str(w), str(h), str(out_raw)],
        env=env, capture_output=True, text=True, timeout=300,
    )
    enc_ms = (time.time() - t0) * 1000.0
    if r.returncode != 0:
        die(2, f"codec failed rc={r.returncode}: {r.stderr[-300:]}")
    import re
    m = re.search(r"ENCODE: (\d+) bytes", r.stderr)
    enc_bytes = int(m.group(1)) if m else 0
    me = re.search(r"ENCODE.*in ([\d.]+) ms", r.stderr)
    enc_ms_reported = float(me.group(1)) if me else enc_ms
    # Detect actual decoded dims from the output file size — the codec
    # halves (w, h) when decimation is on. Reading at the wrong dims
    # yields a misshaped numpy array which is obvious to catch.
    nbytes = os.path.getsize(out_raw)
    dec_w, dec_h = w, h
    if nbytes == (w // 2) * (h // 2) * 2:
        dec_w, dec_h = w // 2, h // 2
    elif nbytes != w * h * 2:
        die(2, f"decoded bayer size {nbytes} bytes doesn't match either "
               f"full ({w}x{h}={w*h*2}) or half ({w//2}x{h//2}={(w//2)*(h//2)*2})")
    dec = np.fromfile(out_raw, dtype=np.uint16).reshape(dec_h, dec_w)
    return dec, enc_bytes, enc_ms_reported


def apply_cnn(
    bayer: np.ndarray,
    cnn: dict,
    dms: dict | None = None,
    src_dng: Path | None = None,
    workdir: Path | None = None,
    full_size: tuple[int, int] | None = None,
):
    """Apply a CNN to a bayer plane. Supports Bayer and joint RGB architectures:
      - 1x denoise (variant F_ane_no_sr): output dims = input dims.
        Result = input + residual_scale * CNN(input). Returns bayer (np.uint16).
      - 2x super-res (variant F_ane): output dims = 2*input dims.
        Result = bicubic(input, 2x) + residual_scale * CNN(input). Returns bayer.
      - Joint demosaic+super-res (variant F_ane_dm_sr): output is 4×spatial RGB,
        not bayer. Returns ("rgb", H×W×3 uint8 image).
    The variant is read from the checkpoint metadata; the registry's
    cnn_arch_variant is a fallback. The "rgb" return tag signals downstream
    code to skip demosaic_to_png and use the RGB result directly."""
    if cnn.get("cnn_arch_variant") == "preview_scene_routed_rgb_refiner":
        die(
            2,
            "preview_scene_routed_rgb_refiner is an external-receipt PREVIEW "
            "candidate; evaluate it with tools/cnn/evaluate_preview_scene_routed.py",
        )
    if cnn.get("cnn_arch_variant") == "preview_q8_threeway_runtime_fullframe":
        die(
            2,
            "preview_q8_threeway_runtime_fullframe is an external-receipt "
            "PREVIEW candidate; evaluate it with "
            "tools/cnn/evaluate_preview_q8_threeway_runtime_fullframe.py",
        )
    if (cnn.get("ckpt_path") is None
            and cnn.get("cnn_arch_variant") not in ("ycbcr_decomp", "lab_chroma_corrector")):
        return bayer
    if cnn.get("cnn_arch_variant") == "ycbcr_decomp":
        die(2, "ycbcr_decomp is archived, not a supported registry pipeline. "
               "See archive/research-and-integration-2026-09-09 for that experiment.")
    if cnn.get("cnn_arch_variant") == "lab_chroma_corrector":
        sys.path.insert(0, str(REPO / "tools/cnn"))
        from run_lab_chroma_corrector import run_lab_chroma_corrector
        y_path = cnn.get("ckpt_y")
        chroma_path = cnn.get("ckpt_chroma") or cnn.get("ckpt_path")
        if y_path is None:
            die(2, "lab_chroma_corrector CNN entry missing field 'ckpt_y'")
        if chroma_path is None:
            die(2, "lab_chroma_corrector CNN entry missing field 'ckpt_chroma'")
        y_cp = resolve_artifact_path(y_path, "lab_chroma_corrector Y checkpoint")
        c_cp = resolve_artifact_path(chroma_path, "lab_chroma_corrector chroma checkpoint")
        raw_norm = cnn.get("raw_norm", 16383.0)
        baseline_rgb = None
        if cnn.get("chroma_baseline") == "demosaic_sips":
            if dms is None or src_dng is None or workdir is None or full_size is None:
                die(2, "lab_chroma_corrector demosaic_sips baseline requires dms/src_dng/workdir/full_size")
            baseline_png = workdir / "lab_chroma_baseline.png"
            demosaic_to_png(bayer, dms, src_dng, workdir, baseline_png, upscale_to=full_size)
            baseline_rgb = np.asarray(Image.open(baseline_png).convert("RGB"))
        rgb_u8 = run_lab_chroma_corrector(
            bayer, str(y_cp), str(c_cp), baseline_rgb_u8=baseline_rgb, raw_norm=raw_norm,
            luma_unsharp_amount=cnn.get("luma_unsharp_amount", 0.0),
            luma_unsharp_sigma=cnn.get("luma_unsharp_sigma", 2.0),
            luma_detail_refiner_path=(
                str(resolve_artifact_path(cnn["luma_detail_refiner"], "luma detail refiner checkpoint"))
                if cnn.get("luma_detail_refiner") else None
            ),
            luma_detail_cnn_path=(
                str(resolve_artifact_path(cnn["ckpt_detail"], "luma detail CNN checkpoint"))
                if cnn.get("ckpt_detail") else None
            ),
            luma_detail_cnn_strength=cnn.get("luma_detail_cnn_strength", 1.0),
            rgb_detail_cnn_path=(
                str(resolve_artifact_path(cnn["ckpt_rgb_detail"], "RGB detail CNN checkpoint"))
                if cnn.get("ckpt_rgb_detail") else None
            ),
            luma_wavelet_hf_gain=cnn.get("luma_wavelet_hf_gain", 1.0),
            luma_wavelet_hf_wavelet=cnn.get("luma_wavelet_hf_wavelet", "sym4"),
            luma_wavelet_hf_levels=cnn.get("luma_wavelet_hf_levels", 3),
            luma_wavelet_hf_hf_levels=cnn.get("luma_wavelet_hf_hf_levels", 1),
            luma_wavelet_hf_max_delta=cnn.get("luma_wavelet_hf_max_delta", 2.0),
        )
        return ("rgb", rgb_u8)
    if cnn.get("cnn_arch_variant") == "codec_raw_signal_sr":
        return apply_codec_raw_signal_sr(bayer, cnn, src_dng, full_size=full_size)
    import torch
    import torch.nn.functional as F
    sys.path.insert(0, str(REPO / "tools/cnn"))
    from model import build as build_variant
    ckpt_path = resolve_artifact_path(cnn["ckpt_path"], "CNN checkpoint")
    ck = torch.load(str(ckpt_path), map_location="cpu", weights_only=False)
    variant = ck.get("variant", cnn["cnn_arch_variant"])
    # BIDO = Bayer In, Demosaic Out (joint demosaic + super-res). Legacy
    # variant name "dm_sr" still recognized.
    is_dm_sr = "dm_sr" in variant or "bido" in variant.lower()
    is_sr2x = (not is_dm_sr) and "no_sr" not in variant
    m = build_variant(variant)
    m.load_state_dict(ck["backbone_state"])
    dev = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    m.to(dev).eval()
    h, w = bayer.shape
    eh, ew = h - (h & 1), w - (w & 1)
    b = bayer[:eh, :ew]
    pl = np.stack([b[0::2, 0::2], b[0::2, 1::2], b[1::2, 0::2], b[1::2, 1::2]], 0)
    raw_norm = cnn.get("raw_norm", 16383.0)
    res_scale = cnn.get("residual_scale", 0.01)
    x = torch.from_numpy(pl.astype(np.float32) / raw_norm).unsqueeze(0).to(dev)
    H, W = x.shape[-2:]
    ph = (16 - H % 16) % 16
    pw = (16 - W % 16) % 16
    if ph or pw:
        x = F.pad(x, (0, pw, 0, ph), mode="reflect")
    with torch.no_grad():
        if is_dm_sr:
            # Joint demosaic + super-res: output is 3ch RGB at 4× spatial.
            # No baseline-plus-residual — the model produces RGB directly.
            y = m(x).clamp(0, 1)
            # Output dims: (B, 3, 4H, 4W). Convert to (H', W', 3) uint8 for PNG.
            y = y[..., :4*H, :4*W].squeeze(0).cpu().numpy()       # (3, 4H, 4W)
            y = np.transpose(y, (1, 2, 0))                          # (4H, 4W, 3)
            rgb_u8 = np.clip(y * 255.0, 0, 255).astype(np.uint8)
            return ("rgb", rgb_u8)
        if is_sr2x:
            base = F.interpolate(x, scale_factor=2, mode="bicubic", align_corners=False).clamp(0, 1)
            cnn_out = m(x)
            y = (base + res_scale * cnn_out).clamp(0, 1)
            y = y[..., :2 * H, :2 * W].squeeze(0).cpu().numpy()
            # Output is 2x in plane dims → 2x in bayer dims
            out_eh, out_ew = 2 * eh, 2 * ew
            out = np.zeros((out_eh, out_ew), dtype=np.uint16)
            out[0::2, 0::2] = np.clip(y[0] * raw_norm, 0, raw_norm).astype(np.uint16)
            out[0::2, 1::2] = np.clip(y[1] * raw_norm, 0, raw_norm).astype(np.uint16)
            out[1::2, 0::2] = np.clip(y[2] * raw_norm, 0, raw_norm).astype(np.uint16)
            out[1::2, 1::2] = np.clip(y[3] * raw_norm, 0, raw_norm).astype(np.uint16)
            return out
        else:
            y = (x + res_scale * m(x)).clamp(0, 1)
            y = y[..., :H, :W].squeeze(0).cpu().numpy()
            out = bayer.copy()
            out[:eh:2, :ew:2] = np.clip(y[0] * raw_norm, 0, raw_norm).astype(np.uint16)
            out[:eh:2, 1:ew:2] = np.clip(y[1] * raw_norm, 0, raw_norm).astype(np.uint16)
            out[1:eh:2, :ew:2] = np.clip(y[2] * raw_norm, 0, raw_norm).astype(np.uint16)
            out[1:eh:2, 1:ew:2] = np.clip(y[3] * raw_norm, 0, raw_norm).astype(np.uint16)
            return out


def _deinterleave_bayer(arr: np.ndarray) -> np.ndarray:
    return np.stack(
        [arr[0::2, 0::2], arr[0::2, 1::2], arr[1::2, 0::2], arr[1::2, 1::2]],
        axis=0,
    ).astype(np.float32)


def _interleave_bayer_planes(planes: np.ndarray, shape: tuple[int, int], raw_norm: float) -> np.ndarray:
    out = np.zeros(shape, dtype=np.uint16)
    out[0::2, 0::2] = np.clip(planes[0] * raw_norm, 0, raw_norm).astype(np.uint16)
    out[0::2, 1::2] = np.clip(planes[1] * raw_norm, 0, raw_norm).astype(np.uint16)
    out[1::2, 0::2] = np.clip(planes[2] * raw_norm, 0, raw_norm).astype(np.uint16)
    out[1::2, 1::2] = np.clip(planes[3] * raw_norm, 0, raw_norm).astype(np.uint16)
    return out


def _codec_hf_rms_counts(planes_counts: np.ndarray) -> float:
    pad = np.pad(planes_counts, ((0, 0), (1, 1), (1, 1)), mode="edge")
    mean4 = (
        pad[:, 1:-1, :-2]
        + pad[:, 1:-1, 2:]
        + pad[:, :-2, 1:-1]
        + pad[:, 2:, 1:-1]
    ) * 0.25
    return float(np.sqrt(np.mean((planes_counts - mean4) ** 2)))


_RAW_SIGNAL_SR_MODEL_CACHE = {}


def apply_codec_raw_signal_sr(
    bayer: np.ndarray,
    cnn: dict,
    src_dng: Path | None,
    full_size: tuple[int, int] | None = None,
):
    """Apply the experimental codec raw-signal SR checkpoint with tiled inference."""
    if src_dng is None:
        die(2, "codec_raw_signal_sr requires source DNG metadata")

    t_start = time.perf_counter()
    import torch
    import torch.nn.functional as F
    sys.path.insert(0, str(REPO / "tools/cnn"))
    from analyze_dng_noise_profile import noise_sigma_map, read_bayer, read_dng_meta
    from train_codec_raw_clean_sr import CodecRawCleanSR

    ckpt_path = resolve_artifact_path(cnn["ckpt_path"], "codec_raw_signal_sr checkpoint")
    dev = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    cache_key = (str(ckpt_path), str(dev))
    model_load_ms = 0.0
    if cache_key in _RAW_SIGNAL_SR_MODEL_CACHE:
        ck, model = _RAW_SIGNAL_SR_MODEL_CACHE[cache_key]
    else:
        t_load = time.perf_counter()
        ck = torch.load(str(ckpt_path), map_location="cpu", weights_only=False)
        model = CodecRawCleanSR(int(ck["width"]), in_channels=int(ck.get("in_channels", 10)))
        model.load_state_dict(ck["state_dict"])
        model.to(dev).eval()
        _RAW_SIGNAL_SR_MODEL_CACHE[cache_key] = (ck, model)
        model_load_ms = (time.perf_counter() - t_load) * 1000.0
    raw_norm = float(cnn.get("raw_norm", ck.get("raw_scale", 16383.0)))

    meta = read_dng_meta(src_dng.stem, src_dng)
    source_raw = None
    if full_size is None:
        source_raw = read_bayer(src_dng)
        target_h, target_w = source_raw.shape[0] // 2, source_raw.shape[1] // 2
    else:
        full_w, full_h = full_size
        target_h, target_w = full_h // 2, full_w // 2

    codec_planes = _deinterleave_bayer(bayer)
    with torch.no_grad():
        codec_up = F.interpolate(
            torch.from_numpy(codec_planes[None] / raw_norm),
            size=(target_h, target_w),
            mode="bilinear",
            align_corners=False,
        ).numpy()[0].astype(np.float32)

    iso_threshold = int(cnn.get("dispatch_iso_threshold", 100))
    hf_threshold = float(cnn.get("dispatch_hf_threshold", 1.7410857677459717))
    conditioning = str(cnn.get("conditioning", ck.get("conditioning", "iso")))
    if conditioning not in {"sigma", "iso", "iso_only"}:
        die(2, f"unsupported codec_raw_signal_sr conditioning={conditioning!r}")
    needs_sigma = conditioning in {"sigma", "iso"}
    sigma_source = str(cnn.get("sigma_source", "source_raw")) if needs_sigma else "unused"
    sigma_planes = None
    if needs_sigma:
        if sigma_source == "source_raw":
            if source_raw is None:
                source_raw = read_bayer(src_dng)
            sigma_raw = source_raw
        elif sigma_source == "codec_up":
            sigma_raw = _interleave_bayer_planes(codec_up, (target_h * 2, target_w * 2), raw_norm)
        else:
            die(2, f"unsupported codec_raw_signal_sr sigma_source={sigma_source!r}")
        sigma_planes = _deinterleave_bayer(noise_sigma_map(sigma_raw, meta)) / raw_norm
    tile = int(cnn.get("tile", 256))
    overlap = int(cnn.get("overlap", 24))
    inference_batch = max(1, int(cnn.get("inference_batch", 1)))
    out = np.empty_like(codec_up, dtype=np.float32)
    tiles_total = 0
    tiles_model = 0
    tiles_bypass = 0
    inference_ms = 0.0
    pending_jobs = {}

    def flush_jobs(jobs):
        nonlocal inference_ms
        if not jobs:
            return
        x_np = np.concatenate([job[0] for job in jobs], axis=0)
        t_infer = time.perf_counter()
        preds = model(torch.from_numpy(x_np).to(dev)).cpu().numpy()
        inference_ms += (time.perf_counter() - t_infer) * 1000.0
        for pred, job in zip(preds, jobs):
            _, y0, y1, x0, x1, yy0, yy1, xx0, xx1 = job
            out[:, y0:y1, x0:x1] = pred[:, yy0:yy1, xx0:xx1]

    with torch.no_grad():
        for y0 in range(0, target_h, tile):
            y1 = min(target_h, y0 + tile)
            yc0 = max(0, y0 - overlap)
            yc1 = min(target_h, y1 + overlap)
            for x0 in range(0, target_w, tile):
                x1 = min(target_w, x0 + tile)
                xc0 = max(0, x0 - overlap)
                xc1 = min(target_w, x1 + overlap)
                core_counts = codec_up[:, y0:y1, x0:x1] * raw_norm
                use_model = meta.iso >= iso_threshold or _codec_hf_rms_counts(core_counts) >= hf_threshold
                tiles_total += 1
                if not use_model:
                    tiles_bypass += 1
                    out[:, y0:y1, x0:x1] = codec_up[:, y0:y1, x0:x1]
                    continue
                tiles_model += 1
                x_parts = [codec_up[:, yc0:yc1, xc0:xc1]]
                if needs_sigma:
                    assert sigma_planes is not None
                    x_parts.append(sigma_planes[:, yc0:yc1, xc0:xc1])
                if conditioning == "iso":
                    hh, ww = yc1 - yc0, xc1 - xc0
                    iso_norm = np.clip(np.log2(max(float(meta.iso), 1.0) / 64.0) / 8.0, 0.0, 1.0)
                    assert sigma_planes is not None
                    sigma_rms = float(np.sqrt(np.mean(sigma_planes[:, yc0:yc1, xc0:xc1] ** 2)))
                    x_parts.append(np.full((1, hh, ww), iso_norm, dtype=np.float32))
                    x_parts.append(np.full((1, hh, ww), sigma_rms, dtype=np.float32))
                elif conditioning == "iso_only":
                    hh, ww = yc1 - yc0, xc1 - xc0
                    iso_norm = np.clip(np.log2(max(float(meta.iso), 1.0) / 64.0) / 8.0, 0.0, 1.0)
                    x_parts.append(np.full((1, hh, ww), iso_norm, dtype=np.float32))
                x_np = np.concatenate(x_parts, axis=0)[None].astype(np.float32)
                yy0 = y0 - yc0
                yy1 = yy0 + (y1 - y0)
                xx0 = x0 - xc0
                xx1 = xx0 + (x1 - x0)
                key = x_np.shape
                jobs = pending_jobs.setdefault(key, [])
                jobs.append((x_np, y0, y1, x0, x1, yy0, yy1, xx0, xx1))
                if len(jobs) >= inference_batch:
                    flush_jobs(jobs)
                    jobs.clear()
        for jobs in pending_jobs.values():
            flush_jobs(jobs)

    stats = {
        "cnn_model_load_ms": model_load_ms,
        "cnn_inference_ms": inference_ms,
        "cnn_total_apply_ms": (time.perf_counter() - t_start) * 1000.0,
        "cnn_tiles_total": tiles_total,
        "cnn_tiles_model": tiles_model,
        "cnn_tiles_bypass": tiles_bypass,
        "cnn_device": str(dev),
        "dispatch_iso_threshold": iso_threshold,
        "dispatch_hf_threshold": hf_threshold,
        "inference_batch": inference_batch,
        "sigma_source": sigma_source,
    }
    return ("bayer_stats", _interleave_bayer_planes(out, (target_h * 2, target_w * 2), raw_norm), stats)


def demosaic_to_png(bayer: np.ndarray, dms: dict, src_dng: Path,
                    workdir: Path, out_png: Path,
                    upscale_to: tuple[int, int] | None = None) -> None:
    """Demosaic a bayer plane to a PNG via gpr_tools + sips. The PNG
    natural dims = bayer dims. If `upscale_to` is given, the PNG is
    bicubic-resized to those (width, height) — used for half-res
    pipelines so crop coords and metric comparisons match the source-DNG
    reference space."""
    binary = REPO / dms["binary"]
    if not binary.exists():
        die(2, f"demosaic binary not built: {binary}")
    # Extract source-DNG params (color matrix etc.)
    params_cache = workdir / "params.json"
    if not params_cache.exists():
        cp = subprocess.run([str(binary), "-i", str(src_dng), "-d", "1"],
                            capture_output=True, text=True)
        if cp.returncode != 0:
            die(2, f"gpr_tools params dump failed: {cp.stderr[-200:]}")
        lines = [l for l in cp.stdout.splitlines() if not l.startswith("[")]
        params_cache.write_text("\n".join(lines))
    params = json.loads(params_cache.read_text())
    h, w = bayer.shape
    params["input_width"] = w
    params["input_height"] = h
    params["input_pitch"] = w * 2
    params_run = workdir / f"params_{w}x{h}.json"
    params_run.write_text(json.dumps(params))
    raw_in = workdir / "bayer.raw"
    bayer.tofile(raw_in)
    dng_out = workdir / "out.dng"
    r = subprocess.run([str(binary), "-i", str(raw_in), "-w", str(w), "-h", str(h),
                        "-x", "rggb14", "-o", str(dng_out), "-a", str(params_run)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        die(2, f"gpr_tools DNG write failed: {r.stderr[-200:]}")
    r = subprocess.run(["sips", "-s", "format", "png", str(dng_out), "--out", str(out_png)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        die(2, f"sips render failed: {r.stderr[-200:]}")
    # Bicubic-upscale the rendered PNG to reference dims so crops in
    # test_set.json coords are content-aligned across all pipelines,
    # regardless of whether the codec decimated. Without this step a
    # half-res pipeline's PNG is at half-source-dims and the gate's
    # crop coords point at completely different content from the REF.
    if upscale_to is not None:
        img = Image.open(out_png).convert("RGB")
        if img.size != upscale_to:
            img.resize(upscale_to, Image.BICUBIC).save(out_png)


def downsample_for_metrics(png_path: Path, target_w: int) -> np.ndarray:
    img = Image.open(png_path).convert("RGB")
    W, H = img.size
    if W > target_w:
        scale = target_w / W
        img = img.resize((target_w, int(H * scale)), Image.LANCZOS)
    return np.array(img)


def crop_at(png_path: Path, crop: dict, out_path: Path) -> None:
    img = Image.open(png_path).convert("RGB")
    W, H = img.size
    x, y, cw, ch = crop["x"], crop["y"], crop["w"], crop["h"]
    x = max(0, min(x, W - cw))
    y = max(0, min(y, H - ch))
    img.crop((x, y, x + cw, y + ch)).save(out_path)


def build_visual_diff(ref_crop: Path, test_crop: Path, last_best_crop: Path | None,
                      out_path: Path, title: str) -> None:
    """Side-by-side image: REF | this pipeline | (optional) last-best.
    Forces visual inspection at fixed dimensions."""
    parts = [Image.open(ref_crop).convert("RGB"), Image.open(test_crop).convert("RGB")]
    if last_best_crop and last_best_crop.exists():
        parts.append(Image.open(last_best_crop).convert("RGB"))
    cw = max(p.width for p in parts)
    ch = max(p.height for p in parts)
    pad = 8
    total_w = cw * len(parts) + pad * (len(parts) + 1)
    total_h = ch + pad * 2 + 24  # space for label band
    diff = Image.new("RGB", (total_w, total_h), (24, 24, 24))
    for i, p in enumerate(parts):
        diff.paste(p, (pad + i * (cw + pad), pad))
    from PIL import ImageDraw
    d = ImageDraw.Draw(diff)
    labels = ["REF", "PIPELINE"] + (["LAST_BEST"] if len(parts) == 3 else [])
    for i, lbl in enumerate(labels):
        d.text((pad + i * (cw + pad) + 4, ch + pad + 4), f"{lbl}  {title}", fill=(220, 220, 220))
    diff.save(out_path)


# --------------------------------------------------------------------- per-image worker


def _process_one_image(
    im: dict,
    codec: dict,
    cnn: dict,
    dms: dict,
    gate_thresholds: dict,
    crops: dict,
    target_w: int,
    run_dir_str: str,
    workdir_str: str,
) -> tuple[str, dict, str]:
    """Run the complete per-image pipeline.

    Returns (image_id, result_dict, captured_stdout_text). Designed to be
    invoked via ProcessPoolExecutor — all args are picklable, the function
    is module-level, and stdout is captured per-image so the parent can
    re-emit it in the original (sequential) image order for deterministic
    log output.

    The output is bit-identical to the sequential path: same numpy ops,
    same metric inputs, same PNG bytes, same crop coords. The metrics
    library's LPIPS model is loaded inside this process (it lives in a
    module-level cache so it persists across images in the same worker,
    but each worker process loads it independently)."""
    run_dir = Path(run_dir_str)
    workdir = Path(workdir_str)
    src_dng = Path(im["path"])
    buf = io.StringIO()
    with redirect_stdout(buf):
        total_t0 = time.perf_counter()
        print(f"\n  -- {im['id']} ({im['character']})")
        read_t0 = time.perf_counter()
        bayer, w, h = read_source_bayer(im["path"])
        read_ms = (time.perf_counter() - read_t0) * 1000.0
        img_work = workdir / im["id"]
        img_work.mkdir(exist_ok=True)
        # 1. encode/decode through codec
        dec, enc_bytes, enc_ms = encode_decode(codec, bayer, w, h, img_work,
                                                src_dng=im["path"])
        # bayer_psnr requires matched dims. If the codec decimated, downsample
        # the source bayer by 2x2 box average to compare in codec-output space.
        if dec.shape != bayer.shape:
            assert dec.shape[0] * 2 == bayer.shape[0] and dec.shape[1] * 2 == bayer.shape[1]
            bayer_for_codec_metric = (
                bayer[0::2, 0::2].astype(np.int32)
                + bayer[0::2, 1::2].astype(np.int32)
                + bayer[1::2, 0::2].astype(np.int32)
                + bayer[1::2, 1::2].astype(np.int32)
            ) // 4
            bayer_for_codec_metric = bayer_for_codec_metric.astype(np.uint16)
        else:
            bayer_for_codec_metric = bayer
        bp_codec = bayer_psnr(bayer_for_codec_metric, dec)
        # 2. apply CNN (no-op if cnn=none).
        cnn_t0 = time.perf_counter()
        post = apply_cnn(dec, cnn, dms=dms, src_dng=src_dng, workdir=img_work, full_size=(w, h))
        cnn_ms = (time.perf_counter() - cnn_t0) * 1000.0
        cnn_stats = {}
        if isinstance(post, tuple) and post[0] == "bayer_stats":
            post, cnn_stats = post[1], post[2]
        is_rgb_output = isinstance(post, tuple) and post[0] == "rgb"
        if is_rgb_output:
            post_rgb = post[1]
            post_bayer = None
            bp_final = None
        else:
            post_bayer = post
            post_rgb = None
            if post.shape == bayer.shape:
                bp_final = bayer_psnr(bayer, post)
            elif post.shape == bayer_for_codec_metric.shape:
                bp_final = bayer_psnr(bayer_for_codec_metric, post)
            else:
                bp_final = None
        # 3. demosaic both REF and pipeline output to PNG.
        demosaic_t0 = time.perf_counter()
        ref_png = run_dir / f"{im['id']}_REF.png"
        pipe_png = run_dir / f"{im['id']}_PIPELINE.png"
        if not ref_png.exists():
            demosaic_to_png(bayer, dms, src_dng, img_work, ref_png)
        if is_rgb_output:
            img = Image.fromarray(post_rgb)
            if img.size != (w, h):
                img = img.resize((w, h), Image.BICUBIC)
            img.save(pipe_png)
        else:
            demosaic_to_png(post_bayer, dms, src_dng, img_work, pipe_png,
                            upscale_to=(w, h))
        demosaic_ms = (time.perf_counter() - demosaic_t0) * 1000.0
        # 4. crop A_detail
        ref_crop_path = run_dir / f"{im['id']}_REF_crop_A_detail.png"
        pipe_crop_path = run_dir / f"{im['id']}_PIPELINE_crop_A_detail.png"
        crop_at(ref_png, crops["A_detail"], ref_crop_path)
        crop_at(pipe_png, crops["A_detail"], pipe_crop_path)
        # 5. downsample to target_w for metric computation
        ref = downsample_for_metrics(ref_png, target_w)
        test = downsample_for_metrics(pipe_png, target_w)
        if test.shape != ref.shape:
            hh = min(ref.shape[0], test.shape[0])
            ww = min(ref.shape[1], test.shape[1])
            ref, test = ref[:hh, :ww], test[:hh, :ww]
        metrics_t0 = time.perf_counter()
        m = compute_visual_metrics(ref, test)
        metrics_ms = (time.perf_counter() - metrics_t0) * 1000.0
        # 6. evaluate per-metric thresholds. bayer_psnr_codec and bayer_psnr_final
        # are gateable alongside the rendered-image metrics — UPRESABLE-class
        # pipelines output editable raw and care primarily about raw-domain
        # fidelity, with rendered comparison informational.
        metrics_for_check = dict(m)
        if bp_codec is not None:
            metrics_for_check["bayer_psnr_codec"] = bp_codec
        if bp_final is not None:
            metrics_for_check["bayer_psnr_final"] = bp_final
        fails = []
        for key, rule in gate_thresholds.items():
            if rule is None or key.startswith("$"):
                # null threshold = informational only (per gates.json $rules);
                # $-prefixed keys are doc fields, not gates.
                continue
            v = metrics_for_check.get(key)
            if v is None:
                fails.append((key, "missing"))
                continue
            if "max" in rule and v > rule["max"]:
                fails.append((key, f"{v:.4f} > {rule['max']}"))
            if "min" in rule and v < rule["min"]:
                fails.append((key, f"{v:.4f} < {rule['min']}"))
        if not gate_thresholds:
            verdict = "INDETERMINATE"
        else:
            verdict = "PASS" if not fails else "FAIL"
        row = {
            **m,
            "bayer_psnr_codec": bp_codec,
            "bayer_psnr_final": bp_final,
            "enc_bytes": enc_bytes,
            "enc_ms": enc_ms,
            "read_ms": read_ms,
            "cnn_ms": cnn_ms,
            "demosaic_ms": demosaic_ms,
            "metrics_ms": metrics_ms,
            "total_ms": (time.perf_counter() - total_t0) * 1000.0,
            "ref_crop": str(ref_crop_path),
            "pipeline_crop": str(pipe_crop_path),
            "verdict": verdict,
            "fails": [{"metric": k, "reason": r} for k, r in fails],
        }
        row.update(cnn_stats)
        print(f"     LPIPS={m.get('lpips'):.4f}  Y-PSNR={m.get('y_psnr'):.2f}  "
              f"MS-SSIM={m.get('ms_ssim'):.4f}  ΔE={m.get('dE2000_mean'):.2f}  "
              f"codec={enc_ms:.1f}ms cnn={cnn_ms:.1f}ms total={row['total_ms']:.1f}ms "
              f"=> {verdict}")
        if fails:
            for k, r in fails:
                print(f"        FAIL {k}: {r}")
    return im["id"], row, buf.getvalue()


# --------------------------------------------------------------------- runner


def _cleanup_fullres_pngs(run_dir: Path) -> tuple[int, int]:
    """Delete the inspection-only full-res REF/PIPELINE PNGs from `run_dir`,
    leaving run.json + WORST_*_visual_diff.png + per-image crop PNGs intact.

    A single REF/PIPELINE pair at 50 MP is ~150 MB each; an 8-image run
    leaks ~1.2 GB of intermediate PNGs that the gate doesn't archive. The
    crops and the worst-image visual diff are the durable evidence the
    runner produces — full-res PNGs only matter for ad-hoc inspection
    (which is opt-in via --keep-fullres-pngs or GATE_KEEP_FULLRES=1).

    Matches files of the form `<id>_REF.png` and `<id>_PIPELINE.png` at
    the run_dir root. Crops `<id>_REF_crop_*.png` /
    `<id>_PIPELINE_crop_*.png` are explicitly preserved because their
    filenames contain `_crop_` (which we filter on), and `WORST_*` files
    are preserved because they don't end in `_REF.png` /
    `_PIPELINE.png` (the WORST is `WORST_<id>_visual_diff.png`).

    Returns (files_deleted, bytes_freed)."""
    n = 0
    freed = 0
    for p in run_dir.iterdir():
        if not p.is_file():
            continue
        name = p.name
        # Crops have `_crop_` in the filename — never delete those.
        if "_crop_" in name:
            continue
        # Only target the two specific full-res filenames.
        if name.endswith("_REF.png") or name.endswith("_PIPELINE.png"):
            try:
                sz = p.stat().st_size
                p.unlink()
                n += 1
                freed += sz
            except OSError:
                pass
    return n, freed


def cnn_hash_fingerprint(cnn: dict) -> str:
    if cnn.get("ckpt_sha256"):
        base = str(cnn["ckpt_sha256"])
        if cnn.get("cnn_arch_variant") == "codec_raw_signal_sr":
            parts = [base]
            for key in (
                "conditioning",
                "dispatch_iso_threshold",
                "dispatch_hf_threshold",
                "inference_batch",
                "sigma_source",
                "tile",
                "overlap",
                "raw_norm",
            ):
                if key in cnn:
                    parts.append(f"{key}={cnn[key]}")
            return "|".join(parts)
        return base
    parts = []
    for key in (
        "ckpt_y_sha256",
        "ckpt_cb_sha256",
        "ckpt_cr_sha256",
        "ckpt_chroma_sha256",
        "ckpt_detail_sha256",
        "ckpt_rgb_detail_sha256",
        "luma_detail_refiner_sha256",
    ):
        if cnn.get(key):
            parts.append(f"{key}={cnn[key]}")
    for key in (
        "chroma_baseline",
        "luma_detail_cnn_strength",
        "luma_unsharp_amount",
        "luma_unsharp_sigma",
        "luma_wavelet_hf_gain",
        "luma_wavelet_hf_wavelet",
        "luma_wavelet_hf_levels",
        "luma_wavelet_hf_hf_levels",
        "luma_wavelet_hf_max_delta",
        "raw_norm",
        "residual_scale",
    ):
        if key in cnn:
            parts.append(f"{key}={cnn[key]}")
    if parts:
        return "|".join(parts)
    return "none"


def pipeline_run_hash(pipeline_id: str, codec: dict, cnn: dict,
                       dms: dict, image_shas: list[str], gates_sha: str,
                       eval_set_sha: str | None = None) -> str:
    payload_data = {
        "pipeline_id": pipeline_id,
        "codec_env_canonical": canonical_env({**codec.get("env", {}),
                                              "QUALITY": codec.get("quality")}),
        "cnn_ckpt_sha256": cnn_hash_fingerprint(cnn),
        "demosaicer": dms.get("binary", ""),
        "image_shas": sorted(image_shas),
        "gates_sha": gates_sha,
    }
    # Preserve historical ship-gate run hashes. Alternate evaluation sets are
    # informational and need their manifest hash in the run identity because
    # crop/eval dimensions can differ while source images stay the same.
    if eval_set_sha:
        payload_data["eval_set_sha"] = eval_set_sha
    payload = json.dumps(payload_data, sort_keys=True)
    return hashlib.sha256(payload.encode()).hexdigest()[:16]


def evaluate_pipeline(
    pipeline_name: str,
    keep_fullres: bool = False,
    test_set_path: Path = TEST_SET_PATH,
) -> dict:
    gates = load_json(GATES_PATH)
    test_set_path = test_set_path.resolve()
    test_set = load_json(test_set_path)
    registry = load_validation_registry(REGISTRY_PATH)
    if pipeline_name not in registry["pipelines"]:
        die(3, f"unknown pipeline: {pipeline_name}. "
               f"Available: {list(registry['pipelines'].keys())}")
    pipe = registry["pipelines"][pipeline_name]
    codec = registry["codecs"][pipe["codec"]]
    cnn = registry["cnns"][pipe["cnn"]]
    dms = registry["demosaicers"][pipe["demosaic"]]
    ship_class = pipe["ship_class"]
    if ship_class not in gates["ship_classes"]:
        print(f"WARN: ship_class {ship_class!r} has no entry in gates.json — "
              f"running in INFORMATIONAL mode (metrics computed, no thresholds "
              f"enforced, verdict INDETERMINATE). Propose thresholds and add "
              f"them to gates.json in a standalone PR.")
        gate_thresholds = {}
        informational_only = True
    else:
        gate_thresholds = gates["ship_classes"][ship_class]["per_image"]
        informational_only = False

    target_w = test_set["metric_eval_dims"]["width"]
    source_path_map = parse_source_path_map(os.environ.get("GATE_SOURCE_PATH_MAP"))
    images = apply_source_path_map(test_set, source_path_map)
    source_root = Path(os.environ.get("GPR_EXTERNAL_ROOT", REPO)).expanduser()
    for im in images:
        source_path = Path(im["path"]).expanduser()
        im["path"] = str(source_path if source_path.is_absolute() else source_root / source_path)
    crops = test_set["crops"]

    # Verify source DNGs exist before any work.
    missing = [im for im in images if not Path(im["path"]).exists()]
    if missing:
        die(2, f"source DNG(s) missing: {[m['id'] for m in missing]}")

    # Source SHAs (truncated for stable hash without re-hashing 50MB each run)
    image_shas = []
    for im in images:
        p = Path(im["path"])
        # Stat-based stamp is fine for local dev; CI would use sha256.
        image_shas.append(f"{im['id']}:{p.stat().st_size}:{int(p.stat().st_mtime)}")

    gates_sha = hashlib.sha256(GATES_PATH.read_bytes()).hexdigest()[:16]
    eval_set_sha = None
    is_ship_gate = test_set_path == TEST_SET_PATH.resolve()
    if not is_ship_gate:
        eval_set_sha = hashlib.sha256(test_set_path.read_bytes()).hexdigest()[:16]
    run_hash = pipeline_run_hash(
        pipeline_name,
        codec,
        cnn,
        dms,
        image_shas,
        gates_sha,
        eval_set_sha=eval_set_sha,
    )
    gate_tmpdir = os.environ.get("GATE_TMPDIR")
    enforce_large_gate_scratch_policy(images, gate_tmpdir)

    run_dir = RUNS_DIR / f"{run_hash}"
    run_dir.mkdir(parents=True, exist_ok=True)
    gate_tmpdir_path = Path(gate_tmpdir).expanduser() if gate_tmpdir else None
    if gate_tmpdir_path is not None:
        gate_tmpdir_path.mkdir(parents=True, exist_ok=True)
    workdir = Path(tempfile.mkdtemp(prefix=f"gate_{run_hash}_", dir=gate_tmpdir_path))

    print(f"\n=== pipeline: {pipeline_name}")
    print(f"=== run_hash: {run_hash}  ship_class: {ship_class}")
    print(f"=== test_set:  {test_set_path}")
    if not is_ship_gate:
        print("=== authority: informational holdout eval, not a ship PASS/FAIL claim")
    if source_path_map:
        print("=== source_path_map:")
        for src, dst in source_path_map:
            print(f"    {src} => {dst}")
    print(f"=== run_dir:  {run_dir}")
    print(f"=== workdir:  {workdir}")

    results = {
        "pipeline": pipeline_name,
        "ship_class": ship_class,
        "run_hash": run_hash,
        "gates_sha": gates_sha,
        "test_set": str(test_set_path),
        "test_set_sha": eval_set_sha or hashlib.sha256(TEST_SET_PATH.read_bytes()).hexdigest()[:16],
        "is_ship_gate": is_ship_gate,
        "verdict_authority": "ship_gate" if is_ship_gate else "informational_holdout",
        "source_path_map": [{"from": src, "to": dst} for src, dst in source_path_map],
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "images": {},
    }

    # Per-image work is independent (each image has its own tempdir; the
    # codec/CNN/demosaic/sips/metrics chain only reads the source DNG and
    # writes into run_dir/img_work). Run images in parallel via
    # ProcessPoolExecutor — workers are subprocess-heavy (codec binary,
    # gpr_tools, sips) AND own a per-process PyTorch/LPIPS state, so process
    # parallelism gives the speedup without thread/GIL contention.
    #
    # Set GATE_MAX_WORKERS=1 to force sequential execution (debugging /
    # numerical-diff verification).
    max_workers = int(os.environ.get("GATE_MAX_WORKERS", str(min(4, len(images)))))
    max_workers = max(1, min(max_workers, len(images)))

    image_logs: dict[str, str] = {}
    try:
        if max_workers == 1:
            for im in images:
                im_id, row, log = _process_one_image(
                    im, codec, cnn, dms, gate_thresholds, crops,
                    target_w, str(run_dir), str(workdir),
                )
                results["images"][im_id] = row
                image_logs[im_id] = log
                # Stream log immediately in the sequential path so behavior
                # matches the pre-parallel runner when debugging.
                sys.stdout.write(log)
                sys.stdout.flush()
        else:
            with _cf.ProcessPoolExecutor(max_workers=max_workers) as ex:
                futures = {
                    ex.submit(
                        _process_one_image, im, codec, cnn, dms,
                        gate_thresholds, crops, target_w,
                        str(run_dir), str(workdir),
                    ): im["id"]
                    for im in images
                }
                for fut in _cf.as_completed(futures):
                    im_id, row, log = fut.result()
                    results["images"][im_id] = row
                    image_logs[im_id] = log
            # Emit per-image logs in the original (registry) image order
            # so stdout is deterministic regardless of completion order.
            for im in images:
                log = image_logs.get(im["id"], "")
                if log:
                    sys.stdout.write(log)
            sys.stdout.flush()
            # Reinsert results in registry order so json.dumps writes them
            # in the same order as the sequential runner — keeps run.json
            # byte-identical for any consumer that diffs it textually.
            ordered = {im["id"]: results["images"][im["id"]] for im in images
                       if im["id"] in results["images"]}
            results["images"] = ordered
    finally:
        pass

    # 7. sort worst-first by LPIPS (mandatory)
    ranked = sorted(
        results["images"].items(),
        key=lambda kv: kv[1].get("lpips", 0.0) or 0.0,
        reverse=True,
    )
    results["worst_first"] = [k for k, _ in ranked]
    worst_id, worst_row = ranked[0]

    # 8. build visual-diff for the worst image
    last_best_link = run_dir / "last_best_crop.png"  # placeholder; updated when --update-baseline
    diff_path = run_dir / f"WORST_{worst_id}_visual_diff.png"
    build_visual_diff(
        Path(worst_row["ref_crop"]),
        Path(worst_row["pipeline_crop"]),
        last_best_link if last_best_link.exists() else None,
        diff_path,
        title=f"{pipeline_name[:50]}",
    )
    results["worst_image"] = {
        "id": worst_id,
        "visual_diff_png": str(diff_path),
        "lpips": worst_row.get("lpips"),
    }

    # 9. overall verdict: any image FAIL -> FAIL; any INDETERMINATE w/o FAIL -> INDETERMINATE
    verdicts = [r["verdict"] for r in results["images"].values()]
    if any(v == "FAIL" for v in verdicts):
        results["verdict"] = "FAIL"
    elif any(v == "INDETERMINATE" for v in verdicts):
        results["verdict"] = "INDETERMINATE"
    else:
        results["verdict"] = "PASS"
    results["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")

    # 10. write the run log
    (run_dir / "run.json").write_text(json.dumps(results, indent=2, default=str))

    # 10b. clean up the full-res REF/PIPELINE PNGs unless the caller asked
    # us to keep them. These are ~150 MB each (8 per run = ~1.2 GB) and
    # only useful for ad-hoc visual inspection — the crops and the WORST
    # diff are the durable artifacts. We only reach this point with a
    # PASS/FAIL verdict; INDETERMINATE paths exit earlier via die(), and
    # an exception in the per-image work would also bypass this — so on
    # any abnormal exit, everything is preserved for debugging.
    if not keep_fullres:
        deleted, freed = _cleanup_fullres_pngs(run_dir)
        if deleted:
            print(f"\n=== Cleaned up {deleted} full-res PNG(s), "
                  f"freed {freed / (1024*1024):.1f} MB "
                  f"(--keep-fullres-pngs / GATE_KEEP_FULLRES=1 to retain)")

    # 11. print worst-first summary (mandatory format)
    print(f"\n=== VERDICT: {results['verdict']}")
    print(f"=== Worst-first by LPIPS:")
    for img_id in results["worst_first"]:
        row = results["images"][img_id]
        print(f"   {img_id:12s}  LPIPS={row.get('lpips'):.4f}  "
              f"verdict={row['verdict']}")
    print(f"\n=== Visual diff for WORST image written to:")
    print(f"   {diff_path}")
    print(f"\n=== Run log: {run_dir / 'run.json'}")
    return results


# --------------------------------------------------------------------- main


def main():
    p = argparse.ArgumentParser()
    p.add_argument("pipeline", help="Full pipeline name from registry.json")
    p.add_argument("--claim", action="store_true",
                   help="After PASS, prompt for inspection-sentence to append to claims_log.md")
    p.add_argument("--claim-sentence",
                   help="Non-interactive inspection sentence for --claim. Must pass the same validation "
                        "as the interactive prompt.")
    p.add_argument("--keep-fullres-pngs", action="store_true",
                   help="Keep the full-res REF/PIPELINE PNGs (~150 MB each, ~1.2 GB/run). "
                        "Default is to delete them after the verdict is computed; the run.json, "
                        "WORST_*_visual_diff.png, and per-image crops are always kept. "
                        "GATE_KEEP_FULLRES=1 in the environment has the same effect.")
    p.add_argument("--test-set", type=Path, default=TEST_SET_PATH,
                   help="Evaluation manifest to run. Defaults to the frozen ship gate. "
                        "Any alternate manifest is informational only and cannot be used with --claim.")
    args = p.parse_args()

    # Env-var override is OR'd with the flag — either one keeps the PNGs.
    keep_fullres = args.keep_fullres_pngs or os.environ.get("GATE_KEEP_FULLRES", "") not in ("", "0")

    res = evaluate_pipeline(args.pipeline, keep_fullres=keep_fullres,
                            test_set_path=args.test_set)

    if args.claim or args.claim_sentence:
        if not res.get("is_ship_gate", False):
            print("\n--claim is only allowed on the frozen ship gate test_set.json.",
                  file=sys.stderr)
            sys.exit(2)
        if res["verdict"] != "PASS":
            print("\n--claim requested but verdict is FAIL. Refusing to log.", file=sys.stderr)
            sys.exit(1)
        if args.claim_sentence:
            sentence = args.claim_sentence.strip()
        elif not sys.stdin.isatty():
            print("\n--claim requires interactive stdin for the inspection sentence.",
                  file=sys.stderr)
            sys.exit(2)
        else:
            print(f"\nReview the visual diff at: {res['worst_image']['visual_diff_png']}")
            sentence = input("Inspection sentence (>=6 words, must include a concrete noun): ").strip()
        nouns = ["rocks", "sky", "edge", "blockiness", "haze", "noise",
                 "detail", "texture", "shadow", "highlight", "crosshatch",
                 "smooth", "ringing", "color"]
        words = sentence.split()
        if len(words) < 6:
            print("Sentence too short (<6 words). Refusing.", file=sys.stderr)
            sys.exit(2)
        if not any(n in sentence.lower() for n in nouns):
            print(f"Sentence has no concrete noun (one of {nouns}). Refusing.",
                  file=sys.stderr)
            sys.exit(2)
        CLAIMS_LOG.parent.mkdir(exist_ok=True)
        line = (f"- {time.strftime('%Y-%m-%d %H:%M')}  pipeline=`{args.pipeline}`  "
                f"run={res['run_hash']}  worst_lpips={res['worst_image']['lpips']:.4f}  "
                f"worst_image={res['worst_image']['id']}  "
                f"visual_description=\"{sentence}\"\n")
        with open(CLAIMS_LOG, "a") as f:
            f.write(line)
        print(f"Logged claim to {CLAIMS_LOG}")

    sys.exit(0 if res["verdict"] == "PASS" else 1)


if __name__ == "__main__":
    main()
