# Realvideo 003 – Stereo-Layout und VR-Projektion

Stand: 2026-10-09

## Ergebnis

Der direkte Videopfad blieb unverändert:

```
HEVC Main 8 Bit → Iris → NV12 DMA-BUF → Vulkan-YCbCr-Sampler → OpenXR
```

SBS-Aufteilung und equirektangulare Projektion erfolgen nur bei der
UV-Berechnung des Fragmentshaders. Es gibt weiterhin keine CPU-Farbwandlung,
keine RGBA-Zwischenkopie und keine zusätzliche 8K-Texturkopie.

Der vorausgehende Sichtlauf bestätigte die normale SBS-Reihenfolge:

```
linke Quellhälfte  → OpenXR View 0 (linkes Auge)
rechte Quellhälfte → OpenXR View 1 (rechtes Auge)
```

Der Nutzer bestätigte nach der Korrektur ein zusammenhängendes Bild mit
Tiefenwahrnehmung. Der vorher übergroße horizontale und vertikale Versatz war
damit auf einen fusionierbaren Stereo-Versatz reduziert. Der Abschlusslauf
verwendete kein `--swap-eyes`.

## Ursache des Projektionsfehlers

Der Vertexshader hatte zunächst jeden der vier FOV-Eckstrahlen normiert und
erst danach vom Rasterizer interpolieren lassen. Bei den stark asymmetrischen
OpenXR-FOVs besitzen diese Eckstrahlen unterschiedliche Längen. Das
Interpolieren der bereits normierten Vektoren krümmt deshalb das Strahlenfeld
nichtlinear und für beide Augen unterschiedlich. Sichtbar waren ein großer
horizontaler Versatz und zusätzlich eine Höhendifferenz.

Korrekt wird der Tangentenstrahl

```
(tan_x, tan_y, -1)
```

unnormiert interpoliert und erst im Fragmentshader normiert. Commit
`342ff2e` setzt diese Änderung um und ergänzt einen Hosttest mit asymmetrischen
FOV-Tangenten.

Eine zusätzliche Diagnose in Commit `515d230` bestätigte, dass beide von
SteamVR gelieferten Augen dieselbe Pose-Quaternion besitzen. Die
Pose-Rotation war daher nicht die Fehlerursache. Die VIEW-Pose wird zur
Nachvollziehbarkeit einmal je Lauf protokolliert.

## Quelle und Augenreihenfolge

Die Quelle hat 8.192×4.096 Pixel, HEVC Main 8 Bit und 60.000/1.001 Bilder pro
Sekunde. Der verwendete 120-Sekunden-Elementary-Stream hat SHA-256
`f78332e37281b24c9c3a3304df785b914d97f84720f3cb5930e3bf0fcadcfa3d`.

Container-Metadaten zur Projektion waren in der MKV-Datei nicht vorhanden.
Der Nutzer bestätigte jedoch, dass Playa und DeoVR denselben Inhalt im Modus
180° SBS korrekt und in deren getrenntem Fisheye-Modus verzerrt darstellen.
[DeoVR dokumentiert](https://deovr.com/documentation) `dome` als
180°-Equirect-Mesh und `fisheye` als separaten Mesh-Typ. Die Quelle wird
deshalb als SBS Equirect 180 behandelt.

Ein zusätzlicher Offline-Abgleich von drei Frames fand in den verkleinerten
1.024×1.024-Augenbildern nur rund 0,2 Pixel medianen vertikalen Unterschied
zwischen korrespondierenden Merkmalen. Nahe Objekte hatten die für
links→rechts erwartete negative horizontale Disparität. Die Quellhälften sind
damit vertikal rektifiziert und in konventioneller Reihenfolge angeordnet.

## Tests

Die Projektion und das Layout werden weiterhin unabhängig ausgewählt:

```
--projection flat|180|360
--stereo mono|sbs|tb
--swap-eyes
```

Vor den Zielläufen bestanden:

- `python3 test-projection-math.py`;
- GLSL-Kompilierung mit `glslc`;
- SPIR-V-Prüfung mit `spirv-val --target-env vulkan1.1`;
- C-Prüfung mit `-Wall -Wextra -Werror`.

Die sequenziellen Zielsystemproben umfassten Flat/Mono, Flat/SBS, Mono/360,
SBS/180 und Flat/TB. Alle begrenzten technischen Läufe dekodierten 900/900
Frames ohne V4L2-Sequenzdrop oder Pipeline-Skip. Der SBS/180-Sichtbefund
führte anschließend zur beschriebenen Korrektur der Strahlinterpolation.

Der vollständige Abschlusslauf mit derselben zuvor visuell bestätigten
Konfiguration war:

```
timeout -k 5s 240s env \
  DISPLAY=:0 STEAM_GAME_DISPLAY_0=:1 \
  XDG_RUNTIME_DIR=/run/user/1000 \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
  ./xr-nv12-playback-probe \
  realvideo-001.hevc 8192 4096 60000 1001 \
  vr-projection-sbs-180-final --projection 180 --stereo sbs
```

Das vollständige Log liegt in
`diagnostics/2026-10-09-vr-projection/vr-projection-sbs-180-final.log` und hat
SHA-256
`f045c6e8831510a197ccce1153c5e8f6e1fb566a80e771a989025c29dd0f0fb3`.
Die übrigen begrenzten Laufprotokolle liegen im selben Verzeichnis.

## Abschlussmessung

| Messwert | Ergebnis |
| --- | ---: |
| Access Units / OUTPUT-Rückgaben | 7.191 / 7.191 |
| dekodierte / an Renderer übergebene Frames | 7.191 / 7.191 |
| V4L2-Sequenzdrops | 0 |
| Programmfehler | 0 |
| Laufzeit | 119,968721 s |
| Quellrate | 59,940060 fps |
| erreichte Decoderrate | 59,940624 fps |
| Echtzeitfaktor | 1,000009 |
| maximale Feed-Verspätung | 2,664852 ms |
| mittlere / maximale Decode-Latenz | 15,626648 / 219,183267 ms |
| kompatible CAPTURE-Wiederverwendungen | 1 |
| OpenXR-Frames | 8.641 |
| gerenderte XR-Frames | 8.519 |
| verschiedene dargestellte Videoframes | 7.093 |
| in nicht sichtbarem Sessionzustand empfangene Frames | 98 |
| Pipeline-Skips | 0 |
| Renderer empfangen / freigegeben | 7.191 / 7.191 |

SteamVR ordnete den Lauf Renderer-PID 5424 zu und meldete:

```
Total:   8641 presents, 5 dropped, 0 reprojected
Startup:   60 presents, 5 dropped, 0 reprojected
App:     CPU 12.142 ms, GPU 2.721 ms, target 72 fps
```

Alle fünf Compositor-Drops lagen damit im Startup-Abschnitt; während des
regulären Laufs kam kein weiterer Drop hinzu. SteamVR protokollierte einmalig
beim Verbindungsaufbau `Failed to signal gpu fence`. Der Lauf setzte sich
danach ohne Programmfehler, weiteren Fence-Befund oder Reprojection fort und
wurde nach 120,075 Sekunden regulär abgemeldet.

## Bewertung

Der Pfad ist als Grundlage für die weitere OpenXR-Ausgabe geeignet:

- 8K-HEVC-Main wird mit 59,94 fps vollständig hardwaredekodiert;
- zwölf NV12-CAPTURE-DMA-BUFs bleiben zyklisch in Benutzung;
- jeder Frame wird direkt über den Vulkan-YCbCr-Sampler projiziert;
- CAPTURE-Buffer werden erst nach abgeschlossenem GPU-Zugriff zurückgegeben;
- SBS Equirect 180 liefert sichtbare Stereo-Tiefenwahrnehmung;
- Kopfrotation steuert den Ausschnitt, während die Videoprojektion keine
  OpenXR-Augenposition als zusätzliche Geometrieparallaxe verwendet;
- der vollständige Zwei-Minuten-Lauf bleibt in Echtzeit und ohne reguläre
  Compositor-Drops oder Reprojection.

Flat, Mono, SBS und TB sind technisch durch die begrenzten Läufe abgedeckt.
Die Mathematik für Equirect 360 und die Naht-/Polgrenzen ist durch den Hosttest
abgedeckt; eine visuelle Abnahme mit passend markiertem realem 360°-Material
steht noch aus. Gleiches gilt für einen eigenständigen realen TB-Inhalt. Diese
offenen Materialprüfungen ändern den bestätigten SBS-180-Zielpfad nicht.

Audio, Container-Demux, UI, Seeking, Untertitel, Jellyfin und Main10 bleiben
außerhalb dieses Auftrags.
