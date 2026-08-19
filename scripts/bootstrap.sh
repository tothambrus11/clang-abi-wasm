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
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$REPO/scripts/config.sh"

mkdir -p "$ABI_CACHE"
JOBS="${JOBS:-$(nproc)}"

stamp()   { echo "$1" > "$ABI_CACHE/.stamp-$2"; }
current() { [[ -f "$ABI_CACHE/.stamp-$2" && "$(cat "$ABI_CACHE/.stamp-$2")" == "$1" ]]; }
step()    { printf '\n\033[1m==> %s\033[0m\n' "$1"; }

# --------------------------------------------------------------- 1. source --
if current "$LLVM_TAG" source; then
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
if current "$LLVM_TAG$CMAKE_COMMON" native; then
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
if current "$EMSDK_VERSION" emsdk; then
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
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1

# ----------------------------------------------------------------- 4. wasm --
if current "$LLVM_TAG$CMAKE_COMMON$EMSDK_VERSION" wasm; then
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
  ninja -C "$WASM_BUILD" -j "$JOBS" clangFrontend clangDriver clangSerialization
  stamp "$LLVM_TAG$CMAKE_COMMON$EMSDK_VERSION" wasm
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
