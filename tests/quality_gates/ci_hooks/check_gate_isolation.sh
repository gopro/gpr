#!/bin/bash
# CI hook: refuse PRs/commits that modify tests/quality_gates/gates.json
# alongside code or registry changes. Threshold changes go in isolated PRs
# with written justification (see CLAUDE.md, gates.json $change_log).
#
# Usage on a PR diff:
#   tests/quality_gates/ci_hooks/check_gate_isolation.sh <base_ref>
# Default base_ref is origin/master.

set -e
BASE=${1:-origin/master}
CHANGED=$(git diff --name-only "$BASE"..HEAD)

gate_touched=$(echo "$CHANGED" | grep -E '^tests/quality_gates/gates\.json$' || true)
if [ -z "$gate_touched" ]; then
    echo "OK: gates.json not modified in this PR."
    exit 0
fi

# Anything else changed besides gates.json itself + claims_log.md notes
other=$(echo "$CHANGED" | grep -vE '^(tests/quality_gates/gates\.json|docs/claims_log\.md)$' || true)
if [ -n "$other" ]; then
    echo "FAIL: tests/quality_gates/gates.json modified in same PR as other files:"
    echo "$other" | sed 's/^/  /'
    echo
    echo "Threshold changes must land in an isolated PR. See CLAUDE.md."
    exit 1
fi

echo "OK: gates.json modified in isolation (only $CHANGED)."
