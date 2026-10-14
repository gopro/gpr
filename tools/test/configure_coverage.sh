#!/usr/bin/env bash
# CI-only instrumentation; all compiler profiles and Python data stay in scratch.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
scratch="${RUNNER_TEMP:?Set RUNNER_TEMP}/gpr-coverage"
mkdir -p "$scratch/hooks" "$scratch/python"
printf '#!/bin/sh\nexec /usr/bin/gcc --coverage -fprofile-update=atomic -fprofile-abs-path "$@"\n' > "$scratch/cc"
printf '#!/bin/sh\nexec /usr/bin/g++ --coverage -fprofile-update=atomic -fprofile-abs-path "$@"\n' > "$scratch/cxx"
chmod +x "$scratch/cc" "$scratch/cxx"
printf 'import coverage\ncoverage.process_startup()\n' > "$scratch/hooks/sitecustomize.py"
printf '[run]\nparallel = True\nbranch = True\nsource =\n    %s/tools\n    %s/tests\ndata_file = %s/python/.coverage\n' \
    "$root" "$root" "$scratch" > "$scratch/coverage.ini"
{
    echo "CC=$scratch/cc"
    echo "CXX=$scratch/cxx"
    echo "GPR_COVERAGE_DIR=$scratch"
    echo "COVERAGE_PROCESS_START=$scratch/coverage.ini"
    echo "PYTHONPATH=$scratch/hooks${PYTHONPATH:+:$PYTHONPATH}"
    echo "GPR_TIMING_TOLERANCE=4.0"
} >> "${GITHUB_ENV:?This setup writes GitHub Actions environment variables}"
