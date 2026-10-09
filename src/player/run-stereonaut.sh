#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$script_dir/build"
exec ./stereonaut-launcher "${1:-$HOME}"
