#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
scratch="${RUNNER_TEMP:?Set RUNNER_TEMP}/gpr-coverage"
out="${GPR_TEST_ARTIFACT_DIR:?Set GPR_TEST_ARTIFACT_DIR}/coverage"
mkdir -p "$out"
unset COVERAGE_PROCESS_START
export COVERAGE_FILE="$scratch/python/.coverage"
inputs=()
failed=0
if python3 -m coverage combine "$scratch/python" && python3 -m coverage json -o "$out/python.json"; then
    inputs+=(--python-json "$out/python.json")
else
    echo 'Python coverage unavailable; remaining inventory is still collected.' >&2
    failed=1
fi
if python3 -m gcovr --root "$root" --object-directory "$root" \
    --gcov-ignore-errors=source_not_found --json "$out/native.json" \
    "${BUILD_DIR:?Set BUILD_DIR}" "$scratch" "$root/tools/gvid_mov"; then
    inputs+=(--gcov-json "$out/native.json")
else
    echo 'Native coverage unavailable; remaining inventory is still collected.' >&2
    failed=1
fi
runtime=$(git rev-parse "${GPR_RUNTIME_REF:-origin/review/product-runtime}")
python3 tools/test/audit_feature_coverage.py --base 446c736a38fb14f51343605c0780d347dc602f89 \
    --head "$runtime" --test-ref HEAD --verify-checkout "${inputs[@]}" \
    --output "$out/feature-coverage.json"
python3 - "$out/feature-coverage.json" "${GITHUB_STEP_SUMMARY:-/dev/stdout}" <<'PY'
import collections
import json
import sys
from pathlib import Path
report = json.loads(Path(sys.argv[1]).read_text())
by_path = {row['path']: row for row in report['files']}
for source in ('gvid_mov.c', 'gvid_mov_pack.c', 'gvid_mov_unpack.c'):
    row = by_path[f'tools/gvid_mov/src/{source}']
    if row['execution']['status'] != 'EXECUTED':
        raise SystemExit(f'MOV coverage missing despite required wrapper tests: {source}')
states = collections.Counter(row['execution']['status'] for row in report['files'])
measured = sum(row['changed_line_coverage']['measured_lines'] for row in report['files'])
executed = sum(row['changed_line_coverage']['executed_lines'] for row in report['files'])
with open(sys.argv[2], 'a', encoding='utf-8') as out:
    out.write('\n### Instrumented Linux feature coverage\n')
    out.write(f"Runtime commit: `{report['refs']['head']}`; validation: `{report['refs']['tests']}`.\n\n")
    out.write(f"Files: {dict(states)}. Changed executable lines: {executed}/{measured}.\n\n")
    out.write('Uninstrumented platforms/backends remain UNKNOWN. Execution is not an assertion or trained-model quality pass. Instrumented timings are not performance receipts.\n')
PY
exit "$failed"
