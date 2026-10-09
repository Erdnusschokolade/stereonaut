#!/bin/sh
set -eu

mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror \
  $(pkg-config --cflags libavformat libavcodec libavutil) \
  -o build/media-demux-probe media-demux-probe.c media-demux.c \
  $(pkg-config --libs libavformat libavcodec libavutil)
