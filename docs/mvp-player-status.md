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
- Pause, ±10 s, Seek-Leiste mit Trickplay-Vorschau, Seek als Neustart mit
  gehaltenem letztem Bild; Griff zum Verschieben und Ein-/Ausblenden.
- Projektion, Stereo, Augentausch, Position, 6DoF und Bildwerte je Titel.
- H.264 nativ, HEVC Main10 nativ mit dem eigenen Turnip (`STEREONAUT_TP10`).
- Jellyfin-Wiedergabemeldungen (Start, Fortschritt, Pause, Stopp).
- Autofokus mit Eye Tracking (Grundfunktion; der eigene Reiter mit Schwelle,
  Hysterese und Überblenden ist neu, siehe unten).
- Tastenbelegung: Einzeltasten (Stick, A/B/X, Steuerkreuz) lösen ihre
  Aktionen aus und lassen sich neu belegen.

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
- Experimentell: AV1 8 Bit bis 4096×2160 per Software (dav1d), Schalter
  „AV1 bis 4K per CPU“ im Reiter „Ansicht“ (Standard aus, gilt ab dem
  nächsten Titel); Jellyfin spielt solche Titel dann direkt statt zu
  transkodieren. Ohne Headset bis 60 fps ohne Verluste, siehe
  `docs/hardware-investigation.md`.

## Bekannte Grenzen

- HEVC Main10 nur mit dem eigenen Turnip (`tools/mesa-tp10`); ohne ihn
  transkodiert Jellyfin bzw. lokale Main10-Dateien werden abgewiesen.
- Seek ist ein Neustart der Session (etwa eine Sekunde Standbild); eine A-B-
  Schleife wäre nur mit einem lückenlosen Bitstream-Splice sinnvoll
  (geschätzt 2–3 Tage).
- Passthrough ist nicht umgesetzt. SteamVR bietet auf dem Frame inzwischen
  `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` an (Abfrage 2026-10-10).
- Parallel laufende Player mit Iris-Nutzung (Playa) führen zu
  `VIDIOC_REQBUFS OUTPUT: ENOMEM`.
- Gelegentliches Stocken bei einem HLS-Transkodier-Titel ist ungeklärt.

## Offen

- Headset-Tests der Punkte oben.
- Ein dokumentierter Messlauf über mehrere Minuten (Framerate, Drops,
  A/V-Sync, Speicher).
