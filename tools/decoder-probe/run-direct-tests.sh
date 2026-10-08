#!/bin/sh
# Supplemental procedure; requires separate approval before target execution.
set -eu
case "$PWD" in "$HOME"/stereonaut-decoder-002-*) ;; *) exit 2;; esac
mkdir -p results-direct
gcc -std=c11 -Wall -Wextra -Werror -O2 v4l2-decode.c -o v4l2-decode
run() {
    clip=$1 format=$2
    name="$clip-$format"
    if timeout -k 3s 30s strace -f -yy -e trace=ioctl,openat,close -o "results-direct/$name.strace" \
        ./v4l2-decode "$clip.hevc" "$format" "results-direct/$name.raw" \
        > "results-direct/$name.log" 2>&1; then
        rc=0
    else
        rc=$?
    fi
    printf '%s exit=%s\n' "$name" "$rc" | tee -a results-direct/status.txt
    return "$rc"
}
run main NV12 || exit $?
run main10 auto || exit $?
run main10 Q10C || exit $?
run gradient10 Q10C || exit $?
if run main10 P010; then
    rc=0
else
    rc=$?
fi
printf 'main10-P010 expected-format-rejection exit=%s\n' "$rc" | tee -a results-direct/status.txt
if [ "$rc" -ne 3 ]; then
    exit "$rc"
fi
# Same GStreamer case, with negotiation diagnostics. No automatic decoder choice.
if GST_DEBUG_NO_COLOR=1 GST_DEBUG='GST_CAPS:6,h265parse:5,v4l2*:5' timeout -k 3s 30s \
    ./gst-probe --gst-disable-registry-update main.hevc auto mmap 60 - \
    > results-direct/gst-negotiation.log 2>&1; then
    rc=0
else
    rc=$?
fi
printf 'gst-negotiation exit=%s\n' "$rc" | tee -a results-direct/status.txt
# A successful pipeline or its known pre-stream negotiation error is diagnostic
# output. Timeout/signal/error values outside 0/1 must stop the procedure.
if [ "$rc" -ne 0 ] && [ "$rc" -ne 1 ]; then
    exit "$rc"
fi
