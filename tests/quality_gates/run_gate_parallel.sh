#!/usr/bin/env bash
# run_gate_parallel.sh — run run_gate.py for N pipelines concurrently.
#
# Composes with the per-image parallelism inside run_gate.py:
#   - Per-pipeline: ProcessPoolExecutor across the 4 test images (internal)
#   - Across pipelines: this script (xargs -P)
#
# Be mindful of total core count. By default we run 2 pipelines in parallel
# (each uses up to 4 image workers; CPU = 16 cores, so 2*4 = 8 fits well).
# Override with GATE_PIPELINE_CONCURRENCY=<N>.
#
# Also caps per-pipeline image workers via GATE_MAX_WORKERS so the product
# stays bounded.
#
# Usage:
#   bash tests/quality_gates/run_gate_parallel.sh \
#       'codec=sl_q3+cnn=none+demosaic=sips_via_gpr_tools' \
#       'codec=sl_q3+cnn=bibo1x_ane_sl_q3+demosaic=sips_via_gpr_tools'
#
#   # or from a file:
#   cat pipelines.txt | bash tests/quality_gates/run_gate_parallel.sh
#
# Exit status is 0 iff all individual pipeline runs returned 0 (PASS).

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUNNER="$REPO_ROOT/tests/quality_gates/run_gate.py"

PIPELINE_CONCURRENCY="${GATE_PIPELINE_CONCURRENCY:-2}"
PER_PIPELINE_WORKERS="${GATE_MAX_WORKERS:-4}"
export GATE_MAX_WORKERS="$PER_PIPELINE_WORKERS"

# Collect pipelines either from args or stdin (one name per line).
pipelines=()
if [ $# -gt 0 ]; then
    pipelines=("$@")
else
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        case "$line" in
            \#*) continue ;;
        esac
        pipelines+=("$line")
    done
fi

if [ "${#pipelines[@]}" -eq 0 ]; then
    echo "usage: $0 PIPELINE [PIPELINE ...]   (or pipe names on stdin)" >&2
    exit 3
fi

echo "=== run_gate_parallel: ${#pipelines[@]} pipelines, "\
"pipeline_concurrency=${PIPELINE_CONCURRENCY}, "\
"per_pipeline_workers=${PER_PIPELINE_WORKERS}"

# Build a tempdir for per-pipeline logs.
LOGDIR="$(mktemp -d -t gate_parallel_logs.XXXX)"
trap 'rm -rf "$LOGDIR"' EXIT

# Run with xargs -P. Each invocation captures stdout/stderr to a per-pipeline
# log file and prints the log when done — keeps interleaving readable.
printf '%s\n' "${pipelines[@]}" | xargs -n 1 -P "$PIPELINE_CONCURRENCY" -I {} \
    bash -c '
        pipeline="$1"; logdir="$2"; runner="$3"
        safe=$(printf "%s" "$pipeline" | tr -c "A-Za-z0-9._-" "_")
        log="$logdir/$safe.log"
        echo ">>> START  $pipeline"
        python3 "$runner" "$pipeline" >"$log" 2>&1
        rc=$?
        echo ">>> DONE   $pipeline  rc=$rc"
        echo "----- log: $log -----"
        cat "$log"
        echo "----- end log -----"
        exit $rc
    ' _ {} "$LOGDIR" "$RUNNER"

rc=$?
echo "=== run_gate_parallel: aggregate exit status = $rc"
exit $rc
