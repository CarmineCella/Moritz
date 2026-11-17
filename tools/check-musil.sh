#!/bin/sh
# check-musil.sh — musil/src is Musil exactly as released: every file in it must be byte-identical to
# the same file in Musil's repository at the tag below. Moritz does not change Musil; what Moritz adds
# to it lives in src/ (musil_host.h, session.cpp).
#
#   tools/check-musil.sh [path/to/a/musil/clone]     without a path, the tag is fetched from GitHub
#
# Used by CI.
TAG=v0.9.1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ -n "$1" ]; then
    SRC=$(mktemp -d) && (cd "$1" && git archive "$TAG" src) | tar -x -C "$SRC" || exit 1
else
    SRC=$(mktemp -d) && git -c advice.detachedHead=false clone -q --depth 1 --branch "$TAG" https://github.com/CarmineCella/musil "$SRC" || exit 1
fi
fails=0
count=0
cd "$ROOT/musil/src" || exit 1
for f in $(find . -type f | sed 's#^\./##' | sort); do
    count=$((count + 1))
    if ! cmp -s "$f" "$SRC/src/$f"; then echo "differs from Musil $TAG: musil/src/$f"; fails=$((fails + 1)); fi
done
rm -rf "$SRC"
echo "musil/src: $count files, $fails differ from Musil $TAG"
[ $fails -eq 0 ]
