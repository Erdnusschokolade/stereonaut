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
alive while stopping audio and bounded decoder feed. A seek resumes at a
keyframe within the running session (see below); the older re-exec of the
session at the new position remains as the last fallback.

The playback panel is world-fixed and movable. Pressing the squeeze button
while the aim ray hits the panel grabs it (it follows the controller and turns
towards the head about the vertical axis); squeezing on empty space toggles its
visibility. A seek bar above the buttons is dragged with the trigger and
seeks on release (`NV12_ACTION_SEEK_TO`). The renderer reports the panel pose
to the session (`NV12_IPC_PANEL`), which forwards it through a decoder
reopen or the seek re-exec (`--panel x,y,z,yaw,visible`).

The seek bindings (stick left/right by default) and the panel's -10/+10
buttons send absolute
`NV12_ACTION_SEEK_TO` targets that continue from a pending one. Up to the
fourth press less than 0.5 s after the previous one each press seeks at
once; from the fifth, or once a held input repeats, the renderer pauses
playback (`NV12_ACTION_TOGGLE_PAUSE` with `player_state` set: pause, not
toggle), shows the panel and moves only the seek bar with the trickplay
preview, in steps that grow while held (10 s, 30 s after 2.5 s, 60 s after
5 s). Once the input rests for 0.5 s it sends the seek with `player_state`
`PLAYING` if playback ran before, which the session takes over as the start
state after the seek. Each `SEEK_TO` carries a number in `frame_index` that
the session repeats in `NV12_IPC_HOLD`/`NV12_IPC_SEEKING`; until the frame
of the last number arrives the panel shows its target, so pictures of
earlier seeks do not make the time jump back. The session keeps pause and
seek in separate slots, so a pause cannot replace a seek queued in the same
batch of messages, and a seek that arrives before the renderer has the new
decoder's buffers waits for them.

Seeks by binding or button show a trickplay strip (`trickplay.c`): the
target large in the middle, two pictures before and two after it, as far
apart as the seek steps. Hovering or dragging the seek bar shows only the
middle picture, since the CPU-drawn panel is redrawn for every new position
(7 ms without the preview, about 13 ms with the strip). There the position
follows the pointer only after 5 panel pixels and preview changes redraw at
most 30 times a second, which the hand's jitter otherwise turned into a
redraw per frame. The trickplay worker fetches every tile of the title in
the background a few seconds after the start (about 30 MB of JPEG for
85 minutes at 5 s intervals), keeps three decoded tiles (20 MB each for
3200x1600) and the last sixteen pictures, and hands a strip over whole
unless it takes longer than 20 ms; straightened 180/360 degree previews use
a mapping computed once per size and field of view. A strip then takes
about 3 ms, 7 ms straightened, on the Frame.

When a seek sets up a new decoder the renderer stays alive: the session
sends `NV12_IPC_HOLD`, the renderer retains only the frame on screen and
acknowledges with `NV12_IPC_HELD`, and the session sends a fresh `INIT` with
the new buffers, either from the same process (decoder reopen) or after a
re-exec that adopts the renderer through the inherited socket
(`--renderer fd,pid`). The held frame stays visible, with the panel and a
LOADING marker, until the first frame of the new stream arrives.

On the Frame both panels are OpenXR quad layers. On a PC the renderer draws
them into the eye images itself, from a mipmapped image with trilinear and
anisotropic filtering: SteamVR on a PC composites quad layers so that text
shimmers and steps while the head moves, even with mipmaps.
`STEREONAUT_UI_LAYER=quad|eye` overrides the choice. Because the panel
images have about twice the pixels per degree of SteamVR's recommended eye
image, a PC renders the eye images 1.3 times larger
(`STEREONAUT_RENDER_SCALE`, 1 to 2); above 1.3, streaming to the Frame shows
the two eyes with mismatching perspectives. The panels are drawn in a
thread of their own, and the host buffers of a new stream (about 30 ms each
for 8K P010) are imported by a worker, so neither stalls the frame loop.

The video shaders average up to 4x4 bilinear taps over each pixel's
footprint when the video is drawn smaller than it is (a 4K flat screen gets
about a quarter of its pixels in the Frame's eye image), and fade the flat
screen's outermost pixel into the background. 8K 180° video stays at one
tap.

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
and shows "LADEN" until the next one. Iris refuses the second `STREAMON`
for HEVC Main10 every time and for 8K now and then (ENOMEM with a kernel
warning). Main10 therefore never seeks in place, and after such a refusal
the title stops trying: the session closes Iris, seeks the still open
source and sets Iris up as at the start, which a freshly opened device
accepts (8K Main10 over Jellyfin: 0.3 to 0.6 s instead of 1.3 s for a
re-exec, most of which went into opening the source again). Only if seeking
the source fails does the session fall back to the re-exec described above.
A stop from the launcher (SIGTERM) reaches the re-executed process: the
session blocks the signal across `execv`, and the new process unblocks it
once its handler is installed.
Seeks that set up a new decoder log each phase (`seek_restart_phase`).
`--seek-test FRAMES:US` triggers such a seek in a bounded test run.

The demuxer picks the keyframe nearest to the target but requires it to
cover at least half the way in the seek's direction; otherwise it takes the
first keyframe beyond the halfway point (reading forward if the index does
not reach it yet), so a short skip over a long GOP neither stays put nor
moves backwards. The keyframe is searched forwards: MP4 index entries carry
decoding times, and a backward search for one of them landed a keyframe
early. The position the seek starts from travels through the pipeline and,
for a re-exec, `--seek-from-us`.

The host keeps a home orientation: the head pose of the first frame (its
heading also when the headset wakes up facing the ceiling or the floor),
renewed by "Centre" in the player and by SteamVR's recenter
(`XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING` for the local space).
The browser stands 2.4 m in front of it at eye height, the player panel
starts below the gaze, and every title starts turned towards it, so a
centring holds across titles. The log shows it as `home x=… yaw=…`.

The interface speaks English and German (`i18n.c`). As with gettext the
English text in the code is the key: `tr("Settings")` returns the text of
the language in effect, strings in static tables are marked with `N_()`.
The language is `language` in `settings.json`: `auto` (the default) or a
code chosen on the settings page. `auto` takes the first of
`STEREONAUT_LANGUAGE`, `LANGUAGE`, `LC_ALL`, `LC_MESSAGES`, the Steam
client language (`~/.steam/registry.vdf`; on SteamOS `LANG` stays `en_US`
whatever the user picked) and `LANG` that names a language, and falls back
to English where no translation exists. Changing it redraws both panels and
reads the current listing again. A translation is one `i18n-CODE.c` with a
`struct i18n_language` (pairs of English and translated text, decimal
point, optional keyboard layout for the search) added to `i18n_languages`
in `i18n.c`; the build scripts pick up every `i18n*.c`. Missing entries
show in English. `python3 tools/i18n/check-translations.py` lists missing
and unused entries and translations whose printf conversions differ;
`--list CODE` prints the missing ones as entries to fill in. The launcher
sends its messages in English and the host translates them.

The panels are drawn on the CPU by `ui-draw.c`. Views being moved to a
declarative layout describe their boxes with Clay
(`third_party/clay/clay.h`, v0.14, zlib licence) through `ui-layout.c`:
the result holds the draw commands and the boxes of the controls, so
drawing and hit testing use the same layout. So far the updates tab of the
settings uses it; the other tabs still place their widgets by hand.

Updates (settings page, tab "Updates"): `updater.c` runs
`stereonaut-update.sh` from the install directory as a child process and
reads its lines without blocking. `check` follows GitHub's redirect of
`/releases/latest` to the tag (no API, no JSON); `install TAG` downloads
the package (on the Frame `stereonaut-steamos-ARCH.tar.gz` with binaries,
on a PC `stereonaut-linux-source.tar.gz`, which its `install.sh` builds)
and its `.sig`, verifies the signature with `ssh-keygen -Y verify` against
`release-signers` (namespace `stereonaut-release`), checks that the
package's MANIFEST holds the requested version (a signed older package
cannot pass for a newer tag), and runs its `install.sh`. The host then
asks the launcher for a restart (`PLAYER_UI_RESTART_REQUESTED`); the
launcher ends the host and executes the start script again in the same
process, which SteamVR keeps as the running app. The installed version is
the MANIFEST of the release package; a build from the sources has none.
Without `release-signers` updates are off.
`STEREONAUT_RELEASES` (a base URL) and `STEREONAUT_RELEASE_SIGNERS` replace
the release page and the keys for tests.

The start script (`stereonaut-start.sh`, installed as `bin/stereonaut` next
to `lib/stereonaut` with the programs in `~/.local`; it tells the launcher
its path in `STEREONAUT_START_SCRIPT`) uses
the update script when the player cannot run after a system update: when
libraries are missing and there are no sources, the rebuild fails or
libraries are still missing after it, and when the XR host fails (exits
with an error or a signal; the launcher then executes the start script
again with `STEREONAUT_HOST_FAILED`). It offers a release newer than the
installed one in a dialog, installs it and starts again; without one it
reports the error. Its dialogs
follow the interface language, which `stereonaut-launcher --language`
prints (the launcher only needs the C library).

Jellyfin titles are addressed as `jf:item:<id>:<folder>`; `jellyfin.c` talks to
the server (Quick Connect, listings, `PlaybackInfo`) and the session plays the
resulting Direct Play or HLS URL.

Experimental AV1 on the Frame (`decoded-video.c`, `DECODED_VIDEO_DAV1D`):
unless `av1_software` is off in `settings.json` (toggle "AV1 up to 4K on
the CPU" on the settings page, on by default), the session
exports `STEREONAUT_AV1`, the demuxer accepts 8-bit AV1 up to 4096×2160 and
the Jellyfin device profile offers it for Direct Play. Such titles skip Iris:
a worker thread decodes with dav1d (`max_frame_delay=8`) and converts each
frame to NV12 in one of eight memfd-backed `udmabuf` DMA-BUFs, which the
renderer imports exactly like Iris CAPTURE buffers. `play_decoded` in the
session reuses the renderer protocol, audio-clock pacing and seek re-exec of
the Iris loop. With the setting off, `STEREONAUT_AV1=1` on the launcher
still enables it for a test run.
