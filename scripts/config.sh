# Shared settings. Sourced by every script; override any of these in the
# environment to point at an existing checkout or build.

# The clang release the module is built from. Bumping this invalidates the
# native and wasm build stamps, and nothing else.
: "${LLVM_TAG:=llvmorg-22.1.8}"
# Pinned, not "latest": the cache key below is built from these values, so a
# floating version would keep the key stable while the toolchain underneath it
# moved — the one way a content-addressed release can lie about its inputs.
: "${EMSDK_VERSION:=6.0.7}"

# Everything heavy lives here, outside the repo, so a clean checkout costs
# nothing and `git clean -xfd` never throws away two hours of build.
: "${ABI_CACHE:=${XDG_CACHE_HOME:-$HOME/.cache}/clang-abi-wasm}"
: "${LLVM_SRC:=$ABI_CACHE/llvm-project}"
: "${NATIVE_BUILD:=$ABI_CACHE/build-native}"
: "${WASM_BUILD:=$ABI_CACHE/build-wasm}"
: "${EMSDK_DIR:=$ABI_CACHE/emsdk}"

# The configuration both LLVM builds share. Changing it re-triggers both, which
# is why it is one variable rather than two argument lists.
#
# LLVM_TARGETS_TO_BUILD is deliberately empty. -fsyntax-only never reaches a
# backend, and clang's per-target ABI knowledge lives in Basic/Targets, which is
# compiled in regardless of this setting — verified: a build with only the wasm
# backends still lays out records correctly for 35 of 41 exotic triples.
: "${CMAKE_COMMON:=
  -DLLVM_TARGETS_TO_BUILD=
  -DLLVM_ENABLE_ASSERTIONS=OFF
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF
  -DCLANG_ENABLE_ARCMT=OFF
  -DLLVM_INCLUDE_TESTS=OFF
  -DCLANG_INCLUDE_TESTS=OFF
  -DLLVM_INCLUDE_EXAMPLES=OFF
  -DLLVM_INCLUDE_BENCHMARKS=OFF
  -DLLVM_INCLUDE_UTILS=OFF
  -DLLVM_ENABLE_ZSTD=OFF
  -DLLVM_ENABLE_ZLIB=OFF
  -DLLVM_ENABLE_LIBXML2=OFF
  -DLLVM_ENABLE_LIBEDIT=OFF
  -DLLVM_ENABLE_PLUGINS=OFF
  -DLLVM_ENABLE_BINDINGS=OFF
}"
