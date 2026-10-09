# Entwicklungsauftrag 011 – MVP-Implementierungsstand

Stand: 2026-10-09, Branch `player/mvp`

## Implementierter Pfad

- libavformat demuxt MKV/MP4, erhält Container-Zeitstempel und wandelt HEVC
  mit `hevc_mp4toannexb` für den direkten Iris-V4L2-Pfad um.
- libavcodec und libswresample dekodieren die gewählte Audiospur nach
  48-kHz-S16-Stereo; ALSA `default` liefert die Audio-Masterclock.
- Begrenzte Audio-/Videoqueues laufen in getrennten Threads. Der initiale
  Audiostart wartet auf Iris-`SOURCE_CHANGE` und den bereiten XR-Renderer.
- Die Session übernimmt den bestätigten NV12-DMA-BUF-/Vulkan-/OpenXR-Pfad und
  terminiert Video anhand der Audio-Clock. Frühe Frames werden gehalten;
  Frames mit mehr als 100 ms Verspätung dürfen übersprungen werden.
- Der Medien-Pipeline-Seek stoppt und verbindet beide Worker neu, leert die
  Queues, springt auf den vorherigen Keyframe und setzt Audio zurück. PCM vor
  dem Ziel wird vollständig verworfen; der überlappende Audioblock wird auf
  48-kHz-Samplegrenze angeschnitten.
- Ein asynchroner lokaler Dateibrowser filtert `.mkv` und `.mp4`, sortiert
  Verzeichnisse und Dateien und ersetzt veraltete Listing-Ergebnisse.
- Die decoderfreie native OpenXR-Dateiauswahl besitzt ein Vulkan-Quadpanel,
  Controller-Aim-Ray, Trigger-Auswahl und Seitenwechsel. Der Launcher wechselt
  zwischen Browser- und Playback-Prozess.

## Lokale Nachweise

- Alle C-Quellen bauen mit `-Wall -Wextra -Werror`.
- MKV und MP4 wurden durch die Demux-Probe gelesen; Annex-B und monotone
  Zeitstempel wurden bestätigt.
- Pipeline-Seek auf 10 s liefert für die MP4-Fixture den vorherigen Keyframe
  bei 9,539 s.
- Der Audio-Seek-Test erreicht 11,008167 s und gibt keine Samples vor 10 s an
  das Ausgabemodell weiter.
- Der Browser-Test listet nur Verzeichnisse sowie MKV/MP4 und sortiert
  unabhängig von Groß-/Kleinschreibung.

## Aktueller Zielhardwarezustand

Der erste Container-Integrationslauf stoppte vor Decoderstream, DMA-BUF und
OpenXR bei `VIDIOC_REQBUFS OUTPUT` mit `ENOMEM`. Ein Kontrolllauf mit dem
unveränderten, früher erfolgreichen `xr-nv12-playback-probe` reproduzierte den
Fehler an derselben Stelle mit demselben Iris-Layout
(`8192×4096`, OUTPUT-`sizeimage=16588800`). Es lief danach kein Probeprozess
und `/dev/video-dec0` war von keinem Prozess geöffnet. Das Frame meldete rund
9,8 GiB verfügbaren RAM und freie hohe Buddy-Orders.

Damit ist dieser konkrete Fehler nicht durch Containerdemux, Audio oder den
neuen Scheduler verursacht. Er wird als aktueller Geräte-/Treiber-
Allokationszustand behandelt. Es erfolgte weder ein Neustart noch ein
Treiber-/Dienst-Recoveryversuch. Decoderaktive Tests bleiben bis zu einem
später autorisierten Systemzustandswechsel ausgesetzt; decoderfreie OpenXR-
UI-Tests können unabhängig davon laufen.

## Noch offen

- Playback-Panel sichtbar in den Video-Renderer integrieren.
- Play/Pause und ±10-s-Aktionen aus dem Renderer an die Session anbinden.
- V4L2 kontrolliert drainen/neu starten und vor dem Seek-Ziel dekodierte
  Videoframes verwerfen.
- Zurück-Aktion aus laufender Wiedergabe und Fehlertext-Rückgabe zum Browser.
- Zielhardwaretests für Browser, Controller, A/V-Sync, wiederholtes Seeking,
  mehrere Dateien und einen längeren Lauf.
