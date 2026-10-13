# Stereonaut – Projektstand

Stand: 2026-10-11, Branch `main` (zuvor `player/mvp` und
`hardware/main10-baseline`)

Der erste vollständige Player (MVP) ist abgeschlossen; er wird
seitdem im Alltag auf dem Steam Frame genutzt und um Komfortfunktionen
erweitert. Messungen und Befunde stehen in den übrigen Dokumenten unter
`docs/`, die Architektur in `README.md` und `src/player/README.md`.

## Videopfad

```text
MKV/MP4 -> libavformat (hevc_/h264_mp4toannexb)
        -> HEVC Main 8 | H.264 8 Bit 4:2:0 | HEVC Main10 (nur mit eigenem Turnip)
        -> Iris V4L2 -> NV12/TP10 DMA-BUF -> Vulkan -> OpenXR
Audio:  libavcodec/libswresample -> 48 kHz S16 -> ALSA (Master-Clock)
```

Jellyfin liefert Direct Play für diese Formate und transkodiert alles andere
nach HEVC Main 8 (Server-Grenze 4096 px Breite). Experimentell und nur nach
Einschalten decodiert dav1d AV1 bis 4K auf der CPU (siehe unten).

## Im Headset bestätigt

- Browser ↔ Wiedergabe über den dauerhaften OpenXR-Host, Rückkehr in Ordner
  und Seite, Fehlertexte, Start über KDE und die SteamVR-Bibliothek.
- Lokale Dateien und Jellyfin (Quick Connect, Bibliotheken, Vorschaubilder,
  Direct Play und Transkodierung, 4K bis 8K60).
- Pause, ±10 s, Seek-Leiste mit Trickplay-Vorschau; Griff zum Verschieben
  und Ein-/Ausblenden.
- Spulen ohne Neustart der Session auf den Keyframe in Spulrichtung (siehe
  Grenzen), das letzte Bild bleibt bis zum neuen stehen. Bis zum ersten
  Bild über Jellyfin: H.264 4K meist 0,15–0,2 s, HEVC 8K um 0,25 s,
  HEVC Main10 8K 0,3–0,6 s; vorher 1,3 s und mehr (Neustart der Session).
  Main10 lehnt Iris nach dem Anhalten der Queues ab (`STREAMON` ENOMEM mit
  Kernel-Warnung, bei 8 Bit gelegentlich auch); dann öffnet die Session
  Iris neu und behält Quelle, Jellyfin-Verbindung und Renderer. Der
  Neustart der Session bleibt nur als letzter Rückfall.
- Flaches Video: verkleinert gezeichnetes Video wird über mehrere Abtastungen
  gemittelt (4K flach statt Treppen an scharfen Kanten), die Ränder der
  Leinwand sind geglättet. Renderer-GPU-Last am Frame: 4K flach 28 %,
  8K 180° 16 %.
- PCVR über SteamVR am PC, mit dem Frame per Streaming im Headset geprüft:
  Wiedergabe, Spulen und Panels. Die Panels zeichnet der Renderer am PC
  selbst ins Augenbild (Mipmaps, anisotrop); als Quad-Layer flimmerte Schrift
  in SteamVR am PC bei Kopfbewegung. Am PC wirkt die Schrift dadurch etwas
  weicher als nativ auf dem Frame.
- Projektion, Stereo, Augentausch, Position, 6DoF und Bildwerte je Titel.
- H.264 nativ, HEVC Main10 nativ mit dem eigenen Turnip (`STEREONAUT_TP10`).
- Jellyfin-Wiedergabemeldungen (Start, Fortschritt, Pause, Stopp).
- Autofokus mit Eye Tracking (Grundfunktion; der eigene Reiter mit Schwelle,
  Hysterese und Überblenden ist neu, siehe unten).
- Tastenbelegung: Einzeltasten (Stick, A/B/X, Steuerkreuz) lösen ihre
  Aktionen aus und lassen sich neu belegen.
- Oberfläche auf Englisch und Deutsch: Die Sprache folgt automatisch der
  Steam- bzw. Systemsprache (am Frame steht `LANG` fest auf `en_US`, daher
  zählt die Steam-Sprache aus `~/.steam/registry.vdf`), ohne Übersetzung
  Englisch; Wechsel unter Einstellungen → Sprache wirkt sofort.
- Ausrichtung: Stereonaut richtet Browser, Player-Panel und Video beim Start
  nach der Kopfhaltung aus (auch wenn das Headset beim Aufwachen zur Decke
  zeigte) und behält das über Titel hinweg; „Zentrieren“ im Player und
  SteamVRs Neuzentrieren setzen es neu.

## Auf dem Frame gebaut, Headset-Test offen

Gebaut, installiert und per Kurzlauf ohne Headset bzw. per `ui-preview`
geprüft:

- Fortsetzen-Dialog, Fortschrittsbalken und „Gesehen“ auf den Kacheln,
  „Weiterschauen“ auf der Startseite.
- Lautstärke, „Am Ende: Stopp/Wiederholen/Nächster“, automatisches
  Ausblenden des Panels, Pause bleibt beim Spulen erhalten.
- Kapitelmarken und Reiter „Info“.
- Formaterkennung ohne Dateinamen (am Frame mit Jellyfin-Trickplay und
  Decoderpuffern im Log bestätigt, im Headset noch nicht gesehen).
- Fisheye-Projektion (äquidistantes Modell, an echtem Fisheye-Material
  ungeprüft).
- Filter nach Genre/Studio/Darsteller, Suche mit Bildschirmtastatur,
  Sortierung lokaler Ordner, Vorschaubilder lokaler Dateien.
- Reiter „Fokus“: Auslösen nur unter einem Abstand, Hysterese, Überblenden.
- Kombinationen mit dem Griff („R Griff + Stick →“): der Griff wurde beim
  Belegen nicht erkannt; Korrektur über den analogen Griffwert eingebaut,
  noch nicht bestätigt (`binding_learn … grip_value=` im Log).
- Schnelleres Öffnen von Bibliotheken (Liste ohne Stream-Infos, Codec und
  Auflösung werden seitenweise nachgeladen; VR-Bibliothek 0,25 s statt 2 s).
- PCVR: Ohne Iris decodiert die Session am PC mit FFmpeg-Hardwaredecodern
  (CUDA, Vulkan Video, VA-API) H.264, HEVC und AV1 mit 8/10 Bit; der
  Renderer kopiert die Bilder aus geteiltem Speicher in Grafikspeicher. Am
  PC (RTX 4080) ohne Headset gemessen: bis 8192×4096 bei 60 fps ohne
  Verluste, Seek 25 ms lokal. Im Headset nur mit dem Frame per Streaming
  geprüft (siehe oben), andere PC-Headsets ungetestet.
- Experimentell: AV1 8 Bit bis 4096×2160 per Software (dav1d), Schalter
  „AV1 bis 4K per CPU“ im Reiter „Ansicht“ (Standard aus, gilt ab dem
  nächsten Titel); Jellyfin spielt solche Titel dann direkt statt zu
  transkodieren. Ohne Headset bis 60 fps ohne Verluste, siehe
  `docs/hardware-investigation.md`.

## Bekannte Grenzen

- HEVC Main10 nur mit dem eigenen Turnip (`tools/mesa-tp10`); ohne ihn
  transkodiert Jellyfin bzw. lokale Main10-Dateien werden abgewiesen.
- Seek landet auf dem Keyframe, der dem Ziel am nächsten liegt, aber
  mindestens die halbe Strecke in Spulrichtung zurücklegt; nicht
  sekundengenau. Bei 10 s Keyframe-Abstand springen ±10 s also um 10 s,
  bei längeren Abständen weiter. Eine lückenlose A-B-Schleife bräuchte
  zusätzlich einen Bitstream-Splice mit durchlaufender Zeitachse.
- Passthrough ist nicht umgesetzt. SteamVR bietet auf dem Frame inzwischen
  `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` an (Abfrage 2026-10-10).
- Parallel laufende Player mit Iris-Nutzung (Playa) führen zu
  `VIDIOC_REQBUFS OUTPUT: ENOMEM`.
- Gelegentliches Stocken bei einem HLS-Transkodier-Titel ist ungeklärt.

## Offen

- Headset-Tests der Punkte oben.
- Ein dokumentierter Messlauf über mehrere Minuten (Framerate, Drops,
  A/V-Sync, Speicher).
