#!/usr/bin/env bash
# The fast loop. Rebuilds only this repo's source against the LLVM that
# bootstrap.sh already built, so editing abi_query.cpp costs a compile and a
# link — seconds, not the hour the underlying toolchain took.
#
#   scripts/build.sh native   compile + run the test harness
#   scripts/build.sh wasm     produce dist/abi_query.{mjs,wasm,data}
#   scripts/build.sh both
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$REPO/scripts/config.sh"
MODE="${1:-native}"
JOBS="${JOBS:-$(nproc)}"

build_native() {
  [[ -d "$NATIVE_BUILD/lib/cmake/clang" ]] || {
    echo "native LLVM missing — run scripts/bootstrap.sh first" >&2; exit 1; }
  cmake -G Ninja -S "$REPO" -B "$REPO/build/native" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_DIR="$NATIVE_BUILD/lib/cmake/llvm" \
    -DClang_DIR="$NATIVE_BUILD/lib/cmake/clang" >/dev/null
  ninja -C "$REPO/build/native" -j "$JOBS"
  echo "built: $REPO/build/native/abi_query_test"
}

build_wasm() {
  [[ -d "$WASM_BUILD/lib/cmake/clang" ]] || {
    echo "wasm LLVM missing — run scripts/bootstrap.sh first" >&2; exit 1; }
  # shellcheck disable=SC1091
  source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1
  "$REPO/scripts/package-headers.sh" "$REPO/build/wasm/sysroot"
  emcmake cmake -G Ninja -S "$REPO" -B "$REPO/build/wasm" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_DIR="$WASM_BUILD/lib/cmake/llvm" \
    -DClang_DIR="$WASM_BUILD/lib/cmake/clang" >/dev/null
  ninja -C "$REPO/build/wasm" -j "$JOBS"
  mkdir -p "$REPO/dist"
  cp "$REPO/build/wasm"/abi_query.{mjs,wasm,data} "$REPO/dist/" 2>/dev/null || true
  cp "$REPO/js/index.mjs" "$REPO/js/index.d.ts" "$REPO/dist/"
  # A manifest, so a linked local build behaves like a release. The loader reads
  # the byte counts from it to report download progress, and without one a
  # developer sees an indeterminate bar where a user sees megabytes — a
  # difference nothing else would surface.
  {
    printf '{\n  "schemaVersion": 1,\n  "version": "%s+dev",\n  "clang": "%s",\n  "files": {\n' \
      "$(node -p "require('$REPO/js/package.json').version")" "${LLVM_TAG#llvmorg-}"
    printf '    "wasm": { "path": "abi_query.wasm", "bytes": %s },\n' "$(stat -c %s "$REPO/dist/abi_query.wasm")"
    printf '    "glue": { "path": "abi_query.mjs", "bytes": %s },\n' "$(stat -c %s "$REPO/dist/abi_query.mjs")"
    printf '    "headers": { "path": "abi_query.data", "bytes": %s }\n' "$(stat -c %s "$REPO/dist/abi_query.data")"
    printf '  }\n}\n'
  } > "$REPO/dist/manifest.json"
  echo
  ls -lh "$REPO/dist"
}

case "$MODE" in
  native) build_native ;;
  wasm)   build_wasm ;;
  both)   build_native; build_wasm ;;
  *) echo "usage: build.sh [native|wasm|both]" >&2; exit 2 ;;
esac
