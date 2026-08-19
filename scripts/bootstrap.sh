#!/usr/bin/env bash
# One-time (slow) setup: everything that has to exist before `build.sh` can
# relink our one source file in seconds.
#
#   1. LLVM source at the pinned tag        (~2 GB, sparse — llvm + clang only)
#   2. a native LLVM/clang, backends off    (host tools + headers to develop against)
#   3. the Emscripten SDK
#   4. an LLVM/clang cross-built to wasm    (the libraries the module links)
#
# Every step is stamped and skipped when its inputs have not moved, so re-running
# this is cheap and a failed run resumes rather than restarting. Nothing here
# needs root, and nothing is installed outside $ABI_CACHE.
#
# One step at a time, for CI:
#
#   scripts/bootstrap.sh source|native|emsdk|wasm
#
# CI caches each step separately and runs them one call at a time, so a run that
# runs out of time still banks the stages it finished. Locally, run it with no
# arguments and it does all four.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$REPO/scripts/config.sh"

mkdir -p "$ABI_CACHE"
JOBS="${JOBS:-$(nproc)}"
ONLY="${1:-all}"
want() { [[ "$ONLY" == all || "$ONLY" == "$1" ]]; }

# A step is current when its inputs hash to what the stamp records.
#
# Hashed rather than stored verbatim, and not for brevity: CMAKE_COMMON is a
# multi-line value ending in a newline, `echo` adds another, and `$(cat)` strips
# every trailing newline back off — so a stamp written from those inputs could
# never equal them again. Every run rebuilt LLVM from scratch, warm cache and
# all, and said "cached" nowhere to give it away.
inputs()  { printf '%s' "$1" | sha256sum | cut -d' ' -f1; }
stamp()   { inputs "$1" > "$ABI_CACHE/.stamp-$2"; }
current() {
  [[ -f "$ABI_CACHE/.stamp-$2" ]] && [[ "$(cat "$ABI_CACHE/.stamp-$2")" == "$(inputs "$1")" ]]
}
step()    { printf '\n\033[1m==> %s\033[0m\n' "$1"; }

# CI restores each stage from its own cache, and the stamps are not part of any
# of them — they record what *this* checkout's config.sh asks for, which is
# exactly what a restored tree cannot claim for itself. So after restoring,
# stamp whatever is actually present and let the cache keys carry the identity:
# they are built from the same inputs, so a tree restored under a matching key
# is by definition the tree those inputs produce.
if [[ "$ONLY" == --stamp-only ]]; then
  [[ -d "$LLVM_SRC/llvm" ]]                && stamp "$LLVM_TAG" source
  [[ -d "$NATIVE_BUILD/lib/cmake/clang" ]] && stamp "$LLVM_TAG$CMAKE_COMMON" native
  [[ -f "$EMSDK_DIR/emsdk_env.sh" ]]       && stamp "$EMSDK_VERSION" emsdk
  [[ -d "$WASM_BUILD/lib/cmake/clang" ]]   && stamp "$LLVM_TAG$CMAKE_COMMON$EMSDK_VERSION" wasm
  step "Stamped the stages already present"
  exit 0
fi

# --------------------------------------------------------------- 1. source --
if ! want source; then
  :
elif current "$LLVM_TAG" source; then
  step "LLVM source at $LLVM_TAG — cached"
else
  step "Fetching LLVM source ($LLVM_TAG)"
  rm -rf "$LLVM_SRC"
  # Sparse + blobless: the full history is ~4 GB, this is ~2 GB and we only
  # ever build two of the projects.
  git clone --depth 1 --branch "$LLVM_TAG" --filter=blob:none --sparse \
    https://github.com/llvm/llvm-project.git "$LLVM_SRC"
  # libcxx is headers-only for us: package-headers.sh copies them, nothing builds it.
  git -C "$LLVM_SRC" sparse-checkout set llvm clang libcxx cmake third-party runtimes
  stamp "$LLVM_TAG" source
fi

# --------------------------------------------------------------- 2. native --
# Also the host build the wasm cross-compile borrows llvm-tblgen/clang-tblgen
# from, so it is not optional even if you never run the native harness.
if ! want native; then
  :
elif current "$LLVM_TAG$CMAKE_COMMON" native; then
  step "Native LLVM — cached"
else
  step "Building native LLVM/clang (backends off)"
  cmake -G Ninja -S "$LLVM_SRC/llvm" -B "$NATIVE_BUILD" $CMAKE_COMMON \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_PROJECTS=clang
  # clang-resource-headers puts the builtin headers where the driver expects
  # them; without it every include resolves against the *host* system instead.
  ninja -C "$NATIVE_BUILD" -j "$JOBS" clangFrontend clangDriver clangSerialization \
    clang-resource-headers llvm-tblgen clang-tblgen llvm-min-tblgen
  stamp "$LLVM_TAG$CMAKE_COMMON" native
fi

# ---------------------------------------------------------------- 3. emsdk --
if ! want emsdk; then
  :
elif current "$EMSDK_VERSION" emsdk; then
  step "Emscripten $EMSDK_VERSION — cached"
else
  step "Installing Emscripten $EMSDK_VERSION"
  [[ -d "$EMSDK_DIR" ]] || git clone https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
  git -C "$EMSDK_DIR" pull --ff-only
  "$EMSDK_DIR/emsdk" install "$EMSDK_VERSION"
  "$EMSDK_DIR/emsdk" activate "$EMSDK_VERSION"
  stamp "$EMSDK_VERSION" emsdk
fi
# shellcheck disable=SC1091
[[ -d "$EMSDK_DIR" ]] && source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1

# ----------------------------------------------------------------- 4. wasm --
if ! want wasm; then
  :
elif current "$LLVM_TAG$CMAKE_COMMON$EMSDK_VERSION" wasm; then
  step "wasm LLVM — cached"
else
  step "Cross-building LLVM/clang for wasm"
  # The cross build cannot run the tblgen binaries it produces, so it borrows
  # the native ones. LLVM_NATIVE_TOOL_DIR rather than naming LLVM_TABLEGEN and
  # CLANG_TABLEGEN individually: the build also reaches for llvm-min-tblgen and
  # friends, and naming two of them leaves the rest to fail later in the build.
  # Exceptions and threads are off — nothing in a syntax-only parse needs them.
  emcmake cmake -G Ninja -S "$LLVM_SRC/llvm" -B "$WASM_BUILD" $CMAKE_COMMON \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_PROJECTS=clang \
    -DLLVM_NATIVE_TOOL_DIR="$NATIVE_BUILD/bin" \
    -DLLVM_DEFAULT_TARGET_TRIPLE=wasm32-unknown-emscripten \
    -DLLVM_HOST_TRIPLE=wasm32-unknown-emscripten \
    -DLLVM_ENABLE_THREADS=OFF \
    -DLLVM_ENABLE_UNWIND_TABLES=OFF \
    -DLLVM_ENABLE_PIC=OFF \
    -DCMAKE_CXX_FLAGS="-fno-exceptions -fno-rtti"
  # clang-resource-headers included: package-headers.sh reads the builtin
  # headers out of this tree, and without them there is no resource directory
  # to point the module at.
  ninja -C "$WASM_BUILD" -j "$JOBS" clangFrontend clangDriver clangSerialization \
    clang-resource-headers
  stamp "$LLVM_TAG$CMAKE_COMMON$EMSDK_VERSION" wasm
fi

if [[ "$ONLY" != all ]]; then
  step "Bootstrap step '$ONLY' complete"
  exit 0
fi

step "Bootstrap complete"
cat <<EOF

  native LLVM : $NATIVE_BUILD
  wasm LLVM   : $WASM_BUILD
  emsdk       : $EMSDK_DIR

Next:
  scripts/build.sh native   # seconds — build and test the C++
  scripts/build.sh wasm     # ~a minute — produce dist/abi_query.mjs
EOF
