# Stereonaut player core

`stereonaut-session` is the current integration executable for the MVP media
path. It accepts an MKV or MP4 container, demuxes HEVC Main access units and
audio with FFmpeg, sends video directly to Iris V4L2, and presents the
resulting NV12 DMA-BUFs through the existing Vulkan/OpenXR renderer. Audio is
decoded to 48-kHz S16 stereo and written to the system's ALSA `default` PCM.

Build on Steam Frame or a Linux PC with an OpenXR runtime:

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
their bounded queues, seeks libavformat to the video keyframe nearest to the
target (from the container index, before or after it; for Jellyfin's HLS the
nearest segment start), reads ahead to the first video packet so
`media_pipeline_start_us` reports where playback really resumes,
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

For startup synchronization, demux may fill its bounded queues while audio
remains paused. Audio is released only when Iris dequeues the first decoded
CAPTURE frame. This prevents decoder/OpenXR setup time from making the audio
clock start hundreds of milliseconds ahead of video.

Compressed video may be queued up to 500 ms ahead of the audio clock. The
twelve CAPTURE buffers provide the actual decoded-frame bound, while this
larger compressed lead keeps Iris fed despite roughly 90 ms of measured 8K
decode latency. Presentation still holds early frames and retains the 100-ms
late-drop threshold.

The ALSA sample-position clock is capped by monotonic time since the first PCM
write, with pause intervals removed. This preserves the backend position when
it is useful while preventing PipeWire's ALSA compatibility layer from making
queued samples appear to have played hundreds of milliseconds early.

The video scheduler never calls ALSA while the audio worker may be blocked in
`snd_pcm_writei`. The worker publishes a clock snapshot after each decoded
audio packet; readers extrapolate that snapshot with monotonic time between
updates. This keeps audio as master without serializing V4L2 progress on audio
device I/O.

The playback renderer adds a head-relative native OpenXR quad panel over the
video layer. The same two-hand aim/select actions used by the browser drive
Back, -10 s, Play/Pause and +10 s. Position, duration and state arrive over
the frame IPC socket. Pause leaves the OpenXR loop and current DMA-BUF frame
alive while stopping audio and bounded decoder feed. A seek resumes at the
keyframe nearest to the target within the running session (see below); the
older re-exec of the session at the new position remains as the fallback.

The playback panel is world-fixed and movable. Pressing the squeeze button
while the aim ray hits the panel grabs it (it follows the controller and turns
towards the head about the vertical axis); squeezing on empty space toggles its
visibility. A seek bar above the buttons is dragged with the trigger and
seeks on release (`NV12_ACTION_SEEK_TO`). The renderer reports the panel pose
to the session (`NV12_IPC_PANEL`), which forwards it through the seek re-exec
via `--panel x,y,z,yaw,visible`.

A seek keeps the renderer alive: the session sends `NV12_IPC_HOLD`, the
renderer retains only the frame on screen and acknowledges with
`NV12_IPC_HELD`, and the re-executed session adopts the renderer through the
inherited socket (`--renderer fd,pid`) and sends a fresh `INIT`. The held frame
stays visible, with the panel and a LOADING marker, until the first frame of
the new stream arrives.

The OpenXR host (`stereonaut-renderer`) is a single long-lived process with a
browser mode and a player mode, so SteamVR never sees the app disappear
between screens. `stereonaut-launcher` starts it once and talks to it over a
SEQPACKET socket (`player-ui-ipc.h`): `BROWSE` shows a location, `PLAY` attaches
a playback session's NV12 socket, and the host answers `FILE_SELECTED` or
`EXIT_REQUESTED`. The separate browser process was removed; its panel logic
lives in `browser-ui.c`. `stereonaut-launcher --play PATH --frames N` runs a
bounded playback without browsing, which is what the hardware tests use.

The OpenXR host also suggests the standardized SteamVR PC controller profiles
(Valve Index, Meta/Oculus Touch, Windows Mixed Reality and HTC Vive/Cosmos).
The ordinary Linux OpenXR loader is selected automatically at build time; the
Frame's bundled loader remains the default on the Frame.

PCVR (`decoded-video.c`, `DECODED_VIDEO_HARDWARE`): without
`/dev/video-dec0` the session decodes every title with FFmpeg's hardware
decoders (CUDA first, then Vulkan Video, then VA-API; `STEREONAUT_HWDEC`
picks one) and downloads each frame, NV12 or for 10 bit P010, into one of
eight memfds. INIT marks them `NV12_BUFFER_HOST`: the renderer imports each
mapping with `VK_EXT_external_memory_host` and copies a new frame into a
device-local image before sampling it (NV12 through the YCbCr sampler, P010
as R16/RG16 planes through the TP10 plane shader). memfds avoid udmabuf's
64 MiB buffer limit (8K P010 frames are 100 MB). Pacing, seeking and the
rest of the session are shared with the dav1d path. On an RTX 4080 Vulkan
Video and NVDEC both decode 8K 10-bit HEVC at about 145 fps, but FFmpeg's
download of Vulkan frames reaches only 60 fps, CUDA 87 fps; hence CUDA
first.

The session seeks without restarting: for Iris it stops both V4L2 queues,
seeks the pipeline, requeues every CAPTURE buffer the renderer does not show
and starts the queues again; the software decoder joins its thread and
flushes dav1d. The renderer gets `NV12_IPC_SEEKING`, keeps the current frame
and shows "LADEN" until the next one. Only if this fails does the session
fall back to the re-exec described above. `--seek-test FRAMES:US` triggers
such a seek in a bounded test run.

Jellyfin titles are addressed as `jf:item:<id>:<folder>`; `jellyfin.c` talks to
the server (Quick Connect, listings, `PlaybackInfo`) and the session plays the
resulting Direct Play or HLS URL.

Experimental AV1 on the Frame (`decoded-video.c`, `DECODED_VIDEO_DAV1D`):
if `av1_software` is set in
`settings.json` (toggle "AV1 bis 4K per CPU" in the view tab), the session
exports `STEREONAUT_AV1`, the demuxer accepts 8-bit AV1 up to 4096×2160 and
the Jellyfin device profile offers it for Direct Play. Such titles skip Iris:
a worker thread decodes with dav1d (`max_frame_delay=8`) and converts each
frame to NV12 in one of eight memfd-backed `udmabuf` DMA-BUFs, which the
renderer imports exactly like Iris CAPTURE buffers. `play_decoded` in the
session reuses the renderer protocol, audio-clock pacing and seek restart of
the Iris loop. For a test run without changing the setting, start the
launcher with `STEREONAUT_AV1=1`.
