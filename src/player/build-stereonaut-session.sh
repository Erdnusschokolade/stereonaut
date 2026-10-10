#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
build_dir="$script_dir/build"
openxr_loader=${OPENXR_LOADER:-/opt/steamvr/bin/linuxarm64/libopenxr_loader.so}

mkdir -p "$build_dir"

cc -std=c11 -O2 -Wall -Wextra -Werror \
  $(pkg-config --cflags libavformat libavcodec libavutil libswresample alsa \
                       libcurl json-c) \
  -I"$project_dir/tools/openxr-playback" \
  -o "$build_dir/stereonaut-session" \
  "$script_dir/stereonaut-session.c" \
  "$script_dir/media-pipeline.c" \
  "$script_dir/packet-queue.c" \
  "$script_dir/audio-output.c" \
  "$script_dir/media-demux.c" \
  "$script_dir/jellyfin.c" \
  "$script_dir/playback-report.c" \
  "$script_dir/title-settings.c" \
  $(pkg-config --libs libavformat libavcodec libavutil libswresample alsa \
                libcurl json-c) \
  -lvulkan -pthread

# XR host: video renderer, media browser and Jellyfin client in one process.
cc -std=c11 -O2 -Wall -Wextra -Werror \
  $(pkg-config --cflags libcurl json-c freetype2 libjpeg) \
  -I/usr/include -I"$script_dir" -I"$project_dir/tools/openxr-playback" \
  -o "$build_dir/stereonaut-renderer" \
  "$script_dir/stereonaut-renderer.c" "$script_dir/browser-ui.c" \
  "$script_dir/player-ui.c" "$script_dir/ui-draw.c" "$script_dir/thumbnails.c" \
  "$script_dir/file-browser.c" "$script_dir/jellyfin.c" \
  "$script_dir/trickplay.c" "$script_dir/settings.c" \
  "$script_dir/title-settings.c" \
  $(pkg-config --libs libcurl json-c freetype2 libjpeg) \
  -lvulkan -lm -pthread "$openxr_loader"

cc -std=c11 -O2 -Wall -Wextra -Werror \
  -I"$script_dir" -o "$build_dir/stereonaut-launcher" \
  "$script_dir/stereonaut-launcher.c"

glslc -fshader-stage=vert \
  "$project_dir/tools/openxr-playback/video-projection.vert" \
  -o "$build_dir/video-projection.vert.spv"
glslc -fshader-stage=frag \
  "$project_dir/tools/openxr-playback/nv12-projection.frag" \
  -o "$build_dir/nv12-projection.frag.spv"
glslc -fshader-stage=frag \
  "$project_dir/tools/openxr-playback/tp10-projection.frag" \
  -o "$build_dir/tp10-projection.frag.spv"
glslc -fshader-stage=comp \
  "$project_dir/tools/openxr-playback/stereo-disparity.comp" \
  -o "$build_dir/stereo-disparity.comp.spv"
glslc -fshader-stage=comp -DTP10 \
  "$project_dir/tools/openxr-playback/stereo-disparity.comp" \
  -o "$build_dir/stereo-disparity-tp10.comp.spv"
