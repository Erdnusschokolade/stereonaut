# Realvideo 001: Ergebnis des einmaligen Steam-Frame-Laufs

Stand: 2026-10-08. Der unter `realvideo-001-plan.md` beschriebene Lauf wurde
genau einmal ausgeführt. Er endete kontrolliert mit Exitcode 2 am vorgesehenen
Stop-Gate für ein unerwartetes zweites `V4L2_EVENT_SOURCE_CHANGE`. Es gab
keinen Wiederholungslauf, keine Layout- oder Synchronisationsvariante und keine
Q10C-/Main10-Aktivität.

## Reproduzierbarer Stand

| Bestandteil | Wert |
|---|---|
| Probe-Commit | `2990475` (`test: add continuous NV12 Vulkan realvideo probe`) |
| Plan-Commit | `dd873e6` (`docs: plan continuous HEVC Main realvideo run`) |
| ausgeführte C-Quelle | SHA-256 `6d104c8db904c19bb234e68d88b86afb9280d4dae261e0e973d945aba32d8e3b` |
| Zielbinary | SHA-256 `25052dd6d7de9f02f278ea252d144edbedf0c02ccadcea7a9b9863ecc06cbbc6` |
| HEVC Elementary Stream | 755.880.785 Byte, SHA-256 `f78332e37281b24c9c3a3304df785b914d97f84720f3cb5930e3bf0fcadcfa3d` |
| Zielverzeichnis | `~/stereonaut-realvideo-001-20261008-01` |
| Prozessbegrenzung | `timeout -k 5s 420s` |
| tatsächliches Ergebnis | Exitcode 2 nach etwa fünf Sekunden Medienzeit |
| vollständiges Log | `tests/realvideo-001/realvideo-001.log`, SHA-256 `e1d5bc8765294ac0e2b4a085ef1c222ed2ace6370caf55949b8bb0e076a03085` |

Die Quelle wurde auf dem Frame mit `-std=c11 -O2 -Wall -Wextra -Werror`
kompiliert. Vor dem Build stimmten Quell- und Streamhash mit dem lokalen Stand
überein. Auf dem Ziel wurden ausschließlich das freigegebene Testverzeichnis,
die beiden übertragenen Dateien, das Binary, das Log und ein ausgewählter
Readback angelegt.

## Bestätigte Fähigkeiten

Der Lauf bestätigt erstmals den konkreten 8K-NV12-DMA-BUF-/Vulkan-Pfad:

| Prüfpunkt | tatsächliches Ergebnis |
|---|---|
| Hardwaredecoder | `/dev/video-dec0`, `iris_driver`, kein Softwarefallback |
| Eingabe | 8192×4096 HEVC Main 8 Bit, 7.191 AUD-delimitierte AUs, 60000/1001 fps |
| Vulkan-Gerät | Turnip Adreno 750, Vendor `0x5143`, 48-Bit-Timestamps |
| Format-Preflight | lineares NV12 mit zwei Modifier-Memory-Planes, `SAMPLED | TRANSFER_SRC`, DMA-BUF-importierbar |
| Producer-Sync | SYNC_FD als temporärer Vulkan-Semaphore-Payload importierbar |
| Iris CAPTURE | `NV12`, coded und sichtbar 8192×4096, ein V4L2-Speicherplane |
| Stride/Größe | Stride 8192, `sizeimage=50.331.648` Byte |
| lineares Plane-Layout | Y Offset 0, UV Offset 33.554.432, beide RowPitch 8192 |
| DMA-BUFs | zwölf Buffer exportiert, jeder exakt 50.331.648 Byte |
| Vulkan-Imports | zwölf dedizierte Imports; Requirements jeweils 50.331.648 Byte, Alignment 64, Memory-Type-Schnittmenge `0x7` |
| GPU-Arbeit | pro verarbeitetem Frame zweiplaniger NV12-Transfer, Producer-Sync, Fence und anschließendes V4L2-Requeue |
| verarbeitete Frames | Frame 0 bis Frame 299; danach Stop vor Fortsetzung |

Der erste protokollierte Frame benötigte 4,128281 ms GPU-Timestamp-Zeit und
4,680790 ms von Submit bis Fence. Am Checkpoint Frame 299 waren es 3,985781 ms
beziehungsweise 4,451739 ms. Diese zwei Checkpoints belegen eine komfortable
Einzeltransferzeit unter dem 16,683-ms-Framebudget. Sie sind kein statistischer
Mittel-/Maximalwert des Laufs.

Iris meldete bei beiden protokollierten CAPTURE-Buffern `sequence=0`. Das
V4L2-Sequenzfeld ist damit auf diesem Treiber nicht als Drop-Zähler brauchbar;
eine nächste Probe muss ihre fortlaufende Framezahl und Timestamp-Zuordnung als
primäre Vollständigkeitsprüfung verwenden.

## Pixelkorrektheit

Nur Frame 0 wurde vor dem Stop-Gate als ausgewählter GPU-Readback erzeugt. Der
Readback ist 50.331.648 Byte groß und hat SHA-256
`775dba0fca937d3be19d607a8a44daff27df4e74ab8b0e22e383ea864795e040`.
Der DMA-BUF selbst wurde nicht auf der CPU gelesen.

Der Offlinevergleicher deinterleavte den unkomprimierten NV12-GPU-Zielbuffer
und verglich ihn mit der Software-Referenz desselben Frames:

| Plane | verglichene Byte | Abweichungen | maximale absolute Abweichung |
|---|---:|---:|---:|
| Y | 33.554.432 | 0 | 0 |
| U | 8.388.608 | 0 | 0 |
| V | 8.388.608 | 0 | 0 |
| Gesamt | 50.331.648 | 0 | 0 |

Der sichtbare I420-Inhalt und die Referenz besitzen denselben SHA-256-Wert wie
oben. Das Ergebnis steht in `tests/realvideo-001/comparison-run-01.json`. Damit
sind für diesen Frame Hardwaredecode, lineare NV12-Auslegung, Producer-Sync,
Vulkan-Import und zweiplaniger GPU-Transfer zusammen pixelgenau bestätigt.

## Abbruchursache

Nach Frame 299 mit Timestamp 4,988316 Sekunden lieferte Iris ein weiteres
`V4L2_EVENT_SOURCE_CHANGE`. Die vorbereitete Probe hatte für den ersten Lauf
bewusst nur die initiale Formatfestlegung freigegeben und brach mit
`unexpected second SOURCE_CHANGE` ab, statt CAPTURE-Buffer während des aktiven
Streams ungeprüft neu zu konfigurieren.

Die lokale Annex-B-Analyse grenzt den Trigger auf Access Unit 300 ein. Sie
beginnt an der Fünf-Sekunden-GOP-Grenze mit:

```text
AUD, VPS, SPS, PPS, VPS, SPS, PPS, IDR_W_RADL
```

Die Access Units 297 bis 299 und 301 bis 302 enthalten dagegen jeweils nur AUD
und eine reguläre Slice-NAL. Die erste VPS/SPS/PPS-Gruppe in AU 300 ist
bytegleich zu den Parametern am Streambeginn. Die zweite Gruppe behält
NAL-Längen und PPS bei, ändert jedoch Bytes in VPS und SPS. Das erklärt einen
neuen Iris-Source-Change plausibel. Weil die Probe am Event vor einem zweiten
`VIDIOC_G_FMT` stoppte, ist nicht aktiv nachgewiesen, ob Iris dabei dieselben
sichtbaren/coded Maße und dasselbe NV12-Layout zurückgegeben hätte.

## Nicht erreichte Abnahmepunkte

Der Lauf dauerte nur bis zur ersten GOP-Grenze. Daher sind folgende Punkte
noch offen:

- vollständige Verarbeitung der 120 Sekunden und aller 7.191 Frames;
- Drops/Count-Differenz über den ganzen Clip;
- mittlere und maximale Decode-, GPU- und Submit-bis-Fence-Zeiten;
- Gesamtdauer und dauerhafter Echtzeitfaktor;
- mittlerer und letzter Referenzframe;
- Stabilität über mehrere Minuten.

Die Probe druckt ihre aggregierten Statistiken erst bei normalem Drain-Ende.
Der kontrollierte Stop über `fail_message` hat sie deshalb nicht ausgegeben.
Die beiden Checkpoint-Zeiten dürfen nicht als Ersatz für diese Metriken
interpretiert werden.

## Architektur-Einschätzung und nächster Schritt

Der 8K-Pfad ist technisch deutlich weiter als der Q10C-Pfad: Ein echter
Iris-NV12-DMA-BUF wurde korrekt und pixelgenau in Turnip importiert und in etwa
4,5 ms Submit-bis-Fence gelesen. Der Pfad ist dennoch noch nicht bereit für
OpenXR, weil reale GOP-Grenzen eine wiederholte CAPTURE-Rekonfiguration
auslösen und der Mehrminuten-/Durchsatznachweis dadurch fehlt.

Der nächste reproduzierbare Quellschritt ist eine V4L2-State-Machine für
wiederholte `SOURCE_CHANGE`-Events:

1. Neue OUTPUT-Zufuhr kontrolliert anhalten, ohne Access Units zu verlieren.
2. Alle bereits dequeueten CAPTURE-Buffer erst nach ihren Vulkan-Fences
   freigeben.
3. CAPTURE gemäß dem stateful-decoder-Protokoll stoppen, Buffer freigeben und
   Format sowie Compose-Region erneut abfragen.
4. Nur ein erneut vollständig validiertes NV12-Layout akzeptieren.
5. Alte Vulkan-Imports nach abgeschlossenem GPU-Zugriff zerstören, neue
   CAPTURE-DMA-BUFs exportieren/importieren und Streaming fortsetzen.
6. Auch bei einem kontrollierten Stop die bis dahin gesammelten Statistiken
   ausgeben; Vollständigkeit über eigene Frame-/Timestamp-Zählung prüfen.

Diese Änderung ist ein neuer aktiver Testpfad und wurde im vorliegenden Lauf
nicht ausprobiert. Sie benötigt vor einer erneuten Ausführung eine neue,
konkrete Freigabe. Es wurden keine Kernel-, Treiber-, Firmware-, Paket- oder
Systemänderungen vorgenommen und kein Recovery-/Neustartversuch durchgeführt.

Die Zustandsmaschine wurde anschließend rein lokal im Commit `ceb39dc`
vorbereitet und mit GCC sowie Clang unter `-Wall -Wextra -Werror` geprüft. Sie
folgt der offiziellen
[stateful V4L2 Dynamic Resolution Change sequence](https://docs.kernel.org/userspace-api/media/v4l/dev-decoder.html#dynamic-resolution-change).
Sie wurde noch nicht auf dem Steam Frame ausgeführt. Der exakt begrenzte
Wiederholungsvorschlag steht in `realvideo-001-source-change-test-plan.md`.
