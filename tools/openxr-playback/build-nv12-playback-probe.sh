#!/bin/sh
set -eu

cc -std=c11 -O2 -Wall -Wextra -Werror -o xr-nv12-playback-probe \
  xr-nv12-playback-probe.c -I/usr/include -lvulkan
cc -std=c11 -O2 -Wall -Wextra -Werror -o xr-nv12-playback-renderer \
  xr-nv12-playback-renderer.c -I/usr/include -lvulkan -lm \
  /opt/steamvr/bin/linuxarm64/libopenxr_loader.so
glslc -fshader-stage=vert quad.vert -o quad.vert.spv
glslc -fshader-stage=frag nv12.frag -o nv12.frag.spv
