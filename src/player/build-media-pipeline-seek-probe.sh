#!/bin/sh
set -eu

mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror \
  $(pkg-config --cflags libavformat libavcodec libavutil libswresample alsa) \
  -o build/media-pipeline-seek-probe \
  media-pipeline-seek-probe.c media-pipeline.c packet-queue.c \
  audio-output.c media-demux.c \
  $(pkg-config --libs libavformat libavcodec libavutil libswresample alsa) \
  -pthread
