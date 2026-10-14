#!/usr/bin/env bash
# Current ancestry only; all builds and copied validation drivers are disposable.
set -euo pipefail
SELF_DIR=$(cd "$(dirname "$0")" && pwd)
exec "${PYTHON_BIN:-python3}" "$SELF_DIR/test_feature_series.py" "$@"
