#!/usr/bin/env bash
# Assembles the sysroot the wasm module carries.
#
# Policy: include eagerly anything small enough that carrying it beats fetching
# it, because the point of the payload is that the app keeps working offline.
# A header the user might `#include` is worth its bytes; a file that cannot
# affect a -fsyntax-only parse is not worth any.
#
#   KEEP   clang builtin headers      stddef/stdint/limits — nothing parses without them
#   KEEP   target intrinsic headers   5.2 MB; avx/neon/altivec appear in real code
#   KEEP   libc++ headers             12 MB; the whole C++ story
#   KEEP   musl headers, per arch     ~1 MB; see below — this is a bug fix
#   DROP   *.a / *.so                 5.9 MB; -fsyntax-only never links
#   DROP   OpenCL / CUDA / HIP        1.9 MB; this tool compiles neither
#   DROP   sanitizer headers          never compiled in a layout query
#
# On musl: the bundle this replaces shipped exactly one C library — wasi-libc —
# and used it for every target. Two consequences, both measured on the old
# pipeline: any 32-bit target failed outright on any libc header (wasi's
# size_t is hardcoded 64-bit), and `sizeof(struct stat)` came back 144 for
# every target because it was always wasi's struct. musl is MIT, is genuinely
# multi-arch, and fixes both.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$REPO/scripts/config.sh"

OUT="${1:?usage: package-headers.sh <sysroot-dir>}"
rm -rf "$OUT"
mkdir -p "$OUT/include"

step() { printf '\033[1m--> %s\033[0m\n' "$1"; }

# ------------------------------------------------- clang's own headers --
# Built alongside clang; they encode the compiler's builtins, so they must come
# from the same release as the frontend.
step "clang builtin + intrinsic headers"
CLANG_RES="$(find "$WASM_BUILD/lib/clang" -maxdepth 2 -name include -type d | head -1)"
[[ -d "$CLANG_RES" ]] || { echo "no clang resource dir under $WASM_BUILD/lib/clang" >&2; exit 1; }
mkdir -p "$OUT/lib/clang/include"
rsync -a \
  --exclude 'opencl-c*.h' \
  --exclude 'cuda_wrappers/' \
  --exclude '__clang_cuda*' \
  --exclude '__clang_hip*' \
  --exclude 'sanitizer/' \
  --exclude 'openmp_wrappers/' \
  "$CLANG_RES/" "$OUT/lib/clang/include/"

# ------------------------------------------------------------ musl libc --
step "musl headers (per architecture)"
MUSL_SRC="$ABI_CACHE/musl"
if [[ ! -d "$MUSL_SRC" ]]; then
  git clone --depth 1 https://git.musl-libc.org/git/musl "$MUSL_SRC"
fi
# Copying musl's include/ is not enough: bits/alltypes.h does not exist in the
# source tree. It is generated per architecture from alltypes.h.in plus that
# arch's own template, so the headers have to be *installed*, once per arch.
# `install-headers` needs no compiler and takes a second or two each.
mkdir -p "$OUT/include/musl-arch"
for arch_dir in "$MUSL_SRC"/arch/*/; do
  arch="$(basename "$arch_dir")"
  staged="$ABI_CACHE/musl-headers/$arch"
  if [[ ! -d "$staged/include" ]]; then
    make -C "$MUSL_SRC" install-headers ARCH="$arch" DESTDIR="$staged" prefix=/ >/dev/null
  fi
  # The generic headers are identical across arches; only bits/ varies, so the
  # shared tree is written once and each arch contributes just its bits/.
  if [[ ! -d "$OUT/include/musl" ]]; then
    mkdir -p "$OUT/include/musl"
    rsync -a --exclude 'bits/' "$staged/include/" "$OUT/include/musl/"
  fi
  mkdir -p "$OUT/include/musl-arch/$arch"
  rsync -a "$staged/include/bits/" "$OUT/include/musl-arch/$arch/bits/"
done

# --------------------------------------------------------------- libc++ --
step "libc++ headers"
mkdir -p "$OUT/include/c++/v1"
rsync -a --exclude '__support/' "$LLVM_SRC/libcxx/include/" "$OUT/include/c++/v1/" 2>/dev/null || {
  echo "note: libcxx not in the sparse checkout; run:" >&2
  echo "  git -C $LLVM_SRC sparse-checkout add libcxx" >&2
}

# ----------------------------------------------------------------- done --
printf '\n\033[1msysroot: %s\033[0m\n' "$OUT"
du -sh "$OUT"/* 2>/dev/null | sort -rh
printf 'total: %s\n' "$(du -sh "$OUT" | cut -f1)"
