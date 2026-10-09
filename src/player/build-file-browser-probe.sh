#!/bin/sh
set -eu

mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror \
  $(pkg-config --cflags libcurl json-c) \
  -o build/file-browser-probe file-browser-probe.c file-browser.c jellyfin.c \
  $(pkg-config --libs libcurl json-c) -pthread
