#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK_ROOT="${GPR_TMPDIR:-${TMPDIR:-/tmp}}"
mkdir -p -- "$WORK_ROOT"
WORK=$(mktemp -d "$WORK_ROOT/render_gvid_sr_registry_smoke.XXXXXX")
PYTHON_BIN="${PYTHON_BIN:-python3}"

cleanup() {
  local status=$?
  if [ "$status" -eq 0 ] && [ "${GPR_KEEP_TEST_ARTIFACTS:-0}" != "1" ]; then
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

mkdir -p "$WORK/artifacts/sr"

printf 'checkpoint\n' > "$WORK/artifacts/sr/model.pt"
cat > "$WORK/registry.json" <<JSON
{
  "codecs": {
    "native12": {
      "source_width": 4096,
      "source_height": 3072
    }
  },
  "cnns": {
    "sr8k": {
      "ckpt_path": "artifacts/sr/model.pt",
      "tile": 384,
      "overlap": 48
    }
  },
  "pipelines": {
    "codec=native12+cnn=sr8k+demosaic=sips": {
      "codec": "native12",
      "cnn": "sr8k",
      "demosaic": "sips",
      "ship_class": "UPRESABLE"
    }
  }
}
JSON

GPR_EXTERNAL_ROOT="$WORK" "$PYTHON_BIN" - "$ROOT/tools/cnn/render_gvid_sr_receipt.py" "$WORK/registry.json" <<'PY'
import importlib.util
import sys
from pathlib import Path

mod_path = Path(sys.argv[1])
registry = Path(sys.argv[2])
spec = importlib.util.spec_from_file_location("render_gvid_sr_receipt", mod_path)
mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(mod)

pipeline_id = "codec=native12+cnn=sr8k+demosaic=sips"
resolved = mod.resolve_pipeline(registry, pipeline_id, external_root=registry.parent)
assert resolved["codec_id"] == "native12"
assert resolved["cnn_id"] == "sr8k"
assert resolved["checkpoint"].name == "model.pt"
assert resolved["checkpoint"].is_file()
assert resolved["cnn"]["tile"] == 384
assert resolved["cnn"]["overlap"] == 48

try:
    mod.resolve_pipeline(registry, "missing", external_root=registry.parent)
except KeyError as exc:
    assert "missing" in str(exc)
else:
    raise AssertionError("missing pipeline did not raise")
PY

echo "test_render_gvid_sr_registry: PASS"
