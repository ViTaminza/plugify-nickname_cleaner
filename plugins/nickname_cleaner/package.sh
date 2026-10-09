#!/bin/bash
# Assembles build/package/nickname_cleaner: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/nickname_cleaner"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/nickname_cleaner/libnickname_cleaner.so" "$OUT/bin/"
cp "$ROOT/build/plugins/nickname_cleaner/nickname_cleaner.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
