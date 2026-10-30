#!/usr/bin/env bash
# Renders the pictures of the README (docs/images/*.png) with the layout
# preview: English interface, the made-up titles of src/player/ui-preview.c
# and generated test patterns as tile pictures, nothing from a real
# library. Run it before each release so the pictures show the current
# panels; unchanged panels give identical files.
set -euo pipefail

project_dir=$(cd "$(dirname "$0")/../.." && pwd)
images="$project_dir/docs/images"
preview="$project_dir/src/player/build/ui-preview"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$project_dir/src/player/build-ui-preview.sh"

# Tile pictures 1.ppm .. 7.ppm (see STEREONAUT_PREVIEW_THUMBS).
patterns=(
    "mandelbrot=s=960x540:start_scale=2.2:inner=convergence:outer=normalized_iteration_count"
    "gradients=s=960x540:c0=0x0b3d91:c1=0xff8a3d:c2=0xffd36e:nb_colors=3:type=linear:seed=7"
    "gradients=s=960x540:c0=0x1a0b3d:c1=0x2bd1c4:c2=0x0e5a8a:nb_colors=3:type=radial:seed=3"
    "mandelbrot=s=960x540:start_x=-0.743643:start_y=0.131825:start_scale=0.02:inner=mincol:outer=normalized_iteration_count"
    "gradients=s=960x540:c0=0x2d6a4f:c1=0xb7e4c7:c2=0x081c15:nb_colors=3:type=circular:seed=11"
    "gradients=s=960x540:c0=0x7b2cbf:c1=0xff6d00:c2=0x10002b:nb_colors=3:type=spiral:seed=5"
    "mandelbrot=s=960x540:start_x=-0.75:start_y=0.1:start_scale=0.4:outer=normalized_iteration_count"
)
mkdir -p "$work/thumbs"
for i in "${!patterns[@]}"; do
    ffmpeg -loglevel error -y -f lavfi -i "${patterns[$i]}" -frames:v 1 \
        -vf "scale=480:270,format=rgb24" "$work/thumbs/$((i + 1)).ppm"
done

# English, without the viewer's settings, without the gaze diagnostics.
export LC_ALL=C.UTF-8 LANG=C.UTF-8 LANGUAGE=en
export XDG_CONFIG_HOME="$work/config"
export STEREONAUT_PREVIEW_THUMBS="$work/thumbs"
export STEREONAUT_PREVIEW_NO_DEBUG=1

"$preview" "$work/browser.ppm" 5            # a tile pointed at
"$preview" "$work/search.ppm" 150           # the search keyboard
"$preview" --player "$work/player.ppm" 0 1 0   # the "View" tab open
"$preview" --player "$work/settings.ppm" 0 1 6 # the "Playback" settings

mkdir -p "$images"
for name in browser search player; do
    ffmpeg -loglevel error -y -i "$work/$name.ppm" "$images/$name.png"
done
# The settings page covers only the top of the playback panel's area.
ffmpeg -loglevel error -y -i "$work/settings.ppm" -vf "crop=1600:356:0:0" \
    "$images/settings.png"
echo "README pictures written to $images"
