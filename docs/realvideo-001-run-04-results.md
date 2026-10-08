# Realvideo 001: Ergebnis von Lauf 04

Stand: 2026-10-08. Lauf 04 wurde genau einmal mit
`timeout -k 5s 420s` ausgeführt und endete kontrolliert mit Exitcode 2, nicht
durch den äußeren Timeout.

| Bestandteil | Wert |
|---|---|
| Quellcommit | `ba240bd` |
| Quell-SHA-256 | `f17c3cbc2819061e02a75a08097c71987ed601fd60b90908990adff1396c99e4` |
| Zielbinary-SHA-256 | `8e0b5e273651ac6d1da2aac60f66e534edf8225b0bfdb04e0533f6f41a99f3b7` |
| Stream-SHA-256 | `f78332e37281b24c9c3a3304df785b914d97f84720f3cb5930e3bf0fcadcfa3d` |
| Log-SHA-256 | `9cc52783b4f9a812aa5f5f4044bfa7466d3e6c0d760273f764f9cab56cf8fee3` |

Bis zum wiederholten SOURCE_CHANGE wurden 300 Frames verarbeitet. Frame 0
blieb bytegenau zur Referenz: Y, U und V hatten jeweils null Abweichungen;
Readback-SHA-256 und Referenz-SHA-256 sind beide
`775dba0fca937d3be19d607a8a44daff27df4e74ab8b0e22e383ea864795e040`.

Nach dem leeren `LAST` bestätigte die Probe NV12, 8192×4096, Stride 8192,
`sizeimage=50.331.648`, unveränderte Compose-Region, zwölf Buffer und alle
zwölf `QUERYBUF`-Längen. `V4L2_CID_MIN_BUFFERS_FOR_CAPTURE` lieferte erneut
`EINVAL` und wurde wie vorgesehen als nicht verfügbares Control protokolliert.
Es gab keinen `REQBUFS`, keinen `CREATE_BUFS`, keinen neuen DMA-BUF-Export und
keinen neuen Vulkan-Import.

Der anschließende CAPTURE-`STREAMON`-Aufruf scheiterte mit `ENOMEM`. Das grenzt
den Fehler auf die Wiederaufnahme nach `STREAMOFF` ein; die Buffer- und
Vulkan-Kompatibilität waren bereits bestätigt. Ein wiederholter Lauf wurde
nicht gestartet.

Der nächste vorbereitete Kandidat in `realvideo-001-run-05-test-plan.md`
behält CAPTURE streaming, reiht nur den leeren LAST-Buffer erneut ein und
verwendet `V4L2_DEC_CMD_START`, wie für diesen kompatiblen Zustand vorgesehen.
