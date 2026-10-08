#!/bin/sh
set -eu

cc -std=c11 -O2 -Wall -Wextra -Werror -o xr-quad-probe xr-quad-probe.c \
  -I/usr/include -lvulkan -lm /opt/steamvr/bin/linuxarm64/libopenxr_loader.so
glslc -fshader-stage=vert quad.vert -o quad.vert.spv
glslc -fshader-stage=frag quad.frag -o quad.frag.spv
