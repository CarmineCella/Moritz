#!/usr/bin/env bash
# fetch_tinysol.sh — download the full TinySOL (about 750 MB of orchestral samples with its feature file) into the
# datasets folder that Musil and Moritz share (~/Documents/Moritz/datasets, or the one chosen with datasets-folder!):
# Musil's (dataset-fetch "tinysol"), which checks the archive's size and SHA-256 and installs it whole or not at all.
# Then (db-load "tinysol") and orchestra band tinysol find it by name; the examples, which name
# (list "fullsol" "tinysol" "microsol"), use it instead of the bundled MicroSOL. Any other *SOL database (FullSOL,
# OrchideaSOL) is put in the datasets folder by hand in the same layout (X.spectrum.db next to a folder X/ with the
# sounds), or registered where it is: (dataset-register! "fullsol" "/path/to/FullSOL2020.spectrum.db").
# Usage: ./fetch_tinysol.sh [path/to/musil]
set -euo pipefail
cd "$(dirname "$0")"
MUSIL="${1:-}"
if [ -z "$MUSIL" ]; then
    for m in ./musil ../build/musil/musil build/musil musil; do
        if [ -f "$m" ] && [ -x "$m" ]; then MUSIL="$m"; break; fi
        case "$m" in */*) ;; *) if command -v "$m" > /dev/null 2>&1; then MUSIL="$m"; break; fi ;; esac
    done
fi
[ -n "$MUSIL" ] || { echo "musil not found: ./fetch_tinysol.sh path/to/musil"; exit 1; }
MUSIL_PATH="$(pwd)/src${MUSIL_PATH:+:$MUSIL_PATH}" "$MUSIL" -e 'load "music.mu"' -e 'print "TinySOL:" (dataset-fetch "tinysol")'
