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
