# Stereonaut – QC10C/DRM/Vulkan-Interop-Recherche 004

Stand: 2026-10-08. Nach der Quellen- und Codeanalyse wurde die gesondert
freigegebene, nicht-streamende Vulkan-Erzeugungsprobe ausgeführt. Auf dem Steam
Frame wurde dabei kein Decoder geöffnet, kein DMA-BUF importiert, kein Speicher
allokiert oder gebunden und keine GPU-Arbeit eingereicht.

## Ergebnis

Der direkte Pfad ist auf Parameterebene rekonstruierbar:

```text
V4L2 Q10C (V4L2_PIX_FMT_QC10C)
  -> ein DMA-BUF / ein V4L2-Allokationsplane
  -> DRM_FORMAT_P010 + DRM_FORMAT_MOD_QCOM_COMPRESSED
  -> zwei Vulkan-Modifier-Memory-Planes in demselben DMA-BUF
  -> explizite Producer-Offsets und -Pitches
```

Für den bereits gemessenen 1920×1088-Buffer ergibt sich aus den Iris-Formeln
und Turnips explizitem Importpfad folgender, in sich konsistenter Parametersatz:

| Vulkan-Memory-Plane | Inhalt im Q10C-Buffer | `offset` | `rowPitch` | intern berechnete Größe |
| --- | --- | ---: | ---: | ---: |
| 0 | Y-Metadaten + Y-TP10/UBWC-Daten | 0 | 2.560 | 2.805.760 |
| 1 | UV-Metadaten + UV-TP10/UBWC-Daten | 2.805.760 | 2.560 | 1.404.928 |
| Gesamt | ein gemeinsamer DMA-BUF | – | – | **4.210.688** |

Diese Zuordnung war zunächst aus öffentlichem Iris- und Mesa-Quellcode
ableitbar. Der anschließend freigegebene Test hat sie auf dem installierten
Turnip durch `vkCreateImage` bestätigt: Das explizite Layout wurde akzeptiert,
der QCOM-Modifier zurückgemeldet und die Memory Requirement betrug exakt
4.210.688 Byte. Auftrag 003 hatte dagegen nur Turnips selbst gewähltes Layout
bei einer Modifierliste gemessen. Ein explizit beschriebenes, extern erzeugtes
Layout ist ein anderer Vulkan-Erzeugungspfad.

Die historische Aussage, QC10C sei auf einem SM8650 über Vulkan-DMA-BUF-Import
dargestellt worden, ist öffentlich belegt. Der zugehörige Vulkan-Testcode oder
ein vollständiges Parameterprotokoll konnte nicht gefunden werden. Der später
gezeigte GStreamer/kmssink-Erfolg ist außerdem ein getrennter Test: Er benötigte
einen als Hack bezeichneten MSM-DRM-Patch und transportierte die beiden
Q10C-Modifier-Plane-Layouts nicht sauber durch GStreamer.

## Evidenzklassen

| Klasse | Befund |
| --- | --- |
| Auf unserem Steam Frame nachgewiesen | Iris decodiert HEVC Main10 nach Q10C; 1920×1088 coded, 1920×1080 sichtbar, ein V4L2-Memory-Plane, `bytesperline=2560`, `sizeimage=4210688`; DMA-BUF-Export funktioniert. Turnip bietet P010-artiges Vulkan-10-Bit-4:2:0 mit QCOM-Modifier, zwei Modifier-Memory-Planes und DMA-BUF-Import für Sampling/Transfer an. Ein ungebundenes Image mit den expliziten Iris-Offsets/-Pitches wird erfolgreich erzeugt und verlangt exakt 4.210.688 Byte. |
| Auf einem anderen SM8650-Upstream-System nachgewiesen | Neil Armstrong meldete P010- und QC10C-Decoding samt Darstellung über kmssink. Im selben Thread steht außerdem, dass die Bildkorrektheit zuvor durch Vulkan-DMA-BUF-Darstellung einschließlich QC10C/QC08C geprüft worden sei. Der konkrete Vulkan-Code fehlt. |
| Aus Quellcode ableitbar | Q10C wird als `P010:DRM_FORMAT_MOD_QCOM_COMPRESSED` bezeichnet. Turnip akzeptiert beim expliziten Modifier-Image zwei vom Produzenten angegebene Offsets und Pitches. Die oben angegebenen Werte bilden die vier Iris-Bereiche exakt auf zwei Modifier-Memory-Planes ab. |
| Hypothese | Der bestätigte Image-Erzeugungspfad lässt sich mit dem von Iris exportierten DMA-BUF binden und per GPU-Transfer auswerten. |
| Unbekannt | Ob DMA-BUF-Import und -Binding, externer Queue-Ownership-Übergang, GPU-Transfer und die bitgenaue 10-Bit-Auswertung auf dem Steam Frame funktionieren. Ebenfalls unbekannt ist der Quellstand des historischen Vulkan-Testprogramms. |

## Upstream-Iris-Diskussion

Der zentrale [April-Thread](https://lkml.iu.edu/hypermail/linux/kernel/2604.1/10609.html)
liefert mehrere voneinander zu trennende Ergebnisse:

1. Nach Ergänzung von HEVC Main10 im Profilmenü funktionierte die lineare
   P010-Ausgabe mit GStreamer git.
2. QC08C wurde in `DMA_DRM` als
   `NV12:0x0500000000000001`, QC10C als
   `P010:0x0500000000000001` angeboten. Der numerische Wert ist
   `DRM_FORMAT_MOD_QCOM_COMPRESSED`.
3. Mit GStreamer MR 8195, MR 11222 und einer zunächst separaten
   V4L2-Plane-Anzahl-Korrektur ließen sich NV12, QC08C, P010 und QC10C bis zu
   kmssink verhandeln.
4. QC10C scheiterte zunächst in `msm_framebuffer_init()` mit
   `plane 1 too small`: kmssink beschrieb den Buffer wie normales P010 mit
   6.266.880 Byte, die Q10C-Allokation hatte nur 4.210.688 Byte.
5. Die Darstellung gelang, nachdem die Größenprüfung für MSM-UBWC-Formate
   ausgesetzt wurde. Neil bezeichnete diese Änderung ausdrücklich als Hack.
6. Die frühere Aussage zur erfolgreichen Vulkan-DMA-BUF-Darstellung enthält
   weder Quellcode noch Vulkan-Parameter. Sie belegt einen erfolgreichen Test,
   aber keinen reproduzierbaren Ablauf und keinen bitgenauen 10-Bit-Vergleich.

### Warum `P010 + QCOM_COMPRESSED` gültig ist

Der DRM-FourCC beschreibt die logische Farbanordnung: 10-Bit-Y und
verschachteltes 10-Bit-UV in zwei semantischen Ebenen. Der Modifier beschreibt
die abweichende physische Speicherung. Bei QC10C ist diese Speicherung Qualcomms
TP10-Packung mit UBWC-Metadaten; deshalb ist der Nutzdaten-Pitch ungefähr
`width × 4/3` statt `width × 2` wie bei linearem P010.

Der FourCC und Modifier allein bestimmen für einen Import noch nicht alle
Parameter. Ein Vulkan-Consumer benötigt zusätzlich Extent, Modifier-
Memory-Plane-Anzahl sowie Offset und Row-Pitch jedes Modifier-Memory-Planes.
Die [Vulkan-Erweiterung](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_image_drm_format_modifier.html)
nennt diese Werte ausdrücklich als typische Importparameter. Die
[`VkImageDrmFormatModifierExplicitCreateInfoEXT`-Spezifikation](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageDrmFormatModifierExplicitCreateInfoEXT.html)
verlangt hier zwei Layout-Einträge, weil Turnip für diese Format/Modifier-
Kombination zwei Memory-Planes meldet.

Ein V4L2-Memory-Plane ist keine Aussage über logische Farb- oder DRM-/Vulkan-
Modifier-Planes. Iris legt alle vier internen Q10C-Bereiche in eine Allokation;
ein einziger exportierter FD darf daher beide Vulkan-Memory-Planes enthalten.
Da das Vulkan-Image nicht disjoint ist, wird eine gemeinsame dedizierte
`VkDeviceMemory`-Allokation importiert und einmal an das gesamte Image gebunden.

## GStreamer

### MR 8195

[GStreamer MR 8195](https://gitlab.freedesktop.org/gstreamer/gstreamer/-/merge_requests/8195)
ist am 2026-10-08 weiterhin ein offener Draft, Head
`cd4395a4abeb802516bd49cb6477754035e9913b`. Er enthält zwei relevante
Commits:

- `QC08C -> DRM_FORMAT_NV12 + DRM_FORMAT_MOD_QCOM_COMPRESSED`
- `QC10C -> DRM_FORMAT_P010 + DRM_FORMAT_MOD_QCOM_COMPRESSED`
- für `DMA_DRM` wird `format.fmt.pix_mp.num_planes` als Zahl der
  **V4L2-Allokationsplanes** benutzt.

Für Iris-Q10C ist diese Zahl eins. Das behebt die vorherige Ablehnung eines
opaque `DMA_DRM`-Formats, bedeutet aber nicht, dass das DRM-/Vulkan-Format nur
eine Modifier-Memory-Plane hätte. Der V4L2-Bufferpool exportiert eine
DMA-BUF-Memory und trägt `sizeimage=4210688`, Offset 0 und den von V4L2
gemeldeten Pitch 2560. MR 8195 berechnet oder transportiert keinen zweiten
Q10C-Modifier-Plane-Offset von 2.805.760.

### MR 11222

[GStreamer MR 11222](https://gitlab.freedesktop.org/gstreamer/gstreamer/-/merge_requests/11222)
wurde am 2026-04-09 gemergt. Er erhält bei Caps-Filterung das
`memory:DMABuf`-Feature, wenn ein Filter `DMA_DRM` erzwingt. Er repariert die
Verhandlung, enthält aber keine QC10C-Layoutberechnung.

Eine projektweite MR-Suche nach `QC10C` und `QCOM_COMPRESSED` ergab bis zum
Stichtag nur MR 8195. Der darin zuletzt ergänzte Commit
`793c513fa735fe780eb296e503fa18c5b55fe802` übernimmt die V4L2-Plane-Anzahl;
ein späterer GStreamer-Fix für die zwei Q10C-Modifier-Layouts ist nicht
öffentlich auffindbar.

### Warum kmssink mit dem MSM-Hack trotzdem anzeigen konnte

Der aktuelle kmssink-Quellpfad wurde am GStreamer-Main-Commit
[`9d88ff5187a7faf3a6f1ea123c89da41ba952ca2`](https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/9d88ff5187a7faf3a6f1ea123c89da41ba952ca2/subprojects/gst-plugins-bad/sys/kms/gstkmssink.c)
untersucht:

- Bei `DMA_DRM` rekonstruiert kmssink zunächst normales `GstVideoInfo` für
  DRM-P010. Das erzeugt zwei logische Ebenen mit 3840 Byte Pitch und dem
  linearen zweiten Offset 4.177.920.
- Das V4L2-`GstVideoMeta` enthält bei Q10C nur ein Allokationsplane und ersetzt
  daher nur die Werte der ersten Ebene.
- kmssink verwendet denselben DMA-BUF für beide DRM-Ebenen und übergibt die
  normalen P010-Pitches/-Offsets an `drmModeAddFB2WithModifiers()`.
- Die generische MSM-Framebuffer-Größenprüfung interpretiert diese Werte als
  benötigte lineare Größe und meldet folgerichtig `plane 1 too small`.

Der [öffentliche MSM-DPU-Code](https://codebrowser.dev/linux/linux/drivers/gpu/drm/msm/disp/dpu1/dpu_formats.c.html)
berechnet bei UBWC dagegen die vier Hardwarebereiche selbst aus Format und
Bildmaßen. Er nimmt die Basisadresse der ersten Framebuffer-Ebene und leitet
daraus Y-Metadaten, Y-Daten, UV-Metadaten und UV-Daten ab. Das erklärt, warum
das Überspringen der vorherigen Größenprüfung die Anzeige ermöglichte. Dieser
MSM-spezifische implizite Vertrag ersetzt keinen sauberen, portablen
Layouttransport und ist keine geeignete Grundlage für Stereonaut.

## Mesa/Turnip

Analysiert wurde Mesa main am Commit
[`a51a418991f883413f10f66cd52398d590a8257b`](https://gitlab.freedesktop.org/mesa/mesa/-/tree/a51a418991f883413f10f66cd52398d590a8257b).
Der auf dem Frame gemeldete Kurzstand `8aa73b4b19` ist über das öffentliche
Mesa-GitLab nicht auflösbar; Quellgleichheit mit dem installierten Build ist
daher nicht bewiesen.

### A. Bedeutung des Auftrag-003-Layouts

[`tu_image.cc`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/a51a418991f883413f10f66cd52398d590a8257b/src/freedreno/vulkan/tu_image.cc)
behandelt zwei Fälle unterschiedlich:

- Eine Modifierliste lässt Turnip Pitch und Offsets selbst wählen. Dies war
  Auftrag 003 und ergab die normale P010-artige Größe von rund 6,3 MB.
- `VkImageDrmFormatModifierExplicitCreateInfoEXT` übernimmt pro Memory-Plane
  `offset` und `rowPitch` des externen Produzenten und reicht sie an
  `fdl6_layout_image()` weiter.

Die selbst gewählte Allokation ist deshalb keine Ablehnung externer QC10C-
Layouts. Auch die in Auftrag 003 beobachteten Null-Offsets sind kein Gegenbeleg:
Für nichtlineare UBWC-Modifier setzt Turnip die von
`vkGetImageSubresourceLayout()` zurückgemeldete `offset`-Angabe bewusst auf
null, weil der Modifier die Lage der internen Daten impliziert. Diese Ausgabe
ist nicht das Eingabelayout eines fremden Producers.

### B. Explizite Werte für 1920×1088

Turnip meldet für
`VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16` mit QCOM-Modifier zwei
Modifier-Memory-Planes. Das explizite Create-Info muss daher enthalten:

```c
drmFormatModifier = DRM_FORMAT_MOD_QCOM_COMPRESSED;
drmFormatModifierPlaneCount = 2;

plane[0] = {
    .offset = 0,
    .rowPitch = 2560,
    .size = 0,
    .arrayPitch = 0,
    .depthPitch = 0,
};
plane[1] = {
    .offset = 2805760,
    .rowPitch = 2560,
    .size = 0,
    .arrayPitch = 0,
    .depthPitch = 0,
};
```

Die Werte sind nicht aus dem in Auftrag 003 erzeugten Image geraten. Sie
folgen aus den veröffentlichten Iris-Formeln:

| Bereich | Größe | Beginn |
| --- | ---: | ---: |
| Y metadata | 20.480 | 0 |
| Y UBWC/TP10 data | 2.785.280 | 20.480 |
| UV metadata | 12.288 | **2.805.760** |
| UV UBWC/TP10 data | 1.392.640 | 2.818.048 |
| Ende | – | **4.210.688** |

Iris berechnet Y- und UV-Pixel-Pitch jeweils als auf 256 Byte ausgerichtetes
`width × 4/3`; für 1920 Pixel sind das 2560 Byte. Der FDL-Pfad in
[`fd6_layout.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/a51a418991f883413f10f66cd52398d590a8257b/src/freedreno/fdl/fd6_layout.c)
setzt bei explizitem Layout genau diesen Pitch und Plane-Basisoffset ein. Er
legt innerhalb jeder Vulkan-Memory-Plane zuerst das UBWC-Metadatenstück und
danach die Pixel-/Bitstreamdaten an. Damit entstehen exakt die vier
Iris-Bereiche und die gemessene Gesamtgröße.

### C. Abbildung der vier Bereiche

Die vier internen Bereiche sind keine vier Vulkan-Memory-Planes. Die
quellcodegestützte Abbildung lautet:

```text
Vulkan memory plane 0 @ 0
  UBWC metadata @ 0
  Y TP10 data   @ 20.480

Vulkan memory plane 1 @ 2.805.760
  UBWC metadata @ 2.805.760
  UV TP10 data  @ 2.818.048
```

Beide Memory-Planes liegen in derselben nicht-disjoint DMA-BUF-Allokation.
Turnips Image-Memory-Requirements müssen bei erfolgreicher expliziter
Erzeugung höchstens in die 4.210.688 Byte große Allokation passen. Auftrag 003
hat bereits ermittelt, dass externer DMA-BUF-Speicher für diese Kombination
`DEDICATED_ONLY` ist; der Import muss daher eine dedizierte Allokation mit
`VkMemoryDedicatedAllocateInfo` verwenden.

## Suche nach dem historischen Vulkan-Test

Geprüft wurden:

- der vollständige öffentliche April-Thread und die zugehörigen Patchserien;
- Neil Armstrongs öffentliche GitHub-Repositories, GitHub-Gists und
  GitLab-Projekte;
- [`pyv4l2compliance`](https://github.com/superna9999/pyv4l2compliance), das
  V4L2-Headerstreams erzeugt, aber keinen Vulkan-Renderer enthält;
- [`ffmpeg-drm`](https://github.com/superna9999/ffmpeg-drm), das DRM-Prime/KMS,
  aber keinen Vulkan-Import implementiert;
- GStreamer MR 8195, MR 11222 und den von MR 8195 verlinkten postmarketOS-MR;
- öffentliche Commit-Suchen nach `QC10C`, `QCOM_COMPRESSED` und der Formulierung
  `Vulkan DMA_BUF`.

Kein öffentliches Programm konnte eindeutig dem erwähnten Vulkan-Test
zugeordnet werden. Gesichert bleiben nur die Mailinglisten-Aussage, der
SM8650-Kontext, der DMA-BUF-Import und die visuelle Prüfung von QC10C/QC08C.
Nicht gesichert sind API-Aufruffolge, Format, Offsets/Pitches, Vulkan-Treiber,
Shader, Synchronisation und Vergleichsmethode. Insbesondere ist daraus kein
Nachweis einer bitgenauen 10-Bit-Pipeline ableitbar.

## P010-Alternativpfad

### Beobachtung auf dem Steam Frame

Der installierte Valve-Iris-Treiber:

- enumeriert nach `S_FMT(HEVC)` Q10C, aber kein P010;
- ersetzt eine vor dem Streaming angeforderte P010-Ausgabe durch NV12;
- enumeriert nach dem Main10-Source-Change ausschließlich Q10C;
- wählt Q10C automatisch und decodiert damit erfolgreich.

Damit ist P010 im bisher getesteten Zustand nicht über die V4L2-API erreichbar.
Eine absichtliche Deaktivierung ist aus diesen Beobachtungen nicht beweisbar,
weil der Valve-Quellbaum nicht öffentlich lesbar war und der Downstream-Treiber
vom aktuellen Upstream-Code abweicht.

### Upstream-Stand und bekannte Negotiation-Ursache

[Linux main am untersuchten Commit](https://github.com/torvalds/linux/blob/47324d3a5b3abd781295044d01d92d09f184e872/drivers/media/platform/qcom/iris/iris_vdec.c)
enthält seit Juni 2026 P010 und QC10C in der Iris-CAPTURE-Liste, die jeweiligen
Größen-/Stridepfade und HEVC-Main10-Unterstützung. Der am Stichtag untersuchte
Stand enthält jedoch weiterhin eine frühe Bit-Tiefenprüfung in
`check_format()`: Vor den Firmware-Streamparametern steht die Bit-Tiefe noch
auf dem 8-Bit-Default.

Die [Juli-Patchserie](https://lists.openwall.net/linux-kernel/2026/07/10/153)
beschreibt dies als konkreten Fehler. Eine valide frühe QC10C-Anforderung wird
verworfen, NV12 gespeichert und nach Erkennen von Main10 in das Defaultformat
P010 umgeschaltet. Der
[`1/2`-Patch](https://lists.openwall.net/linux-kernel/2026/07/07/492) entfernt
deshalb die Bit-Tiefenprüfung bei der CAPTURE-Formatwahl.

Dieser Bug erklärt nicht direkt den Valve-Befund: Dort wird Q10C vor den
Headern akzeptiert, P010 fehlt aber aus der Enumeration. Der Valve-Treiber
enthält somit bereits eine andere Auswahlpolitik oder einen anderen
Downstream-Stand. Ein reguläres Update mit den Upstream-10-Bit-Patches kann
P010 grundsätzlich bereitstellen; ob Valve P010 anschließend tatsächlich
exponiert, ist ohne deren Quellstand oder einen späteren Gerätetest offen.

## Entscheidungsmatrix

| Weg | Technischer Stand | Eignung für 8K Main10 | Entscheidung |
| --- | --- | --- | --- |
| A – Q10C direkt in Vulkan | Format, Modifier und explizites Zwei-Plane-Layout sind rekonstruiert und durch eine erfolgreiche Image-Erzeugung mit exakt passender Memory Requirement bestätigt; echter Import und GPU-Zugriff noch ungetestet | Beste Chance auf Zero-Copy und geringe Speicherbandbreite | **Bevorzugt; als Nächstes einmaligen echten Import-/Präzisionstest ausführen** |
| B – Iris P010 | Upstream enthält P010; Valve enumeriert es aktuell nicht. Eine bekannte frühe 10-Bit-Negotiation-Regressionsklasse existiert, trifft den Downstream-Befund aber nicht vollständig | Einfacherer Standardpfad, jedoch rund 50 % mehr unkomprimierte Bilddaten als Q10C und derzeit nicht verfügbar | Beobachten und nach regulärem SteamOS-Update erneut prüfen; keine Kerneländerung |
| C – Zwischenkonvertierung | Nach erfolgreichem A-Import kann Vulkan Q10C GPU-seitig in ein unkomprimiertes 10-Bit-/Renderingziel überführen. Ohne A oder B gibt es keinen belegten GPU-residenten Eingang | Zusätzliche GPU-Arbeit und Bandbreite; als einmalige GPU-Stufe noch vertretbar | Nur als GPU-Konvertierung nach A; keine CPU-Dekompression/-Uploads für 8K |

## Ausgeführte explizite Layoutprobe

Der Nutzer gab genau eine nicht-streamende Erzeugungsprobe frei. Die neue
[`vulkan-explicit-layout-probe.c`](../tools/vulkan-q10c-probe/vulkan-explicit-layout-probe.c)
erzeugte auf dem Frame ein ungebundenes Image mit
`VkImageDrmFormatModifierExplicitCreateInfoEXT` und folgenden festen Werten:

- Format:
  `VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16`
- Extent: 1920×1088×1, ein Mip-Level, ein Array-Layer
- Modifier: `DRM_FORMAT_MOD_QCOM_COMPRESSED`
- zwei Layouts: `(offset=0, rowPitch=2560)` und
  `(offset=2805760, rowPitch=2560)`
- Usage: exakt die bereits positiv geprüfte Kombination
  `SAMPLED | TRANSFER_SRC`
- keine Speicherallokation, kein FD, kein Binding, keine Queue-Submission und
  keine GPU-Arbeit
- 10 Sekunden Timeout plus 3 Sekunden Kill-Frist; bei Vulkan-/Treiberfehler
  sofort beenden

Kompilierung und der genau einmal ausgeführte Programmlauf endeten innerhalb
des Limits mit Exitcode 0. Das vollständige
[Ergebnislog](vulkan-video-diagnostics/run-02/vulkan-explicit-layout-probe.log)
enthält:

```text
create_image result=0
chosen_modifier=0x0500000000000001
memory size=4210688 alignment=4096 memory_type_bits=0x7
prefers_dedicated=1 requires_dedicated=1 iris_buffer_size=4210688
```

Ausgeführt wurde ausschließlich im vorhandenen Testverzeichnis:

```sh
cd ~/stereonaut-decoder-002-20261008-01/vulkan-q10c-probe
timeout -k 3s 10s cc -std=c11 -O2 -Wall -Wextra -Werror \
  -o vulkan-explicit-layout-probe vulkan-explicit-layout-probe.c -lvulkan
timeout -k 3s 10s ./vulkan-explicit-layout-probe
```

SHA-256 der Quelle:
`902038091d96e48217b0d4aa951e7a0e3560ebf1a60c781d05d0f2dd1473f0f4`.
SHA-256 des gesicherten Logs:
`15fd44f3d77b1e658e4aa19045d912f0ace430b293bdd24a93e6964cce06bae4`.
Es trat kein Timeout, Vulkan-/Treiberfehler oder Hänger auf.

Damit ist die explizite Layoutzuordnung auf dem installierten Turnip bestätigt.
Die Größe passt exakt zur Iris-Allokation; die 4096-Byte-Anforderung ist für
ein Binding bei Offset 0 grundsätzlich erfüllbar. Die tatsächliche
Importallokation und das Binding wurden jedoch nicht ausgeführt. Der Befund
beweist daher noch keinen DMA-BUF-Import, kein korrektes Bild und keinen Erhalt
der 10-Bit-Präzision.

Der notwendige echte Import- und Präzisionstest ist als
[separater Ablauf](q10c-import-test-plan.md) vorbereitet. Der einmal
freigegebene Lauf erreichte das bestätigte Q10C-SOURCE_CHANGE-Layout, brach
jedoch wegen einer zu strengen `VIDIOC_DQEVENT`-Fehlerbehandlung in der Probe
vor `DQBUF`, DMA-BUF-Export und Vulkan-Initialisierung ab. Damit bleibt der
echte Import- und Präzisionsnachweis offen. Die korrigierte Quelle wurde nicht
ausgeführt und benötigt eine neue ausdrückliche Freigabe.

> **Nachtrag – Wiederholungslauf:** Die Korrektur wurde anschließend separat
> freigegeben. Der eine Wiederholungslauf importierte einen Q10C-DMA-BUF
> erfolgreich in Turnip, kopierte beide logischen Planes, signalisierte das
> Fence und erzeugte einen 6.266.880-Byte-Readback. Der sichtbare Output stimmt
> jedoch nicht pixelgenau mit der Software-Referenz überein. Damit ist der
> Importpfad technisch erreicht, aber die korrekte Bildinterpretation und der
> verlustfreie 10-Bit-Pfad bleiben unbewiesen. Details stehen im
> [Importtestbericht](q10c-import-test-plan.md).
