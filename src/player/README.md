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
