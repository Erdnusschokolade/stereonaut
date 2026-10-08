# Stereonaut – Decoder-Untersuchung 002

Stand: 2026-10-08. **Hardware-HEVC Main und Main10 sind über den Iris-V4L2-Treiber nachgewiesen.** Main10 liefert Q10C, einen Qualcomm-komprimierten 10-Bit-CAPTURE-Buffer, nicht P010. Der 10-Bit-Pixelinhalt ist damit noch nicht auswertbar; der verlustfreie Präzisionserhalt ist ausdrücklich nicht nachgewiesen.

## Testverfahren und Freigabe

Alle Verbindungen verwenden den bestehenden Alias `frame`. Der Nutzer hat die Socket-Ausnahme `SSH_AUTH_SOCK=/run/user/1000/ssh-agent.socket` zunächst für diesen Auftrag und anschließend dauerhaft für das Projekt freigegeben; siehe [Projektregeln](../AGENTS.md). Sie wird nur pro Prozess gesetzt, ohne globale Konfigurationsänderung. Kein BatchMode, keine neuen Schlüssel, kein Agent-Forwarding. Die Socket-Freigabe ersetzt nicht die gesonderte Freigabe aktiver Zieltests.

Nach gesonderter Freigabe wurde ausgeführt:

1. Ein neues Verzeichnis `~/stereonaut-decoder-002-<Zeitstempel>` anlegen. Drei HEVC-Dateien (zusammen ~7,7 MB), zwei kleine C-Quelldateien und das Testskript dorthin übertragen. Nur dort kompilieren und Ergebnisse ablegen; keine Installation.
2. Mit vorhandenem GCC und GStreamer 1.24.2 bauen. Ein V4L2-Kontext ohne Streaming enumeriert CAPTURE zuerst unverändert, dann nach `S_FMT(HEVC)`; fragt HEVC-Profile ab und führt `TRY_FMT(P010/Q10C)` aus. Alle Änderungen bleiben in diesem geöffneten Kontext.
3. Acht sequenzielle Tests mit explizitem `v4l2h265dec` auf `/dev/video22`, jeweils 60 Frames/2 Sekunden Inhalt. Pro Prozess 30 Sekunden Timeout und 3 Sekunden Kill-Frist. Main auto/MMAP, Main auto/DMA-BUF, Main10 auto/MMAP, Main10 auto/DMA-BUF, Main10 P010/MMAP, Main10 P010/DMA-BUF, Gradient auto/MMAP, Gradient P010/DMA-BUF.
4. Appsink zählt ausgegebene Frames, protokolliert Caps, Bit-Tiefe, Strides, Offsets und DMA-BUF-Memory. Der erste Frame wird für einen späteren lokalen Präzisionsvergleich kopiert. Keine Display-/VR-Ausgabe. `strace` erfasst Gerätezugriffe und IOCTLs einschließlich `G_FMT`, Source-Change und `EXPBUF`. Keine automatische Softwaredecoder-Auswahl.
5. Ergebnisse lokal abholen und mit Software-Referenzen vergleichen. Zielverzeichnis zunächst für Reproduzierbarkeit erhalten. Keine weiteren Streamingvarianten oder umfassenderen direkten Decoder ohne Erläuterung/Freigabe.

Ein aktiver Decodertest beansprucht kurzzeitig die Hardware und könnte bei einem Treiberfehler mit der laufenden SteamVR-Nutzung interferieren. Es erfolgen keine Dienststopps, Treiberwechsel oder Systemänderungen.

Prüfbare Implementierung: [Generator](../tools/decoder-probe/generate-fixtures.py), [GStreamer-Probe](../tools/decoder-probe/gst-probe.c), [V4L2-Formatprobe](../tools/decoder-probe/v4l2-formats.c), [Testskript](../tools/decoder-probe/run-approved-tests.sh). Beide C-Dateien lokal mit `-Wall -Wextra -Werror` kompiliert; Shellsyntax geprüft. Ein lokaler Build ist kein Zielhardwaretest.

## Testdateien

Erzeugt auf dem Entwicklungsrechner mit FFmpeg n9.0.2 / libx265 4.3:

```sh
python3 tools/decoder-probe/generate-fixtures.py
```

| Datei | Profil / Pixelformat | Abmessungen / Inhalt | Bytes |
| --- | --- | --- | --- |
| `main.hevc` | Main / yuv420p | 1920×1080, 60 Frames bei 30 fps, testsrc2 | 3873201 |
| `main10.hevc` | Main 10 / yuv420p10le | 1920×1080, 60 Frames bei 30 fps, testsrc2 | 3795855 |
| `gradient10.hevc` | Main 10 / yuv420p10le | 1920×1080, 60 Frames bei 30 fps, echter 10-Bit-YUV-Verlauf | 15977 |

Annex-B-Elementarstreams mit AUDs, wiederholten Headern, ohne B-Frames; GOP 30, limited range, 4:2:0. Die beiden testsrc2-Clips signalisieren die BT.709-Matrix, aber laut ffprobe keine Transfer-/Primärfarbenangaben; der lokale GStreamer-Parser zeigt deshalb `colorimetry=2:3:0:0`. Das ist eine mögliche zusätzliche Negotiation-Hürde, kein Hardwarefehler. Nur der Verlauf signalisiert vollständiges BT.709. Der Verlauf wird direkt aus 10-Bit-Codewerten erzeugt, nicht aus einem hochskalierten 8-Bit-Bild. Softwaredekodierte Referenzen des jeweils ersten Frames liegen lokal vor. Der Verlauf nutzt CRF 4, Level 4.1 und laut GStreamer-Parser High Tier; er ist komprimiert, nicht als verlustfreier Source-Roundtrip zu verstehen. Maßgeblich für den Hardwarevergleich ist die Softwaredecodierung desselben Bitstreams. Die Referenz enthält 877 verschiedene Luma-Werte; bei 1.554.101 Luma-Samples sind die unteren zwei Bits ungleich null. Raw-HEVC hat keine Containerdauer: Erzeugung und Parser bestätigen 30 fps/60 Frames; ffprobes `avg_frame_rate=25/1` ist hier kein verlässlicher Dauerbeleg.

Eigenschaften, SHA-256, Versionsdaten und vollständige Erzeugungsbefehle: [manifest.json](../tests/decoder-fixtures/manifest.json). Die generierten Videos/Rawframes sind über `.gitignore` von Git ausgeschlossen und mit dem Generator reproduzierbar. Kein Inhalt wurde vor Freigabe auf das Frame übertragen.

## Upstream und installierter Treiber: bisheriger Befund

Die [Patchserie v5, 0/6](https://lkml.rescloud.iu.edu/2606.0/02890.html) ergänzt 10-Bit-Bufferberechnung, HFI-Kommandos, Strides und Formatauswahl. Die Changelog-Angabe zur v2 entfernt ausdrücklich die Filterung von ENUM_FMT. Daher ist „P010 erscheint grundsätzlich erst nach Main10-Headern“ keine pauschal aus dieser Serie ableitbare Erklärung.

[Patch 6/6](https://lkml.rescloud.iu.edu/2606.0/02873.html) erweitert das HEVC-Profilmenü um Main10, hebt die interne Bit-Tiefengrenze an und ergänzt P010/QC10C in der CAPTURE-Formatliste. Nach Bitstreamauswertung wird bei 10 Bit ein ungeeignetes Ausgangsformat auf P010 gesetzt. Der UAPI-FourCC heißt `Q10C`; der symbolische Kernelname in dieser Serie lautet `V4L2_PIX_FMT_QC10C`.

Der installierte Kernel ist unverändert `6.18.0-gfbdbca41fd45`. Paketquellstand:
`fbdbca41fd45111219b6a188d5d7fe5c9e7872b7`, hinterlegt unter [Valve GitLab](https://gitlab.steamos.cloud/deckard/deckardos/linux-valve/-/tree/fbdbca41fd45111219b6a188d5d7fe5c9e7872b7).
Der öffentliche Abruf einer Quelldatei führt zu einer GitLab-Anmeldeseite; kein passender Quellbaum unter `/lib/modules/<kernel>/{build,source}`. Deshalb ist eine Commit-/Patchgleichheit derzeit **nicht nachweisbar**. Die Versionszahl allein beantwortet keine Backport-Frage.

Die Moduldatei unter `drivers/media/platform/qcom/vcodec/iris/iris.ko` enthält allerdings `Q10C`, `HFI_COLOR_FMT_P010`, `is_10bit_colorformat` und `vdec_set_bit_depth`. Ihr SHA-256 ist `0b96e2197272ee77aa5ddcbe08c36ab11bd1dfb65eebded93e8e82e9baa45d28`. Diese Marker belegen enthaltenen 10-Bit-Code, weder Erreichbarkeit noch Gleichheit mit der Upstream-Serie. Der abweichende Treiberpfad und Symbolnamen sprechen für einen separaten Downstream-Stand. Die Formatprobe und aktiven Tests müssen die tatsächliche API klären.

Alle sechs Patches wurden gelesen und als Quellbelege unter `docs/decoder-diagnostics/upstream-v5-patch*.txt` gesichert:

| Patch | Relevante Änderung |
| --- | --- |
| [1/6](https://lkml.rescloud.iu.edu/2606.0/02878.html) | Gemeinsame Tests für 8-/10-Bit-Pixelformate |
| [2/6](https://lkml.rescloud.iu.edu/2606.0/02891.html) | Getrennte P010-/QC10C-Allokationsgrößen; QC10C einschließlich UBWC-Metadaten |
| [3/6](https://lkml.rescloud.iu.edu/2606.0/02879.html) | HFI-Farbformat, Strides und Split-Mode; zusätzliche UBWC-Stride-Meldung speziell für AV1 |
| [4/6](https://lkml.rescloud.iu.edu/2606.0/02881.html) | P010: Breite auf 128, Höhe auf 32, Byte-Stride auf 256 ausrichten; QC10C: Breite auf 192, Höhe auf 16, gepackter 10-Bit-Stride |
| [5/6](https://lkml.rescloud.iu.edu/2606.0/02880.html) | Zulässige gewählte Ausgabe anhand der bekannten Stream-Bit-Tiefe prüfen; nicht mit bloßer Format-Enumeration gleichsetzen |
| [6/6](https://lkml.rescloud.iu.edu/2606.0/02873.html) | Main10-Profil, 10-Bit-Grenze, CAPTURE-Formate und Reaktion auf erkannte Stream-Bit-Tiefe |

Die Upstream-Layoutformeln dürfen nicht ungeprüft als exakte Beschreibung des abweichenden installierten Valve-Treibers verwendet werden.

## Ergebnisse

Die erste Testserie wurde vom Nutzer ausdrücklich freigegeben und unter `~/stereonaut-decoder-002-20261008-01` ausgeführt. Alle drei Video-SHA-256 stimmen mit dem lokalen Manifest überein.

Die direkte Formatprobe zeigt: Q10C erscheint bereits nach `S_FMT(HEVC)`, ohne Bitstreamheader. Das HEVC-Profilmenü enthält Main10. `TRY_FMT(P010)` liefert NV12 zurück; `TRY_FMT(Q10C)` erhält Q10C. Damit ist die ursprüngliche H.264-Auswahl eine belegte Ursache dafür, dass Q10C in Auftrag 001 fehlte. P010 bleibt nicht angeboten.

Alle acht GStreamer-Versuche lieferten 0 Frames: auto scheitert mit `not-negotiated` vor `S_FMT`/`STREAMON`, P010 scheitert beim Verknüpfen der Elemente. Das ist kein negativer Hardwaredecodiernachweis. Belege: [erste Testserie](decoder-diagnostics/run-01/status.txt), [Formatprobe](decoder-diagnostics/run-01/v4l2-formats.log).

### Ergänzendes Verfahren zur Freigabe

[Direkte V4L2-Probe](../tools/decoder-probe/v4l2-decode.c) und [ergänzendes Skript](../tools/decoder-probe/run-direct-tests.sh): lokal kompiliert, maximal 60 AUD-begrenzte Eingabeframes, 20 Sekunden interne Laufzeit plus 30-Sekunden-Prozesstimeout. Fünf Versuche: Main→NV12, Main10→auto, Main10→Q10C, Gradient→Q10C, Main10→P010 (Abbruch vor Streaming, falls ersetzt). Die Probe konfiguriert ausschließlich ihren `/dev/video22`-Kontext, wartet auf SOURCE_CHANGE, protokolliert Formate/Strides/Buffer, zählt DQBUF-Ausgaben und exportiert CAPTURE-Planes per EXPBUF. Ein erster Frame/Buffer wird zur lokalen Analyse gespeichert. Zusätzlich wird genau der erste GStreamer-Fall mit ausführlichem Negotiation-Logging wiederholt. Keine Softwaredecoder, kein Kernelwechsel. Die Serie wurde am 2026-10-08 im bereits genehmigten Benutzerverzeichnis ausgeführt. Das Skript behandelt erwartete P010-Formatersetzung und den bekannten GStreamer-Negotiation-Fehler bei künftigen Läufen korrekt, bricht aber weiterhin bei jedem sonstigen direkten Testfehler oder Timeout ab.

| Prüfkriterium | Stand |
| --- | --- |
| Hardware-HEVC Main | Nachgewiesen: 60/60 Frames über `/dev/video22`, `iris_driver`, CAPTURE NV12; der sichtbare 1920×1080-Bereich stimmt bytegenau mit der Softwaredecodierung überein |
| Hardware-HEVC Main10 | Nachgewiesen: 60/60 Frames für Main10-testsrc2 und Main10-Verlauf über `/dev/video22`; CAPTURE jeweils Q10C |
| Ausgabeformat nach Streaminitialisierung | Main: NV12. Main10: Q10C, 1920×1088, ein Speicherplane, Stride 2560, `sizeimage=4210688` |
| Erhalt der 10-Bit-Präzision | Nicht nachgewiesen. Q10C ist komprimiert; kein Q10C-Entpacker oder GPU-Import wurde ausgeführt |
| DMA-BUF-Export | Nachgewiesen: `VIDIOC_EXPBUF` erfolgreich für alle 12 CAPTURE-Buffer je erfolgreichem Fall; strace zeigt `fd=…</dmabuf:>` |
| DRM-Modifier | Nicht aus bloßem DMA-BUF-FD ableitbar; keine Vulkan-Importprüfung vorgesehen |

### Direkte V4L2-Ergebnisse

Die direkte Probe nutzte ausschließlich den nativ geöffneten `iris_driver` auf `/dev/video22`. Sie schrieb Annex-B-AUs in dessen OUTPUT-Queue, erhielt `V4L2_EVENT_SOURCE_CHANGE`, allokierte/queue-te anschließend CAPTURE-Buffer und zählte nur vom selben V4L2-Dateideskriptor dequeuete Frames. Es existiert in diesem Pfad kein Softwaredecoder. Die mitgeschnittenen IOCTLs zeigen `S_FMT`, `STREAMON`, `DQBUF` und zum Abschluss `STREAMOFF`; Beispiel: [Main10/Q10C-strace](decoder-diagnostics/run-02/main10-Q10C.strace).

| Eingabe / CAPTURE-Anforderung | Format nach Source-Change | Frames | DMA-BUF-Export | Ergebnis |
| --- | --- | ---: | --- | --- |
| Main 8 Bit / NV12 | NV12, 1920×1088, ein Plane, Stride 1920, 3133440 Bytes | 60/60 | 12/12 Buffer | Erfolgreich |
| Main10 / auto | Q10C, 1920×1088, ein Plane, Stride 2560, 4210688 Bytes | 60/60 | 12/12 Buffer | Erfolgreich; Treiber wählt Q10C selbst |
| Main10 / Q10C | Q10C, gleiche Parameter | 60/60 | 12/12 Buffer | Erfolgreich |
| Main10-Verlauf / Q10C | Q10C, gleiche Parameter | 60/60 | 12/12 Buffer | Erfolgreich; Farbmetadaten BT.709/limited range wurden gemeldet |
| Main10 / P010 | P010 vor Streaming auf NV12 ersetzt | 0 | nicht versucht | P010 nicht wählbar |

Die coded height wird auf 1088 ausgerichtet. Die V4L2-Compose-Region ist in allen vier erfolgreichen Fällen 1920×1080. Für Main wurde der NV12-Dump lokal auf diese Region zugeschnitten, nach planarem 4:2:0 konvertiert und mit dem softwaredecodierten Referenzframe verglichen: SHA-256 beider Dateien `daa47c3f9b53124c0e7d53227615e1826c4c92420745f814a201ed4a05c936d8`; `cmp` ergab Gleichheit. Das belegt den sichtbaren 8-Bit-Pixelinhalt zusätzlich zum Hardwarepfad.

Bei `main10-auto` wurden 60 Frames ausgegeben, aber nur 59 OUTPUT-Buffer vor dem CAPTURE-`LAST` zurückgegeben. Alle 60 AUs waren zuvor queuet, kein Frame- oder Fehlerflag wurde gemeldet, und der explizite Q10C-Lauf gab 60 OUTPUT-Buffer zurück. Der Unterschied wird als Drain-/Dequeue-Reihenfolge dokumentiert, nicht als verlorenes Bild gewertet.

Q10C wird vom Treiber als `QCOM Compressed 10-bit Format` gemeldet. Die Dumps wurden als Beleg erhalten: [Main10 auto](decoder-diagnostics/run-02/main10-auto.raw), [Main10 Q10C](decoder-diagnostics/run-02/main10-Q10C.raw), [Main10-Verlauf Q10C](decoder-diagnostics/run-02/gradient10-Q10C.raw). Der Auto- und der explizite-Q10C-Dump desselben ersten Frames haben denselben SHA-256 `1f2220163da9c739cb78b800d9efb6951e8b5951d33f63133b5f16590d7c5d93`; der Verlauf hat erwartungsgemäß einen anderen Blob. Diese Gleichheit prüft die gewählte Ausgabeform, keine Pixelpräzision. **Kein Q10C-Dump wurde mit einer unkomprimierten Referenz verglichen.**

`VIDIOC_EXPBUF` gelang mit `O_RDONLY|O_CLOEXEC` auf der einzigen CAPTURE-Plane jedes der zwölf allokierten Buffer. Die resultierenden FDs wurden per `fstat` mit jeweils der CAPTURE-Allokationsgröße bestätigt und wieder geschlossen. Damit ist Exportierbarkeit als DMA-BUF nachgewiesen, nicht aber ein bestimmter DRM-Modifier, explizite Synchronisation oder Vulkan-Import.

### GStreamer-Diagnose

Der freigegebene Wiederholungstest verwendet explizit `v4l2h265dec` auf `/dev/video22`; es wurde kein `decodebin` und kein Softwarefallback verwendet. Er beendet sich mit `not-negotiated` und 0 Frames **vor** `S_FMT` oder `STREAMON`. Das ausführliche Log zeigt als Ursache eine Caps-Ablehnung: Der Parser liefert für Main `colorimetry=2:3:0:0`, während die vom Element gemeldeten erlaubten Eingabecaps diese konkrete Farbeinordnung nicht akzeptieren. Das ist eine GStreamer-1.24.2-Interoperabilitäts-/Caps-Grenze, kein Fehlschlag des Iris-Decoders; der direkte V4L2-Test widerlegt einen solchen Schluss. Beleg: [GStreamer-Negotiation-Log](decoder-diagnostics/run-02/gst-negotiation.log).

### Bewertung und nächster Schritt

Der entscheidende Hardwareteil ist erfolgreich: Der SM8650-Iris-Treiber decodiert HEVC Main und Main10, wählt für Main10 Q10C und exportiert dessen CAPTURE-Buffer als DMA-BUF. Für die Player-Architektur ist ein direkter V4L2-Adapter derzeit der belastbare Ausgangspunkt; der vorhandene GStreamer-`v4l2h265dec` ist für diesen Stack nicht unmittelbar nutzbar.

Der nächste Schritt muss ein **gesondert freizugebender Vulkan-Import-/Darstellungstest** sein: einen Q10C-DMA-BUF importieren, den vom Treiber verwendeten DRM-Modifier und die Ebenenbeschreibung abfragen, korrekt darstellen beziehungsweise in ein prüfbares Ziel konvertieren und erst dann mit dem 10-Bit-Verlauf gegen die Referenz vergleichen. Ohne diesen Schritt ist ein Main10-Hardwaredecode ein Teilerfolg, aber kein Nachweis für den verlustfreien Erhalt der 10-Bit-Präzision.

## Bisher ausgeführte Zielbefehle

Lesend über `ssh frame` mit der inzwischen dauerhaft für das Projekt genehmigten Socket-Ausnahme:

```sh
uname -r
command -v gcc pkg-config strace timeout
pkg-config --modversion gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0
modinfo -F filename iris
modinfo -F srcversion iris
ls -ld /lib/modules/$(uname -r)/build /lib/modules/$(uname -r)/source
pacman -Qi linux-618-deckard
readelf -sW /lib/modules/6.18.0-gfbdbca41fd45/kernel/drivers/media/platform/qcom/vcodec/iris/iris.ko
strings /lib/modules/6.18.0-gfbdbca41fd45/kernel/drivers/media/platform/qcom/vcodec/iris/iris.ko
sha256sum /lib/modules/6.18.0-gfbdbca41fd45/kernel/drivers/media/platform/qcom/vcodec/iris/iris.ko
```

Nach erster Testfreigabe: neues Benutzerverzeichnis per `mkdir` erstellt, lokal gepacktes Testarchiv über SSH-stdin mit `tar -xf -` dort ausgepackt; SHA-256 der drei Clips geprüft; `sh run-approved-tests.sh` ausgeführt. Die genauen Compile-/Streamingbefehle stehen im oben verlinkten Skript. Ergebnisse mit `tar -cf -` über SSH-stdout lokal abgerufen.

Nach ergänzender Freigabe: ausschließlich im selben Verzeichnis `v4l2-decode.c` und `run-direct-tests.sh` ergänzt, mit vorhandenem GCC kompiliert und der direkte Testlauf ausgeführt. Die vier erfolgreichen V4L2-Streamingfälle wurden jeweils sequenziell beendet, ohne Timeout, Hänger oder Treiber-Recovery. P010 wurde vor Streaming als erwartete Formatersetzung beendet. Der abschließende GStreamer-Diagnosefall wurde anschließend einmal separat ausgeführt, weil die erste Skriptversion beim erwarteten P010-Exit beendet hatte; daraus entstand kein zusätzlicher Decoder-Streamingtest. Keine weiteren aktiven Tests nach dieser Serie.

## Präzisionsvergleich

[compare-frame.py](../tools/decoder-probe/compare-frame.py) vergleicht einen dicht gepackten NV12-/P010-Dump ohne Farbkonvertierung mit dem softwaredecodierten planaren Referenzframe. Für P010 werden die signifikanten oberen 10 Bits extrahiert und UV deinterleaved. Ein positiver Nachweis verlangt P010, exakte Übereinstimmung zum Referenzframe und mehr als 256 tatsächlich genutzte Luma-Werte mit nichttrivialen unteren Bits. Ein rein lokal synthetisierter P010-Dump wurde als Selbsttest des Vergleichers geprüft; das ist ausdrücklich kein Hardwareergebnis. Q10C benötigt dagegen eine nachgewiesene Entpack-/Dekompressionsmethode, bevor ein solcher Pixelvergleich möglich ist.
