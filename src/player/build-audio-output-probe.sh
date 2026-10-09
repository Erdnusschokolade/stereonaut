#!/bin/sh
set -eu

mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror \
  $(pkg-config --cflags libavformat libavcodec libavutil libswresample alsa) \
  -o build/audio-output-probe \
  audio-output-probe.c audio-output.c media-demux.c \
  $(pkg-config --libs libavformat libavcodec libavutil libswresample alsa)
