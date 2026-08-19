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
[[ -d "$CLANG_RES" ]] || {
  echo "no clang resource directory under $WASM_BUILD/lib/clang" >&2
  echo "  The wasm build has not produced clang-resource-headers." >&2
  echo "  Run: scripts/bootstrap.sh   (or ninja -C \"$WASM_BUILD\" clang-resource-headers)" >&2
  exit 1
}
# Keep clang's own layout, version directory and all: the resource directory the
# module is compiled to expect is lib/clang/<major>, and flattening it to
# lib/clang/include puts the builtin headers somewhere nothing looks. That
# failure hides on hosted targets, where musl supplies <stdint.h> anyway, and
# only surfaces on a bare-metal one that has no libc to fall back to.
CLANG_MAJOR="$(basename "$(dirname "$CLANG_RES")")"
mkdir -p "$OUT/lib/clang/$CLANG_MAJOR/include"
rsync -a \
  --exclude 'opencl-c*.h' \
  --exclude 'cuda_wrappers/' \
  --exclude '__clang_cuda*' \
  --exclude '__clang_hip*' \
  --exclude 'sanitizer/' \
  --exclude 'openmp_wrappers/' \
  "$CLANG_RES/" "$OUT/lib/clang/$CLANG_MAJOR/include/"

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
    # musl generates bits/alltypes.h and bits/syscall.h into one obj/ directory
    # that carries no architecture in its path, and make will not regenerate a
    # file whose template is older than it. Installing several architectures in
    # a row therefore gives every one of them the *first* one's generated
    # headers — and it says nothing while doing it. That put 64-bit size_t,
    # uintptr_t and time_t into every 32-bit architecture's tree, which is a
    # wrong answer of exactly the kind this whole module exists to avoid.
    rm -rf "$MUSL_SRC/obj/include"
    make -C "$MUSL_SRC" install-headers ARCH="$arch" DESTDIR="$staged" prefix=/ >/dev/null
  fi
  # Belt and braces: regenerate the one that matters straight from this
  # architecture's template, with musl's own generator. Cheap, and it does not
  # depend on make having been persuaded.
  sed -f "$MUSL_SRC/tools/mkalltypes.sed" \
      "$arch_dir/bits/alltypes.h.in" "$MUSL_SRC/include/alltypes.h.in" \
      > "$staged/include/bits/alltypes.h"
  # The generic headers are identical across arches; only bits/ varies, so the
  # shared tree is written once and each arch contributes just its bits/.
  if [[ ! -d "$OUT/include/musl" ]]; then
    mkdir -p "$OUT/include/musl"
    rsync -a --exclude 'bits/' "$staged/include/" "$OUT/include/musl/"
  fi
  mkdir -p "$OUT/include/musl-arch/$arch"
  rsync -a "$staged/include/bits/" "$OUT/include/musl-arch/$arch/bits/"
done

# ------------------------------------------- a generic architecture layer --
# musl has templates for the architectures musl runs on. Every other target —
# Windows, Darwin, and the whole bare-metal world — had no C library headers at
# all, which meant `#include <string>` failed outright: libc++ reaches for
# <wchar.h> and <stdint.h> whatever it is running on.
#
# Borrowing another architecture's tree is not the answer and is exactly how
# this went wrong before: x86_64's says `uint64_t` is `unsigned long`, which on
# Windows is four bytes, so a struct came out 32 rather than 40 with nothing to
# say why. This tree instead takes every type from the *compiler's* own macros,
# which clang defines correctly for every target it knows. musl's portable
# headers sit on top unchanged.
#
# What it deliberately does not contain: bits/stat.h, bits/signal.h,
# bits/socket.h and the rest of the operating-system layer. Those genuinely
# differ per platform and this tree does not know them, so `#include
# <sys/stat.h>` on a Darwin target fails to find a header rather than quietly
# answering with Linux's struct.
step "generic architecture layer (targets musl has no template for)"
GENERIC="$OUT/include/musl-arch/generic/bits"
mkdir -p "$GENERIC"

cat > "$ABI_CACHE/generic-alltypes.h.in" <<'GENERIC_ALLTYPES'
/* The integer type a pointer fits in, spelled with the keywords musl's common
 * template composes with (`unsigned _Addr size_t`). LLP64 — Windows x64, where
 * a pointer is eight bytes and a long is four — is why this is not just
 * `long`, which is what every musl arch template says. */
#if __SIZEOF_POINTER__ == __SIZEOF_LONG__
#define _Addr long
#elif __SIZEOF_POINTER__ == __SIZEOF_INT__
#define _Addr int
#elif __SIZEOF_POINTER__ == __SIZEOF_LONG_LONG__
#define _Addr long long
#else
#define _Addr int
#endif

#if __SIZEOF_LONG__ == 8
#define _Int64 long
#else
#define _Int64 long long
#endif

#define _Reg _Addr

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define __BYTE_ORDER 4321
#else
#define __BYTE_ORDER 1234
#endif
#define __LONG_MAX __LONG_MAX__

/* Everything the compiler already knows the answer to. These come first, and
 * the mkalltypes guards mean the common template's own versions are then
 * skipped — which is the point: it says `int32_t` is `int`, and there are
 * targets here whose int is sixteen bits. */
#ifndef __cplusplus
TYPEDEF __WCHAR_TYPE__ wchar_t;
#endif
TYPEDEF __WINT_TYPE__ wint_t;
TYPEDEF __SIZE_TYPE__ size_t;
TYPEDEF __PTRDIFF_TYPE__ ptrdiff_t;
TYPEDEF __PTRDIFF_TYPE__ ssize_t;
TYPEDEF __INTPTR_TYPE__ intptr_t;
TYPEDEF __UINTPTR_TYPE__ uintptr_t;
TYPEDEF __INTMAX_TYPE__ intmax_t;
TYPEDEF __UINTMAX_TYPE__ uintmax_t;
TYPEDEF __INT8_TYPE__ int8_t;
TYPEDEF __INT16_TYPE__ int16_t;
TYPEDEF __INT32_TYPE__ int32_t;
TYPEDEF __INT64_TYPE__ int64_t;
TYPEDEF __UINT8_TYPE__ uint8_t;
TYPEDEF __UINT16_TYPE__ uint16_t;
TYPEDEF __UINT32_TYPE__ uint32_t;
TYPEDEF __UINT64_TYPE__ uint64_t;
TYPEDEF __UINT64_TYPE__ u_int64_t;

#if defined(__FLT_EVAL_METHOD__) && __FLT_EVAL_METHOD__ == 2
TYPEDEF long double float_t;
TYPEDEF long double double_t;
#elif defined(__FLT_EVAL_METHOD__) && __FLT_EVAL_METHOD__ == 1
TYPEDEF double float_t;
TYPEDEF double double_t;
#else
TYPEDEF float float_t;
TYPEDEF double double_t;
#endif

TYPEDEF struct { long long __ll; long double __ld; } max_align_t;
GENERIC_ALLTYPES

# musl's own generator, over our template and musl's common one — so the
# guards, the ordering and the set of types are musl's, not a reimplementation.
sed -f "$MUSL_SRC/tools/mkalltypes.sed" \
    "$ABI_CACHE/generic-alltypes.h.in" "$MUSL_SRC/include/alltypes.h.in" \
    > "$GENERIC/alltypes.h"

cat > "$GENERIC/stdint.h" <<'GENERIC_STDINT'
/* The fast types and the pointer-width limits, from the compiler's macros.
 * musl's arch files are byte-identical across every architecture it supports;
 * this one is right for the ones it does not. */
typedef __INT_FAST16_TYPE__  int_fast16_t;
typedef __INT_FAST32_TYPE__  int_fast32_t;
typedef __UINT_FAST16_TYPE__ uint_fast16_t;
typedef __UINT_FAST32_TYPE__ uint_fast32_t;

#define INT_FAST16_MIN  (-__INT_FAST16_MAX__ - 1)
#define INT_FAST32_MIN  (-__INT_FAST32_MAX__ - 1)
#define INT_FAST16_MAX  __INT_FAST16_MAX__
#define INT_FAST32_MAX  __INT_FAST32_MAX__
#define UINT_FAST16_MAX __UINT_FAST16_MAX__
#define UINT_FAST32_MAX __UINT_FAST32_MAX__

#define INTPTR_MIN      (-__INTPTR_MAX__ - 1)
#define INTPTR_MAX      __INTPTR_MAX__
#define UINTPTR_MAX     __UINTPTR_MAX__
#define PTRDIFF_MIN     (-__PTRDIFF_MAX__ - 1)
#define PTRDIFF_MAX     __PTRDIFF_MAX__
#define SIZE_MAX        __SIZE_MAX__
GENERIC_STDINT

cat > "$GENERIC/limits.h" <<'GENERIC_LIMITS'
/* musl's arch files differ here only in PAGESIZE, which is a property of an
 * operating system this layer does not claim to know. */
GENERIC_LIMITS

# errno values are macros, not layout, and musl spells them the same way on
# every architecture but mips and powerpc — both of which have musl trees of
# their own and never reach this one.
cp "$OUT/include/musl-arch/x86_64/bits/errno.h" "$GENERIC/errno.h"
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
#
# Every value is `#ifndef`-guarded so the query can override it per target. A
# real __config_site records one build's configuration, which is right when
# there is one target; here there are hundreds, and a bare-metal target has no
# threads while a Windows one has no locale API we ship. Those are facts about
# the target, so the module decides them per query — see the libc++ knobs in
# abi_query.cpp.
cat > "$OUT/include/c++/v1/__config_site" <<'CONFIG'
#ifndef _LIBCPP___CONFIG_SITE
#define _LIBCPP___CONFIG_SITE

#ifndef _LIBCPP_ABI_VERSION
#  define _LIBCPP_ABI_VERSION 1
#endif
#ifndef _LIBCPP_ABI_NAMESPACE
#  define _LIBCPP_ABI_NAMESPACE __1
#endif
#ifndef _LIBCPP_ABI_FORCE_ITANIUM
#  define _LIBCPP_ABI_FORCE_ITANIUM 0
#endif
#ifndef _LIBCPP_ABI_FORCE_MICROSOFT
#  define _LIBCPP_ABI_FORCE_MICROSOFT 0
#endif
#ifndef _LIBCPP_HAS_THREADS
#  define _LIBCPP_HAS_THREADS 1
#endif
#ifndef _LIBCPP_HAS_MONOTONIC_CLOCK
#  define _LIBCPP_HAS_MONOTONIC_CLOCK 1
#endif
#ifndef _LIBCPP_HAS_TERMINAL
#  define _LIBCPP_HAS_TERMINAL 1
#endif
// 1, not the upstream default of 0: this pack ships musl, and libc++ picks its
// locale and ctype implementation from this. Left at 0 it looks for a rune
// table no musl system has and <string> fails to parse at all.
#ifndef _LIBCPP_HAS_MUSL_LIBC
#  define _LIBCPP_HAS_MUSL_LIBC 1
#endif
#ifndef _LIBCPP_HAS_THREAD_API_PTHREAD
#  define _LIBCPP_HAS_THREAD_API_PTHREAD 0
#endif
#ifndef _LIBCPP_HAS_THREAD_API_EXTERNAL
#  define _LIBCPP_HAS_THREAD_API_EXTERNAL 0
#endif
#ifndef _LIBCPP_HAS_THREAD_API_WIN32
#  define _LIBCPP_HAS_THREAD_API_WIN32 0
#endif
#ifndef _LIBCPP_HAS_THREAD_API_C11
#  define _LIBCPP_HAS_THREAD_API_C11 0
#endif
#ifndef _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS
#  define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0
#endif
#ifndef _LIBCPP_HAS_FILESYSTEM
#  define _LIBCPP_HAS_FILESYSTEM 1
#endif
#ifndef _LIBCPP_HAS_RANDOM_DEVICE
#  define _LIBCPP_HAS_RANDOM_DEVICE 1
#endif
#ifndef _LIBCPP_HAS_LOCALIZATION
#  define _LIBCPP_HAS_LOCALIZATION 1
#endif
#ifndef _LIBCPP_HAS_UNICODE
#  define _LIBCPP_HAS_UNICODE 1
#endif
#ifndef _LIBCPP_HAS_WIDE_CHARACTERS
#  define _LIBCPP_HAS_WIDE_CHARACTERS 1
#endif
#ifndef _LIBCPP_HAS_TIME_ZONE_DATABASE
#  define _LIBCPP_HAS_TIME_ZONE_DATABASE 1
#endif
#ifndef _LIBCPP_INSTRUMENTED_WITH_ASAN
#  define _LIBCPP_INSTRUMENTED_WITH_ASAN 0
#endif
#define _LIBCPP_PSTL_BACKEND_SERIAL
// Hardening: libc++'s own cmake defaults are mode "none" (2) and assertion
// semantic "hardening_dependent" (2). The values are the bit constants from
// __configuration/hardening.h, not ordinals.
#ifndef _LIBCPP_HARDENING_MODE_DEFAULT
#  define _LIBCPP_HARDENING_MODE_DEFAULT 2
#endif
#ifndef _LIBCPP_ASSERTION_SEMANTIC_DEFAULT
#  define _LIBCPP_ASSERTION_SEMANTIC_DEFAULT 2
#endif
#ifndef _LIBCPP_LIBC_PICOLIBC
#  define _LIBCPP_LIBC_PICOLIBC 0
#endif
#ifndef _LIBCPP_LIBC_NEWLIB
#  define _LIBCPP_LIBC_NEWLIB 0
#endif

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
