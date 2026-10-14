#!/usr/bin/env bash
# Smoke-test Mission 1 metadata dump + RAW repack when local camera fixtures exist.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
PYTHON_BIN=${PYTHON_BIN:-python3}
GPR_TOOLS=${GPR_TOOLS:-"$REPO/build-local/source/app/gpr_tools/gpr_tools"}
SOURCE_DNG=${SOURCE_DNG:-}
SOURCE_JPG=${SOURCE_JPG:-}
# Caller-supplied WORK is a parent; only the unique child belongs to this test.
WORK_PARENT=${WORK:-"${GPR_TMPDIR:-${TMPDIR:-/tmp}}/mission1_metadata_repack"}

cleanup() {
  local status=$?
  if [ "$status" -eq 0 ] && [ "${GPR_KEEP_TEST_ARTIFACTS:-0}" != "1" ]; then
    rm -rf "$WORK"
  fi
}

if [ ! -x "$GPR_TOOLS" ]; then
  echo "test_mission1_metadata_repack: SKIP missing gpr_tools: $GPR_TOOLS"
  exit 0
fi
if ! command -v exiftool >/dev/null 2>&1; then
  echo "test_mission1_metadata_repack: SKIP missing exiftool"
  exit 0
fi
if [ ! -f "$SOURCE_DNG" ] || [ ! -f "$SOURCE_JPG" ]; then
  echo "test_mission1_metadata_repack: SKIP missing Mission 1 fixtures"
  echo "  SOURCE_DNG=$SOURCE_DNG"
  echo "  SOURCE_JPG=$SOURCE_JPG"
  exit 0
fi

mkdir -p "$WORK_PARENT"
WORK=$(mktemp -d "$WORK_PARENT/mission1_metadata_repack.XXXXXX")
trap cleanup EXIT

"$GPR_TOOLS" -i "$SOURCE_DNG" -d 1 > "$WORK/params.json"
"$GPR_TOOLS" -i "$SOURCE_DNG" -o "$WORK/source.raw" > "$WORK/dng_to_raw.log" 2>&1
"$GPR_TOOLS" -i "$WORK/source.raw" -w 4096 -h 3072 -p 8192 -x rggb14 \
  -a "$WORK/params.json" -o "$WORK/repacked.dng" > "$WORK/raw_to_dng.log" 2>&1
"$GPR_TOOLS" -i "$WORK/source.raw" -w 4096 -h 3072 -p 8192 -x rggb14 \
  -a "$WORK/params.json" -o "$WORK/repacked.gpr" > "$WORK/raw_to_gpr.log" 2>&1

"$PYTHON_BIN" "$REPO/tools/mission1_camera_raw_metadata_audit.py" \
  --reference-dng "$SOURCE_DNG" \
  --camera-jpeg "$SOURCE_JPG" \
  --candidate "$WORK/repacked.dng" \
  --candidate "$WORK/repacked.gpr" \
  --json-out "$WORK/audit.json" \
  --md-out "$WORK/audit.md"

"$PYTHON_BIN" - "$WORK/audit.json" <<'PY'
import json
import sys
from pathlib import Path

audit = json.loads(Path(sys.argv[1]).read_text())
allowed_diffs = {"AsShotNeutral", "NoiseProfile"}
for candidate in audit["candidates"]:
    assert candidate["readable_by_exiftool"], candidate
    assert candidate["missing_required"] == [], candidate
    assert candidate["missing_recommended"] == ["RawDataUniqueID"], candidate
    diff_tags = {item["tag"] for item in candidate["diffs_from_reference"]}
    assert diff_tags <= allowed_diffs, candidate
PY

echo "test_mission1_metadata_repack: PASS"
