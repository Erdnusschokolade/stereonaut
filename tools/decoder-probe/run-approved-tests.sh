#!/bin/sh
# Run ONLY after explicit approval, from the new dedicated directory under $HOME.
# No SSH setup, package changes, or decoder fallbacks.
set -eu
case "$PWD" in "$HOME"/stereonaut-decoder-002-*) ;; *) echo 'Use the approved home test directory' >&2; exit 2;; esac
mkdir -p results
gcc -std=c11 -Wall -Wextra -Werror -O2 gst-probe.c -o gst-probe \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0)
gcc -std=c11 -Wall -Wextra -Werror -O2 v4l2-formats.c -o v4l2-formats
timeout -k 3s 10s ./v4l2-formats > results/v4l2-formats.log 2>&1
run() {
    clip=$1 format=$2 mode=$3
    name="$clip-$format-$mode"
    set +e
    GST_DEBUG_NO_COLOR=1 GST_DEBUG=v4l2*:4 timeout -k 3s 30s \
        strace -f -yy -e trace=ioctl,openat,close -o "results/$name.strace" \
        ./gst-probe --gst-disable-registry-update "$clip.hevc" "$format" "$mode" 60 \
        "results/$name.raw" > "results/$name.log" 2>&1
    rc=$?
    set -e
    printf '%s exit=%s\n' "$name" "$rc" | tee -a results/status.txt
}
run main auto mmap
run main auto dmabuf
run main10 auto mmap
run main10 auto dmabuf
run main10 P010_10LE mmap
run main10 P010_10LE dmabuf
run gradient10 auto mmap
run gradient10 P010_10LE dmabuf
