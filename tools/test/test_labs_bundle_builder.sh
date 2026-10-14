#!/usr/bin/env bash
# Smoke-test deterministic Labs bundle manifest/checksum generation.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
  GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi
GPR_TMPDIR="${GPR_TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
# Caller-supplied WORK is a parent; only the unique child belongs to this test.
WORK_PARENT=${WORK:-$GPR_TMPDIR/labs_bundle_builder_smoke}
mkdir -p "$WORK_PARENT"
WORK=$(mktemp -d "$WORK_PARENT/labs_bundle_builder_smoke.XXXXXX")
PYTHON_BIN="${PYTHON_BIN:-python3}"

cleanup() {
  local status=$?
  if [ "$status" -eq 0 ] && [ "${GPR_KEEP_TEST_ARTIFACTS:-0}" != "1" ]; then
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

mkdir -p "$WORK/samples" "$WORK/review" "$WORK/receipts"

"$PYTHON_BIN" - "$WORK" <<'PY'
import json
import struct
import sys
from pathlib import Path

root = Path(sys.argv[1])
(root / "release_evidence.json").write_text(
    json.dumps({"schema": "gpr.release_evidence.v1", "receipts": []}),
    encoding="utf-8",
)
payloads = [b"builder-frame-0", b"builder-frame-1"]
data = bytearray()
data += struct.pack("<IBBHHHIIIII", 0x44495647, 1, 0, 4, 3, 0, 640, 360, 24000, 0, len(payloads))
for tag, payload in enumerate(payloads):
    data += struct.pack("<IIQ", 0x004D5246, len(payload), tag)
    data += payload
(root / "samples" / "half_res_capture.gvid").write_bytes(data)
(root / "samples" / "half_res_capture.gvid.meta.json").write_text(
    json.dumps({"schema": "gvid_source_metadata.v1", "frame_count": len(payloads)}, indent=2),
    encoding="utf-8",
)
(root / "review" / "preview_review_dashboard.html").write_text(
    "<!doctype html><title>builder smoke</title>\n",
    encoding="utf-8",
)
(root / "receipts" / "pi5_proxy_receipt.json").write_text(
    json.dumps({"fps_median": 19.98, "proxy_fps": 20.0}, indent=2),
    encoding="utf-8",
)
PY

"$PYTHON_BIN" "$REPO/tools/build_labs_bundle.py" "$WORK" \
  --repo-commit synthetic-builder-smoke \
  --ci-run https://github.com/dcliftreaves/gpr/actions/runs/0 \
  --target-name "Pi 5 stand-in" \
  --target-role stand-in \
  --note "synthetic builder smoke" \
  --product-pillars-from "$WORK/release_evidence.json" \
  --artifact samples/half_res_capture.gvid:gvid \
  --artifact samples/half_res_capture.gvid.meta.json:json \
  --artifact review/preview_review_dashboard.html:dashboard \
  --artifact receipts/pi5_proxy_receipt.json:json

"$PYTHON_BIN" "$REPO/tools/verify_labs_bundle.py" "$WORK/manifest.json"
"$PYTHON_BIN" - "$WORK/manifest.json" "$REPO/tools/build_labs_bundle.py" <<'PY'
import importlib.util
import json
import sys
from pathlib import Path

manifest = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
assert "product_pillars" not in manifest
spec = importlib.util.spec_from_file_location("bundle_builder", sys.argv[2])
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
builder.DEFAULT_RELEASE_EVIDENCE = Path(sys.argv[1]).parent / "absent_default.json"
assert builder.load_product_pillars(builder.DEFAULT_RELEASE_EVIDENCE) is None
assert builder.load_product_pillars(None) is None
try:
    builder.load_product_pillars(Path(sys.argv[1]).parent / "absent_explicit.json")
except FileNotFoundError:
    pass
else:
    raise AssertionError("explicit missing index must fail")
PY
(cd "$WORK" && shasum -a 256 -c hashes/sha256sums.txt)

echo "test_labs_bundle_builder: PASS"
