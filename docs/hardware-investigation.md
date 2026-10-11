# Stereonaut – Hardwareanalyse 001

Stand: 2026-10-08. Zielgerät: über den vorhandenen SSH-Alias `frame` untersuchtes Steam Frame. Projekt: `stereonaut`.

## Ergebnis und Beweisniveau

**Ein nativer ARM64-Vulkan-/OpenXR-Pfad ist vorhanden. Ein durchgängiger 8K-HEVC-Main10-Hardwarepfad ist noch nicht nachgewiesen.** Der wichtigste offene Punkt ist die 10-Bit-Ausgabe des installierten Iris-Decoders: In seiner initialen H.264-Konfiguration meldet er nur eindeutig 8-Bit-fähige beziehungsweise nicht näher identifizierte Ausgabeformate. Das ist ein konkretes Integrationsrisiko, aber noch kein Beweis, dass HEVC Main10 grundsätzlich unmöglich ist.

Diese Analyse unterscheidet:

| Stufe | Bedeutung |
| --- | --- |
| Gerätebefund | Auf diesem Gerät mit lesenden Befehlen abgefragt; bei Capabilities zunächst eine Treiberaussage |
| Funktionsnachweis | Eine Operation erfolgreich ausgeführt; hier insbesondere Vulkan-Inventur und OpenXR-Extension-Enumeration |
| Herstellerangabe | Öffentlich dokumentierte SoC-/Produktfähigkeit, keine Bestätigung für diesen Linux-Stack |
| Schlussfolgerung / offen | Aus Befunden abgeleitet beziehungsweise noch zu prüfen |

**Es wurde kein Video decodiert, kein DMA-BUF exportiert/importiert und keine OpenXR-Session gestartet.** Deshalb sind Echtzeitfähigkeit, Main10-Decodierung, Bildqualität, Zero-Copy und Energiebedarf nicht als funktionierend bestätigt. Die lesende Untersuchung ist abgeschlossen; diese Laufzeitnachweise gehören in den anschließenden Prototypenauftrag.

## Zugriff und Sicherheitsgrenzen

Alle Zielverbindungen verwendeten ausschließlich `ssh frame`, ohne `BatchMode`, ohne Agent-Forwarding und ohne neue Schlüssel oder SSH-Konfiguration. Es gab keine Paketinstallation, keinen Remount, keine Firmware-/Treiberänderung, kein `sudo` auf dem Ziel und keine dort angelegten Diagnoseprogramme oder Ergebnisdateien. Python-Abfragen liefen aus stdin im Speicher. GStreamer wurde mit `--gst-disable-registry-update` aufgerufen, damit seine Registry nicht aktualisiert wird. Bibliotheksinterne Seiteneffekte wie Laufzeitlogging lassen sich mit einer API-Abfrage nicht grundsätzlich ausschließen; es wurden keine solchen Schreibaktionen angefordert.

Anfangs scheiterte SSH innerhalb der lokalen Sandbox an der Eigentümer-/Berechtigungsprüfung einer systemweiten SSH-Konfigurationsdatei. Außerhalb der Sandbox erschien eine Schlüssel-Passphrase-Abfrage. Diese wurde zugelassen und anschließend abgebrochen, ohne eine Passphrase entgegenzunehmen.

Der Nutzer autorisierte daraufhin ausdrücklich die Verwendung des zuvor ermittelten Agent-Sockets. Für diese Untersuchung wurde deshalb pro Aufruf verwendet:

```sh
SSH_AUTH_SOCK=/run/user/1000/ssh-agent.socket ssh frame '…lesende Diagnose…'
```

Zum Untersuchungszeitpunkt war dies eine nachträglich genehmigte Ausnahme zur ursprünglichen Vorgabe, `SSH_AUTH_SOCK` unverändert zu lassen; keine globale Konfiguration wurde geändert. **Nachtrag:** Der Nutzer hat diese prozessbezogene Socket-Ausnahme inzwischen dauerhaft für das Projekt einschließlich späterer Aufträge und Deployment-Skripte freigegeben; siehe [Projektregeln](../AGENTS.md). Weiter gelten der Alias `frame`, keine neuen Schlüssel, keine erzwungene alternative Authentifizierung und kein Agent-Forwarding. Verbindungsprobleme dokumentieren. Keine privaten Zugangsdaten sind Teil dieser Dokumentation.

## 1. Hardware und System

| Merkmal | Gerätebefund |
| --- | --- |
| Betriebssystem | SteamOS `0.3.0`, `VARIANT_ID=vr`, Codename `holo` |
| Build | `20260922.6101926` |
| Kernel | `6.18.0-gfbdbca41fd45`, SMP PREEMPT, Build 2026-09-16 |
| Architektur | `aarch64`, Little Endian, 64 Bit |
| Device Tree | Modell `SM8650 MP rev1 4slam 2et`; kompatibel mit `qcom,sm8650-dv1`, `qcom,sm8650` |
| CPU | 8 Kerne: 2 Cortex-A520, 5 Cortex-A720, 1 in `lscpu` unbenannter Kern |
| Gemeldete CPU-Maxima | A520: 2265,6 MHz; A720: 3148,8 MHz; unbenannter Kern: 3052,8 MHz; keine Dauertaktmessung |
| RAM | `/proc/meminfo`: `MemTotal: 16037980 kB` (~15,30 GiB nutzbar); kein Bandbreitenbenchmark |
| GPU | `Turnip Adreno (TM) 750`, integrierte GPU, Vendor `0x5143` |
| Vulkan-Treiber | `DRIVER_ID_MESA_TURNIP`, Mesa `26.3.0-devel (git-8aa73b4b19)` |
| DRM | `/dev/dri/card0`, `/dev/dri/renderD128`; zugehöriger Plattformtreiber-Link `msm_dpu` |
| Video-Treiber | Modul `iris`, Beschreibung Qualcomm Iris video driver; Device-Treiber `qcom-iris`; V4L2-Name `iris_driver` |

Die SoC-Identifikation stimmt mit Valves [Steam-Frame-Spezifikation](https://store.steampowered.com/hardware/steamframe) überein: Snapdragon 8 Gen 3, ARM64 und nominell 16 GB Unified LPDDR5X. Den unbenannten CPU-Kern nicht als lokal identifizierten Cortex-X4 ausgeben. Ebenso bezeichnet der DRM-Sysfs-Link den Display-Plattformtreiber; er ersetzt nicht die Vulkan-GPU-Identifikation.

Installierte relevante Pakete:

| Paket | Version |
| --- | --- |
| `deckard-mesa-linux-aarch64` | `26.3.0_devel+git8aa73b4b-1` |
| `deckard-steamvr-rel` | `r25358740+28b72a4f-1` |
| `deckard-steamvr-session` | `1.6.1-8` |
| `linux-618-deckard` | `6.18.0+gfbdbca41fd45-1` |
| `linux-firmware-deckard` | `20260801.1+gita0b66dde-1` |
| `ffmpeg` / `ffmpeg4.4` | `2:7.0-1.1` / `4.4.4-5` |
| GStreamer, Base, Good, Bad-Libs | `1.24.2-2` |
| `libdrm` | `2.4.134-1` |
| `v4l-utils` | `1.33.0-1` |
| Vulkan-Loader / Tools | `1.4.309.0-4` |

`file` bestätigt AArch64 für `/usr/bin/ffmpeg`, `/usr/bin/gst-inspect-1.0`, das V4L2-GStreamer-Plugin und SteamVRs `vrclient.so`. Die untersuchten Multimedia-Binaries sind somit keine x86-Programme unter Emulation. Parallel installierte x86-/Android-Mesa-Pakete belegen keine entsprechende native Decoder-API.

### Geräte und Berechtigungen

- `/dev/video0` bis `/dev/video21`: Qualcomm Camera Subsystem; `/dev/media0` gehört zu `qcom-camss`, nicht zum Iris-Decoder.
- `/dev/video-dec0 -> /dev/video22`: Iris-Decoder.
- `/dev/video-enc0 -> /dev/video23`: Encoder; keine Encoding-Tests.
- `/dev/video99`: SteamVR-V4L2-Loopback, kein Nachweis eines Hardwaredecoders.
- SSH-Benutzer gehört zu `video` und `render`; Decoder-Inventur gelingt ohne erhöhte Zielrechte.
- `/dev/dma_heap/system` existiert, ist aber `0600 root:root`. Keine Umgehung. Eigene Allokationen über diesen Heap stehen dem SSH-Benutzer damit nicht unmittelbar offen. Decoder-eigene Buffer mit DMA-BUF-Export sind ein separater, noch zu testender Weg.
- Kernel meldet `CONFIG_VIDEO_QCOM_IRIS=m`, `CONFIG_MEDIA_CONTROLLER=y`, `CONFIG_SYNC_FILE=y`, `CONFIG_DMABUF_HEAPS=y`, `CONFIG_DMABUF_HEAPS_SYSTEM=y`. Das Iris-Modul importiert den Namespace `DMA_BUF` und hängt von `videobuf2_dma_contig` ab. Das belegt Infrastruktur, noch keinen erfolgreichen Buffer-Austausch.

## 2. Vulkan

Die Vulkan-Inventur funktioniert über SSH ohne gesetztes `DISPLAY`; Surface-Informationen werden dabei übersprungen. Keine Aussage über eine erfolgreich präsentierte Desktop-/XR-Swapchain.

| Eigenschaft | Gerätebefund |
| --- | --- |
| Vulkan Instance Version | `1.4.309` |
| GPU API Version | `1.4.359` |
| Driver Version | `26.2.99`; aussagekräftige Buildkennung siehe Mesa-String oben |
| Conformance Version | `1.4.0.0` |
| `maxImageDimension2D` | `16384`; Texturlimit, kein Decodierlimit |
| Queue-Familien | Graphics/Compute/Transfer/Sparse sowie separate Sparse-Familie; keine Video-Decode-Queue gemeldet |
| `samplerYcbcrConversion` | `true` |
| `timelineSemaphore` / `synchronization2` | jeweils `true` |

Für den Video-Transfer relevante gemeldete Device-Extensions:

```text
VK_EXT_external_memory_dma_buf
VK_EXT_image_drm_format_modifier
VK_EXT_physical_device_drm
VK_KHR_external_memory
VK_KHR_external_memory_fd
VK_KHR_external_fence
VK_KHR_external_fence_fd
VK_KHR_external_semaphore
VK_KHR_external_semaphore_fd
VK_KHR_sampler_ycbcr_conversion
VK_KHR_synchronization2
VK_KHR_timeline_semaphore
```

**Keine `VK_KHR_video_*`-Extensions in der vollständigen Enumeration.** Der installierte Turnip-Stack bietet damit keinen nachgewiesenen Vulkan-Video-Decoder für H.264, HEVC oder AV1. FFmpegs `vulkan`-Eintrag und GStreamers `vulkanh265dec` ändern diesen Gerätebefund nicht.

`vulkaninfo --show-formats` meldet für `G8_B8R8_2PLANE_420_UNORM` und `G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16` Sampling, lineare Filterung, Transfer und YCbCr-Filterfunktionen sowohl für lineares als auch optimales Tiling. Damit ist die GPU-Seite für NV12-/P010-artige Bilddaten grundsätzlich plausibel. **Die Abfrage validiert nicht den Import eines konkreten Decoder-DMA-BUFs oder dessen DRM-Modifier.**

Vollständige Inventur: [Vulkan-Rohbefund](diagnostics/2026-10-08-vulkaninfo.txt); [YUV-Format-Auszug](diagnostics/2026-10-08-vulkan-yuv-formats.txt).

## 3. OpenXR

`~/.config/openxr/1/active_runtime.json` verweist auf `/opt/steamvr/steamxr_linuxarm64.json`. Dieses Manifest nennt `SteamVR` und `bin/linuxarm64/vrclient.so`. Es wurde kein `XR_RUNTIME_JSON`-Override in der SSH-Umgebung gefunden.

`xrinfo` und `openxr-info` sind nicht im PATH; vorhanden sind OpenXR-Header, `/opt/steamvr/bin/linuxarm64/libopenxr_loader.so` und `helloxr`. `helloxr` wurde nicht gestartet, weil das über die reine Inventur hinaus eine Render-/XR-Sitzung beginnen würde.

Mit Python `ctypes` und dem vorhandenen Loader wurde ausschließlich `xrEnumerateInstanceExtensionProperties` aufgerufen. Beide Aufrufe lieferten `XR_SUCCESS` (0), insgesamt **48 Extensions**. Dies bestätigt die Enumeration über den Loader, nicht Session-Erstellung, Tracking, Swapchain-Formate oder nutzbare Bildwiederholraten.

```text
XR_KHR_vulkan_enable 10
XR_KHR_vulkan_enable2 4
XR_KHR_opengl_enable 12
XR_KHR_convert_timespec_time 1
XR_KHR_opengl_es_enable 10
XR_MNDX_egl_enable 2
XR_KHR_binding_modification 1
XR_KHR_composition_layer_depth 6
XR_KHR_locate_spaces 1
XR_KHR_visibility_mask 2
XR_EXT_active_action_set_priority 1
XR_EXT_dpad_binding 1
XR_EXT_eye_gaze_interaction 2
XR_EXT_frame_composition_report 4
XR_EXT_hand_interaction 2
XR_EXT_hand_tracking 4
XR_EXT_hand_tracking_data_source 1
XR_EXT_hand_joints_motion_range 1
XR_EXT_hp_mixed_reality_controller 1
XR_EXT_local_floor 1
XR_EXT_palm_pose 3
XR_EXT_uuid 1
XR_EXT_user_presence 1
XR_EXT_view_configuration_views_change 1
XR_EXT_render_model 1
XR_EXT_interaction_render_model 1
XR_EXT_interaction_profile_battery_state_display 1
XR_FB_display_refresh_rate 1
XR_HTC_vive_cosmos_controller_interaction 1
XR_HTC_vive_focus3_controller_interaction 2
XR_HTC_vive_wrist_tracker_interaction 1
XR_MND_headless 3
XR_VALVE_analog_threshold 2
XR_META_performance_metrics 2
XR_VALVE_timing_utils 1
XR_META_recommended_layer_resolution 1
XR_FB_swapchain_update_state 3
XR_FB_foveation 1
XR_FB_foveation_vulkan 1
XR_FB_foveation_configuration 1
XR_META_foveation_eye_tracked 1
XR_META_vulkan_swapchain_create_info 1
XR_FB_space_warp 2
XR_EXT_frame_synthesis 1
XR_HTCX_vive_tracker_interaction 3
XR_KHR_generic_controller 1
XR_VALVE_frame_controller_interaction 1
XR_EXT_debug_utils 5
```

Die Zahlen sind Extension-Revisionen. `XR_KHR_vulkan_enable2` ist der bevorzugte Kandidat für die native Vulkan-Anbindung. Geräteauswahl und Vulkan-Erstellung müssen sich nach den Runtime-Anforderungen richten; siehe [Khronos-Spezifikation](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable2.html). Die vorhandene Refresh-Rate-Extension belegt keine bestimmte Bildrate. Eye Tracking/Foveation reduziert außerdem nicht automatisch die notwendige vollständige Decodierung eines üblichen HEVC-Videostreams.

## 4. Decoder und Codec-Grenzen

### V4L2 M2M und Request API

`/dev/video22` meldet `Video Memory-to-Memory Multiplanar`, `Streaming` und `Extended Pix Format` (`Device Caps 0x04204000`). OUTPUT bedeutet beim Decoder komprimierte Eingabe, CAPTURE decodierte Ausgabe.

| Queue | Enumerierte FourCCs |
| --- | --- |
| OUTPUT | `H264`, `HEVC`, `VP90` |
| CAPTURE, initiale H.264-Konfiguration | `Q08C`, `NV12`, `NV21`, `AB24`, `QC24` |

`Q08C` wird als QCOM Compressed 8-bit Format beschrieben; das ist ein komprimiertes Bildlayout, kein zusätzlicher Videobitstream-Codec. `AB24` wird als RGBA 8-8-8-8 beschrieben. `QC24` heißt lediglich `colorformat`; dessen Layout und Bit-Tiefe bleiben offen. Es wurden weder P010 noch ein eindeutig benanntes 10-Bit-Qualcomm-Format gemeldet.

**Schlussfolgerung:** Der sichtbare Iris-Pfad ist ein stateful V4L2-M2M-Kandidat. Vollständige H.264-/HEVC-Bitstreamformate und GStreamers registrierter stateful Decoder sprechen dafür. Ein stateless Request-Pfad ist nicht nachgewiesen: keine HEVC-Slice-Eingabe, kein decoderzugehöriges `/dev/media*`, kein registriertes `v4l2slh265dec`. Das installierte `v4l2codecs`-Plugin meldet `0 features`.

M2M und Request API sind keine Gegensätze: stateless M2M-Decoder verwenden die Request API für bildbezogene Steuerdaten. Ein Nachweis würde passende Slice-/Frame-Formate, Codec-Controls und `V4L2_BUF_CAP_SUPPORTS_REQUESTS` der OUTPUT-Queue erfordern. Es wurde bewusst kein `REQBUFS`/`CREATE_BUFS` ausgeführt; daher ist das Request-Capability-Bit nicht direkt geprüft. `CONFIG_MEDIA_CONTROLLER=y` und eine Kamera-Media-Node reichen nicht aus. Referenzen: [stateful Decoder-API](https://cdn.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-decoder.html), [Request API](https://cdn.kernel.org/doc/html/latest/userspace-api/media/mediactl/request-api.html), [stateless M2M](https://www.kernel.org/doc/html/v5.5/media/uapi/v4l/dev-stateless-decoder.html).

Die initiale Control-Liste ist H.264-lastig und enthält auch Encoder-artige Controls. Sie ist keine verlässliche HEVC-Profilmatrix. Ohne Umkonfiguration und Stream-Header-Verarbeitung bleibt offen, ob weitere CAPTURE-Formate oder Controls erscheinen. `--list-formats all` lieferte nur die Überschrift, keine zusätzliche Liste; daraus wird kein Supportausschluss abgeleitet.

### Codec-Matrix

| Codec | Hardware-/Herstellerseite | Installierter Treiber / Userspace | Funktion, Auflösung, Bildrate, Bit-Tiefe |
| --- | --- | --- | --- |
| H.264 | Keine gesonderte AVC-Grenze aus den hier verwendeten Herstellerquellen abgeleitet | `H264`; Profile Baseline, Constrained Baseline, Main, High, Constrained High; FFmpeg/GStreamer M2M vorhanden | Kein Decode-Test. Initiale Standardausgabe NV12/8 Bit. Keine High-10-Bestätigung |
| HEVC Main | Qualcomm nennt Hardware-H.265-Decodierung | `HEVC`; `hevc_v4l2m2m` und `v4l2h265dec` vorhanden | Profilakzeptanz nicht getestet; 8-Bit-Ausgabekandidaten vorhanden |
| HEVC Main10 | H.265 und HDR-Playback laut Qualcomm; keine hier belegte exakte Main10-Auflösungs-/FPS-Kombination | HEVC-Eingabe vorhanden, aber initial kein eindeutiges 10-Bit-CAPTURE-Format; GStreamer-Template nur NV12/NV21/RGBA | **Offen und Hauptblocker für die Architekturfreigabe**; Main10-Eingabe und Erhalt der 10 Bit separat testen |
| AV1 | Qualcomm nennt explizit Hardware-AV1-Decodierung | Kein AV1-FourCC auf Iris; kein V4L2-AV1-Decoder registriert; Softwaredecoder in FFmpeg vorhanden | Kein Hardwarepfad auf dem untersuchten Stack nachgewiesen; Software-8K-Leistung nicht gemessen |

Quelle für SoC-Angaben: [Qualcomm Snapdragon 8 Gen 3 Product Brief](https://docs.qualcomm.com/bundle/publicresource/87-71408-1_REV_D_Snapdragon_8_gen_3_Mobile_Platform_Product_Brief.pdf). Dessen Angaben zu 8K-Aufnahme beziehungsweise externen Displays sind **keine** Decoder-Durchsatzgarantie. Der Inhalt des abgerufenen PDFs trägt Rev G, obwohl sein URL-Dateiname REV_D enthält.

### AV1-Nachprüfung (2026-10-10)

Nur lesende Abfragen auf dem Frame, kein Decode-Versuch:

- `/dev/video-dec0` meldet weiterhin nur `H264`, `HEVC` und `VP90` als
  OUTPUT-Formate.
- Das Modul `iris.ko` (`kernel/drivers/media/platform/qcom/vcodec/iris`,
  Kernel `6.18.0-gfbdbca41fd45`, Aliase `qcom,sm8550-iris` und
  `qcom,sm8650-iris`) enthält Symbole für HEVC, VP9 und H.264, aber keine
  AV1-Strings.
- Die Firmware `/lib/firmware/qcom/vpu/vpu33_4v.mbn` enthält dagegen
  AV1-Decoderlogik (43 Strings, u. a. `HFI_PROP_AV1_FILM_GRAIN_PRESENT`,
  `HFI_PROP_AV1_OP_POINT`, `HFI_PROP_AV1_DRAP_CONFIG`, „AV1 ERROR code").

Upstream hat Qualcomm die Serie „Enable support for AV1 stateful decoder"
(v1 2025-10-01, v10 2025-12-10,
<https://lkml.iu.edu/hypermail/linux/kernel/2512.1/03167.html>) für den
Iris-Treiber eingereicht: neues `V4L2_PIX_FMT_AV1` (stateful, getrennt von
`V4L2_PIX_FMT_AV1_FRAME`), Gen1-/Gen2-Plattformdaten; getestet nur auf SM8550,
Fluster 135/242 über GStreamer (v7). Laut Phoronix
(<https://www.phoronix.com/news/Linux-7.0-Media>) ist AV1 für Iris in Linux 7.0
aufgenommen; ein konkreter Commit wurde nicht geprüft.

Bewertung: Die Hardware/Firmware kann AV1 wahrscheinlich decodieren, der
ausgelieferte SteamOS-Kernel stellt es nicht bereit. Nutzbar erst mit einem
Kernel ≥ 7.0 bzw. mit zurückportierter Serie (Kernel-Eingriff, nur nach
Rücksprache gemäß `AGENTS.md`); ob es auf SM8650 mit `vpu33_4v.mbn`
fehlerfrei läuft, ist offen. Bis dahin bleibt AV1 auf die
Jellyfin-Transkodierung nach HEVC angewiesen. Für die Session wäre der
Aufwand später klein (OUTPUT-FourCC und Demuxer-Freigabe, kein
Bitstreamfilter nötig).

### AV1 per Software mit dav1d (2026-10-11)

Durchsatz von dav1d 1.4.1 (FFmpeg n7.0, `libdav1d`, Threads automatisch) auf
dem Frame, gemessen mit `ffmpeg -benchmark -c:v libdav1d … -f null -`. Die
Clips sind je 30 s aus dem 8K-Referenzausschnitt (`realvideo-001`, 59,94
fps), am PC zu AV1 8 Bit kodiert:

| Clip | Bitrate | Decodiert | Echtzeitfaktor |
| --- | --- | --- | --- |
| 3840×1920, NVENC | 25,7 Mbit/s | 77 fps (110 fps mit `max_frame_delay=8`) | 1,29× (1,83×) |
| 3840×1920, SVT-AV1 | 19,8 Mbit/s | 139 fps | 2,31× |
| 8192×4096, NVENC | 63,5 Mbit/s | 49 fps, alle 8 Kerne voll | 0,81× |

Mit Echtzeit-Tempo (`-re`) belegt 4K60 etwa 1,6 (SVT) bzw. 2,5 (NVENC) der
8 Kerne, im Leerlauf mit SteamVR etwa 0,7. Die Leistungsaufnahme ist nicht
belastbar gemessen: Am Ladegerät zeigt der Akkustrom nur den Saldo (etwa
1,9 W Entladung im Leerlauf, 5,7 W beim Decodieren).

Bewertung: 4K60 reicht mit Reserve, 8K60 nicht. Der Player bietet AV1 daher
als experimentelle, abschaltbare Option nur bis 4096×2160 und 8 Bit an
(`src/player/software-video.c`): dav1d schreibt in NV12-Puffer aus
`/dev/udmabuf` (Gruppe `video`), die der Renderer wie Iris-Puffer importiert.
Im Player ohne Headset: 1700 Bilder des NVENC-Clips ohne verspätete Bilder
(rund 3 Kerne belegt, mit `max_frame_delay=8`; ohne diese Einstellung fielen
448 von 1200 Bildern zu spät aus), ein 4096×2048-Titel mit 60 fps per
Jellyfin Direct Play ohne Verluste.

### Auflösung und Framerate

`VIDIOC_ENUM_FRAMESIZES` meldet für H264/HEVC/VP90 sowie die enumerierten CAPTURE-Formate jeweils `128x128 – 8192x8192`, Schritt `1/1`. Das ist eine gemeldete Breiten-/Höhenhülle, kein erfolgreicher 8192²-Test und keine Zusage für alle Kombinationen, Profile oder Bildraten. Pixel-/Makroblockbudget, Alignment, Referenzbilder, Bitrate und thermische Grenzen können enger sein.

`v4l2-ctl --list-frameintervals=width=7680,height=4320,pixelformat=HEVC` scheiterte bereits mit `The pixelformat 'HEVC' is invalid`. Eine direkte lesende IOCTL-Abfrage beseitigte diese Werkzeugmehrdeutigkeit: `VIDIOC_ENUM_FRAMEINTERVALS` liefert für H264 und HEVC bei 3840×2160, 7680×4320 und 8192×4096 jeweils `ENOTTY` (25). **Keine nachweisbare maximale FPS aus dieser Schnittstelle.**

Für Stereonaut sind „8K UHD“ (7680×4320) und verbreitete VR-Abmessungen wie 8192×4096 getrennt zu testen. Auch SBS/TB beschreibt zunächst den Inhalt eines Frames; zwei getrennt codierte Augenstreams würden zusätzliche Decoderlast erzeugen.

### FFmpeg

Vorhanden: FFmpeg `n7.0`, libavcodec `61.3.100`, libavformat `61.1.100`, libavutil `59.8.100`; Build mit libdrm, Vulkan, libv4l2 und libplacebo. Relevante Decoder: `h264_v4l2m2m`, `hevc_v4l2m2m`, `vp9_v4l2m2m`, Software-H.264/HEVC sowie AV1/libdav1d/libaom. Decoder-Hilfe für HEVC-M2M meldet `hardware`, 16 OUTPUT- und 20 CAPTURE-Buffer als Defaultwerte.

`-hwaccels` listet `vdpau`, `cuda`, `vaapi`, `qsv`, `drm`, `opencl`, `vulkan`. Diese Build-Liste beweist keine dazu passende Hardware; siehe [FFmpeg-Dokumentation](https://ffmpeg.org/ffmpeg.html). Der explizite V4L2-Decoder muss separat berücksichtigt werden. CUDA/QSV-Einträge sind kein sinnvoller Qualcomm-Pfad.

Ein vorhandener M2M-Wrapper garantiert weder DRM-PRIME-Frames noch einen GPU-residenten Transfer. Für den konkreten Distributionsbuild sind AVFrame-Format, Speicherherkunft und Export noch zu prüfen. Der [FFmpeg-7.0-M2M-Quelltext](https://ffmpeg.org/doxygen/7.0/v4l2__m2m__dec_8c_source.html) ist Referenz für die stateful Initialisierung, keine Verifikation unveränderter Distributionsquellen. `-hwaccel_output_format drm_prime` allein erzeugt keinen fehlenden Exportpfad.

### GStreamer

Vorhanden: `1.24.2`, `h265parse`, `h264parse`, `av1parse`, `appsink`, `v4l2h264dec`, `v4l2h265dec`, `v4l2vp9dec`, GL- und Vulkan-Elemente. `libgstvideoparsersbad.so` und `libgstvulkan.so` gehören auf diesem System zum Paket `gst-plugins-bad-libs`; nicht anhand des Paketnamens ihre Abwesenheit unterstellen.

`v4l2h265dec` zeigt `/dev/video22`, HEVC byte-stream/AU am Eingang sowie NV12/NV21/RGBA am Ausgang. `capture-io-mode` bietet `mmap`, `dmabuf` und `dmabuf-import`. Das belegt angebotene Modi, keinen erfolgreichen Export. Die Template-Grenzen bis 32768 und die praktisch unbeschränkte FPS-Range sind keine Gerätegrenzen.

Das stateless Plugin ist installiert, registriert jedoch keine Features. Die öffentliche [Dokumentation von v4l2slh265dec](https://gstreamer.freedesktop.org/documentation/v4l2codecs/v4l2slh265dec.html) beschreibt einen DMA-BUF-/Request-Pfad, der auf diesem Gerät gerade nicht als Element verfügbar ist. Ebenso funktionieren die vorhandenen Vulkan-Decoder nicht allein aufgrund ihrer Registrierung ohne Vulkan-Video-Unterstützung des Geräts.

## 5. GPU-residenter Frame-Transfer

Angestrebter Weg, **noch nicht implementiert oder getestet**:

```text
Datei → Demux/HEVC-Parser → Iris stateful V4L2-M2M
     → CAPTURE-Buffer → DMA-BUF-Export
     → Vulkan-Import mit passendem Format/Modifier
     → YCbCr-Sampling + Projektion/Tonemapping
     → OpenXR-Swapchain → SteamVR-Compositor
```

Auf einem Unified-Memory-System bedeutet GPU-resident hier insbesondere: keine CPU-Konvertierung, kein CPU-Readback und kein erneuter Upload vollständiger decodierter Frames. Ein GPU-Konvertierungspass kann trotzdem erforderlich und akzeptabel sein; er ist nicht automatisch Zero-Copy im strengen Sinn.

Zu validieren:

1. Decoder-CAPTURE-Buffer per `VIDIOC_EXPBUF` exportieren oder kompatible DMA-BUFs importieren. Die [V4L2-DMA-BUF-Import-API](https://cdn.kernel.org/doc/html/latest/userspace-api/media/v4l/dmabuf.html) beschreibt eine andere Rolle als den Export eigener Decoder-Buffer. Nicht aus einem angebotenen Modus auf den anderen schließen.
2. FourCC, Modifier, Plane-Anzahl, FD-Zuordnung, Offset, Stride, Allocation-Größe und Crop vollständig ermitteln. Die initiale NV12-Abfrage meldet einen V4L2-Speicherplane; dieser enthält trotzdem Y und UV als Bildplanes.
3. Vulkan-Importfähigkeit für genau diesen Modifier, Format, Usage und DMA-BUF-Handle-Typ abfragen; `vkGetPhysicalDeviceImageFormatProperties2` und externe Speicherproperties verwenden. [DMA-BUF-Extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_external_memory_dma_buf.html) und [DRM-Modifier-Extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_image_drm_format_modifier.html) definieren die Bausteine. Extension-Präsenz ist kein Importnachweis.
4. Synchronisation zwischen Decoder, Vulkan und Rückgabe an V4L2 ausdrücklich lösen. GPU-Zugriffe müssen abgeschlossen sein, bevor ein Buffer erneut zur Decodierung freigegeben wird. Verfügbare FD-Semaphore-/Fence-Extensions beweisen noch kein passendes Decoder-Fence-Protokoll. Queue-Family-Ownership, Layoutwechsel und implizite DMA-BUF-Synchronisation prüfen.
5. 10-Bit-YUV bis zur GPU erhalten. BT.2020/BT.709, Full/Limited Range, Chroma-Siting sowie PQ/HLG aus Metadaten behandeln. 10-Bit-Decodierung ist unabhängig von einer möglichen HDR-Ausgabe des Displays; XR-Swapchain-Farbräume wurden nicht ermittelt.

Alternativen: GPU-Konvertierung in ein renderbares RGB-Bild, falls direktes YCbCr-Sampling oder Engine-Integration nicht passt; EGL/GL-DMA-BUF-Interop als gesonderter Kandidat, aber EGL-Import-Extensions wurden nicht abgefragt. CPU-Upload nur als Diagnosevergleich, nicht als freigegebener 8K-Zielpfad. Android MediaCodec wird aus der SoC-Herkunft nicht als native SteamOS-API vorausgesetzt.

Rechnerische Größenordnung, keine Messung: Ein ungepaddetes P010-Frame mit 7680×4320 benötigt 99.532.800 Byte (~94,9 MiB). Bei 60 fps sind das 5,97 GB/s pro vollständigem Datenstrom. Eine zusätzliche CPU-Kopie verursacht nominell Lesen plus Schreiben (~11,94 GB/s), weitere Upload-/Konvertierungsschritte kommen hinzu. 20 solche Buffer entsprächen bereits ~1,99 GB ohne DPB-/Alignment-/sonstige Allokationen; Defaults deshalb nicht unbesehen übernehmen.

## 6. Architekturvergleich

| Ansatz | Eignung | Risiko für 8K Main10 |
| --- | --- | --- |
| Godot 4.6 + OpenXR + C++-GDExtension | Gute Basis für UI, Interaktion und Projektionen; native Decodererweiterung grundsätzlich sinnvoll | Zusätzliche Verantwortung für Fremdtexturen, YUV, aktivierte Device-Extensions, Renderthread und Synchronisation; behebt fehlende Decoderformate nicht |
| Native C++-App + GStreamer + Vulkan/OpenXR | Vorhandener stateful Decoder, Parser und DMA-BUF-Modus ermöglichen einen schnellen Vergleichsprototyp | Installiertes HEVC-Element zeigt nur 8-Bit-Caps; Buffer-Mapping/Negotiation können Kopien oder Downconversion verursachen |
| Native C++-App + FFmpeg + Vulkan/OpenXR | libavformat/Parser für Container und Bitstreams; präzise Kontrolle über Playback und Ressourcen | Vorhandener FFmpeg-M2M-Wrapper muss 10-Bit-/Exportfähigkeit erst beweisen; eventuell direkter V4L2-Adapter nötig |
| Native C++-App + direkter V4L2-Decoderadapter + Vulkan/OpenXR | Maximale Kontrolle über Iris-Formate, Bufferexport, Rückgabe und GPU-Import | Höherer Aufwand für stateful Source-Change, Drain, Seek, Fehlerbehandlung und Lebensdauer |

Godot 4.6 besitzt die OpenXR-Integration; Vulkan-XR muss früh aktiviert werden ([XR-Einrichtung](https://docs.godotengine.org/en/4.6/tutorials/xr/setting_up_xr.html)). Die [RenderingDevice-API](https://docs.godotengine.org/en/4.6/classes/class_renderingdevice.html) bietet native Ressourcen und `texture_create_from_extension`. Das macht externe Vulkan-Bilder grundsätzlich integrierbar, übernimmt jedoch nicht automatisch DMA-BUF-Import, YCbCr-Layouts und Fremdsynchronisation. Eine GDExtension muss am tatsächlich von Godot/OpenXR verwendeten Device arbeiten; ein getrenntes lokales RenderingDevice löst dieses Problem nicht von selbst. Godot wurde auf dem Gerät nicht gestartet oder benchmarked. Ein zusätzlicher GPU-RGB-Pass wäre gegen direkten YUV-Zugriff zu messen.

FFmpeg gegen GStreamer ist nicht gleichbedeutend mit „native gegen nicht native“: Beide sind native Bibliotheken. GStreamer ist hier für den schnellen stateful Vergleich attraktiv. FFmpeg eignet sich gut als Demux-/Parser-Baustein; sein Vorhandensein reicht nicht als Beleg für den gewünschten Speicherpfad. **Keines der beiden Frameworks kann ein vom Kernel/Firmwarepfad nicht zugänglich gemachtes 10-Bit-Format ersetzen.**

## 7. Diagnoseprotokoll und Reproduktion

Die folgenden Befehle wurden auf dem Ziel über den Alias ausgeführt; die oben dokumentierte autorisierte Agent-Ausnahme galt nur für die lokale SSH-Umgebung. Keine der hier aufgeführten Geräteabfragen setzt Formate oder startet Streams. Ausgaben wurden teilweise für die Sichtung gefiltert; entscheidende Rohdaten liegen unter `docs/diagnostics/`.

```sh
uname -a
cat /etc/os-release
lscpu
cat /proc/device-tree/model
tr '\000' '\n' < /proc/device-tree/compatible
grep -E 'MemTotal|MemAvailable' /proc/meminfo
id
ls -l /dev/video* /dev/media* /dev/dri/*
ls -l /dev/dma_heap
readlink /sys/class/drm/renderD128/device/driver
readlink /sys/class/video4linux/video22/device/driver
cat /proc/modules
modinfo iris
zcat /proc/config.gz | grep -E 'CONFIG_(VIDEO_QCOM|V4L2_MEM2MEM|MEDIA_CONTROLLER|VIDEO_V4L2|DMABUF|SYNC_FILE)'
vulkaninfo --summary
vulkaninfo
vulkaninfo --show-formats
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video-dec0 --all
v4l2-ctl -d /dev/video-dec0 --list-formats-out
v4l2-ctl -d /dev/video-dec0 --list-formats
v4l2-ctl -d /dev/video-dec0 --list-ctrls-menus
v4l2-ctl -d /dev/video22 --list-formats-out-ext
v4l2-ctl -d /dev/video22 --list-formats-ext
v4l2-ctl -d /dev/video22 --list-formats all
v4l2-ctl -d /dev/video22 --list-framesizes=HEVC
v4l2-ctl -d /dev/video22 --list-framesizes=H264
v4l2-ctl -d /dev/video22 --list-frameintervals=width=7680,height=4320,pixelformat=HEVC
v4l2-ctl --help-vidout
media-ctl -p -d /dev/media0
ffmpeg -version
ffmpeg -hide_banner -hwaccels
ffmpeg -hide_banner -decoders
ffmpeg -hide_banner -h decoder=hevc_v4l2m2m
gst-inspect-1.0 --gst-disable-registry-update --version
gst-inspect-1.0 --gst-disable-registry-update
gst-inspect-1.0 --gst-disable-registry-update v4l2h265dec
gst-inspect-1.0 --gst-disable-registry-update v4l2slh265dec
gst-inspect-1.0 --gst-disable-registry-update v4l2codecs
pacman -Q | grep -Ei 'mesa|vulkan|openxr|steamvr|ffmpeg|gstreamer|gst-|v4l|libdrm|firmware|linux|qcom|video'
pacman -Qo /usr/lib/gstreamer-1.0/libgstvulkan.so /usr/lib/gstreamer-1.0/libgstvideoparsersbad.so
ls -l /etc/openxr/1 /usr/share/openxr/1 "$HOME/.config/openxr/1"
cat /opt/steamvr/steamxr_linuxarm64.json
printenv XR_RUNTIME_JSON
ls -l /usr/lib/libopenxr* /opt/steamvr/bin/linuxarm64/*openxr*
ls /usr/include/openxr
ls /usr/src/debug
ls -l /opt/steamvr/bin/linuxarm64/helloxr
file /usr/bin/ffmpeg /usr/bin/gst-inspect-1.0 /usr/lib/gstreamer-1.0/libgstvideo4linux2.so /opt/steamvr/bin/linuxarm64/vrclient.so
grep -nE 'Q08C|Q10C|QC24|P010|TP10|HEVC_SLICE|struct v4l2_frmival|VIDIOC_ENUM_FRAMEINTERVALS|SUPPORTS_REQUESTS' /usr/include/linux/videodev2.h
```

Zusätzlich wurde die Werkzeugverfügbarkeit mit `command -v` geprüft. Vorhanden: `vulkaninfo`, `ffmpeg`, `ffprobe`, `gst-inspect-1.0`, `v4l2-ctl`, `media-ctl`, `eglinfo`, `python3`, `strings`, `readelf`. Nicht im PATH gefunden: `xrinfo`, `openxr-info`, `xrgears`, `vainfo`. Keine Nachinstallation. Unter `/usr/src/debug` lag kein passender Iris-Kernelquellbaum; kein Abgleich mit dem genauen Treiberquellstand möglich.

### OpenXR-Enumeration ohne Session

Ausgeführt mit `ssh frame 'python3 -'` und folgendem stdin (Agent-Ausnahme siehe oben):

```python
import ctypes as c

class Ext(c.Structure):
    _fields_ = [("type", c.c_int32), ("next", c.c_void_p),
                ("extensionName", c.c_char * 128),
                ("extensionVersion", c.c_uint32)]

lib = c.CDLL("/opt/steamvr/bin/linuxarm64/libopenxr_loader.so")
fn = lib.xrEnumerateInstanceExtensionProperties
fn.argtypes = [c.c_char_p, c.c_uint32, c.POINTER(c.c_uint32), c.POINTER(Ext)]
fn.restype = c.c_int32
count = c.c_uint32()
result = fn(None, 0, c.byref(count), None)
print("xrEnumerateInstanceExtensionProperties count:", result, count.value)
if result >= 0:
    props = (Ext * count.value)()
    for p in props:
        p.type = 2  # XR_TYPE_EXTENSION_PROPERTIES
    result = fn(None, count.value, c.byref(count), props)
    print("enumeration result:", result)
    for p in props:
        print(p.extensionName.decode(), p.extensionVersion)
```

### Direkte Framerate-Abfrage

Ebenfalls nur über stdin ausgeführt. `0xc034564b` ist das auf diesem Linux-AArch64-UAPI verwendete `VIDIOC_ENUM_FRAMEINTERVALS` für die 52-Byte-Struktur. Ergebnis in allen sechs Fällen: `ENOTTY`; keine Framerate gemeldet.

```python
import os, fcntl, struct
fd = os.open("/dev/video22", os.O_RDONLY | os.O_NONBLOCK)
for fourcc in ("HEVC", "H264"):
    for w, h in ((3840, 2160), (7680, 4320), (8192, 4096)):
        b = bytearray(struct.pack("=13I", 0,
            int.from_bytes(fourcc.encode(), "little"), w, h, *([0] * 9)))
        try:
            fcntl.ioctl(fd, 0xc034564b, b, True)
            print(fourcc, w, h, "VIDIOC_ENUM_FRAMEINTERVALS",
                  struct.unpack("=13I", b))
        except OSError as e:
            print(fourcc, w, h, "VIDIOC_ENUM_FRAMEINTERVALS", e)
os.close(fd)
```

Weitere Belege: [System](diagnostics/2026-10-08-system.txt), [V4L2](diagnostics/2026-10-08-v4l2.txt), [GStreamer](diagnostics/2026-10-08-gstreamer.txt). API- und Produktquellen wurden am Untersuchungstag abgerufen; neuere Upstream-Dokumentation beschreibt nicht automatisch den installierten Build.

## 8. Offene Fragen und Abnahmekriterien

| Priorität | Frage | Erforderlicher Nachweis |
| --- | --- | --- |
| P0 | Akzeptiert Iris HEVC Main10, und bleibt die 10-Bit-Präzision erhalten? | Main10-Stream, nach Headerverarbeitung ausgehandeltes CAPTURE-Format, dokumentiertes Layout, Bildvergleich mit 10-Bit-Referenz |
| P0 | Gibt es einen importierbaren Decoder-DMA-BUF? | Erfolgreicher Export, exakte Plane-/Modifierdaten, Vulkan-Import und korrektes Sampling |
| P0 | Läuft 8K im erforderlichen Tempo? | 7680×4320 und 8192×4096, getrennte 30-/60-fps-Messungen, repräsentative Bitraten/GOPs, kein Softwarefallback |
| P1 | Bleibt der Pfad unter XR-Last stabil? | Mindestens zehn Minuten mit aktivem Rendering/Tracking; Drops, CPU/GPU-Zeit, Thermik, Speicher und Energie soweit zugänglich |
| P1 | Welche XR-Formate und Refresh-Raten sind nutzbar? | Session, Swapchain-Format-Enumeration, Refresh-Rate-Abfrage und erfolgreicher Frame-Loop |
| P1 | Gibt es versteckte Kopien oder 8-Bit-Konvertierung? | Buffer-Metadaten über die ganze Pipeline, dokumentierte GPU-Pässe, keine CPU-Mappings decodierter Frames im Messpfad |
| P2 | Ist AV1 über einen anderen installierten Pfad zugänglich? | Dokumentierte native API plus Decode-Test; aus SoC-Daten allein nicht ableitbar |

Die Zielbildrate war nicht vorgegeben. Für den ersten Prototyp werden 30 und 60 fps als getrennte Testziele vorgeschlagen, **nicht als bereits zugesagte Fähigkeit**. HDR, Profile/Level, Chroma-Subsampling und Bitratenbereich des späteren Medienkorpus müssen festgelegt werden.

## 9. Empfehlung und erster Decoder-Prototyp

**Empfehlung: zunächst einen kleinen nativen C++-Decoder-/Vulkan-Prototypen bauen, dann über Godot entscheiden.** Für den späteren Player ist C++/OpenXR/Vulkan derzeit die bevorzugte Architektur, weil sich damit der kritische Iris-/DMA-BUF-/10-Bit-Pfad unmittelbar kontrollieren lässt. Diese Empfehlung ist eine technische Bewertung, kein gemessener Performancevergleich. Godot 4.6 bleibt eine Option für UI und Interaktion, sobald derselbe Decoderpfad einschließlich Synchronisation und zusätzlicher GPU-Konvertierungskosten nachgewiesen ist.

Der Prototyp heißt vorgeschlagen `stereonaut-decoder-probe`. Er ist ein gesonderter Entwicklungsauftrag; hier wurde weder Code dafür deployed noch eine Testdatei auf das Frame kopiert.

1. **Format-Gate vor 8K-Benchmark:** Mit einem kleinen HEVC-Main-Clip und einem Main10-Clip beginnen, etwa 1920×1080. libavformat demuxt; HEVC wird als Annex-B/AUs an einen explizit gewählten stateful Iris-Adapter gegeben. Nach Source-Change die tatsächlichen CAPTURE-Formate, Bit-Tiefe, Crop und Strides protokollieren. Falls kein 10-Bit-erhaltender Ausgang zugänglich ist, mit dieser Diagnose stoppen; keinen stillen Softwarefallback oder Main10→8-Bit-Erfolg melden.
2. **GStreamer als Vergleichspfad:** Das vorhandene `h265parse → v4l2h265dec capture-io-mode=dmabuf → appsink` prüfen. Pipeline und Appsink müssen Buffer ohne CPU-Mapping inspizieren. NV12 kann den 8-Bit-Grundpfad bestätigen, erfüllt aber nicht das Main10-Ziel. Einen nicht vorhandenen stateless Decoder nicht voraussetzen. Wenn GStreamer einen korrekten 10-Bit-DMA-BUF liefert, kann es auch das erste produktive Backend sein; andernfalls Formatverlust gegenüber direktem V4L2 isolieren.
3. **Ein Frame bis Vulkan:** Decoder-Buffer exportieren, Layout und Importfähigkeit abfragen, importiertes YUV auf ein einfaches Ziel rendern. Korrektheit anhand eines gezielten 10-Bit-Verlaufs und einer Software-Referenz prüfen; Readback ausschließlich für diesen separaten Korrektheitstest, nicht in der Performancepipeline. Buffer erst nach GPU-Abschluss zurückgeben.
4. **Skalieren:** 4K, danach 7680×4320 und 8192×4096; Main und Main10 4:2:0, jeweils 30/60 fps und mehrere Bitraten/GOP-Strukturen. Gemessene Ausgabe-FPS, Fehler/Drops, Spitzen-RAM, Bufferzahl und CPU-/GPU-Zeit protokollieren. Durchsatzmessung ungebremst und zeitgesteuerte Wiedergabe unterscheiden. Decode-only und Decode-plus-Render separat vergleichen.
5. **Minimales OpenXR ergänzen:** Runtime-konforme Vulkan-Geräteerstellung mit `XR_KHR_vulkan_enable2`, einfache Projektionsfläche und unabhängiger XR-Frame-Loop. Video nach PTS auswählen; Headtracking weiter mit der Runtime-Taktung rendern. Noch keine Medienbibliothek, Netzwerkstreams, Untertitel oder vollständige Player-UI.

**Erfolgskriterium:** dokumentierter Hardwarepfad mit 10-Bit-Erhalt, importierbaren Frames, korrekter Darstellung und gemessenem 8K-Durchsatz ohne CPU-Readback/-Upload pro Frame. Schlägt bereits das Format-Gate fehl, ist zuerst die konkrete Iris-/Firmware-/Userspace-Schnittstelle mit dem Plattformanbieter zu klären; ein Wechsel zwischen Godot, FFmpeg und GStreamer allein löst diese Lücke nicht.
