#!/usr/bin/env sh
set -eu

src="$1"
dst="$2"

if command -v rsvg-convert >/dev/null 2>&1; then
    rsvg-convert -w 128 -h 128 "$src" -o "$dst"
elif command -v magick >/dev/null 2>&1; then
    magick "$src" -resize 128x128 "$dst"
elif command -v convert >/dev/null 2>&1; then
    convert "$src" -resize 128x128 "$dst"
elif command -v inkscape >/dev/null 2>&1; then
    inkscape "$src" --export-type=png --export-width=128 --export-height=128 --export-filename="$dst"
else
    echo "No SVG converter found. Install rsvg-convert, ImageMagick, or Inkscape." >&2
    exit 1
fi
