#!/bin/sh
# objects-pdf.sh — docs/objects/moritz_objects.pdf, the reference of every object, from OBJECTS.md
# (itself made by moritz --reference-md from help/reference/). Needs pandoc and xelatex.
#
#   ./moritz --reference-md > OBJECTS.md && tools/objects-pdf.sh      (the result is committed)
set -e
cd "$(dirname "$0")/.."
VERSION=$(sed -n 's/^VERSION *:= *//p' Makefile)
TMP=$(mktemp -d 2>/dev/null || mktemp -d -t objects)
# the title and the list of links become the PDF's title page and table of contents
awk 'NR == 1 && /^# / { next } /^- \[/ { next } { print }' OBJECTS.md > "$TMP/objects.md"
mkdir -p docs/objects
pandoc "$TMP/objects.md" -o docs/objects/moritz_objects.pdf \
    --pdf-engine=xelatex --shift-heading-level-by=-1 --toc --toc-depth=1 \
    -M title="Moritz objects" -M subtitle="Every object, with its arguments, messages, inlets and outlets" \
    -M date="version $VERSION" \
    -V documentclass=article -V fontsize=10pt -V geometry:margin=2.2cm \
    -V colorlinks=true -V linkcolor=blue -V toccolor=black \
    -V header-includes='\usepackage{titlesec}\titleformat{\section}{\clearpage\Large\bfseries}{}{0pt}{}'
rm -rf "$TMP"
echo "docs/objects/moritz_objects.pdf: $(pdfinfo docs/objects/moritz_objects.pdf 2>/dev/null | sed -n 's/^Pages: *//p') pages"
