# Stereonaut – Q10C/P010-Readback offline analysiert

Stand: 2026-10-08. Die erste Analysephase dieses Entwicklungsauftrags 006
verwendete ausschließlich die lokal gesicherten Artefakte aus Lauf 04; dabei
gab es keinen Zugriff auf das Steam Frame und keinen V4L2- oder Vulkan-Lauf.
Der anschließend separat freigegebene einzelne Sync-File-Wiederholungslauf ist
am Ende dieses Dokuments ergänzt. Es gab keine Systemänderung.

## Ergebnis

**Entscheidung: Kategorie D.** Der GPU-Readback ist strukturell falsch und
spricht für eine falsche Interpretation der QC10C-/Vulkan-Repräsentation. Er
enthält gültig ausgerichtete 10-Bit-Werte und erkennbare blockweise Struktur,
aber kein räumlich korrekt rekonstruiertes Referenzbild. Kein Referenzframe,
keine begrenzte Geometrie- oder Pitchkorrektur und keine untersuchte einfache
Werte- oder Chromatransformation erklärt die Daten.

Das ist kein Gegenbeleg zur zuvor nachgewiesenen Iris-Hardwaredecodierung. Der
Lauf erreichte `HEVC Main10 → Iris → Q10C → DMA-BUF → Turnip/Vulkan` und einen
GPU-Readback. Nicht nachgewiesen ist weiterhin, dass Turnip die vom
Iris-Produzenten geschriebene Q10C-Allokation in diesem Importpfad korrekt
dekomprimiert und räumlich interpretiert.

Der spätere explizite Producer-Sync-File-Wait veränderte einzelne Blockbereiche,
stellte aber kein korrektes Bild her. Fehlendes Producer-Warten ist damit als
alleinige Erklärung stark abgeschwächt; gegenüber dem Layoutbefund bleibt
Synchronisation nur eine mögliche zusätzliche Fehlerquelle.

## Eingaben und Reproduzierbarkeit

| Artefakt | Größe | SHA-256 |
|---|---:|---|
| [`p010-readback.bin`](vulkan-video-diagnostics/run-04/p010-readback.bin) | 6.266.880 Byte | `8b30ec89b13f741303fd9623ad61f472c306594f18c20c48fca851534aff601b` |
| [`gradient10.hevc`](../tests/decoder-fixtures/gradient10.hevc) | 15.977 Byte | `343ab95a434112472268177e7730aa9e1478731d0a9be472522dfe3c7b47790d` |
| bisherige Frame-0-Referenz [`gradient10-reference.yuv`](../tests/decoder-fixtures/gradient10-reference.yuv) | 6.220.800 Byte | `7ddb0036ff37ee58dcf6cfd955f8f32a3bee78a8ebb2f304f67162e283aa7a8f` |

Das neue Analyseprogramm liegt unter
[`tools/vulkan-q10c-probe/analyze-q10c-readback.py`](../tools/vulkan-q10c-probe/analyze-q10c-readback.py).
Es benötigt Python 3, NumPy, Pillow und Matplotlib; SciPy wird nur für die
optionale optimale Bitpermutation verwendet. `ffmpeg` dekodiert den HEVC-Clip
frameweise nach `yuv420p10le`. Der ausgeführte Befehl war:

```sh
python3 tools/vulkan-q10c-probe/analyze-q10c-readback.py \
  docs/vulkan-video-diagnostics/run-04/p010-readback.bin \
  tests/decoder-fixtures/gradient10.hevc \
  docs/vulkan-video-diagnostics/run-05
```

Die vollständigen Maschinenresultate stehen in
[`analysis.json`](vulkan-video-diagnostics/run-05/analysis.json), die
Einzelwerte aller Frames in
[`frame-comparison.csv`](vulkan-video-diagnostics/run-05/frame-comparison.csv).
Das Programm prüft seinen P010-Parser zusätzlich mit einem synthetischen
Roundtrip: Ein bekannter 10-Bit-Referenzframe wird in die erwartete
1920×1088-P010-Anordnung gepackt und wieder gelesen. Y, U und V stimmen danach
für alle 3.110.400 Samples exakt; die Testgröße beträgt ebenfalls 6.266.880
Byte. Damit ist ein grundlegender Fehler in Plane-Trennung, sichtbarer Höhe,
UV-Interleaving oder `word >> 6` des Offline-Vergleichers ausgeschlossen.

## Vergleich mit allen 60 Referenzframes

`ffmpeg` lieferte genau 60 Frames. Wegen des synthetischen Testinhalts gibt es
vier unterschiedliche Frame-Hashes: Frame 0, Frames 1–29, Frame 30 und Frames
31–59. Der Hash von Frame 0 ist identisch mit der bisher verwendeten
`gradient10-reference.yuv`.

Numerisch ist Frame 31 der beste Kandidat. Der Abstand zu Frame 0 ist jedoch
vernachlässigbar: Der kombinierte RMSE beträgt 477,280471 statt 477,281280;
die Spanne über alle 60 Frames ist nur 0,000809. Ein falscher Referenzframe
erklärt den Fehler damit nicht.

| Plane gegen Frame 31 | abweichend / gesamt | Maximum | MAE | RMSE | Korrelation |
|---|---:|---:|---:|---:|---:|
| Y | 2.070.880 / 2.073.600 | 956 | 438,474 | 519,328 | −0,0648 |
| U | 349.359 / 518.400 | 960 | 295,609 | 421,940 | −0,2388 |
| V | 348.998 / 518.400 | 960 | 230,254 | 331,573 | 0,4260 |

## Luma und räumliche Struktur

Die sichtbare Y-Plane hat Minimum 0, Maximum 1023, Mittelwert 167,286 und
Standardabweichung 290,619. Sie enthält 601 verschiedene Werte. Alle unteren
sechs Bits sämtlicher P010-Wörter sind null; 469.547 sichtbare Luma-Werte sind
nach dem Rechtsschieben nicht durch vier teilbar. Der Readback enthält also
mehr als eine 8-Bit-Stufung. Wegen der falschen räumlichen Zuordnung ist das
weiterhin kein Nachweis eines korrekten verlustfreien Main10-Pfads.

Das auffällige Merkmal ist die blockweise Verteilung:

- 1.432.017 von 2.073.600 sichtbaren Y-Samples sind null, entsprechend
  69,06 %.
- 288 vollständige Zeilen und 97 vollständige Spalten sind null.
- Zeilen 568–639 sind vollständig null. Danach folgen bis zum Bildende
  wiederholt 16 Nullzeilen auf 16 nicht vollständig leere Zeilen; auch
  1072–1079 sind null.
- In den unteren acht Bildvierteln liegen die Nullanteile zwischen 88,51 % und
  96,92 %.
- Der mittlere Betrag des Sprungs zwischen Nachbarzeilen beträgt an
  8-Zeilen-Grenzen 69,81 statt 19,34, an 16-Zeilen-Grenzen 85,64 statt 21,63
  und an 32-Zeilen-Grenzen 83,10 statt 23,79.

Die Verteilung und die scharfen Potenz-von-zwei-Grenzen sprechen für falsch
zugeordnete komprimierte Blöcke beziehungsweise Metadaten. Sie sind nicht das
typische Bild einer bloßen Wertebereichsumrechnung.

Die vollständigen Histogramme liegen als
[`luma-histogram.csv`](vulkan-video-diagnostics/run-05/luma-histogram.csv) und
[`luma-difference-histogram.csv`](vulkan-video-diagnostics/run-05/luma-difference-histogram.csv)
vor; dazu gehören die Diagramme
[`luma-histogram.png`](vulkan-video-diagnostics/run-05/luma-histogram.png) und
[`luma-difference-histogram.png`](vulkan-video-diagnostics/run-05/luma-difference-histogram.png).

## Begrenzte Geometrie- und Pitchsuche

Die Geometriesuche prüfte reproduzierbar X-Verschiebungen von −128 bis +128,
Y-Verschiebungen von −16 bis +16 sowie horizontale und vertikale Spiegelungen.
Der nominell beste Treffer liegt mit `x=+127`, `y=+16` praktisch an beiden
Suchgrenzen und verlangt horizontale Spiegelung. Trotz der beschnittenen
Überlappung bleiben 1.907.200 von 1.907.752 Samples verschieden; die
Korrelation ist nur 0,1327, der RMSE 502,44. Das ist kein plausibler
geometrischer Treffer, sondern eine Randselektion aus weitgehend unpassenden
Daten.

Für die Zeileninterpretation wurden 1920 bis 1936, 2048 und 2560 16-Bit-Wörter
pro Zeile sowie Zeilenstarts 0 bis 8 geprüft. Darin ist die coded/visible
Differenz 1088/1080 enthalten. Der beste Stichprobentreffer verwendet 2560
**Wörter** beziehungsweise 5120 Byte pro Zeile, erreicht aber nur Korrelation
0,0348 und MAE 406,56. Diese Zahl ist nicht mit dem 2560-**Byte**-Pitch der
komprimierten Q10C-Producer-Plane gleichzusetzen. Kein getesteter linearer
Row-Pitch rekonstruiert das Bild.

## Wertetransformationen

Von den Rohinterpretationen ist die erwartete P010-Auswertung der oberen zehn
Bits bereits die beste. Untere zehn Bits, Byte-Swap, Bitreihenfolge und alle
zehn Rotationen der 10-Bit-Werte sind schlechter.

Geprüfte Umrechnungen waren Identität, `1023 - reference`, konstantes mittleres
und medianes Offset, lineare Regression, Full/Limited-Range-Abbildungen und
Links-/Rechtsschieben um ein bis vier Bits. Eine Regression reduziert den
RMSE nur durch Zusammenziehen in Richtung Mittelwert auf 290,00; ihre Steigung
ist −0,0744 und `R² = 0,00419`. Sie beschreibt keine belastbare Abbildung. Die
bestmögliche bijektive Zuordnung der zehn Einzelbits ist ebenfalls schlechter
als die normale Interpretation: RMSE 528,18 und Korrelation −0,110. Die
einzelnen Bitübereinstimmungen liegen nur ungefähr zwischen 0,50 und 0,53.

Damit liegt keine identifizierte systematische Wertetransformation vor.

## Chroma

Für Plane 1 wurden interleaved UV, interleaved VU sowie zwei angenommene
planare Hälften in UV- und VU-Reihenfolge geprüft. Die dokumentierte
interleaved-UV-Auswertung ist mit kombiniertem RMSE 379,46 bereits die beste;
U/V-Tausch ist mit RMSE 400,72 schlechter. Begrenzte Suchen von ±8
Chroma-Samples horizontal und ±4 vertikal ergeben weder für U und V noch für
die vertauschten Komponenten eine überzeugende Verbesserung. U bleibt sogar
negativ korreliert. Ein einfacher Deinterleaving-, U/V- oder Chroma-Siting-
Fehler erklärt den Readback daher nicht.

## Bildliche Diagnose

Die PNGs verwenden ausschließlich die offline gelesenen Planes:

- [Software-Referenz, Frame 31](vulkan-video-diagnostics/run-05/reference-frame-31.png)
- [GPU-Readback](vulkan-video-diagnostics/run-05/gpu-readback.png)
- [absolute Y-Differenz](vulkan-video-diagnostics/run-05/y-absolute-difference.png)
- [vorzeichenbehaftete Y-Differenz in Falschfarben](vulkan-video-diagnostics/run-05/y-difference-false-color.png)

Die Referenz ist ein sauberer horizontaler Luma-Verlauf mit einem
Chroma-Verlauf in der unteren Hälfte. Der GPU-Readback zeigt dagegen
wiederholte Streifen und Blöcke, großflächige Nullbereiche und in der unteren
Hälfte zerlegte Farbmuster. Er ist weder ein verschobenes noch ein bloß
wertetransformiertes Exemplar der Referenz. Visuell und statistisch ähnelt er
einer linearen Betrachtung falsch angeordneter Tiles beziehungsweise einer
nicht korrekt aufgelösten QCOM-Kompressionsrepräsentation.

## Synchronisationshypothese – Offline-Zwischenbewertung

Der vorhandene Lauf schloss `VIDIOC_DQBUF` vor dem Vulkan-Zugriff ab, hielt den
CAPTURE-Buffer bis nach dem signalisierten Vulkan-Fence aus der V4L2-Queue und
führte Ownership `FOREIGN → Vulkan → FOREIGN` aus. Das belegt die Reihenfolge
im Anwendungsablauf und verhindert eine Wiederverwendung des Buffers während
des GPU-Zugriffs. Es belegt für sich allein nicht, dass ein eventueller
Iris-Producer-Fence als explizite Vulkan-Abhängigkeit übergeben wurde.

Die Linux-DMA-BUF-Dokumentation beschreibt `dma_resv`-Fences für implizite
Synchronisation und für explizite APIs den Übergang über
`DMA_BUF_IOCTL_EXPORT_SYNC_FILE` und `DMA_BUF_IOCTL_IMPORT_SYNC_FILE`. Das
exportierte Producer-`sync_file` soll von der GPU-Arbeit abgewartet und deren
Abschluss-`sync_file` danach wieder in den DMA-BUF importiert werden. Vulkan
stellt Linux-`sync_fd` über `VK_KHR_external_semaphore_fd` bereit; diese
Extension ist auf dem untersuchten Gerät bereits inventarisiert. Die konkrete
Importfähigkeit des Handle-Typs wurde bislang nicht abgefragt und darf aus der
bloßen Extension-Liste nicht abgeleitet werden.

Ein fehlender Sync-File-Übergang ist deshalb technisch plausibel. Gegen eine
vorläufige Einordnung als Kategorie E sprechen jedoch:

- `DQBUF` war bereits abgeschlossen, bevor Vulkan las;
- der CAPTURE-Buffer wurde erst nach dem Vulkan-Fence zurückgegeben;
- die Ausgabe ist nicht bloß leer oder wie ein älterer vollständiger Frame,
  sondern zeigt klar an 8-/16-/32-Zeilen ausgerichtete Fragmente;
- keine der vier tatsächlich unterschiedlichen Software-Referenzen passt.

Ohne einen kontrollierten Sync-File-Lauf lässt sich die Hypothese nicht
endgültig ausschließen. Der Offline-Befund selbst weist stärker auf Layout/
Dekompression als auf fehlende Fertigstellung oder Cache-Sichtbarkeit.

Quellen:

- [Linux-Kernel-Dokumentation zu DMA-BUF und Sync-Files](https://docs.kernel.org/driver-api/dma-buf.html)
- [Vulkan `VK_KHR_external_semaphore_fd`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_external_semaphore_fd.html)
- [lokale Vulkan-Inventur](hardware-investigation.md#vulkan)

## Bewertung der Kategorien nach der Offlinephase

| Kategorie | Bewertung | Evidenz |
|---|---|---|
| A – Vergleicher falsch | ausgeschlossen | Parser-Roundtrip ist für alle Y/U/V-Samples exakt; Frame 0 stimmt per Hash mit der alten Referenz überein; alle 60 Frames wurden geprüft. |
| B – systematische Transformation | ausgeschlossen für den geprüften Suchraum | Geometrie, linearer Pitch, Wertebereich, Bit-/Bytevarianten, U/V und Chroma-Shifts liefern keinen plausiblen Treffer. |
| C – Bild erkennbar, Ursache offen | als Beschreibung zu schwach | Es gibt strukturierte Fragmente, aber kein räumlich zusammenhängendes Bild; Nullbereiche und Blockgrenzen sind dominant. |
| **D – QC10C-/Vulkan-Interpretation strukturell falsch** | **ausgewählt** | Tileartige Wiederholungen, 69,06 % Null-Luma, 288 Nullzeilen und starke 8-/16-/32-Zeilen-Grenzen bei gleichzeitig gültigen 10-Bit-Wörtern. |
| E – Synchronisation/Cache wahrscheinlicher | derzeit nicht ausgewählt | Der explizite Sync-File-Vertrag fehlt, doch DQBUF/Fence-Reihenfolge und das deterministische Blockmuster sprechen auf Basis eines einzelnen Laufs stärker für D. |

## Vorgelegter Sync-File-Testaufbau

Als kleinsten unterscheidenden Folgetest sollte **derselbe eine
`gradient10.hevc`-Lauf** mit unveränderten Codec-, Q10C-, Modifier-, Plane-,
Offset-, Pitch-, Image-, Copy- und Readbackparametern wiederholt werden. Die
einzige funktionale Änderung wäre eine explizite DMA-BUF-Synchronisationsbrücke:

1. Vor dem Decoderstream mit
   `vkGetPhysicalDeviceExternalSemaphoreProperties` prüfen, dass
   `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT` importierbar ist; andernfalls
   vor dem Stream stoppen.
2. Nach `DQBUF` und `EXPBUF` mit `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` und
   `DMA_BUF_SYNC_READ` einen Snapshot der Producer-Fences erzeugen.
3. Den `sync_fd` mit `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT` in ein
   Vulkan-Semaphore importieren und genau diese Semaphore beim bestehenden
   `vkQueueSubmit` abwarten.
4. Nach dem bestehenden Vulkan-Fence den CAPTURE-Buffer wie bisher erst dann
   zurückgeben. Für diesen nur lesenden Diagnosezugriff ist das CPU-Warten auf
   das Fence vor `QBUF` bereits die Verbrauchersequenz; ein zusätzliches
   Rückimportieren eines GPU-Read-Fences würde den Test um eine zweite Variable
   erweitern und wird daher für diesen ersten Vergleich nicht vorgeschlagen.
5. Den neuen 6.266.880-Byte-Readback bitgenau mit Lauf 04 und mit der
   Software-Referenz vergleichen. Keine weitere Variante im selben Auftrag.

Interpretation: Ein korrekter oder deutlich veränderter Readback würde die
Sync-Hypothese stärken. Ein identischer SHA-256 würde einen fehlenden
Producer-Wait als Ursache dieses Fehlerbilds stark abschwächen und die weitere
Arbeit auf die Q10C-/Turnip-Modifier-Abbildung konzentrieren.

Dieser Ablauf wurde anschließend mit den bisherigen Timeout- und Abbruchregeln
separat freigegeben. Das folgende Kapitel dokumentiert den einzigen Lauf.

## Ergebnis des freigegebenen Sync-File-Wiederholungslaufs

Der oben beschriebene Test wurde anschließend separat freigegeben und am
2026-10-08 genau einmal ausgeführt. Die Kandidatenquelle hatte lokal und auf
dem Steam Frame die SHA-256
`dc10048c04548b739b7d34f72ca1194ae8f10b3dcc0933b0e2eaab30ca9edd47`.
Sie wurde mit dem bisherigen Build-Limit sowie `-Wall -Wextra -Werror`
fehlerfrei kompiliert. Der Programmlauf mit `timeout -k 3s 15s` endete mit
Exitcode 0; es gab keinen Timeout oder gemeldeten Vulkan-/Iris-Fehler.

Die wesentlichen Befehle innerhalb des bestehenden Testverzeichnisses waren:

```sh
timeout -k 2s 10s cc -std=c11 -O2 -Wall -Wextra -Werror \
  -o v4l2-vulkan-q10c-sync-import \
  v4l2-vulkan-q10c-sync-import.c \
  $(pkg-config --cflags --libs vulkan)

timeout -k 3s 15s ./v4l2-vulkan-q10c-sync-import \
  gradient10.hevc p010-readback-sync.bin
```

Die nicht-streamende Vorprüfung lief vor dem Öffnen von `/dev/video-dec0`:

```text
sync_fd_preflight features=0x3 compatible=0x11 export_from_imported=0x11
sync_fd_preflight=importable decoder_stream_not_started_yet
```

`features=0x3` enthält `EXPORTABLE` und `IMPORTABLE`; der benötigte
`SYNC_FD`-Typ ist in `compatible=0x11` enthalten. Nach dem ersten erfolgreichen
Q10C-`DQBUF` und `EXPBUF` exportierte die Probe mit `DMA_BUF_SYNC_READ` einen
Producer-Sync-File als FD 6. Turnip importierte ihn mit
`VK_SEMAPHORE_IMPORT_TEMPORARY_BIT`, und der bestehende Transfer-Submit wartete
auf genau dieses Semaphore. Das Vulkan-Fence signalisierte anschließend; erst
danach wurde der CAPTURE-Buffer zurückgegeben. Wie freigegeben wurde kein
GPU-Completion-Sync-File in den DMA-BUF importiert.

Der Lauf erzeugte genau einen Readback mit 6.266.880 Byte. Seine SHA-256 ist
`6f9884a1414db9fa0f0c4574e1bcaa884bd50f7c59673d07e73b826128afce7d`.
Damit ist er **nicht** bitgleich mit Lauf 04
(`8b30ec89b13f741303fd9623ad61f472c306594f18c20c48fca851534aff601b`):

| Vergleich mit Lauf 04 | Ergebnis |
|---|---:|
| unterschiedliche Bytes | 286.594 / 6.266.880 |
| unterschiedliche 16-Bit-Wörter | 183.413 / 3.133.440 |
| unterschiedliche sichtbare Y-Samples | 57.901 / 2.073.600 |
| unterschiedliche U-Samples | 62.788 / 518.400 |
| unterschiedliche V-Samples | 62.724 / 518.400 |
| Korrelation Y / U / V | 0,9757 / 0,8650 / 0,7798 |

Die Änderungen folgen weiterhin der blockweisen Struktur. Das
[Differenzbild zu Lauf 04](vulkan-video-diagnostics/run-06/run-04-vs-sync-y-difference.png)
zeigt lokale Streifen- und Tilebereiche statt einer globalen Korrektur.

### Vergleich mit der Software-Referenz

Auch mit Producer-Semaphore bleibt Frame 31 der numerisch beste der 60
Softwareframes. Der kombinierte RMSE stieg leicht von 477,280 auf 478,015. Die
Korrelationen und Fehler bleiben unvereinbar mit einem korrekten Bild:

| Plane gegen Frame 31 | abweichend / gesamt | Maximum | MAE | RMSE | Korrelation |
|---|---:|---:|---:|---:|---:|
| Y | 2.070.140 / 2.073.600 | 956 | 439,169 | 519,978 | −0,0658 |
| U | 345.802 / 518.400 | 960 | 293,507 | 420,828 | −0,2217 |
| V | 345.298 / 518.400 | 960 | 230,495 | 335,242 | 0,4076 |

Der Readback enthält weiterhin formal P010-ausgerichtete Daten: Alle unteren
sechs Bits sind null, es gibt 605 Luma-Werte und 468.202 sichtbare Werte sind
nicht durch vier teilbar. Gleichzeitig sind 69,15 % der sichtbaren Luma-Werte,
288 vollständige Zeilen und 146 vollständige Spalten null. Die starken
8-/16-/32-Zeilen-Grenzen bleiben bestehen. Das neue
[Diagnosebild](vulkan-video-diagnostics/run-06/gpu-readback.png) zeigt dieselbe
Art von Tile-/Blockfehlinterpretation wie Lauf 04.

### Schlussfolgerung nach dem Sync-Test

Der explizite Producer-Wait war ausführbar und **nicht ausreichend**, um einen
korrekten Q10C-Readback herzustellen. Damit ist ein bloß fehlender
`DMA_BUF_IOCTL_EXPORT_SYNC_FILE`-/Vulkan-Wait auf die vom DMA-BUF
bereitgestellten Fences als alleinige Erklärung stark abgeschwächt. Der Lauf
beweist allerdings nicht, dass Iris überhaupt einen inhaltlich relevanten
Producer-Fence an `dma_resv` angehängt hatte; der exportierte Sync-File-Payload
wurde nicht zusätzlich inspiziert. Dass der Readback gegenüber Lauf 04 nicht
bitstabil ist, zeigt eine Timing- oder Zustandsabhängigkeit; bei nur je einem
Lauf mit und ohne Semaphore darf diese Differenz aber nicht eindeutig der
Synchronisation zugeschrieben werden. Sie kann ebenso aus nicht deterministisch
sichtbaren komprimierten Blöcken oder Metadaten bei der weiterhin falschen
Modifier-Interpretation entstehen.

Die Kategorie bleibt daher **D**. Kategorie E ist als mögliche zusätzliche
Ursache nicht vollständig ausgeschlossen, wird durch diesen erfolgreichen,
aber weiterhin falschen Producer-Wait jedoch nicht zur führenden Erklärung.
Ein vollständiger Produktionsvertrag müsste später auch den Consumer-Abschluss
für den nächsten implizit synchronisierten Nutzer korrekt abbilden; dieser
Rückweg war ausdrücklich nicht Teil des Diagnoseversuchs.

Belege:

- [Laufprotokoll](vulkan-video-diagnostics/run-06/v4l2-vulkan-q10c-sync.log)
- [Status und Prüfsummen](vulkan-video-diagnostics/run-06/status.txt)
- [ausgeführte Quelle](vulkan-video-diagnostics/run-06/v4l2-vulkan-q10c-sync-import.c)
- [Sync-Readback](vulkan-video-diagnostics/run-06/p010-readback-sync.bin)
- [vollständige Referenzanalyse](vulkan-video-diagnostics/run-06/analysis.json)
- [Bitvergleich mit Lauf 04](vulkan-video-diagnostics/run-06/run-04-vs-sync-comparison.json)

Es wurde kein zweiter Lauf und keine weitere Synchronisations-, Layout- oder
Importvariante ausgeführt.

## Decoderfreier Vulkan-Selbsttest

Der nächste Test sollte Decoder und DMA-BUF vollständig aus der Gleichung
nehmen und ausschließlich Turnips Selbstkonsistenz für das bestätigte explizite
QCOM-Layout prüfen:

1. Nicht-streamend abfragen, ob das Format mit QCOM-Modifier die kombinierte
   Usage `TRANSFER_DST | TRANSFER_SRC` unterstützt; andernfalls stoppen.
2. Ein dediziertes Vulkan-Image mit demselben expliziten Zwei-Plane-Layout,
   1920×1088 und exakt 4.210.688 Byte anlegen, jedoch ohne externen Speicher
   und ohne DMA-BUF.
3. Einen bekannten unkomprimierten P010-Frame über die beiden logischen Planes
   aus einem Staging-Buffer in das Image kopieren und anschließend über dieselben
   Plane-Aspekte wieder in einen zweiten unkomprimierten Buffer kopieren.
4. Nach einem Fence den zweiten Buffer CPU-seitig bitgenau mit dem ersten
   vergleichen.

Ein exakter Roundtrip würde Turnips Copy-/Dekompressionspfad für genau dieses
explizite Layout intern bestätigen und den Fehler auf den
Iris-Produzentenvertrag beziehungsweise dessen Metadatenabbildung eingrenzen.
Ein fehlerhafter Roundtrip würde den Vulkan-seitigen Layout-/Transferpfad selbst
belasten.

### Ergebnis des freigegebenen Einmallaufs

Dieser Test wurde anschließend separat freigegeben. Die neue Quelle wurde auf
dem Entwicklungsrechner mit den read-only nach `/tmp` kopierten öffentlichen
Vulkan-Headern des Frames geprüft:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror \
  -I/tmp/stereonaut-vulkan-headers -fsyntax-only \
  tools/vulkan-q10c-probe/vulkan-q10c-selftest.c
```

Die lokale Prüfung und die Kompilierung im bestehenden Frame-Testverzeichnis
endeten mit Exitcode 0. Die lokal und remote übereinstimmende SHA-256 der
ausgeführten Quelle war
`e1212141d483f9eab2c917f8336e47643633d69ece28042a09377c9e9502a7d9`.
Danach wurde genau ein Programmlauf gestartet:

```sh
timeout -k 3s 15s ./vulkan-q10c-selftest \
  p010-selftest-source.bin p010-selftest-readback.bin \
  vulkan-selftest-y-difference.pgm
```

Der Lauf endete mit Exitcode 2 und wurde nicht wiederholt. Die Capability-
Prüfung war vollständig erfolgreich:

```text
capability_modifier=0x0500000000000001 memory_planes=2 features=0x8ff001 transfer_src=1 transfer_dst=1
capability_combined_usage result=0 max_extent=16384x16384 max_mips=15 max_layers=2048
capability_preflight=supported gpu_work_not_started_yet
image_memory size=4210688 alignment=4096 type_bits=0x7 preferred_dedicated=0 required_dedicated=0
```

Damit sind der QCOM-Modifier mit zwei Memory-Planes und die kombinierte Usage
`TRANSFER_DST | TRANSFER_SRC` bestätigt. `vkCreateImage` war ebenfalls
erfolgreich; Größe, Alignment und Memory-Type-Bits entsprachen dem bestätigten
Layout.

Der Stopp wurde durch eine zu strenge Bedingung der Probe ausgelöst. Sie
verlangte zusätzlich `requiresDedicatedAllocation == 1`. Ohne External-Memory-
Deklaration meldet Turnip hier `0`; das verbietet keine freiwillig dedizierte
Allokation über `VkMemoryDedicatedAllocateInfo`. Der geplante Code wählte eine
solche Allokation ohnehin ausdrücklich. Größe, Alignment, Modifier, Planes,
Offsets und Pitches waren nicht die Ursache des Stopps.

Der Prozess stoppte vor `vkAllocateMemory`, Memory-Binding, Command-Buffer-
Submission und GPU-Arbeit. Deshalb entstanden weder Ausgangs- noch
Readback-Datei, Hashes, Abweichungszahlen oder ein Diagnosebild. Es gab keinen
V4L2-, Decoder-, DMA-BUF-, External-Memory- oder FOREIGN-Ownership-Zugriff.

Belege:

- [Laufprotokoll](vulkan-video-diagnostics/run-07/vulkan-q10c-selftest.log)
- [Status](vulkan-video-diagnostics/run-07/status.txt)
- [exakt ausgeführte Quelle](vulkan-video-diagnostics/run-07/vulkan-q10c-selftest.c)

### Vorgelegte Korrektur – nicht ausgeführt

Lokal wurde ausschließlich die fehlerhafte Zusatzbedingung entfernt. Die
korrigierte Probe prüft weiterhin Größe 4.210.688, Alignment 4096, Modifier,
Planes und alle festen Layoutparameter. Sie wählt weiterhin ausdrücklich eine
dedizierte Allokation; sie verlangt nur nicht mehr, dass der Treiber diese
Allokationsart zwingend vorschreibt. Die korrigierte Quelle besteht erneut die
lokale Prüfung mit `-Wall -Wextra -Werror` und hat SHA-256
`5f32593e3fdf2cb562a37194aac1cb45ef4a485a265c725f8b34d86facada412`.

Die Korrektur wurde weder auf das Frame übertragen noch ausgeführt. Ein
einmaliger Wiederholungslauf benötigt eine neue ausdrückliche Freigabe. Dabei
blieben alle Format-, Modifier-, Layout-, Plane-, Offset-, Pitch-, Usage-,
Copy-, Synchronisations- und Timeoutparameter unverändert.

Review-Artefakte: [korrigierte, nicht ausgeführte Quelle](vulkan-video-diagnostics/run-07/vulkan-q10c-selftest.corrected-not-executed.c)
und [Diff zur ausgeführten Quelle](vulkan-video-diagnostics/run-07/corrected-candidate.diff).

### Ergebnis des freigegebenen Korrekturlaufs

Der Korrekturlauf wurde danach genau einmal separat freigegeben und mit der
SHA-256 `5f32593e3fdf2cb562a37194aac1cb45ef4a485a265c725f8b34d86facada412`
ausgeführt. Die lokale Warnungsprüfung sowie die Kompilierung auf dem Frame
waren erfolgreich. Der Lauf mit `timeout -k 3s 15s` endete mit Exitcode 1,
weil der programmierte bitgenaue Vergleich fehlschlug. Es gab keinen Timeout
und keinen geloggten Vulkanfehler.

Der Test erreichte nun alle freigegebenen Stufen: Die kombinierte
`TRANSFER_DST | TRANSFER_SRC`-Capability wurde bestätigt, das Image verwendete
den QCOM-Modifier und die bestätigten zwei Memory-Planes, die Requirements
blieben bei 4.210.688 Byte und Alignment 4096, und die freiwillig dedizierte
Allokation wurde gebunden. Ein Command Buffer kopierte die beiden logischen
Planes einmal in das Image und einmal zurück; danach signalisierte das einzige
Fence. Es gab weiterhin keinen V4L2-, Decoder-, DMA-BUF-, External-Memory-
oder FOREIGN-Ownership-Zugriff.

| Artefakt | SHA-256 |
|---|---|
| deterministisch rekonstruierter, beabsichtigter P010-Eingang | `7192b373425caa2c4b0f1e6b3829969716a1b869ff0a39990a6c41818ee9d6c4` |
| nach dem Fence gesicherter Source-Buffer | `bce7ccb6df176b430fd4a55da25005052c466a74045912c639388aed127cb992` |
| GPU-Readback | `0ad8f4793283bb75af713746ab77f647997c80d213338833f5e198a88f422feb` |

Source und Readback haben jeweils 6.266.880 Byte. Der programminterne
16-Bit-Vergleich zählt 1.044.538 abweichende Wörter: 699.003 in Y und 345.535
interleaved UV-Komponenten. Die planeweise Offline-Auswertung trennt diese in
172.298 U- und 173.237 V-Wörter. Das erste abweichende Sample ist Y bei
`(x=0, y=0)`: gesicherter Source-Wert `44370` (10-Bit-Wert 693), Readback
`59840` (10-Bit-Wert 935), absolute Rohdifferenz 15.470.

Zusätzlich weicht der nach dem Fence gesicherte Source-Buffer in 2.815
Y-Wörtern vom deterministisch aus der Testformel rekonstruierten Eingang ab;
U und V stimmen dort vollständig. Der erste solcher Unterschied liegt ebenfalls
bei Y `(0,0)`: erwartetes Wort 0, gesicherter Source-Wert 44.370. Daher ist
der vor dem Submit tatsächlich befüllte Eingang über die deterministische
Testformel dokumentiert, während die nachträglich gesicherte Source-Datei nicht
als unveränderte Vorher-Aufnahme behandelt werden darf.

Der Readback weicht jedoch auch direkt vom rekonstruierten Eingang deutlich ab:
696.556 Y-, 172.298 U- und 173.237 V-Samples stimmen nicht. Das
[Diagnosebild](vulkan-video-diagnostics/run-08/intended-vs-readback-y-difference.png)
zeigt das wiederkehrende Tile-/Streifenmuster. Der decoderfreie Selbsttest
belastet damit den Vulkan-seitigen expliziten QCOM-Layout-/Transferpfad selbst;
ein Iris-Producervertrag kann dieses Ergebnis nicht erklären. Der unerwartet
veränderte nach-Fence-Source-Buffer bedeutet jedoch, dass dieser eine Lauf die
Fehlerquelle nicht weiter zwischen Modifier-Layout, Transferpfad und
Host-/Device-Sichtbarkeit der Testpipeline auftrennen kann.

Belege:

- [Laufprotokoll](vulkan-video-diagnostics/run-08/vulkan-q10c-selftest-rerun.log)
- [Status und Prüfsummen](vulkan-video-diagnostics/run-08/status.txt)
- [ausgeführte Quelle](vulkan-video-diagnostics/run-08/vulkan-q10c-selftest.c)
- [vollständige Offline-Auswertung](vulkan-video-diagnostics/run-08/selftest-analysis.json)
- [Source-Puffer](vulkan-video-diagnostics/run-08/p010-selftest-source.bin) und [Readback](vulkan-video-diagnostics/run-08/p010-selftest-readback.bin)

Es wurde kein weiterer GPU-Lauf und keine weitere Parameter- oder
Synchronisationsvariante ausgeführt.
