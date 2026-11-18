#!/bin/sh
# check-musil.sh — Musil stays independent of Moritz: every file that musil/ includes with quotes is found
# inside musil/ (next to the file, or in musil/src), never in Moritz's src/. musil/ also builds alone
# (cmake -S musil -B build-musil); this check names the offending line instead of a compiler error.
#
#   tools/check-musil.sh        (CI runs it)
cd "$(dirname "$0")/../musil" || exit 1
bad=$(find . -name '*.h' -o -name '*.cpp' | while IFS= read -r f; do
    dir=$(dirname "$f")
    grep -n '^[[:space:]]*#[[:space:]]*include[[:space:]]*"' "$f" | while IFS= read -r line; do
        inc=$(printf '%s\n' "$line" | sed -E 's/.*#[[:space:]]*include[[:space:]]*"([^"]+)".*/\1/')
        [ -f "$dir/$inc" ] || [ -f "src/$inc" ] || echo "musil/${f#./}:${line%%:*}: includes \"$inc\", which is not in musil/"
    done
done)
if [ -n "$bad" ]; then printf '%s\n' "$bad"; echo "Musil must not depend on Moritz"; exit 1; fi
echo "musil/: stands alone"
