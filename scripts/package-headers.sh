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
# git.musl-libc.org is unreachable from some networks, so mirrors are tried in
# turn. musl is an improvement, not a prerequisite: without it the module still
# answers every question that does not reach a libc header, so a fetch failure
# warns and carries on rather than failing the build.
MUSL_OK=1
if [[ ! -d "$MUSL_SRC" ]]; then
  MUSL_OK=0
  for remote in \
    https://git.musl-libc.org/git/musl \
    https://github.com/kraj/musl.git \
    https://github.com/esmil/musl.git
  do
    if git clone --depth 1 "$remote" "$MUSL_SRC" 2>/dev/null; then MUSL_OK=1; break; fi
    echo "  musl: $remote unreachable, trying the next mirror" >&2
  done
fi
if [[ "$MUSL_OK" != 1 ]]; then
  echo "  WARNING: no musl mirror reachable — the pack will have no C library." >&2
  echo "  Targets will still lay out records; anything including a libc header" >&2
  echo "  will not resolve. Re-run when a mirror is available." >&2
fi
if [[ "$MUSL_OK" == 1 ]]; then
# Copying musl's include/ is not enough: bits/alltypes.h does not exist in the
# source tree. It is generated per architecture from alltypes.h.in plus that
# arch's own template, so the headers have to be *installed*, once per arch.
# `install-headers` needs no compiler and takes a second or two each.
mkdir -p "$OUT/include/musl-arch"
for arch_dir in "$MUSL_SRC"/arch/*/; do
  arch="$(basename "$arch_dir")"
  # `generic` is a shared overlay, not a target: it has no alltypes template of
  # its own and musl's build rejects it as an ARCH.
  [[ -f "$arch_dir/bits/alltypes.h.in" ]] || continue
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
fi

# --------------------------------------------------------------- libc++ --
step "libc++ headers"
mkdir -p "$OUT/include/c++/v1"
rsync -a --exclude '__support/' --exclude '*.in' --exclude 'CMakeLists.txt' \
  "$LLVM_SRC/libcxx/include/" "$OUT/include/c++/v1/" 2>/dev/null || {
  echo "note: libcxx not in the sparse checkout; run:" >&2
  echo "  git -C $LLVM_SRC sparse-checkout add libcxx" >&2
}

# __config_site does not exist in the source tree: libc++'s build generates it
# from __config_site.in, recording which options that build was configured with.
# Every libc++ header includes it, so without it nothing C++ parses at all.
#
# These are the upstream defaults — what a plain `cmake` with no libc++ options
# set produces, and what distributions therefore ship. They are not cosmetic:
# _LIBCPP_ABI_VERSION and the ABI namespace decide the layout of every standard
# type, so a layout viewer that reported them wrong would be wrong in exactly
# the way that matters. Anyone needing a different configuration should
# substitute their own build's __config_site here.
cat > "$OUT/include/c++/v1/__config_site" <<'CONFIG'
#ifndef _LIBCPP___CONFIG_SITE
#define _LIBCPP___CONFIG_SITE

#define _LIBCPP_ABI_VERSION 1
#define _LIBCPP_ABI_NAMESPACE __1
#define _LIBCPP_ABI_FORCE_ITANIUM 0
#define _LIBCPP_ABI_FORCE_MICROSOFT 0
#define _LIBCPP_HAS_THREADS 1
#define _LIBCPP_HAS_MONOTONIC_CLOCK 1
#define _LIBCPP_HAS_TERMINAL 1
// 1, not the upstream default of 0: this pack ships musl, and libc++ picks its
// locale and ctype implementation from this. Left at 0 it looks for a rune
// table no musl system has and <string> fails to parse at all.
#define _LIBCPP_HAS_MUSL_LIBC 1
#define _LIBCPP_HAS_THREAD_API_PTHREAD 0
#define _LIBCPP_HAS_THREAD_API_EXTERNAL 0
#define _LIBCPP_HAS_THREAD_API_WIN32 0
#define _LIBCPP_HAS_THREAD_API_C11 0
#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0
#define _LIBCPP_HAS_FILESYSTEM 1
#define _LIBCPP_HAS_RANDOM_DEVICE 1
#define _LIBCPP_HAS_LOCALIZATION 1
#define _LIBCPP_HAS_UNICODE 1
#define _LIBCPP_HAS_WIDE_CHARACTERS 1
#define _LIBCPP_HAS_TIME_ZONE_DATABASE 1
#define _LIBCPP_INSTRUMENTED_WITH_ASAN 0
#define _LIBCPP_PSTL_BACKEND_SERIAL
// Hardening: libc++'s own cmake defaults are mode "none" (2) and assertion
// semantic "hardening_dependent" (2). The values are the bit constants from
// __configuration/hardening.h, not ordinals.
#define _LIBCPP_HARDENING_MODE_DEFAULT 2
#define _LIBCPP_ASSERTION_SEMANTIC_DEFAULT 2
#define _LIBCPP_LIBC_PICOLIBC 0
#define _LIBCPP_LIBC_NEWLIB 0

#endif // _LIBCPP___CONFIG_SITE
CONFIG

# __assertion_handler is the other generated header: libc++'s build copies a
# vendor template into the include tree. LLVM's own default is the one to use.
cp "$LLVM_SRC/libcxx/vendor/llvm/default_assertion_handler.in" \
   "$OUT/include/c++/v1/__assertion_handler"

# ----------------------------------------------------------------- done --
printf '\n\033[1msysroot: %s\033[0m\n' "$OUT"
du -sh "$OUT"/* 2>/dev/null | sort -rh
printf 'total: %s\n' "$(du -sh "$OUT" | cut -f1)"
