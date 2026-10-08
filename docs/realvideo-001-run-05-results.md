# Realvideo 001: erfolgreicher 120-Sekunden-Lauf

Stand: 2026-10-08. Run 05 verarbeitete den vollständigen HEVC-Main-8-Bit-
Stream erfolgreich. Der Prozess endete mit Exitcode 0, ohne Timeout, V4L2-
oder Vulkan-Fehler.

## Reproduzierbarer Stand

| Bestandteil | Wert |
|---|---|
| Wiederaufnahme-Commit | `c73020d` |
| ausgeführte C-Quelle | SHA-256 `377135870ba604071e515356b9bce723a8c07a7457826b34579626ce42e9bd7c` |
| separates Zielbinary | SHA-256 `ccab04c6e718bc1c7e19914530b1b249ee56c15d871e2aaac8b9841ddd42ca4b` |
| HEVC-Stream | SHA-256 `f78332e37281b24c9c3a3304df785b914d97f84720f3cb5930e3bf0fcadcfa3d` |
| Prozessbegrenzung | `timeout -k 5s 420s` |
| Laufzeitlog | `tests/realvideo-001/realvideo-001-run-05.log` |
| Log-SHA-256 | `944eeb809500e021cdd93f2608176118649af10cc2cd83df5c3e05571c021728` |

## Decoder-, Reuse- und Durchsatzbefund

| Messwert | Ergebnis |
|---|---:|
| zugeführte Access Units | 7.191 |
| dekodierte CAPTURE-Frames | 7.191 |
| Frame-/AU-Differenz | 0 |
| Sequenz-Drops | 0 |
| V4L2-Fehler | 0 |
| SOURCE_CHANGE-Ereignisse | 2 |
| erfolgreiche CAPTURE-Wiederverwendungen | 1 |
| EOS-Event | 0 |
| leeres `LAST` am Drain-Ende | ja |
| Gesamtlaufzeit | 120,050850 s |
| effektive FPS | 59,899617 |
| Quellframerate | 59,940060 |
| maximaler Feed-Verzug | 90,220596 ms |

Beim SOURCE_CHANGE nach Frame 300 blieben Format und Speichervertrag bei NV12,
8192×4096, einer V4L2-Memory-Plane, Stride 8192 und
`sizeimage=50.331.648`. Die Compose-Region blieb unverändert. Alle zwölf
bereits importierten Buffer bestanden `QUERYBUF`; das nicht verfügbare
`V4L2_CID_MIN_BUFFERS_FOR_CAPTURE` wurde mit `EINVAL` dokumentiert. Der leere
LAST-Buffer wurde erneut gequeued und der weiterhin streamende Decoder mit
`V4L2_DEC_CMD_START` fortgesetzt. Es gab keinen `REQBUFS`, kein `CREATE_BUFS`,
keinen neuen DMA-BUF-Export und keinen neuen Vulkan-Import.

Die effektive FPS liegt 0,0675 % unter dem strikten Pacing-Ziel. Der komplette
Stream wurde trotzdem ohne Frameverlust, Stall-Timeout oder Fehler verarbeitet;
für eine spätere Wiedergabe bleibt die Pacing-Reserve ein Messpunkt, aber kein
Funktionsblocker dieses Decode-/Transfernachweises.

## Vulkan- und Pixelkorrektheit

| Messwert | Ergebnis |
|---|---:|
| GPU-Transfer, Mittelwert | 4,235276 ms |
| GPU-Transfer, Maximum | 6,268021 ms |
| Submit bis Fence, Mittelwert | 4,740742 ms |
| Submit bis Fence, Maximum | 10,770046 ms |
| geprüfte Frames | 0, 3595, 7190 |
| abweichende Bytes | 0 |

Der dauerhafte Pfad wartete für jeden Frame den Producer-SYNC_FD, kopierte die
beiden logischen NV12-Planes auf GPU-Seite, wartete den Fence ab und gab den
CAPTURE-Buffer erst danach an V4L2 zurück.

Die ursprünglichen zeitbasiert aus dem Container gesuchten Referenzen konnten
an GOP-Grenzen ein benachbartes Bild wählen. Die Referenzerzeugung dekodiert
jetzt den tatsächlich getesteten Annex-B-Stream sequenziell und wählt die drei
Frameindizes daraus aus. Diese Änderung ist in Commit `aebfe8f` festgehalten;
der neue Referenz-Hash ist
`4a1da6b3fe877b61b23348455d81ea3d9e2d4d3bac3caad6546679044df590b0`.
Der Vergleich in `tests/realvideo-001/comparison-run-05.json` bestätigt für Y,
U und V aller drei Frames jeweils null Abweichungen.

## Abschluss und nächster Meilenstein

Realvideo 001 ist abgeschlossen: Der reale 8K60-HEVC-Main-8-Bit-Pfad über Iris,
NV12-DMA-BUF und Vulkan-Transfer läuft über den vollständigen Stream stabil
und pixelgenau. Der Pfad ist bereit, in Realvideo 002 für sichtbare Ausgabe
weiterverwendet zu werden; Vulkan-YCbCr-Sampling und OpenXR-Swapchain-Ausgabe
sind damit noch nicht nachgewiesen.

Der nächste Entwicklungsmeilenstein ist **Realvideo 002 – erste sichtbare
OpenXR-Wiedergabe** mit dem Pfad:

```text
Iris -> NV12 DMA-BUF -> Vulkan YCbCr-Sampling -> OpenXR Swapchain
```

Es wurden keine Kernel-, Mesa-, Treiber-, Firmware-, Paket- oder
Systemänderungen vorgenommen und kein Recovery- oder Neustartversuch benötigt.
