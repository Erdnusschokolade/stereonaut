# Main10-Blocker: erneute Bewertung

Stand: 2026-10-10. Ausschließlich Auswertung vorhandener Dokumente, Logs,
Testquellen und früher gesicherter Mesa-Quellen; kein SSH-Zugriff, Decoderlauf
oder GPU-Test. Der installierte Treiberstand wurde heute nicht neu inventarisiert.

Nachtrag: Die danach freigegebene Inventur und decoderfreie Gegenprobe sind
separat in [main10-baseline-test.md](main10-baseline-test.md) dokumentiert.
Mit treibereigenem Layout blieben 3.580 falsche Y-Samples; Source und U/V
waren exakt korrekt. Die angekündigte bedingte Samplingprobe wurde nach
diesem Mismatch nicht gestartet. Die folgenden Abschnitte halten den
ursprünglichen Reviewstand vor dieser Ausführung fest.

## Was funktioniert und was fehlt

| Stufe | Nachweis aus den bisherigen Läufen |
| --- | --- |
| HEVC Main10 über Iris | 60/60 Frames für 1080p-Testbild und Verlauf; kein Softwarevideodecoder im Testpfad |
| Ausgabe | Q10C, coded 1920×1088, sichtbar 1920×1080, ein V4L2-Memory-Plane, Pitch 2560, Größe 4.210.688 Byte |
| DMA-BUF-Export | Erfolgreich |
| Lineares P010 aus Iris | Nicht verfügbar: Formatwunsch wurde ersetzt |
| Vulkan-Image mit explizitem QCOM-Layout | Erstellung, Speicheranforderung, Import/Binding und GPU-Submit erfolgreich |
| Korrekte Pixel nach GPU-Zugriff | Fehlgeschlagen; große strukturierte Abweichungen |
| Verlustfreier 10-Bit-Inhalt / 8K60 Main10 | Nicht nachgewiesen |

Quellen: [Decoderbefunde](decoder-investigation.md),
Importlauf (Ergebnis in der [Q10C-Recherche](q10c-interop-research.md)),
[Offline- und Selbsttestbefunde](q10c-readback-offline-analysis.md).
Die erfolgreichen 8K-8-Bit-Läufe belegen keine entsprechende Main10-Leistung.

## Entscheidender letzter Befund

Der explizite Producer-Sync-File-Wait beseitigte die Bildfehler nicht.
Anschließend scheiterte auch der decoderfreie Vulkan-Selbsttest (Lauf 08):
bekannte P010-Testdaten → explizites QCOM-Image → P010-Readback.
Dabei waren weder Iris, DMA-BUF noch externe Queue-Ownership beteiligt.

Der programmierte Vergleich meldete 1.044.538 abweichende 16-Bit-Wörter.
Auch der Vergleich der oberen zehn Bits mit dem deterministisch
rekonstruierten Eingang scheiterte deutlich. Zusätzlich waren 2.815 Wörter
des nach dem Fence gespeicherten Quellbuffers verändert. Das ist ein eigener
ungeklärter Befund; der Source-Dump ist keine unveränderte Vorher-Referenz.

Damit liegt mindestens ein Problem bereits im Vulkan-Test-/Layout-/Transferpfad.
Es ist weder ein Nachweis fehlender Main10-Decodierung noch bereits eine
eindeutige Diagnose eines Mesa-Fehlers. Die Quelle enthält keine explizite
Validation-Layer-Aktivierung; fehlerfreie API-Rückgaben allein validieren nicht
den gesamten Testvertrag.

## Wichtigste Korrektur der bisherigen Interpretation

`vkCreateImage == VK_SUCCESS` plus exakt passende Memory Requirements
bestätigt API-Akzeptanz und berechnete Allokationsgröße. Es bestätigt **nicht**
die semantische Kompatibilität von Iris-TP10/UBWC mit Turnips tatsächlicher
Pixeladressierung. Die Formulierungen in Auftrag 004 zum „bestätigten Layout“
müssen mit dieser Einschränkung gelesen werden.

Q10C verwendet gepackte TP10-Daten mit UBWC-Metadaten. Bei 1920 Pixeln beträgt
der Producer-Pitch 2560 Byte. Ein P010-Y-Plane mit 16 Bit pro Komponente hat
linear dagegen 3840 Byte pro Zeile. Das sind unterschiedliche physische
Repräsentationen; ein geänderter Pitch allein konvertiert sie nicht.

Im früher gesicherten Mesa-main-Quellstand aus Auftrag 004 führt
`tu6_plane_format()` dieses 10-Bit-Vulkanformat durch den generischen
Plane-Formatpfad. Die expliziten Offsets/Pitches werden in `tu_image.cc` an
`fdl6_layout_image()` weitergegeben. Der gesicherte FDL-Code übernimmt den
expliziten Pitch und prüft dort sein Alignment; diese Prüfung allein belegt
keine TP10-Unterstützung. Der 8-Bit-NV12-Fall besitzt hingegen eine explizite
Formatsonderbehandlung an dieser Stelle.

**Arbeitshypothese:** Der damalige Import beschreibt eine TP10-Allokation,
während wenigstens ein Turnip-Zugriffspfad sie wie P010-kompatible Einzelplanes
behandelt. Überlappende/falsch adressierte Tiles oder ein spezieller Copy-Fehler
wären damit vereinbar. Weder diese genaue Ursache noch eine dadurch verursachte
Speicherüberschreibung ist bewiesen. Der untersuchte öffentliche Mesa-Stand
ist nicht nachweislich identisch mit dem installierten Valve-Build
`26.3.0-devel (git-8aa73b4b19)`.

Quellstand und Links: [Mesa-Analyse in Auftrag 004](q10c-interop-research.md#mesaturnip).
Für diese Neubewertung waren die damaligen lokalen Kopien unter
`/tmp/stereonaut-source-004/` verfügbar; sie sind keine versionierten Artefakte.

## Vergleichsmethode präzisieren

Die sechs X-Paddingbits eines G10X6-Formats sind bei Vulkan-Copies undefiniert.
Der verbindliche Präzisionsvergleich muss deshalb die zehn Nutzbits vergleichen;
vollständige 16-Bit-Gleichheit ist nur eine zusätzliche Diagnose.
Siehe [Vulkan-Spezifikation: Copy Commands](https://docs.vulkan.org/spec/latest/chapters/copies.html).
Dies erklärt Lauf 08 ausdrücklich nicht: Auch die Nutzbits waren falsch.

## Empfohlene Fortsetzung

1. Zunächst read-only aktuellen Kernel-/Mesa-Stand mit den alten Befunden
   vergleichen und die tatsächliche TP10-Unterstützung im zugehörigen
   Format-, Textur- und Transferpfad klären. Keine Pitch-/Offset-Varianten raten.
2. Einen neuen, separat vorzulegenden Testplan vorbereiten: immutable
   CPU-Referenz und Source-Snapshot vor Submit, Vergleich der zehn Nutzbits,
   Kontrolle des Sourcebuffers nach Fence, verfügbare Validation nutzen.
   Zuerst eine gültige Vulkan-interne P010-Baseline mit vom Treiber gewähltem
   Layout prüfen; diese wäre kein Nachweis eines Iris-Q10C-Imports.
3. Erst nach geklärter Q10C-Repräsentation Copy- und Samplingpfad getrennt
   bewerten. Ein Copy-Fehler beweist nicht automatisch, dass der Texturpfad
   ebenfalls fehlschlägt. Ziel bleibt ein kontrollierter Vergleich gegen ein
   hochpräzises GPU-Ziel, bevor Decoder-DMA-BUF wieder hinzukommt.
4. Nach pixelkorrektem 1080p-Main10 erst höhere Auflösung, kontinuierlichen
   Betrieb und 8K60 messen. HDR-Farbwiedergabe ist ein separater Nachweis.

Noch kein neuer aktiver Test wurde vorbereitet oder ausgeführt. Erneute
Main10/Q10C-/UBWC-Experimente benötigen gemäß AGENTS.md eine konkrete
ausdrückliche Freigabe; die vorliegende Ursachenanalyse ist rein lesend.
