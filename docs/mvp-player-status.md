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
- Der Video-Renderer besitzt ein zweites, transparentes OpenXR-Quadpanel mit
  Zeit/Duration, Zurück, -10 s, Play/Pause und +10 s. Pause hält Audio und
  Decoderfeed an, lässt Headtracking und den letzten Frame aber aktiv. Seek
  beendet die dateigebundenen Ressourcen kontrolliert und startet dieselbe
  Datei am vorherigen Keyframe neu; Vorlaufvideo und -PCM werden verworfen.

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

## Zielhardwarebefunde

Der erste Container-Integrationslauf stoppte vor Decoderstream, DMA-BUF und
OpenXR bei `VIDIOC_REQBUFS OUTPUT` mit `ENOMEM`. Ein Kontrolllauf mit dem
unveränderten, früher erfolgreichen `xr-nv12-playback-probe` reproduzierte den
Fehler an derselben Stelle mit demselben Iris-Layout
(`8192×4096`, OUTPUT-`sizeimage=16588800`). Es lief danach kein Probeprozess
und `/dev/video-dec0` war von keinem Prozess geöffnet. Das Frame meldete rund
9,8 GiB verfügbaren RAM und freie hohe Buddy-Orders.

Der Nutzer stellte anschließend fest, dass Playa parallel einen Stream offen
hielt. Nach dessen Beendigung funktionierten Iris-OUTPUT- und alle zwölf
50.331.648-Byte-CAPTURE-Allokationen sofort wieder. Der Befund ist damit als
Ressourcenkonflikt mit dem parallelen Player eingegrenzt; ein Neustart oder
Treiber-/Dienst-Recovery war nicht nötig.

Der danach ausgeführte 300-Frame-Containerlauf bestätigte 300/300 dekodierte
8K-Frames, keine Drops, keine V4L2-Fehler und 59,60 fps einschließlich Start-
und Drain-Overhead. Alle NV12-DMA-BUFs wurden importiert und alle 300 Übergaben
vom Renderer freigegeben. Die veröffentlichte Audio-Clock lag am letzten Frame
etwa 10 ms hinter dessen PTS. Das Headset war während dieses Messlaufs nicht
sichtbar, daher wurden die Frames vom XR-Renderer als `hidden` gezählt.

Der decoderfreie Browser lief auf dem Frame. Das Panel war sichtbar, und der
Nutzer bestätigte, dass sich das Highlight mit der Controller-Aim-Pose bewegt.
Eine sichtbare Laserlinie ist noch nicht implementiert.

## Noch offen

- Playback-Panel und seine vier Controlleraktionen praktisch im Headset
  validieren, einschließlich wiederholtem Seek und Browser-Rückkehr.
- Fehlertext-Rückgabe zum Browser ergänzen.
- Zielhardwaretests für Browser, Controller, A/V-Sync, wiederholtes Seeking,
  mehrere Dateien und einen längeren Lauf.
