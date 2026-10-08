# Realvideo 001 probe

`continuous-nv12-vulkan.c` is the bounded continuous path for development
order 007:

`HEVC Main 8-bit Annex B -> Iris V4L2 -> NV12 DMA-BUF -> Vulkan transfer`

The probe uses `/dev/video-dec0` directly and has no GStreamer or software
decoder fallback. It requires an AUD-delimited elementary stream. Before
opening the decoder it checks Turnip's linear NV12 DMA-BUF support and SYNC_FD
semaphore import. After `SOURCE_CHANGE` it accepts only a contiguous,
single-memory-plane V4L2 `NV12` allocation whose size is exactly
`stride * coded_height * 3 / 2`.

Each CAPTURE allocation is exported once and imported as a dedicated Vulkan
image with `DRM_FORMAT_MOD_LINEAR`, `SAMPLED | TRANSFER_SRC`, and the explicit
two-memory-plane layout derived from the V4L2 NV12 definition. Every decoded
frame waits on a producer SYNC_FD, copies both logical planes to a Vulkan
buffer, waits for the fence, and only then requeues the CAPTURE buffer. CPU
mapping occurs only for the first, middle, and last selected frames.

Build:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror \
  -o continuous-nv12-vulkan continuous-nv12-vulkan.c \
  $(pkg-config --cflags --libs vulkan)
```

Prepare a two-minute stream and three software references without re-encoding:

```sh
./prepare-realvideo.sh INPUT.mkv ../../tests/realvideo-001 120
```

Run syntax:

```sh
./continuous-nv12-vulkan realvideo-001.hevc WIDTH HEIGHT FPS_NUM FPS_DEN \
  realvideo-001
```

The active Steam Frame command and all stop gates are specified in
`docs/realvideo-001-plan.md`; running it requires separate approval.

`inspect-annexb.py` counts the exact AUD boundaries without invoking a video
decoder. `compare-selected-frames.py` accepts one or more selected frames,
crops their coded-height NV12 readbacks, deinterleaves UV, and compares the
visible I420 bytes plane by plane.
