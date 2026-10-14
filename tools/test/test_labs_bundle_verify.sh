#!/usr/bin/env bash
# Smoke-test the Labs artifact bundle verifier with a tiny synthetic bundle.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
  GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi
GPR_TMPDIR="${GPR_TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
# Caller-supplied WORK is a parent; only the unique child belongs to this test.
WORK_PARENT=${WORK:-$GPR_TMPDIR/labs_bundle_verify_smoke}
mkdir -p "$WORK_PARENT"
WORK=$(mktemp -d "$WORK_PARENT/labs_bundle_verify_smoke.XXXXXX")
PYTHON_BIN="${PYTHON_BIN:-python3}"

cleanup() {
  local status=$?
  if [ "$status" -eq 0 ] && [ "${GPR_KEEP_TEST_ARTIFACTS:-0}" != "1" ]; then
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

mkdir -p "$WORK/samples" "$WORK/review" "$WORK/receipts" "$WORK/hashes"

"$PYTHON_BIN" - "$WORK" <<'PY'
import hashlib
import json
import struct
import sys
from pathlib import Path

root = Path(sys.argv[1])
gvid = root / "samples" / "half_res_capture.gvid"
payloads = [b"frame-0000", b"frame-0001-longer"]
data = bytearray()
data += struct.pack("<IBBHHHIIIII", 0x44495647, 1, 0, 4, 3, 0, 640, 360, 24000, 0, len(payloads))
for tag, payload in enumerate(payloads):
    data += struct.pack("<IIQ", 0x004D5246, len(payload), tag)
    data += payload
gvid.write_bytes(data)

meta = root / "samples" / "half_res_capture.gvid.meta.json"
meta.write_text(json.dumps({
    "schema": "gvid_source_metadata.v1",
    "frame_count": len(payloads),
    "frames": [{"frame_index": i, "frame_tag": i, "raw_clean_tiles": []} for i in range(len(payloads))],
}, indent=2), encoding="utf-8")

receipt = root / "receipts" / "gvid_validate.txt"
receipt.write_text("synthetic labs bundle validator smoke\n", encoding="utf-8")

review = root / "review" / "preview_review_dashboard.html"
review.write_text("<!doctype html><title>synthetic dashboard</title>\n", encoding="utf-8")

hashes = root / "hashes" / "sha256sums.txt"
hashes.write_text("synthetic hashes placeholder\n", encoding="utf-8")

def row(path, kind):
    b = path.read_bytes()
    return {
        "path": str(path.relative_to(root)),
        "kind": kind,
        "size_bytes": len(b),
        "sha256": hashlib.sha256(b).hexdigest(),
    }

product_pillars = [
    {
        "id": "raw_stills",
        "release_label": "RAW stills",
        "status": "locked_with_sample_gaps",
        "summary": "synthetic raw stills label",
        "open_gate": "synthetic raw stills gate",
    },
    {
        "id": "raw_video_mvp",
        "release_label": "RAW video MVP",
        "status": "pi_stand_in_pass_camera_handoff_open",
        "summary": "synthetic raw video label",
        "open_gate": "synthetic raw video gate",
    },
    {
        "id": "premium_still_sr",
        "release_label": "Premium still/SR",
        "status": "research_loop_working_candidate_not_promoted",
        "summary": "synthetic premium still label",
        "open_gate": "synthetic premium still gate",
    },
    {
        "id": "raw_video_reconstruction",
        "release_label": "RAW video reconstruction",
        "status": "approved_offline_reconstruction_psf_research_optional",
        "summary": "synthetic raw video reconstruction label",
        "open_gate": "synthetic raw video reconstruction gate",
    },
]

manifest = {
    "schema": "gpr_labs_bundle.v1",
    "repo_commit": "synthetic-smoke",
    "ci_run": "https://github.com/dcliftreaves/gpr/actions/runs/0",
    "target": {"name": "synthetic"},
    "notes": ["synthetic smoke bundle"],
    "product_pillars": product_pillars,
    "artifacts": [
        row(gvid, "gvid"),
        row(meta, "json"),
        row(review, "dashboard"),
        row(receipt, "text"),
        row(hashes, "text"),
    ],
}
(root / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
bad = json.loads(json.dumps(manifest))
bad["artifacts"][0]["sha256"] = "0" * 64
(root / "bad_manifest.json").write_text(json.dumps(bad, indent=2), encoding="utf-8")

empty_gvid = root / "samples" / "empty_capture.gvid"
empty_gvid.write_bytes(struct.pack("<IBBHHHIIIII", 0x44495647, 1, 0, 4, 3, 0, 640, 360, 24000, 0, 0))
empty_manifest = json.loads(json.dumps(manifest))
empty_manifest["artifacts"][0] = row(empty_gvid, "gvid")
(root / "empty_manifest.json").write_text(json.dumps(empty_manifest, indent=2), encoding="utf-8")

bad_ci = json.loads(json.dumps(manifest))
bad_ci["ci_run"] = "local-smoke"
(root / "bad_ci_manifest.json").write_text(json.dumps(bad_ci, indent=2), encoding="utf-8")

missing_notes = json.loads(json.dumps(manifest))
del missing_notes["notes"]
(root / "missing_notes_manifest.json").write_text(
    json.dumps(missing_notes, indent=2), encoding="utf-8"
)

missing_target = json.loads(json.dumps(manifest))
del missing_target["target"]
(root / "missing_target_manifest.json").write_text(
    json.dumps(missing_target, indent=2), encoding="utf-8"
)

missing_receipts = json.loads(json.dumps(manifest))
missing_receipts["artifacts"] = [
    item for item in missing_receipts["artifacts"]
    if not item["path"].startswith("receipts/")
]
(root / "missing_receipts_manifest.json").write_text(
    json.dumps(missing_receipts, indent=2), encoding="utf-8"
)

bad_pillars = json.loads(json.dumps(manifest))
bad_pillars["product_pillars"] = bad_pillars["product_pillars"][:-1]
(root / "bad_pillars_manifest.json").write_text(
    json.dumps(bad_pillars, indent=2), encoding="utf-8"
)
PY

"$PYTHON_BIN" "$REPO/tools/verify_labs_bundle.py" "$WORK/manifest.json"

if "$PYTHON_BIN" "$REPO/tools/verify_labs_bundle.py" "$WORK/bad_manifest.json" > "$WORK/bad_manifest.log" 2>&1; then
  echo "test_labs_bundle_verify: expected bad manifest to fail" >&2
  exit 1
fi
grep -q "sha256 mismatch" "$WORK/bad_manifest.log"

if "$PYTHON_BIN" "$REPO/tools/verify_labs_bundle.py" "$WORK/empty_manifest.json" > "$WORK/empty_manifest.log" 2>&1; then
  echo "test_labs_bundle_verify: expected empty .gvid manifest to fail" >&2
  exit 1
fi
grep -q "zero-frame .gvid stream" "$WORK/empty_manifest.log"

if "$PYTHON_BIN" "$REPO/tools/verify_labs_bundle.py" "$WORK/bad_ci_manifest.json" > "$WORK/bad_ci_manifest.log" 2>&1; then
  echo "test_labs_bundle_verify: expected bad CI URL manifest to fail" >&2
  exit 1
fi
grep -q "GitHub Actions URL" "$WORK/bad_ci_manifest.log"

if "$PYTHON_BIN" "$REPO/tools/verify_labs_bundle.py" "$WORK/missing_notes_manifest.json" > "$WORK/missing_notes_manifest.log" 2>&1; then
  echo "test_labs_bundle_verify: expected missing notes manifest to fail" >&2
  exit 1
fi
grep -q "manifest notes" "$WORK/missing_notes_manifest.log"

if "$PYTHON_BIN" "$REPO/tools/verify_labs_bundle.py" "$WORK/missing_target_manifest.json" > "$WORK/missing_target_manifest.log" 2>&1; then
  echo "test_labs_bundle_verify: expected missing target manifest to fail" >&2
  exit 1
fi
grep -q "manifest target" "$WORK/missing_target_manifest.log"

if "$PYTHON_BIN" "$REPO/tools/verify_labs_bundle.py" "$WORK/missing_receipts_manifest.json" > "$WORK/missing_receipts_manifest.log" 2>&1; then
  echo "test_labs_bundle_verify: expected missing receipts manifest to fail" >&2
  exit 1
fi
grep -q "receipts/" "$WORK/missing_receipts_manifest.log"

if "$PYTHON_BIN" "$REPO/tools/verify_labs_bundle.py" "$WORK/bad_pillars_manifest.json" > "$WORK/bad_pillars_manifest.log" 2>&1; then
  echo "test_labs_bundle_verify: expected bad product_pillars manifest to fail" >&2
  exit 1
fi
grep -q "product_pillars" "$WORK/bad_pillars_manifest.log"

echo "test_labs_bundle_verify: PASS"
