#!/bin/sh
set -eu

mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror \
  -o build/file-browser-probe file-browser-probe.c file-browser.c -pthread
