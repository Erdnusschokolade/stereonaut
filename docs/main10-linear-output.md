# Main10 – lineare Iris-Ausgabe nach dem SOURCE_CHANGE

Stand: 2026-10-10. Ziel: prüfen, ob Iris für HEVC Main10 ein unkomprimiertes
CAPTURE-Format liefert, damit der Q10C-/UBWC-Import (siehe
[main10-baseline-test.md](main10-baseline-test.md)) umgangen werden kann.
Bisher war P010 nur **vor** dem Streaming angefragt worden; dort kennt der
Treiber die Bittiefe noch nicht und ersetzt P010 durch NV12.

Werkzeug: `tools/decoder-probe/v4l2-decode.c`, neuer Modus `after-FOURCC`
(Commits `46edf62`, Folgecommit): CAPTURE bleibt bis zum SOURCE_CHANGE beim
Treiberstandard, danach `TRY_FMT` und `S_FMT` mit dem gewünschten Format.
Eingaben: `tests/decoder-fixtures/main10.hevc` (testsrc2) und
`gradient10.hevc`, je 60 Frames 1920×1080. Nur V4L2, kein Vulkan/OpenXR.
Logs: [linear-output-20261010](main10-diagnostics/linear-output-20261010/).

## Ergebnis

| Anfrage nach SOURCE_CHANGE | Treiber meldet | Daten | Bewertung |
| --- | --- | --- | --- |
| P010 | `TRY_FMT`/`S_FMT` → Q10C | – | P010 ist im V4L2-Formatsatz des Treibers nicht vorhanden |
| NV12 | NV12, Stride 1920, 3.133.440 B | 60/60 Frames, `bytesused` = NV12-Größe | **Geometrie bitgenau, Werte gesättigt:** `hw = min(ref10, 255)` für Y, U und V; unterhalb 255 exakt gleich der 10-Bit-Referenz |
| AB24 / QC24 | AB24/QC24, 8,3 MB | 60/60, aber `bytesused` = 4.210.688 (Q10C-Größe), Inhalt beginnt mit UBWC-Metadaten | Firmware ignoriert die RGB-Anfrage |
| Q08C | Q08C, 3.153.920 B | 60/60 | nicht ausgewertet (komprimiert); vermutlich derselbe Wandler wie NV12 |
| NV21 | NV21 | 0 Frames, Stillstand | nicht nutzbar |

Nach dem SOURCE_CHANGE enumeriert der Treiber nur noch Q10C; `S_FMT` akzeptiert
trotzdem jedes Format seiner Tabelle. Die Modulstrings zeigen die
Treiberformate `FMT_NV12`, `FMT_NV12C`, `FMT_TP10C` (plus NV21/RGBA);
`HFI_COLOR_FMT_P010` existiert nur als Firmwarekonstante ohne V4L2-Zuordnung.

Die NV12-Werteabbildung wurde über alle Samples des ersten Frames ermittelt
(Verlauf 64…940 in Y): bis 253 identisch, alle höheren Werte 255. Die Firmware
schreibt die 10-Bit-Samples also ohne Bittiefenwandlung in die 8-Bit-Ausgabe.
Die Decodierung selbst ist korrekt (Formen, Kanten, Bewegungsinhalt stimmen).
Es gibt keine V4L2-Control für Ausgabebittiefe oder Dithering.

## Folgerungen

1. Ein linearer 10-Bit- oder korrekt gewandelter 8-Bit-Ausgang ist mit diesem
   Kernelmodul nicht erreichbar. Eine Änderung (P010-Zuordnung oder
   Bittiefenwandlung im HFI-Setup) läge im Kerneltreiber und braucht
   Rücksprache nach `AGENTS.md`.
2. Damit bleibt nur Q10C (TP10 + UBWC). Die gesicherten Mesa-Quellen
   (`fd6_format_table.c`) enthalten keine TP10-/P010-Hardwareformate; Vulkan-P010
   läuft über generische 16-Bit-Planes. Turnip kann Iris-TP10-UBWC daher
   voraussichtlich nicht direkt korrekt lesen – passend zu den Copy-/
   Sampling-Fehlern der bisherigen Proben.
3. Offene, noch nicht freigegebene Idee: Die TP10-Y-Ebene hat dieselbe
   Bytegeometrie wie ein 32-bpp-Bild (1920 px = 640 Wörter à 4 Byte, Pitch
   2560). Falls die UBWC-Dekompression nur von der Bytegeometrie abhängt,
   könnte ein Import als RGBA8-UBWC-Bild plus eigenes TP10-Entpacken im
   Shader funktionieren. Das ist eine neue UBWC-Probe und muss vorher
   freigegeben werden.
4. Bis dahin bleibt Main10 auf die Jellyfin-Transkodierung nach HEVC Main 8
   angewiesen (bis 4096 px Breite).

## Nachtrag: SteamOS-Update und TP10 über 32-bpp-UBWC (2026-10-10)

**Update** auf Build `20261007.6125817` (vorher `20260922.6101926`): Kernel
`6.18.0-ge66bc2ca6f8c` mit neuem `iris.ko` (SHA-256 `7a5647a0…`, vorher
`0b96e219…`), Mesa `26.3.0_devel+gitabc426bb` (`libvulkan_freedreno.so`
`dfed0652…`), Firmware `vpu33_4v.mbn` unverändert (`7b829fc1…`). Das
Iris-Verhalten ist gleich geblieben: P010 nach den Headern → Q10C, NV12-Dump
bytegleich mit dem Stand vor dem Update. Die Benutzerinstallation
(`~/.local/lib|bin|share/…stereonaut`, Konfiguration, SteamVR-Manifest,
`appconfig.json`-Eintrag) hat das Update unverändert überstanden; der Player
lief danach (8K-HEVC, 4K-H.264). Ein erster Start 45 s nach dem Booten
scheiterte einmalig mit `VIDIOC_REQBUFS OUTPUT: ENOMEM` (`current session
not supported(-12)`), der Wiederholungsversuch lief.

**Probe** `tools/vulkan-q10c-probe/v4l2-vulkan-tp10-rgba.c` (freigegeben):
Q10C-Frame → DMA-BUF → je TP10-Ebene ein Vulkan-Bild eines 32-bpp-Formats mit
QCOM-UBWC-Modifier (640×1088 bzw. 640×544, Pitch 2560) → `vkCmdCopyImageToBuffer`
→ Auswertung `analyze-tp10-rgba.py` (drei 10-Bit-Werte je Wort, beide
Reihenfolgen).

| Format | Layout | Ergebnis |
| --- | --- | --- |
| R8G8B8A8_UNORM | Turnips UBWC-Layout endet exakt an den Iris-Ebenengrenzen (2.805.760 / 4.210.688 B) | GPU-Lauf sauber, Werte falsch (Y 2.071.584/2.073.600 abweichend) |
| A2B10G10R10_UNORM_PACK32 | dito | Werte falsch, andere Bytes als RGBA8 |
| R32_UINT | dito | Werte falsch, wieder andere Bytes |

Die Rohdaten sind komprimiert (Metadaten überwiegend `0x11`), der Readback
unterscheidet sich vom Rohpuffer und spaltenweise Struktur bleibt teilweise
erhalten: Die GPU dekomprimiert, rekonstruiert aber formatabhängig. Die
Bytegeometrie stimmt, die UBWC-Kodierung von TP10 entspricht keinem der
getesteten 32-bpp-Farbformate. Belege:
[tp10-ubwc-20261010](main10-diagnostics/tp10-ubwc-20261010/).

**Stand:** Mit Kernel und Mesa des Systems gibt es keinen bekannten Weg,
Iris-Main10 korrekt auf die GPU zu bringen. Verbleibende Möglichkeiten,
jeweils nicht begonnen:

1. Eigene Turnip-Build (nur im Prozess über `VK_ICD_FILENAMES`, keine
   Systemänderung) mit einem TP10-Hardwareformat, falls Adreno eines hat –
   nicht geprüft, größerer Aufwand.
2. Kerneltreiber: P010 freischalten oder 10→8-Bit-Wandlung konfigurieren
   (Systemeingriff, Rücksprache).
3. Jellyfin-Transkodierung nach HEVC Main 8 (vorhanden, ≤ 4096 px).

## Durchbruch: Q10C über gepatchten Turnip mit TP10-Hardwareformat (2026-10-10)

Die Adreno-Registerbeschreibung (`a6xx_enums.xml` im Kernel) enthält
`FMT6_TP10` (0x9d), `FMT6_TP10_Y` (0x9e) und `FMT6_TP10_UV` (0x9f), mit dem
Hinweis, dass Tiling/UBWC dieser Medienformate von äquivalenten Farbformaten
abweicht. Turnip nutzt sie nicht – deshalb lieferten alle Standardformate
falsche Werte.

**Aufbau** (Freigabe Option 1, nur Userspace, kein Systemeingriff):

- Mesa `main` (`tools/mesa-tp10/MESA_COMMIT`, Quellen unter
  `third_party/mesa`, per `.gitignore` ausgenommen) wird auf dem
  Entwicklungsrechner mit clang/lld gegen einen vom Frame kopierten Sysroot
  cross-kompiliert, nur Turnip (`tools/mesa-tp10/build-turnip.sh`,
  675 Build-Schritte, wenige Minuten). Geladen wird die Bibliothek
  ausschließlich über `VK_ICD_FILENAMES` im eigenen Prozess.
- Gegencheck ohne Patch: Der Player läuft mit diesem Turnip unverändert
  (8K-HEVC, 300 Frames, 0 Drops, [Log](main10-diagnostics/tp10-turnip-20261010/player-custom-turnip.log)).
- Patch `tools/mesa-tp10/tp10-view.patch` (17 Zeilen in
  `fdl6_view_init`): Ist `STEREONAUT_TP10` gesetzt, werden UBWC-Views im
  Format RGBA8 bzw. RGB10A2 auf `FMT6_TP10_Y` bzw. `FMT6_TP10_UV` umgestellt
  (Swap WZYX, Breite ×3 bzw. ×3/2). Das 32-bpp-UBWC-Layout passt exakt zur
  Iris-Allokation (siehe oben), es wird also nur das Texturformat getauscht.
- Probe `tools/vulkan-q10c-probe/v4l2-vulkan-tp10-sample.c` + `tp10-sample.comp`:
  Q10C-Frame → DMA-BUF → zwei Views → `texelFetch` im Compute-Shader →
  `round(x·1023)` → Vergleich `analyze-tp10-sample.py`.

**Ergebnis** (Turnip-Build SHA-256 `269e64ea…`):

| Lauf | Y | U | V |
| --- | --- | --- | --- |
| gradient10, `STEREONAUT_TP10=1` | 0 / 2.073.600 | 0 / 518.400 | 0 / 518.400 |
| main10 (testsrc2), `STEREONAUT_TP10=1` | 0 / 2.073.600 | 0 / 518.400 | 0 / 518.400 |
| gradient10, ohne Schalter (Kontrolle) | 2.072.331 falsch | 518.212 falsch | 517.860 falsch |

Iris-Main10 wird damit **bitgenau in 10 Bit** auf der GPU gelesen, ohne
Kopie und ohne Kerneländerung. Belege:
[tp10-turnip-20261010](main10-diagnostics/tp10-turnip-20261010/).

**Offen für den Player:** saubere Aktivierung statt Umgebungsvariable und
Formattrick, Sampler im Projektionsshader (zwei Ebenen statt
YCbCr-Sampler, Farbmatrix im Shader), Session wählt Q10C für Main10,
OpenXR/SteamVR mit eigenem Turnip im Renderer, Leistung bei 8K60,
Höhen/Breiten außerhalb 1920×1088 (UBWC-Layout muss für jede Größe zur
Iris-Allokation passen), Auslieferung der eigenen Bibliothek.

### Größen bis 8K (2026-10-10)

Die Probe liest die Geometrie jetzt aus dem SOURCE_CHANGE und berechnet die
Ebenenoffsets nach den Venus/Iris-TP10-UBWC-Formeln (Y-Metadaten,
Y-Daten, UV-Metadaten, UV-Daten, je 4-KiB-ausgerichtet). Iris polstert die
Breite auf ganze 192-Pixel-Kacheln (4096 → 4224, 8192 → 8256); Wörter je
Zeile und Metadaten-Pitch folgen der gepolsterten Breite, gesampelt wird die
sichtbare. Testclips: libx265 Main10, testsrc2, je erstes Bild gegen
FFmpeg-Softwaredecode.

| Größe | Iris-CAPTURE (Breite, Stride, Größe) | Formel = sizeimage | Turnip-Layout passt | Y / U / V falsch |
| --- | --- | --- | --- | --- |
| 1920×1080 | 1920, 2560, 4.210.688 | ja | ja | 0 / 0 / 0 |
| 3840×1920 | 3840, 5120, 14.839.808 | ja | ja | 0 / 0 / 0 |
| 4096×2048 | 4224, 5632, 17.399.808 | ja | ja | 0 / 0 / 0 |
| 5760×2880 | 5760, 7680, 33.320.960 | ja | ja | 0 / 0 / 0 |
| 7680×3840 | 7680, 10240, 59.260.928 | ja | ja | 0 / 0 / 0 |
| 8192×4096 | 8256, 11008, 67.928.064 | ja | ja | 0 / 0 / 0 |

Logs: `main10-diagnostics/tp10-turnip-20261010/run-size-*.log`.
