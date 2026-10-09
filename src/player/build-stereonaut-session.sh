#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
build_dir="$script_dir/build"
openxr_loader=${OPENXR_LOADER:-/opt/steamvr/bin/linuxarm64/libopenxr_loader.so}

mkdir -p "$build_dir"

cc -std=c11 -O2 -Wall -Wextra -Werror \
  $(pkg-config --cflags libavformat libavcodec libavutil libswresample alsa) \
  -I"$project_dir/tools/openxr-playback" \
  -o "$build_dir/stereonaut-session" \
  "$script_dir/stereonaut-session.c" \
  "$script_dir/media-pipeline.c" \
  "$script_dir/packet-queue.c" \
  "$script_dir/audio-output.c" \
  "$script_dir/media-demux.c" \
  $(pkg-config --libs libavformat libavcodec libavutil libswresample alsa) \
  -lvulkan -pthread

cc -std=c11 -O2 -Wall -Wextra -Werror \
  -I/usr/include -I"$project_dir/tools/openxr-playback" \
  -o "$build_dir/stereonaut-renderer" \
  "$project_dir/tools/openxr-playback/xr-nv12-playback-renderer.c" \
  -lvulkan -lm "$openxr_loader"

glslc -fshader-stage=vert \
  "$project_dir/tools/openxr-playback/video-projection.vert" \
  -o "$build_dir/video-projection.vert.spv"
glslc -fshader-stage=frag \
  "$project_dir/tools/openxr-playback/nv12-projection.frag" \
  -o "$build_dir/nv12-projection.frag.spv"
