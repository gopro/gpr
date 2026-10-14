#!/usr/bin/env bash
# Route large local scratch for GPR development to the external drive.
#
# Usage:
#   source tools/dev/external_drive_env.sh
#   tools/dev/external_drive_env.sh python3 tests/quality_gates/run_gate.py ...
#
# Keep the repo for source, registry metadata, and committed run.json receipts.
# Choose an artifact drive with GPR_EXTERNAL_ROOT before sourcing this file.
set -euo pipefail

: "${GPR_EXTERNAL_ROOT:?Set GPR_EXTERNAL_ROOT to your artifact directory first}"
ROOT="$GPR_EXTERNAL_ROOT"

mkdir -p \
  "$ROOT/tmp" \
  "$ROOT/gate_tmp" \
  "$ROOT/models" \
  "$ROOT/checkpoints" \
  "$ROOT/pycache" \
  "$ROOT/torch" \
  "$ROOT/xdg_cache" \
  "$ROOT/matplotlib" \
  "$ROOT/cnn" \
  "$ROOT/artifacts"

export GPR_EXTERNAL_ROOT="$ROOT"
export GPR_CNN_ROOT="${GPR_CNN_ROOT:-$ROOT/cnn}"
export GPR_ARTIFACT_ROOT="${GPR_ARTIFACT_ROOT:-$ROOT/artifacts}"
export GPR_MODEL_ROOT="${GPR_MODEL_ROOT:-$ROOT/models}"
export GPR_CHECKPOINT_ROOT="${GPR_CHECKPOINT_ROOT:-$ROOT/checkpoints}"
export GPR_TMPDIR="${GPR_TMPDIR:-$ROOT/tmp}"

# tempfile uses TMPDIR; run_gate.py also honors GATE_TMPDIR explicitly.
export TMPDIR="$GPR_TMPDIR/"
export TEMP="$TMPDIR"
export TMP="$TMPDIR"
export GATE_TMPDIR="$ROOT/gate_tmp"
export GATE_KEEP_FULLRES="${GATE_KEEP_FULLRES:-0}"

# Training/checkpoint defaults. Production registry artifacts resolve from
# GPR_MODEL_ROOT; transient training checkpoints resolve from GPR_CHECKPOINT_ROOT.
export CKPT_DIR="${CKPT_DIR:-$GPR_CHECKPOINT_ROOT}"

# Keep language/runtime caches off the internal disk.
export PYTHONPYCACHEPREFIX="${PYTHONPYCACHEPREFIX:-$ROOT/pycache}"
export TORCH_HOME="${TORCH_HOME:-$ROOT/torch}"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$ROOT/xdg_cache}"
export MPLCONFIGDIR="${MPLCONFIGDIR:-$ROOT/matplotlib}"

if [[ $# -gt 0 ]]; then
  exec "$@"
fi

cat <<EOF
External-drive GPR environment active:
  GPR_EXTERNAL_ROOT=$GPR_EXTERNAL_ROOT
  TMPDIR=$TMPDIR
  GPR_TMPDIR=$GPR_TMPDIR
  GATE_TMPDIR=$GATE_TMPDIR
  CKPT_DIR=$CKPT_DIR
  GPR_CNN_ROOT=$GPR_CNN_ROOT
  GPR_ARTIFACT_ROOT=$GPR_ARTIFACT_ROOT
  GPR_MODEL_ROOT=$GPR_MODEL_ROOT
  GPR_CHECKPOINT_ROOT=$GPR_CHECKPOINT_ROOT
EOF
