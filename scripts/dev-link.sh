#!/usr/bin/env bash
# Point a local web app at this working copy, so a change here shows up there
# after `scripts/build.sh wasm` and a page reload — no publish, no version bump.
#
#   scripts/dev-link.sh ~/abi-explorer-2
#
# Symlinks rather than copies: rebuilding updates the app in place. Undo with
# scripts/dev-unlink.sh, or just delete the link.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="${1:?usage: dev-link.sh <path-to-web-app>}"
APP="$(cd "$APP" && pwd)"

[[ -d "$REPO/dist" ]] || { echo "nothing built yet — run scripts/build.sh wasm" >&2; exit 1; }

TARGET="$APP/public/vendor/abi"
mkdir -p "$(dirname "$TARGET")"
rm -rf "$TARGET"
ln -s "$REPO/dist" "$TARGET"
echo "linked $TARGET -> $REPO/dist"

# npm link makes `import { load } from 'clang-abi-wasm'` resolve to this repo,
# so the app's own type checking sees the schema you are editing.
( cd "$REPO/js" && npm link >/dev/null 2>&1 ) || true
( cd "$APP" && npm link clang-abi-wasm >/dev/null 2>&1 ) || true

cat <<EOF

Linked. In the app:

  VITE_ABI=1 npm run dev

Vite serves public/vendor/abi/ directly, so after
  scripts/build.sh wasm
a reload picks the new module up. Types come from this repo via npm link.
EOF
