#!/usr/bin/env bash
# Opt-in scratch build; existing GPR libraries must be PIC (see --help).
set -euo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${PYTHON_BIN:-python3}" "$SELF_DIR/test_ffmpeg_integration.py" "$@"
