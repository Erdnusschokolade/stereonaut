# Realvideo 002 – Ergebnis des direkten NV12-OpenXR-Pfads

Stand: 2026-10-09. Der Auftrag ist auf der technischen Seite erfolgreich
abgeschlossen. Ein echter 8192×4096-HEVC-Main-8-Bit-Stream lief vollständig
über Iris, zwölf NV12-DMA-BUFs, direktes Vulkan-YCbCr-Sampling und den
SteamVR-OpenXR-Swapchain. Es gab keine permanente NV12-zu-RGBA-Kopie und
keinen CPU-Zugriff auf die Videoframes.

## Sichtbarer Einzelbildnachweis

Der erste aktive Import hielt einen echten Iris-CAPTURE-Buffer außerhalb der
Decoderqueue und stellte ihn wiederholt auf dem headtracked Quad dar. Frame 0
war zwar bytegenau korrekt, im Ausgangsvideo jedoch nahezu gleichmäßig
hellgrau. Die aussagekräftige Sichtprüfung wurde daher mit Frame 1800 bei
ungefähr 30 Sekunden wiederholt.

Der Nutzer bestätigte nach der Korrektur der View- und Projektionsmatrizen:

- das Bild steht richtig herum;
- horizontale und vertikale Kopfbewegungen lassen das Quad raumfest;
- die Farben entsprechen soweit beurteilbar der Referenz;
- sichtbare Skalierungsartefakte und die Gesamtverzerrung gehören zur
  flachen, unprojizierten 2D-Darstellung des 2:1-Ausgangsbildes.

Damit sind der reale Decoderinhalt, die BT.709-YCbCr-Konvertierung,
Orientierung und Pose-Verarbeitung unabhängig vom kontinuierlichen Timing
sichtbar bestätigt.

## Kontinuierlicher Endlauf

Der Endlauf verwendete Commit `6f9e612` und den unveränderten Stream:

```
f78332e37281b24c9c3a3304df785b914d97f84720f3cb5930e3bf0fcadcfa3d  realvideo-001.hevc
```

Ausführung im bestehenden Frame-Testverzeichnis:

```
timeout -k 5s 240s env DISPLAY=:0 STEAM_GAME_DISPLAY_0=:1 \
  XDG_RUNTIME_DIR=/run/user/1000 \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
  ./xr-nv12-playback-probe realvideo-001.hevc 8192 4096 60000 1001 \
  realvideo-002
```

Das vollständige kleine Textlog liegt unter
`diagnostics/2026-10-09-realvideo-002-continuous-fifo.log`; seine SHA-256 ist
`2e6b7bf8acba7b08fba0c68107a01d5a06fcc57246fe09b6096c286b8dd53b52`.

| Messwert | Ergebnis |
| --- | ---: |
| Access Units / OUTPUT-Rückgaben | 7.191 / 7.191 |
| dekodierte CAPTURE-Frames | 7.191 |
| V4L2-Sequenzdrops | 0 |
| V4L2-/Vulkan-/OpenXR-Programmfehler | 0 |
| Laufzeit | 119,968742 s |
| Quellrate | 59,940060 fps |
| erreichte Decoderrate | 59,940614 fps |
| Echtzeitfaktor | 1,000009 |
| maximale Feed-Verspätung | 2,150942 ms |
| mittlere / maximale Decode-Latenz | 15,622259 / 218,139350 ms |
| `SOURCE_CHANGE` | 2 Ereignisse |
| kompatible CAPTURE-Wiederverwendung | 1 |
| XR-Frames laut Anwendung | 8.642 |
| sichtbar gerenderte XR-Frames | 5.500 |
| verschiedene dargestellte Videoframes | 4.579 |
| Wiederholungen des aktuellen Videoframes | 921 |
| vor einer Darstellung freigegebene Videoframes | 2.612 |
| Renderer empfangen / freigegeben | 7.191 / 7.191 |

Die 2.612 nicht dargestellten Videoframes sind kein Durchsatz- oder
FIFO-Verlust: Zwischen OpenXR-Session-State `SYNCHRONIZED` und dem ersten
`VISIBLE` lagen 43,612 Sekunden. Bei 60000/1001 Hz entsprechen diese
43,58 Sekunden beziehungsweise rund 2.612 Frames. Ab Sichtbarkeit wurden alle
verbleibenden 4.579 neuen Videoframes dargestellt. Eine nachträgliche reine
Diagnoseänderung trennt deshalb künftig `hidden_video_frames` und
`pipeline_skipped_frames` ausdrücklich.

## SteamVR-Befund

Der zu PID 33811 gehörende SteamVR-Abschnitt bestätigt eine Laufzeit von
120,087831 Sekunden. Der Compositor meldete:

```
Total:      8643 presents, 0 dropped, 1 reprojected
Startup:      62 presents, 0 dropped, 1 reprojected
App:       CPU 8.136 ms, GPU 1.927 ms, target 72 fps
```

Im Gegensatz zu einzelnen früheren Startläufen trat beim Endlauf kein
`Failed to signal gpu fence` auf. `vrserver` meldete das erwartete fehlende
Action-Manifest; die Probe besitzt absichtlich noch keine Eingabeaktionen.
Der Prozess wurde nach dem Streamende regulär abgemeldet.

Die abschließende Sichtprüfung des kontinuierlichen Laufs bestätigt den
automatisierten Befund: Die Darstellung war sichtbar in Ordnung und lief
augenscheinlich flüssig. Zusammen mit den null Compositor-Drops ist damit auch
das qualitative Erfolgskriterium für diesen Prototyp erfüllt.

## Nachgewiesener Daten- und Bufferpfad

Für jeden der zwölf CAPTURE-Indizes wurde genau ein 50.331.648-Byte-DMA-BUF
exportiert und genau einmal in das von OpenXR gewählte Turnip-Vulkan-Device
importiert. Der bestätigte Vertrag blieb:

```
VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
DRM_FORMAT_MOD_LINEAR
Plane 0: offset 0,        rowPitch 8192
Plane 1: offset 33554432, rowPitch 8192
YCbCr: BT.709, ITU narrow, midpoint chroma
```

Decoder und XR-Renderer laufen getrennt. Der IPC-Socket enthält höchstens die
durch zwölf CAPTURE-Buffer begrenzte FIFO. Der Renderer importiert pro neuem
Frame den DMA-BUF-Producer-Sync-FD temporär, übernimmt das Image von FOREIGN,
sampelt es direkt im Fragmentshader und gibt es nach dem Fence wieder an
FOREIGN zurück. Erst anschließend sendet er den Index an den Decoder zurück;
nur dieser führt `QBUF` aus.

Beim zweiten `SOURCE_CHANGE` wurde die alte Generation bis zum leeren
`V4L2_BUF_FLAG_LAST` abgearbeitet. Der Renderer bestätigte einen leeren
GPU-Bestand, alle Format-, Compose- und Allokationsparameter wurden erneut
geprüft und dieselben zwölf Buffer samt Vulkan-Imports weiterverwendet. Es gab
kein `REQBUFS`, keinen neuen DMA-BUF-Export und keinen neuen Vulkan-Import.

## Bewertung und nächster Schritt

Der direkte Main-8-Bit-Pfad ist bereit für die nächste OpenXR-Ausgabestufe:
Iris hält 8K60 in Echtzeit, Turnip sampelt NV12 direkt und SteamVR bleibt im
sichtbaren Abschnitt ohne Compositor-Drops. Die derzeitige Darstellung ist
absichtlich nur ein flaches Quad; ihre Verzerrung und Skalierung sagen nichts
gegen den Decoder- oder Interop-Pfad aus.

Der nächste Auftrag sollte ausschließlich die eigentliche stereoskopische
Projektion einführen: SBS-Augenaufteilung und Abbildung der Videokoordinaten
auf eine geeignete 180°-/360°-Geometrie oder einen entsprechenden Shader.
Audio, Container-Demux, UI, Seeking und Jellyfin bleiben davon getrennte
spätere Schritte.
