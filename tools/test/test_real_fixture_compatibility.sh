#!/usr/bin/env bash
# Local real-fixture compatibility audit.
#
# This is intentionally fixture-path driven and skips absent external files so
# CI can run it without the local photo volumes. On the production workstation it
# exercises representative Mission 1, Z8, X2D, and iPhone DNG/GPR inputs.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GPR_TOOLS="${GPR_TOOLS:-$REPO/build-local/source/app/gpr_tools/gpr_tools}"

if [ ! -x "$GPR_TOOLS" ]; then
  echo "test_real_fixture_compatibility: SKIP missing gpr_tools: $GPR_TOOLS"
  exit 0
fi

if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
  GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi

GPR_TMPDIR="${GPR_TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
ARTIFACT_ROOT="${GPR_ARTIFACT_ROOT:-$GPR_EXTERNAL_ROOT/artifacts}"
# Caller-supplied WORK is a parent; only the unique child belongs to this test.
WORK_PARENT="${WORK:-$GPR_TMPDIR/real_fixture_compatibility}"
RECEIPT_DIR="${RECEIPT_DIR:-$ARTIFACT_ROOT/real_fixture_compatibility}"
mkdir -p "$WORK_PARENT" "$RECEIPT_DIR"
WORK=$(mktemp -d "$WORK_PARENT/real_fixture_compatibility.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

RECEIPT="$RECEIPT_DIR/receipt_$(date -u +%Y%m%dT%H%M%SZ).txt"
: >"$RECEIPT"

MISSION1_50_DNG="${MISSION1_50_DNG:-}"
MISSION1_12_DNG="${MISSION1_12_DNG:-}"
MISSION1_GPR="${MISSION1_GPR:-}"
Z8_DNG="${Z8_DNG:-$ARTIFACT_ROOT/fixtures/barn_sky_dngs/Z8Z_1349.dng}"
X2D_DNG="${X2D_DNG:-$ARTIFACT_ROOT/fixtures/x2d_dngs/2024_April_X2D_1742.dng}"
IPHONE_CFA_DNG="${IPHONE_CFA_DNG:-}"
IPHONE_LINEAR_RAW_DNG="${IPHONE_LINEAR_RAW_DNG:-}"

pass_count=0
skip_count=0

log() {
  printf '%s\n' "$*" | tee -a "$RECEIPT"
}

require_cfa_dng() {
  local path="$1"
  if command -v exiftool >/dev/null 2>&1; then
    exiftool -s -PhotometricInterpretation -SamplesPerPixel "$path" >"$WORK/tags.txt"
    grep -q "PhotometricInterpretation.*Color Filter Array" "$WORK/tags.txt"
    grep -q "SamplesPerPixel.*1" "$WORK/tags.txt"
  fi
}

roundtrip_dng() {
  local label="$1"
  local src="$2"
  if [ ! -f "$src" ]; then
    log "SKIP dng_roundtrip $label missing=$src"
    skip_count=$((skip_count + 1))
    return
  fi

  local stem="$WORK/${label//[^A-Za-z0-9_]/_}"
  log "RUN dng_roundtrip $label src=$src"
  "$GPR_TOOLS" -i "$src" -o "$stem.gpr" -q 3 >"$stem.encode.stdout" 2>"$stem.encode.stderr"
  test -s "$stem.gpr"
  "$GPR_TOOLS" -i "$stem.gpr" -o "$stem.roundtrip.dng" >"$stem.decode.stdout" 2>"$stem.decode.stderr"
  test -s "$stem.roundtrip.dng"
  require_cfa_dng "$stem.roundtrip.dng"
  "$GPR_TOOLS" -i "$stem.gpr" -d 1 >"$stem.dump.json" 2>"$stem.dump.stderr"
  test -s "$stem.dump.json"
  log "PASS dng_roundtrip $label gpr_bytes=$(wc -c <"$stem.gpr") dng_bytes=$(wc -c <"$stem.roundtrip.dng")"
  pass_count=$((pass_count + 1))
}

roundtrip_gpr() {
  local label="$1"
  local src="$2"
  if [ ! -f "$src" ]; then
    log "SKIP gpr_roundtrip $label missing=$src"
    skip_count=$((skip_count + 1))
    return
  fi

  local stem="$WORK/${label//[^A-Za-z0-9_]/_}"
  log "RUN gpr_roundtrip $label src=$src"
  "$GPR_TOOLS" -i "$src" -o "$stem.dng" >"$stem.to_dng.stdout" 2>"$stem.to_dng.stderr"
  test -s "$stem.dng"
  require_cfa_dng "$stem.dng"
  "$GPR_TOOLS" -i "$src" -o "$stem.raw" >"$stem.to_raw.stdout" 2>"$stem.to_raw.stderr"
  test -s "$stem.raw"
  "$GPR_TOOLS" -i "$src" -d 1 >"$stem.dump.json" 2>"$stem.dump.stderr"
  test -s "$stem.dump.json"
  log "PASS gpr_roundtrip $label dng_bytes=$(wc -c <"$stem.dng") raw_bytes=$(wc -c <"$stem.raw")"
  pass_count=$((pass_count + 1))
}

linear_raw_rejection() {
  local src="$1"
  if [ ! -f "$src" ]; then
    log "SKIP linear_raw_rejection missing=$src"
    skip_count=$((skip_count + 1))
    return
  fi

  log "RUN linear_raw_rejection src=$src"
  if "$GPR_TOOLS" -i "$src" -o "$WORK/linear_raw_should_fail.gpr" -q 3 >"$WORK/linear.stdout" 2>"$WORK/linear.stderr"; then
    log "FAIL linear_raw_rejection unexpectedly accepted $src"
    exit 1
  fi
  grep -q "requires single-plane 2x2 RGGB/GBRG CFA input" "$WORK/linear.stderr"
  log "PASS linear_raw_rejection"
  pass_count=$((pass_count + 1))
}

iphone_metadata_roundtrip() {
  local src="$1"
  if [ ! -f "$src" ]; then
    log "SKIP iphone_metadata_roundtrip missing=$src"
    skip_count=$((skip_count + 1))
    return
  fi

  local stem="$WORK/iphone_cfa_metadata"
  log "RUN iphone_metadata_roundtrip src=$src"
  "$GPR_TOOLS" -i "$src" -o "$stem.gpr" -q 3 >"$stem.encode.stdout" 2>"$stem.encode.stderr"
  "$GPR_TOOLS" -i "$stem.gpr" -o "$stem.dng" >"$stem.decode.stdout" 2>"$stem.decode.stderr"
  if command -v exiftool >/dev/null 2>&1; then
    exiftool -a -G1 -s -OpcodeList3 -NoiseProfile "$src" "$stem.gpr" "$stem.dng" >"$stem.tags.txt"
    grep -q "NoiseProfile" "$stem.tags.txt"
    grep -q "FixVignetteRadial" "$stem.tags.txt"
  fi
  log "PASS iphone_metadata_roundtrip"
  pass_count=$((pass_count + 1))
}

log "test_real_fixture_compatibility receipt"
log "repo=$REPO"
log "gpr_tools=$GPR_TOOLS"
log "work=$WORK"

roundtrip_dng "mission1_50mp_dng" "$MISSION1_50_DNG"
roundtrip_dng "mission1_12mp_dng" "$MISSION1_12_DNG"
roundtrip_gpr "mission1_50mp_gpr" "$MISSION1_GPR"
roundtrip_dng "z8_50mp_dng" "$Z8_DNG"
roundtrip_dng "x2d_100mp_dng" "$X2D_DNG"
roundtrip_dng "iphone_cfa_dng" "$IPHONE_CFA_DNG"
iphone_metadata_roundtrip "$IPHONE_CFA_DNG"
linear_raw_rejection "$IPHONE_LINEAR_RAW_DNG"

log "SUMMARY pass=$pass_count skip=$skip_count receipt=$RECEIPT"
echo "test_real_fixture_compatibility: PASS pass=$pass_count skip=$skip_count receipt=$RECEIPT"
