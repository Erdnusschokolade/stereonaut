# Realvideo 001: Ergebnis des SOURCE_CHANGE-Laufs 02

Stand: 2026-10-08. Der unter
`realvideo-001-source-change-test-plan.md` beschriebene Wiederholungslauf wurde
genau einmal ausgeführt. Er endete kontrolliert mit Exitcode 2 bei
`VIDIOC_REQBUFS CAPTURE: errno=12 (Cannot allocate memory)`. Es gab keinen
Timeout, Hänger, sichtbaren Recovery-Versuch oder weiteren aktiven Lauf.

## Reproduzierbarer Stand

| Bestandteil | Wert |
|---|---|
| SOURCE_CHANGE-Korrektur | Commit `ceb39dc` |
| ausgeführte C-Quelle | SHA-256 `f3551126cd070659a60ebb6501d47bdfd643b7f3954338a6cc9ea555cec1bee1` |
| separates Zielbinary | SHA-256 `9282b72d07facb05c91065fdb204d420d3b80fc336daa1b4eef0ee39349881ce` |
| unveränderter HEVC-Stream | SHA-256 `f78332e37281b24c9c3a3304df785b914d97f84720f3cb5930e3bf0fcadcfa3d` |
| Prozessbegrenzung | `timeout -k 5s 420s` |
| Ergebnis | Exitcode 2 nach der ersten dynamischen Rekonfiguration |
| Log | `tests/realvideo-001/realvideo-001-run-02.log` |
| Log-SHA-256 | `da58865085df57b6148bfa390607af11e79ae4268aab099a89bb451284c4ecd0` |

Die korrigierte Quelle wurde unter eigenem Namen in das bestehende
Testverzeichnis übertragen. Quell- und Streamhash wurden vor dem Build auf dem
Frame geprüft. Das separate Binary wurde mit den unveränderten Compilerregeln
gebaut; Dateien aus Lauf 01 wurden weder gelöscht noch überschrieben.

## Erfolgreich durchlaufene Sequenz

Der Initialpfad wiederholte die bereits bestätigten Fakten:

- Turnip bestätigte lineares zweispeicherplaniges NV12 mit
  `SAMPLED | TRANSFER_SRC`, DMA-BUF-Import und SYNC_FD.
- Iris lieferte 8192×4096 NV12, Stride 8192 und 50.331.648 Byte.
- Alle zwölf CAPTURE-DMA-BUFs wurden dediziert importiert; Offsets 0 und
  33.554.432, RowPitch 8192 und Vulkan Memory Requirement 50.331.648 Byte
  stimmten für jeden Buffer.
- Frame 0 bis Frame 299 wurden verarbeitet.
- Event 2 wurde als dynamischer `SOURCE_CHANGE` mit
  `V4L2_EVENT_SRC_CH_RESOLUTION` erkannt.
- Die alte Generation wurde bis zu einem leeren CAPTURE-Buffer mit `LAST`
  abgearbeitet. Zu diesem Zeitpunkt waren 300 Frames abgeschlossen.
- OUTPUT blieb streaming; es wurde kein OUTPUT-`STREAMOFF` ausgeführt.
- CAPTURE wurde danach gestoppt, die abgeschlossenen Vulkan-Imports und
  exportierten FDs wurden freigegeben und `REQBUFS(count=0)` ausgeführt.
- `G_FMT` und Compose-Region der neuen Streamkonfiguration wurden erneut
  abgefragt und ergaben unverändert lineares NV12 8192×4096, Stride 8192,
  `sizeimage=50.331.648` sowie denselben Y-/UV-Vertrag.

Damit ist die grundlegende Event-/Drain-Zustandsmaschine bestätigt. Der Lauf
scheiterte erst beim anschließenden `VIDIOC_REQBUFS` für die neue CAPTURE-
Generation. Es wurden keine neuen DMA-BUFs exportiert und keine neuen
Vulkan-Images importiert.

## Pixelkorrektheit und Checkpoint-Zeiten

Der einzige ausgewählte Readback ist Frame 0 mit 50.331.648 Byte und SHA-256
`775dba0fca937d3be19d607a8a44daff27df4e74ab8b0e22e383ea864795e040`.
Der Offlinevergleich in `tests/realvideo-001/comparison-run-02.json` ergab:

| Plane | Byte | Abweichungen | maximale absolute Abweichung |
|---|---:|---:|---:|
| Y | 33.554.432 | 0 | 0 |
| U | 8.388.608 | 0 | 0 |
| V | 8.388.608 | 0 | 0 |
| Gesamt | 50.331.648 | 0 | 0 |

Frame 0 benötigte 4,035937 ms GPU-Zeit und 4,678926 ms Submit-bis-Fence.
Frame 299 benötigte 4,011927 ms beziehungsweise 4,529349 ms. Wegen des
kontrollierten Abbruchs wurden erneut keine aggregierten Mittel-/Maximalwerte
ausgegeben; die beiden Checkpoints sind kein Langzeit-Durchsatznachweis.

## Einordnung von `ENOMEM`

Das Log beweist, dass der neue Formatvertrag vor dem Fehler weiterhin in die
bisherigen CAPTURE-Allokationen gepasst hätte. Der Fehler kam aus der
Neuanforderung der MMAP-CAPTURE-Buffer, nachdem die erste Generation
freigegeben worden war. Er ist damit von Hardwaredecode, Pixelinhalt,
Producer-Sync und Vulkan-Import getrennt.

Aus dem vorhandenen Log lässt sich nicht entscheiden, ob die erneute
Allokation wegen zusammenhängender Speicherfragmentierung oder wegen einer
geänderten, nicht protokollierten Mindestbufferzahl scheiterte. Der
V4L2/VB2-Unterbau darf `ENOMEM` liefern, wenn er die verlangte Bufferzahl und
-größe nicht erneut bereitstellen kann. Eine kleinere Bufferzahl wurde nicht
ausprobiert.

Die offizielle
[Capture-Setup-Sequenz für stateful Decoder](https://docs.kernel.org/userspace-api/media/v4l/dev-decoder.html#capture-setup)
definiert für einen dynamischen Wechsel einen besseren Kandidaten: Wenn
`sizeimage` der neuen Konfiguration in die vorhandenen Buffer passt und deren
Anzahl weiterhin mindestens dem aktuellen Minimum entspricht, dürfen die
vorhandenen CAPTURE-Buffer weiterverwendet werden. Bei noch streaming CAPTURE
wird anschließend `V4L2_DEC_CMD_START` verwendet; eine Neuanlage ist dann
nicht erforderlich. Die
[REQBUFS-Dokumentation](https://docs.kernel.org/userspace-api/media/v4l/vidioc-reqbufs.html)
bestätigt außerdem, dass MMAP-Buffer bei `REQBUFS` tatsächlich neu aus
Gerätespeicher allokiert werden.

## Status und nächster Erkenntnisschritt

Der kontinuierliche Zwei-Minuten-Nachweis bleibt offen. Der Pfad ist noch nicht
für OpenXR-Ausgabe freigegeben, obwohl zwei unabhängige Läufe den ersten
8K-Frame pixelgenau und die Einzeltransferzeit um 4,5 ms bestätigt haben.

Der nächste Quellschritt wäre eine streng gegatete Wiederverwendung der
bestehenden CAPTURE-Generation:

1. Nach dem dynamischen `LAST` neues Format, Compose-Region und aktuellen
   `V4L2_CID_MIN_BUFFERS_FOR_CAPTURE` abfragen.
2. Nur fortfahren, wenn Format, coded/visible Maße, Stride, `sizeimage`, Plane-
   Layout und Buffergröße exakt kompatibel bleiben und die vorhandenen zwölf
   Buffer das aktuelle Minimum erfüllen.
3. CAPTURE stoppen, die bestehenden DMA-BUF-/Vulkan-Imports behalten, alle
   vorhandenen Buffer erneut per `QBUF` einstellen und CAPTURE mit `STREAMON`
   fortsetzen; OUTPUT bleibt dabei aktiv.
4. Bei jeder Abweichung stoppen; keine kleinere Bufferzahl oder neue
   Allokationsstrategie im selben Lauf versuchen.

Diese funktionale Variante ist inzwischen lokal in Commit `9c48185`
vorbereitet und geprüft. Der exakte Diff, die Gates und der begrenzte
Laufvorschlag stehen in `realvideo-001-run-03-test-plan.md`. Der freigegebene
Lauf bestätigte den kompatiblen Speichervertrag und stoppte anschließend am
nicht verfügbaren `V4L2_CID_MIN_BUFFERS_FOR_CAPTURE`; Einzelheiten stehen in
`realvideo-001-run-03-results.md`. Es wurden keine Paket-, Kernel-, Treiber-,
Firmware- oder Systemänderungen vorgenommen.
