# Vulkan/Q10C probe

`vulkan-format-probe.c` performs Vulkan capability queries only. It enumerates
the DRM modifiers and DMA-BUF external-memory properties that Turnip exposes
for the relevant 8-bit and 10-bit 4:2:0 formats. It does not open the V4L2
decoder, import a DMA-BUF, submit GPU work, or render.

Build:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror \
  -o vulkan-format-probe vulkan-format-probe.c \
  $(pkg-config --cflags --libs vulkan)
```

`v4l2-vulkan-q10c-import.c` is the separately approved one-shot integration
probe. It accepts only the known 60-AU `gradient10.hevc` fixture, opens only
`/dev/video-dec0`, rejects any CAPTURE layout other than the measured
1920x1088 Q10C allocation, and exports exactly the first dequeued frame. It
imports that FD with the confirmed explicit layout, performs one two-plane
GPU copy, maps only the uncompressed destination after the fence, then returns
the CAPTURE buffer to V4L2. There are no fallback layouts or parameter variants.

`compare-p010-readback.py` crops the coded padding rows, separates the
interleaved P010 chroma values, verifies the six zero padding bits, and compares
all visible 10-bit Y, U and V samples with the software-decoded fixture.

The constant used to label `DRM_FORMAT_MOD_QCOM_COMPRESSED` is the public DRM
modifier value (`vendor 0x05`, payload `1`). It is used only for labeling
enumerated values, never to assert that Q10C uses this modifier.

`vulkan-layout-probe.c` is a second, still non-streaming probe. It asks Turnip
to create an unbound 1920x1088 10-bit 4:2:0 image from a one-element QCOM
modifier list, then reports Turnip's chosen modifier, memory requirements, and
all modifier memory-plane layouts. It does not allocate memory, import an FD,
or submit work to a queue.

On the Steam Frame, the layout probe found a 6,299,648-byte Turnip requirement
for the only supported 10-bit/QCOM tuple. Iris Q10C is 4,210,688 bytes. The
approved procedure therefore stopped before DMA-BUF import and GPU work. See
[`docs/vulkan-video-investigation.md`](../../docs/vulkan-video-investigation.md)
and the preserved logs under `docs/vulkan-video-diagnostics/run-01/`.

`vulkan-explicit-layout-probe.c` performs the narrower follow-up test. It asks
Turnip to create the same unbound image with
`VkImageDrmFormatModifierExplicitCreateInfoEXT` and the exact two memory-plane
offsets and row pitches reported by Iris for the 1920x1088 Q10C buffer. It
checks the returned modifier and rejects a memory requirement larger than the
4,210,688-byte Iris allocation. Like the other probes, it does not allocate or
bind memory, import a DMA-BUF, or submit GPU work.

On the Steam Frame this explicit probe succeeded. Turnip returned the QCOM
compressed modifier and a 4,210,688-byte memory requirement with 4,096-byte
alignment, exactly matching the Iris allocation size. The preserved output is
in `docs/vulkan-video-diagnostics/run-02/`. A later one-shot test imported and
bound the Q10C DMA-BUF and produced a two-plane GPU readback, but its pixels did
not match the software reference. See
[`docs/q10c-interop-research.md`](../../docs/q10c-interop-research.md).

Build:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror \
  -o vulkan-explicit-layout-probe vulkan-explicit-layout-probe.c \
  $(pkg-config --cflags --libs vulkan)
```

`analyze-q10c-readback.py` performs the follow-up analysis entirely offline.
It decodes all 60 HEVC frames, compares every Y/U/V plane, runs bounded
geometry, row-pitch, value and chroma searches, and creates diagnostic PNGs and
machine-readable CSV/JSON output. It does not contact the Steam Frame or use
V4L2, DMA-BUF, or Vulkan.

```sh
python3 tools/vulkan-q10c-probe/analyze-q10c-readback.py \
  docs/vulkan-video-diagnostics/run-04/p010-readback.bin \
  tests/decoder-fixtures/gradient10.hevc \
  docs/vulkan-video-diagnostics/run-05
```

The result is documented in
[`docs/q10c-readback-offline-analysis.md`](../../docs/q10c-readback-offline-analysis.md).

`v4l2-vulkan-q10c-sync-import.c` is the single approved sync-file follow-up.
Before opening the decoder it verifies that `SYNC_FD` semaphore import is
supported. After Q10C `DQBUF`/`EXPBUF`, it exports the producer snapshot with
`DMA_BUF_SYNC_READ`, imports that FD temporarily into a Vulkan semaphore and
waits on it in the otherwise unchanged transfer submission. The approved run
is preserved under `docs/vulkan-video-diagnostics/run-06/`.

`compare-p010-readbacks.py` compares two readbacks byte-for-byte and as visible
P010 Y/U/V planes and creates a Y-difference PNG. It is an offline-only tool.

`vulkan-q10c-selftest.c` is the decoder-free Vulkan self-test. It first checks
the combined `TRANSFER_DST | TRANSFER_SRC` capability for the P010-like format
and QCOM modifier. It then intends to upload a synthetic P010 frame into an
image with the exact explicit producer layout and copy both logical planes
back for a bit-exact comparison. The first approved run stopped before memory
allocation or GPU work because the executed source incorrectly required the
driver to mandate a dedicated allocation; its artifacts are under
`docs/vulkan-video-diagnostics/run-07/`. The separately approved corrected run
completed the copy roundtrip, but the P010 output was not bit-exact. Its logs,
buffers, comparison, and diagnostic images are under
`docs/vulkan-video-diagnostics/run-08/`.
