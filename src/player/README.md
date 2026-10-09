# Stereonaut player core

`stereonaut-session` is the current integration executable for the MVP media
path. It accepts an MKV or MP4 container, demuxes HEVC Main access units and
audio with FFmpeg, sends video directly to Iris V4L2, and presents the
resulting NV12 DMA-BUFs through the existing Vulkan/OpenXR renderer. Audio is
decoded to 48-kHz S16 stereo and written to the system's ALSA `default` PCM.

Build on Steam Frame:

```sh
./build-stereonaut-session.sh
```

Run from the build directory so the renderer and shader assets are found:

```sh
cd build
./stereonaut-session /path/to/video.mkv \
  --projection 180 --stereo sbs
```

For a bounded diagnostic run, append `--frames N`. The session starts audio
only after Iris has reported the initial source format and the OpenXR renderer
is ready. Container DTS controls decoder feed, while audio playback position
is the presentation clock for decoded video frames. Iris chooses the
compressed OUTPUT-buffer capacity; every access unit is checked against the
actual allocation before it is queued.

The current bounded OUTPUT format requests 2 MiB per compressed access unit,
but Iris may negotiate a larger allocation from the coded dimensions. The
session retains the six-OUTPUT-buffer queue validated by the continuous 8K60
probe. Every access unit is checked against the actual buffer length. A larger
unit stops with an explicit error instead of overrunning the V4L2 buffer.

`file-browser.c` provides the nonblocking browser data source for the XR UI.
One worker resolves and sorts directories, subdirectories and `.mkv`/`.mp4`
files while the render thread only polls completed immutable listings. Newer
requests supersede older results, which keeps slow mounts from replacing a
more recent navigation result.

`media_pipeline_seek` stops and joins both media workers, aborts and rebuilds
their bounded queues, seeks libavformat to the preceding video keyframe,
flushes the audio decoder/output, and then restarts both workers in either the
playing or paused state requested by the controller. The seek probe verifies
that a 10-second request resumes at the preceding 9.539-second keyframe in the
MP4 fixture. V4L2 drain/restart and suppression of decoded frames before the
target remain the session controller's responsibility.

After a seek, the audio path decodes packets from that same preceding
keyframe but discards complete PCM frames before the target and trims the
crossing frame at 48-kHz sample precision. The ALSA clock is rebased to the
first retained sample. The decode-only seek check reaches 11.008167 seconds
after a 10-second request without exposing the pre-target samples.

`stereonaut-browser` is a decoder-free native OpenXR process. It uploads a
CPU-rasterized 1024×768 panel to a Vulkan-backed quad-layer swapchain, locates
both controller aim poses, intersects their rays with the panel and treats a
simple-controller select action as a click. The panel provides nine entries
per page, previous/next paging and an exit button. Selecting directories asks
the asynchronous browser worker for a new listing; selecting a media file
returns its absolute path over a fixed-size local socket message.

`stereonaut-launcher` keeps the user-facing browser/playback loop outside both
OpenXR child processes. It starts in `$HOME`, launches the browser, starts the
container playback session for the selected path, and returns to that file's
directory after playback finishes or reports a controlled error. Run the
current application bundle with `run-stereonaut.sh`.
