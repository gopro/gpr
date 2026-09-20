#!/usr/bin/env bash
#
# Build the GPR SDK with each optional feature flag disabled, one at a time,
# and once with all of them disabled, to make sure every configuration at
# least compiles. Each flag is a CMake option defined in the top-level
# CMakeLists.txt that maps to a preprocessor symbol defaulted in
# source/lib/common/public/gpr_platform.h.
#
# Usage:
#   scripts/test_build_flags.sh
#
# Environment overrides:
#   BUILD_ROOT  directory for the build trees (default: <repo>/build/flag_checks)
#   JOBS        parallel build jobs (default: number of CPUs)
#
# Exits non-zero if any configuration fails to configure or build. Full logs
# for each configuration are kept in $BUILD_ROOT/<config>.log.

set -u

SOURCE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${SOURCE_DIR}/build/flag_checks}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

FLAGS=(
  GPR_NEON
  GPR_READING
  GPR_WRITING
  GPR_JPEG_AVAILABLE
  GPR_TIMING
)

RESULTS_NAMES=()
RESULTS_STATUS=()
FAILED=0

build_config() {
  local name="$1"
  shift
  local build_dir="${BUILD_ROOT}/${name}"
  local log_file="${BUILD_ROOT}/${name}.log"

  printf '%-28s %-45s ' "${name}" "$*"

  rm -rf "${build_dir}"
  if cmake -S "${SOURCE_DIR}" -B "${build_dir}" "$@" >"${log_file}" 2>&1 &&
     cmake --build "${build_dir}" -j "${JOBS}" >>"${log_file}" 2>&1; then
    echo "PASS"
    RESULTS_STATUS+=("PASS")
  else
    echo "FAIL (see ${log_file})"
    echo "----- last 60 lines of ${log_file} -----"
    tail -n 60 "${log_file}"
    echo "-----"
    RESULTS_STATUS+=("FAIL")
    FAILED=1
  fi
  RESULTS_NAMES+=("${name}")
}

mkdir -p "${BUILD_ROOT}"

echo "Source: ${SOURCE_DIR}"
echo "Builds: ${BUILD_ROOT} (${JOBS} jobs)"
echo

# Baseline: everything at its default (all features on where supported)
build_config "baseline"

# Each flag off individually
for flag in "${FLAGS[@]}"; do
  build_config "no_$(echo "${flag}" | tr '[:upper:]' '[:lower:]')" "-D${flag}=OFF"
done

# All flags off together
ALL_OFF_ARGS=()
for flag in "${FLAGS[@]}"; do
  ALL_OFF_ARGS+=("-D${flag}=OFF")
done
build_config "all_off" "${ALL_OFF_ARGS[@]}"

echo
echo "Summary:"
for i in "${!RESULTS_NAMES[@]}"; do
  printf '  %-28s %s\n' "${RESULTS_NAMES[$i]}" "${RESULTS_STATUS[$i]}"
done

if [ "${FAILED}" -ne 0 ]; then
  echo
  echo "One or more configurations failed to build."
  exit 1
fi

echo
echo "All configurations built successfully."
