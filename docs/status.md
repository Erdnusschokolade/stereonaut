# Stereonaut – Project status

As of 2026-10-23, branch `main` (previously `player/mvp` and
`hardware/main10-baseline`)

What the player does, confirmed in the headset, and its known limits.
Measurements and findings are in the other documents under `docs/` (mostly
in German), the architecture in `README.md` and `src/player/README.md`.

## Video path

```text
MKV/MP4 -> libavformat (hevc_/h264_mp4toannexb)
        -> HEVC Main 8 | H.264 8-bit 4:2:0 | HEVC Main10 (own Turnip only)
        -> Iris V4L2 -> NV12/TP10 DMA-BUF -> Vulkan -> OpenXR
Audio:  libavcodec/libswresample -> 48 kHz S16 -> ALSA (master clock)
```

Jellyfin uses Direct Play for these formats and transcodes everything else
to HEVC Main 8 (the server limits the width to 4096 px). Experimentally,
dav1d decodes AV1 up to 4K on the CPU (on by default, see below).

## Confirmed in the headset

- Browser ↔ playback through the persistent OpenXR host, return to the
  folder and page, error messages, start from KDE and the SteamVR library.
- Local files and Jellyfin (Quick Connect, libraries, thumbnails, Direct
  Play and transcoding, 4K to 8K60).
- Pause, ±10 s, seek bar with trickplay preview; grip to move and to show
  or hide the panel.
- Seeking with the stick: up to four quick presses jump at once; from the
  fifth (less than 0.5 s apart), or when held, playback pauses, the panel
  appears and only the seek bar and the trickplay preview move (held, with
  growing steps: 10 s, after 2.5 s 30 s, after 5 s 60 s); 0.5 s after the
  last press the jump follows, and playback resumes if it was playing
  before. The time display stays until playback lands on the target. The
  −10/+10 buttons on the panel behave the same way.
- When seeking with the stick or the buttons, the panel shows a trickplay
  strip (the target large in the middle, two pictures before and after it,
  spaced like the jumps); pointing at the seek bar shows a single picture,
  debounced against the controller's jitter. The player preloads a title's
  tiles in the background after the start.
- Pointer beam in the browser and the player smoothed, as the raw aim pose
  is too jittery (as in the Steam interface; 1€ filter: strong at rest,
  hardly any lag on quick moves; `STEREONAUT_AIM_FILTER=MIN,BETA`, `0` =
  off).
- The settings window (popup tabs and the settings page) is laid out with
  Clay, and the XR host is split into modules; both without a visible
  change in behaviour.
- Seeking without restarting the session, to the keyframe in the seek
  direction (see limits); the last picture stays until the new one arrives.
  Time to the first picture over Jellyfin: H.264 4K mostly 0.15–0.2 s, HEVC
  8K around 0.25 s, HEVC Main10 8K 0.3–0.6 s; previously 1.3 s and more
  (session restart). For Main10, Iris refuses to restart the queues after
  stopping them (`STREAMON` ENOMEM with a kernel warning, occasionally also
  for 8-bit); the session then reopens Iris and keeps the source, the
  Jellyfin connection and the renderer. Restarting the session remains only
  as the last fallback.
- Flat video: video drawn smaller than its size is averaged over several
  samples (4K flat without staircases on sharp edges), the screen's edges
  are smoothed. Renderer GPU load on the Frame: 4K flat 28 %, 8K 180°
  16 %.
- PCVR through SteamVR on a PC, checked in the headset with the Frame
  streaming: playback, seeking and panels. Without Iris, the session on a
  PC decodes H.264, HEVC and AV1 with 8/10 bits using FFmpeg hardware
  decoders (CUDA, Vulkan Video, VA-API); the renderer copies the pictures
  from shared memory into graphics memory. Measured on a PC (RTX 4080):
  up to 8192×4096 at 60 fps without losses, seek 25 ms locally. On the PC the renderer draws the
  panels into the eye images itself (mipmaps, anisotropic); as quad layers,
  text flickered in SteamVR on the PC when moving the head. Text therefore
  looks a little softer on the PC than natively on the Frame, so the eye
  images there are 1.3 times the recommended size (beyond that, the two
  eyes' perspectives no longer match when streaming). The PC also draws the
  pointers and controllers into the eye images (a second projection layer
  stayed black there). Over USB-C playback is as smooth as natively; slight
  stutter over Wi-Fi comes from the streaming (the renderer misses no
  frames). Panel drawing and buffer import run outside the frame loop;
  hovering and seeking no longer drop frames.
- Projection, stereo, eye swap, position, 6DoF and picture settings per
  title.
- H.264 natively, HEVC Main10 natively with the own Turnip
  (`STEREONAUT_TP10`).
- Jellyfin playback reports (start, progress, pause, stop).
- Autofocus with eye tracking; "Focus" tab: trigger only below a
  distance, hysteresis, fading.
- Button bindings: single buttons (stick, A/B/X, d-pad) and combinations
  with the grip ("R grip + stick →") trigger their actions and can be
  rebound.
- Interface in English and German: the language follows the Steam or
  system language automatically (on the Frame `LANG` is fixed to `en_US`,
  so the Steam language from `~/.steam/registry.vdf` counts), English
  without a translation; switching under Settings → Language takes effect
  at once.
- Orientation: Stereonaut aligns the browser, the player panel and the
  video with the head at the start (also when the headset pointed at the
  ceiling on waking) and keeps that across titles; "Centre" in the player
  and SteamVR's recenter set it anew.
- Resume dialog, progress bars and "watched" on the tiles, "Continue
  watching" on the start page.
- Volume, "At the end: stop/repeat/next", automatic hiding of the panel,
  pause is kept when seeking.
- Chapter marks and the "Info" tab.
- Format detection without the file name (Jellyfin trickplay or the first
  decoded pictures).
- Fisheye projection (equidistant model).
- Filters by genre/studio/cast, search with the on-screen keyboard, sorting
  of local folders, thumbnails of local files.
- Faster opening of libraries (list without stream info, codec and
  resolution are loaded page by page; VR library 0.25 s instead of 2 s).
- Experimental: AV1 8-bit up to 4096×2160 in software (dav1d), switch "AV1
  up to 4K on the CPU" under Settings → Playback (on by default, applies
  from the next title); Jellyfin then plays such titles directly instead of
  transcoding them. Up to 60 fps without losses, see
  `docs/hardware-investigation.md`.
- Passthrough: where there is no video (browser background, around the flat
  screen, outside 180° or fisheye), the room shows through, provided
  SteamVR offers `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` (Frame since the
  SteamVR update of 2026-10-10). Switch for all titles under Settings →
  Playback and as a button on the control card; dimming of the room is
  adjustable.
- Chroma key per title in the "Passthrough" tab: green, blue or a colour
  picked in the video with the trigger (threshold, softness, colour
  spill).
- Controller models from SteamVR (`XR_EXT_render_model`) with moving
  buttons and sticks, and a pointer beam to the panel or the video.
- Settings page over the browser ("Settings" in the bar) with seek
  preview, panel hiding, AV1, passthrough, autofocus details, button
  bindings and language; the popup in the player keeps what concerns the
  title.

## Known limits

- HEVC Main10 only with the own Turnip (`tools/mesa-tp10`); without it,
  Jellyfin transcodes and local Main10 files are rejected.
- A seek lands on the keyframe nearest to the target that still covers at
  least half the distance in the seek direction; not accurate to the
  second. With keyframes 10 s apart, ±10 s jumps by 10 s, with longer
  intervals further. A gapless A-B loop would additionally need a bitstream
  splice with a continuous timeline.
- PCVR is checked only with the Frame streaming; other PC headsets are
  untested.
- The video decoder (Iris) shares its capacity among all programs using
  it. While another program holds it (a video playing in Steam is
  enough), a title may not start; the browser then says that another
  program is using the decoder, and the log names the processes
  (`decoder_busy`).
- The logs show Jellyfin URLs without the server and query (`<server>`,
  `?…`); the API key no longer appears there, older logs still contain it.
- Occasional stalls with one HLS transcoding title are unexplained.
