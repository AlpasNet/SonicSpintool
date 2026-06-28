#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-web"
LOG_FILE="${ROOT_DIR}/build-web.log"

if ! command -v emcmake >/dev/null 2>&1; then
    echo "Emscripten is not active. Run: source /path/to/emsdk/emsdk_env.sh" >&2
    exit 1
fi

cmake -E rm -rf "${BUILD_DIR}"
rm -f "${LOG_FILE}"

emcmake cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_VERBOSE_MAKEFILE=ON \
    -DSPINTOOL_BUILD_WEB=ON 2>&1 | tee "${LOG_FILE}"

set +e
cmake --build "${BUILD_DIR}" --parallel 1 --verbose 2>&1 | tee -a "${LOG_FILE}"
build_status=${PIPESTATUS[0]}
set -e

if (( build_status != 0 )); then
    printf '\n================ FIRST BUILD ERRORS ================\n' >&2
    grep -nE '(^|[[:space:]])(fatal error:|error:|undefined reference|wasm-ld: error:|em\+\+: error:)' \
        "${LOG_FILE}" | head -40 >&2 || true
    printf '====================================================\n' >&2
    printf 'Complete log: %s\n' "${LOG_FILE}" >&2
    exit "${build_status}"
fi

printf '\nWeb build created in: %s\n' "${BUILD_DIR}"
printf 'Run: python3 %s/serve.py %s\n' "${ROOT_DIR}/web" "${BUILD_DIR}"
