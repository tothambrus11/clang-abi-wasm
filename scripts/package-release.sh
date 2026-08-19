#!/usr/bin/env bash
# Turns a built dist/ into a release: content-addressed files plus the one
# mutable manifest that points at them.
#
# The naming is the whole mechanism. Every file but manifest.json carries a hash
# of its own contents, so every file but manifest.json can be served
# `immutable` and never revalidated — and two releases that differ only in the
# wasm share the header packs byte for byte, so a clang bump costs returning
# visitors the wasm and nothing else.
#
#   scripts/package-release.sh [dist-dir] [out-dir]
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$REPO/scripts/config.sh"

DIST="${1:-$REPO/dist}"
OUT="${2:-$REPO/release}"

[[ -f "$DIST/abi_query.wasm" ]] || { echo "no build in $DIST — run scripts/build.sh wasm" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"

short() { sha256sum "$1" | cut -c1-12; }
bytes() { stat -c %s "$1"; }
gzipped() { gzip -9c "$1" | wc -c; }

# Copy one file under a content-addressed name and print its manifest entry.
emit() {
  local key="$1" src="$2" ext="$3"
  local hash name
  hash="$(short "$src")"
  name="${key}-${hash}.${ext}"
  cp "$src" "$OUT/$name"
  printf '    "%s": { "path": "%s", "sha256": "%s", "bytes": %s, "gzip": %s }' \
    "$key" "$name" "$(sha256sum "$src" | cut -d' ' -f1)" "$(bytes "$src")" "$(gzipped "$src")"
}

VERSION="$(node -p "require('$REPO/js/package.json').version" 2>/dev/null || echo 0.0.0)"
CLANG="${LLVM_TAG#llvmorg-}"
BUILT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

{
  echo '{'
  echo '  "schemaVersion": 1,'
  echo "  \"version\": \"${VERSION}+llvm${CLANG}\","
  echo "  \"clang\": \"${CLANG}\","
  echo "  \"built\": \"${BUILT}\","
  # What the build was made from. Anything that changes the output appears here,
  # and nothing that does not — this doubles as the CI cache key.
  echo '  "inputs": {'
  echo "    \"llvmTag\": \"${LLVM_TAG}\","
  echo "    \"emsdk\": \"${EMSDK_VERSION}\","
  echo "    \"cmakeHash\": \"$(printf '%s' "$CMAKE_COMMON" | sha256sum | cut -c1-16)\""
  echo '  },'
  echo '  "files": {'
  emit wasm "$DIST/abi_query.wasm" wasm; echo ','
  emit glue "$DIST/abi_query.mjs" mjs; echo ','
  emit headers "$DIST/abi_query.data" data
  echo
  echo '  }'
  echo '}'
} > "$OUT/manifest.json"

# The JS API is small, changes with the schema rather than with the build, and
# is what a bundler resolves `clang-abi-wasm` to — so it keeps its plain name.
cp "$REPO/js/index.mjs" "$REPO/js/index.d.ts" "$OUT/"

node -e "JSON.parse(require('fs').readFileSync('$OUT/manifest.json','utf8'))" \
  || { echo "manifest.json is not valid JSON" >&2; exit 1; }

printf '\n\033[1mrelease: %s\033[0m\n' "$OUT"
ls -lh "$OUT" | tail -n +2 | awk '{printf "  %-46s %s\n", $9, $5}'
printf '\ntotal over the wire (gzip): %s\n' \
  "$(node -p "const m=require('$OUT/manifest.json');(Object.values(m.files).reduce((a,f)=>a+f.gzip,0)/1048576).toFixed(1)+' MB'")"
