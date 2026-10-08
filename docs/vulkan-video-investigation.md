# Stereonaut – Vulkan/Q10C-Untersuchung 003

Stand: 2026-10-08. **Der aktive Q10C-Importtest wurde entsprechend der
Freigabebedingung nicht gestartet:** Die damals ausgeführte
Modifierlisten-Probe ließ Turnip ein anderes Speicherlayout als die
Iris-Q10C-Ausgabe wählen; ein expliziter Importparameter-Satz war zu diesem
Zeitpunkt nicht geprüft.

> **Korrektur aus Auftrag 004:** Diese Schlussfolgerung war für die damalige
> Stop-Entscheidung vorsichtig und korrekt, ist technisch aber zu allgemein.
> Die Probe hatte Turnips selbst gewähltes Layout über eine Modifierliste
> gemessen. Die anschließende Quellenanalyse zeigte einen getrennten expliziten
> Importpfad, der Producer-Offsets und -Pitches akzeptiert. Die danach separat
> freigegebene Erzeugungsprobe bestätigte auf dem installierten Turnip ein
> konsistentes Zwei-Plane-Layout von exakt 4.210.688 Byte. Siehe
> [QC10C-Interop-Recherche 004](q10c-interop-research.md).

## Ergebnis

| Prüfschritt | Ergebnis |
| --- | --- |
| Turnip-Modifier für 10-Bit 4:2:0 | `DRM_FORMAT_MOD_QCOM_COMPRESSED` wird für `VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16` angeboten |
| Sampling und Transfer | Für diesen Modifier sind `SAMPLED`, `TRANSFER_SRC`, `TRANSFER_DST` und die kombinierte Abfrage `SAMPLED | TRANSFER_SRC` unterstützt |
| DMA-BUF-Eigenschaften | Importierbar, exportierbar und `DEDICATED_ONLY`; Abfragen für Sampling, Transfer und kombiniert erfolgreich |
| Vulkan-Speicherplanes | Zwei |
| Turnip-Layout 1920×1088 | 6.299.648 Byte Anforderung; Plane 0: 4.177.920 Byte, Row-Pitch 3.840; Plane 1: 2.088.960 Byte, Row-Pitch 3.840 |
| Iris-Q10C 1920×1088 | Ein V4L2-Speicherplane; 4.210.688 Byte; `bytesperline=2560`; intern laut Iris-Patch vier Q10C-Bereiche |
| Layoutkompatibilität | Mit der damals geprüften Modifierlisten-Erzeugung nicht gegeben. Auftrag 004 rekonstruierte ein separates explizites Producer-Layout; die nachfolgende Probe bestätigte dessen Akzeptanz und exakt 4.210.688 Byte Memory Requirement auf dem installierten Turnip |
| Aktiver DMA-BUF-Import | Nicht ausgeführt, weil die vorab festgelegte Stop-Bedingung erfüllt war |
| GPU-Konvertierung/Readback | Nicht ausgeführt |
| Erhalt der 10-Bit-Präzision | Weiterhin nicht nachgewiesen |

Die Capability-Seite von Turnip ist grundsätzlich geeignet. Zum Zeitpunkt
dieses Tests fehlte eine geprüfte Abbildung **dieser Iris-Q10C-Allokation** auf
das von Vulkan angebotene Format/Modifier/Plane-Modell. Auftrag 004 hat diese
Abbildung aus den Iris- und Turnip-Quellen rekonstruiert; die anschließend
freigegebene Erzeugungsprobe hat sie auf dem installierten Treiber bestätigt.
Ein gleicher Modifiername allein genügt weiterhin nicht; der explizite Import
benötigt die bestätigten Producer-Offsets und -Pitches.

## Beweisgrenze

Ziel war die Kette `HEVC Main10 → Iris → Q10C → DMA-BUF → Vulkan/Turnip →
prüfbares 10-Bit-Bild`. Ein Vollnachweis hätte einen GPU-erzeugten,
unkomprimierten Output aus `gradient10.hevc` und dessen Vergleich mit der
Softwaredecodierung desselben Bitstreams verlangt. Zum Zeitpunkt von Auftrag
003 waren die Importparameter nicht widerspruchsfrei bestimmt; ein damaliger
Import mit geratenen Offsets oder einem nur ähnlich benannten Vulkanformat
wäre kein gültiger Nachweis gewesen und hätte einen zu kleinen DMA-BUF
außerhalb seiner Grenzen lesen können.

## Ausgangslage aus Auftrag 002

- Iris decodiert Main10 nachweislich in Q10C. Nach
  `V4L2_EVENT_SOURCE_CHANGE` wurden coded 1920×1088, sichtbare Compose-Region
  1920×1080, `bytesperline=2560` und `sizeimage=4210688` gemessen.
- V4L2 beschreibt die Allokation als ein Speicherplane. `VIDIOC_EXPBUF`
  exportiert jeden CAPTURE-Buffer erfolgreich als DMA-BUF.
- P010 ist am installierten Iris-Treiber nicht wählbar; eine P010-Anforderung
  wird vor dem Streaming durch NV12 ersetzt.
- Die untersuchte Iris-Patchserie beschreibt Q10C innerhalb dieses einen
  Speicherplanes als Y-Metadaten, Y-UBWC-Daten, UV-Metadaten und UV-UBWC-Daten.
  Das V4L2-Speicherplane ist daher weder mit einer logischen Bildebene noch mit
  der Vulkan-Speicherplanezahl gleichzusetzen.

## Testprogramme

[`vulkan-format-probe.c`](../tools/vulkan-q10c-probe/vulkan-format-probe.c)
enumeriert für relevante 8-/10-Bit-4:2:0-Formate:

1. DRM-Modifier und deren Vulkan-Speicherplanezahl;
2. Sampling- und Transfermerkmale pro Format/Modifier;
3. `vkGetPhysicalDeviceImageFormatProperties2` mit
   `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`, getrennt für Sampling,
   Transfer-Quelle sowie deren Kombination;
4. Importierbarkeit, Exportierbarkeit und Dedicated-Allocation-Pflicht.

[`vulkan-layout-probe.c`](../tools/vulkan-q10c-probe/vulkan-layout-probe.c)
übergibt Turnip für coded 1920×1088 eine Modifierliste, die ausschließlich den
zuvor enumerierten QCOM-Modifier enthält. Die Probe erstellt nur ein
ungebundenes Vulkan-Image und fragt danach den tatsächlich gewählten Modifier,
die Speicheranforderung sowie alle gemeldeten Modifier-Speicherplanes ab. Sie
allokiert und importiert keinen DMA-BUF und reicht keine GPU-Arbeit ein.

Beide Programme wurden zunächst lokal gegen die vom Frame gelesenen Vulkan-
Header mit `-Wall -Wextra -Werror` kompiliert. Der lokale Lauf auf einer
anderen GPU war nur ein Programm-Selbsttest und ist kein Hardwarebefund.

## Ausgeführtes Verfahren und Freigabe

Der Nutzer gab das zuvor in diesem Dokument beschriebene sequenzielle Verfahren
ausdrücklich frei. Neue Dateien durften ausschließlich im bereits vorhandenen
Verzeichnis `~/stereonaut-decoder-002-20261008-01` entstehen. Der aktive
Decoder-/Importtest war nur bei widerspruchsfreier Format-, Modifier-, Plane-
und Layoutzuordnung erlaubt.

Ausgeführt wurde über den SSH-Alias `frame` mit der dauerhaft genehmigten,
prozesslokalen Socket-Ausnahme:

```sh
cd ~/stereonaut-decoder-002-20261008-01/vulkan-q10c-probe

timeout -k 3s 10s cc -std=c11 -O2 -Wall -Wextra -Werror \
  -o vulkan-format-probe vulkan-format-probe.c \
  $(pkg-config --cflags --libs vulkan)

timeout -k 3s 10s cc -std=c11 -O2 -Wall -Wextra -Werror \
  -o vulkan-layout-probe vulkan-layout-probe.c \
  $(pkg-config --cflags --libs vulkan)

timeout -k 3s 10s ./vulkan-format-probe
timeout -k 3s 10s ./vulkan-layout-probe
```

Beide Läufe endeten innerhalb des Timeouts mit Exitcode 0. Es gab keinen
Vulkan-/Treiberfehler, Hänger oder Recovery-Versuch. Anschließend wurden nur
die beiden Logs gelesen und lokal gesichert:

- [Capability-Log](vulkan-video-diagnostics/run-01/vulkan-format-probe.log),
  SHA-256 `9267d7f37c698e77e28b055e7a2b9b40b2f99e986ccc9deee5149d8b7c795299`
- [Layout-Log](vulkan-video-diagnostics/run-01/vulkan-layout-probe.log),
  SHA-256 `f9f0116f02d4729efbf4d2446a00c62b8b1c9b6ae98bfe4cfac35bbb536532ca`

Kein V4L2-Gerät wurde in Auftrag 003 geöffnet, kein Decoderstream gestartet,
kein DMA-BUF importiert, kein Command-Buffer eingereicht und kein Pixelinhalt
gelesen.

## Phase A: Vulkan-Capabilities

Die Probe lief auf `Turnip Adreno (TM) 750`, Vendor `0x5143`, Device
`0x43051401`, Vulkan-Geräte-API 1.4.359. Folgende benötigte Erweiterungen sind
vorhanden:

- `VK_EXT_image_drm_format_modifier`
- `VK_EXT_external_memory_dma_buf`
- `VK_KHR_external_memory_fd`
- `VK_KHR_sampler_ycbcr_conversion`

Zusätzlich hatte die vorherige Systeminventur
`VK_EXT_queue_family_foreign` bestätigt.

### Relevante Formate

| Vulkanformat | Modifier | Speicherplanes | Merkmale | DMA-BUF-Abfrage |
| --- | --- | ---: | --- | --- |
| `G8_B8R8_2PLANE_420_UNORM` | linear | 2 | Sampling, Transfer src/dst, lineare YCbCr-Filterung | erfolgreich; dedicated-only |
| `G8_B8R8_2PLANE_420_UNORM` | QCOM compressed | 2 | gleich | erfolgreich; dedicated-only |
| `G8_B8_R8_3PLANE_420_UNORM` | nur linear | 3 | Sampling, Transfer src/dst | erfolgreich; dedicated-only |
| `G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16` | linear | 2 | Sampling, Transfer src/dst, lineare YCbCr-Filterung | erfolgreich; dedicated-only |
| `G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16` | QCOM compressed | 2 | gleich | Sampling, Transfer src und kombiniert jeweils erfolgreich; dedicated-only |
| `G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16` | keiner | 0 | nicht unterstützt | entfällt |

Der relevante QCOM-Modifier ist numerisch `0x0500000000000001`, also
`DRM_FORMAT_MOD_QCOM_COMPRESSED`. Für die kombinierte Usage
`SAMPLED | TRANSFER_SRC` trat kein Sonderfall auf; sie wurde ausdrücklich
erfolgreich abgefragt.

Die Vulkan-Spezifikation definiert das angebotene 2-Plane-10-Bit-Format als
P010-artig: Jeder Wert liegt in den oberen zehn Bits eines 16-Bit-Wortes. Das
ist eine logische Formatbeschreibung; ein Modifier kann die physische Ablage
ändern. Gerade deshalb musste Turnips konkrete Layoutberechnung mit Iris-Q10C
verglichen werden.

## Phase B: tatsächliches Layout

Turnip wählte aus der einelementigen Liste tatsächlich
`DRM_FORMAT_MOD_QCOM_COMPRESSED`. Für
`VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16`, 1920×1088, ein Mip-Level,
ein Array-Layer und `SAMPLED | TRANSFER_SRC` meldete es:

```text
memory size=6299648 alignment=4096 memory_type_bits=0x7
prefers_dedicated=1 requires_dedicated=1
memory_plane=0 offset=0 size=4177920 row_pitch=3840
memory_plane=1 offset=0 size=2088960 row_pitch=3840
```

Die Plane-Größen entsprechen `3840 × 1088` und `3840 × 544`. Ihre Summe ist
6.266.880 Byte; die gesamte Vulkan-Speicheranforderung enthält weitere 32.768
Byte. Die beiden gemeldeten Offsets sind jeweils null. Für nichtlineare
Modifier ist die Bedeutung von `rowPitch` implementationsabhängig; diese Werte
werden daher dokumentiert, aber nicht als direkt CPU-adressierbares Layout
interpretiert.

Iris-Q10C meldet dagegen:

```text
V4L2 memory planes=1
bytesperline=2560
sizeimage=4210688
```

Die Formeln der untersuchten Upstream-Patchserie reproduzieren die gemessene
Iris-Größe exakt, ohne dass sie als Importparameter verwendet wurden:

| Q10C-Bereich | Größe | daraus folgender Beginn |
| --- | ---: | ---: |
| Y-Metadaten | 20.480 | 0 |
| Y-UBWC-Daten | 2.785.280 | 20.480 |
| UV-Metadaten | 12.288 | 2.805.760 |
| UV-UBWC-Daten | 1.392.640 | 2.818.048 |
| Gesamt | **4.210.688** | Ende 4.210.688 |

Diese Tabelle erklärt den beobachteten Treiberwert und die vier internen
Bereiche. Sie ist **keine** freigegebene oder getestete Vulkan-
Ebenenbeschreibung. Der installierte Valve-Treiber ist außerdem nicht als
quellgleich mit der Patchserie nachgewiesen.

### Warum native DRM-/GBM-Schnittstellen die Lücke nicht schließen

- V4L2 `G_FMT` liefert für Q10C nur FourCC, coded Maße, ein Speicherplane,
  `bytesperline` und `sizeimage`; es liefert keinen DRM-Modifier und keine vier
  Teilbereichsoffsets.
- `VIDIOC_EXPBUF` exportiert den Speicher als FD, aber keine Bildformat-
  Metadaten. Ein DMA-BUF-FD trägt kein abfragbares FourCC/Modifier/Plane-Layout.
- DRM PRIME kann einen DMA-BUF-FD in einen GEM-Handle umwandeln. Die
  Framebuffer-API erwartet Format, Modifier, Handles, Pitches und Offsets vom
  Aufrufer; sie entdeckt diese Werte nicht aus dem FD.
- `GBM_BO_IMPORT_FD_MODIFIER` verlangt vom Aufrufer `format`, `num_fds`, alle
  `strides`, alle `offsets` und den `modifier`. GBM ist daher ebenfalls kein
  Layout-Erkennungsmechanismus für den nackten Iris-FD.
- Ein expliziter Vulkan-Import verlangt pro Modifier-Speicherplane Offset und
  Row-Pitch. Die alternative Modifierlisten-Erzeugung lässt Turnip sein eigenes
  Layout wählen; genau dieses wurde hier gemessen und passt nicht zur
  Decoderallokation. Die spätere Quellenanalyse zeigt, dass daraus keine
  Ablehnung eines abweichenden expliziten Producer-Layouts folgt.

Damit war in Auftrag 003 keine geprüfte native Schnittstelle bekannt, die die
fehlenden Importparameter vom Iris-Produzenten an Turnip übergibt. Auftrag 004
rekonstruierte die Parameter aus dem Producer-Layout; die nachfolgende
explizite Erzeugungsprobe bestätigte sie auf der installierten Implementierung.

## Stop-Entscheidung

Der aktive Test war nur bei widerspruchsfreier Zuordnung freigegeben. Die
folgenden Unterschiede erfüllen die dokumentierte Stop-Bedingung:

1. 6.299.648 Byte Anforderung des selbst erzeugten Vulkan-Images gegenüber
   4.210.688 Byte DMA-BUF;
2. zwei Vulkan-Speicherplanes gegenüber einem V4L2-Speicherplane mit vier
   intern beschriebenen Q10C-Bereichen;
3. 3.840 Byte selbst gewählter Turnip-Row-Pitch gegenüber 2.560 Byte
   Iris-`bytesperline`;
4. keine native API-Ausgabe, die die Iris-Bereiche in die zwei von Turnip
   erwarteten Modifier-Speicherplanes überführt;
5. kein zu diesem Zeitpunkt geprüfter expliziter Parametersatz.

Ein Import des 4.210.688-Byte-FDs in ein Image mit einer
6.299.648-Byte-Speicheranforderung wäre nicht valide. Ein expliziter Import mit
aus den Kernel-Patchformeln übernommenen Offsets wäre ungeprüft und durch die
Freigabe ausdrücklich ausgeschlossen. Deshalb wurden Phase C bis E nicht
ausgeführt.

## Synchronisation

Der geplante Einmallauf hätte den CAPTURE-Buffer nach V4L2-`DQBUF` bis zum
signalisierten Vulkan-Fence beziehungsweise `vkQueueWaitIdle` dequeuet
gehalten und ihn erst danach zurückgegeben. Queue-Ownership sollte bei
`VK_QUEUE_FAMILY_FOREIGN_EXT` beginnen. Dieser Pfad wurde wegen der vorherigen
Layoutinkompatibilität nicht ausgeführt; insbesondere ist für diese konkrete
Kette kein Synchronisationsnachweis erbracht.

## Empfehlung

Auftrag 004 rekonstruiert für Q10C eine gemeinsame Repräsentation als
P010-Basisformat plus QCOM-Kompressionsmodifier und zwei explizite
Modifier-Memory-Planes im selben DMA-BUF. Die kleine, nicht-streamende
`vkCreateImage`-Probe hat diese Producer-Layouts inzwischen bestätigt. Der
nächste Schritt ist der separat freizugebende einmalige echte Import- und
Präzisionstest aus dem
[vorbereiteten Ablauf](q10c-import-test-plan.md). P010 bleibt ein sinnvoller
Alternativpfad, ist im Valve-Treiber derzeit aber nicht enumeriert.

Der direkte V4L2-Decode nach Q10C und das passende explizite Vulkan-Image sind
als getrennte Funktionen bestätigt. DMA-BUF-Import, GPU-Zugriff und der
verlustfreie Erhalt der 10-Bit-Präzision sind weiterhin offen. Ein
CPU-Q10C-Entpacker oder ein vollständiger CPU-Upload wäre für die angestrebte
8K-Architektur keine geeignete Wiedergabelösung und wurde nicht entwickelt.

## Quellen

- [Vulkan: `VK_EXT_image_drm_format_modifier`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_image_drm_format_modifier.html)
- [Vulkan-Formatspezifikation](https://docs.vulkan.org/spec/latest/chapters/formats.html)
- [Linux DRM-Formatmodifier](https://github.com/torvalds/linux/blob/master/include/uapi/drm/drm_fourcc.h)
- [Linux KMS: Format und Modifier](https://dri.freedesktop.org/docs/drm/gpu/drm-kms.html)
- [Iris-Patchserie und Auftrag 002](decoder-investigation.md)
