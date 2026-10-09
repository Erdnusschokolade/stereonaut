#!/bin/sh
# Builds the off-headset preview of the browser panel (any Linux host with
# freetype2, libjpeg, libcurl and json-c):
#   build/ui-preview out.ppm [HOVER_ID]
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$script_dir/build"
cc -std=c11 -O2 -Wall -Wextra -Werror \
  $(pkg-config --cflags freetype2 libjpeg libcurl json-c) -I"$script_dir" \
  -o "$script_dir/build/ui-preview" \
  "$script_dir/ui-preview.c" "$script_dir/browser-ui.c" \
  "$script_dir/ui-draw.c" "$script_dir/thumbnails.c" "$script_dir/jellyfin.c" \
  $(pkg-config --libs freetype2 libjpeg libcurl json-c) -lm -pthread
