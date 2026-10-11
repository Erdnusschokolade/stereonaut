# Main10 – isolierte Vulkan-Baseline

Stand: 2026-10-10. Der Nutzer hat die Empfehlung aus
`main10-blocker-review.md` zur weiteren Untersuchung freigegeben.

## Read-only Befund

Kernel `6.18.0-gfbdbca41fd45`, SteamOS Build `20260922.6101926`,
Mesa `26.3.0-devel (git-8aa73b4b19)` unverändert. Paketname:
`deckard-mesa-linux-aarch64 26.3.0_devel+git8aa73b4b-1`.
Bibliothek `/usr/lib/libvulkan_freedreno.so`, Build-ID
`f2722f1dcb5052fcb8958fcca979d493fc92218d`, SHA-256
`9106f6912b452fa95a1f6a1c2289dcc5adb7e4369cc6a8d45de8462314eb5497`.

Keine Mesa-Debugquellen unter `/usr/src/debug`; keine dazugehörige
Debugbibliothek am geprüften Pfad. Der genaue Commit ist im geprüften
öffentlichen Mesa-Mirror nicht verfügbar (HTTP 404). Der bereits untersuchte
öffentliche Commit `a51a418991f883413f10f66cd52398d590a8257b` bleibt nur
Vergleichsquellcode. Dessen Transferpfad `tu_clear_blit.cc` verwendet
`tu_aspects_to_plane()`; keine TP10/P010-Sonderbehandlung wurde per Textsuche
in dieser Datei gefunden. Das ist kein Beweis für den installierten Build.

Vulkan-Headers, C-Compiler und glslc sind vorhanden; Khronos-Validation ist
weder als Layer enumeriert noch als Paket installiert. Keine Nachinstallation.
Inventur: [inventory.log](main10-diagnostics/review-20261010/inventory.log).

## Konkreter Ablauf

Die neuen Dateien `tools/vulkan-q10c-probe/vulkan-p010-baseline.c` und
`p010-planes.comp` werden vor dem aktiven Lauf committet. Verwendet wird nur
ein neues Unterverzeichnis `main10-baseline-20261010` im bestehenden
`~/stereonaut-decoder-002-20261008-01`. Kein Decoder, DMA-BUF oder OpenXR.

Zwei sequenzielle, einzeln begrenzte Prozesse:

1. `copy`: bekannter P010-Inhalt → Vulkan-Image → P010-Readback.
2. Nur wenn 1 vollständig korrekt ist: `sample`: gleicher Inhalt →
   Vulkan-Image → zwei kompatible Plane-Views → Compute-`texelFetch` → SSBO.

Beide verwenden 1920×1088, das bisherige 10-Bit-4:2:0-Vulkanformat und eine
ein-elementige QCOM-Modifierliste. **Turnip bestimmt Layout und Größe selbst.**
Keine expliziten Iris-Pitches oder Offsets; keine Aussage zur Q10C-Kompatibilität.
Die Image-Allokation ist dediziert und auf maximal 16 MiB begrenzt. Der
zurückgemeldete Modifier muss QCOM_COMPRESSED sein. Memory-Plane-Layout und
Requirements werden nur protokolliert, nicht verändert.

Copy-Usage: `TRANSFER_DST | TRANSFER_SRC`; Sampling-Usage:
`TRANSFER_DST | SAMPLED` mit `MUTABLE_FORMAT`. Jede Kombination wird separat
abgefragt. Sampling verwendet R10X6 und R10X6G10X6 mit den expliziten logischen
Plane-Aspekten. Plane-Format-Samplingfähigkeiten werden vorab geprüft.
Shader verwendet Texelkoordinaten ohne Filterung, YCbCr-Matrix oder RGB8-Ziel.
Er rekonstruiert zehn Nutzbits durch Rundung von `sample * 1023` und schreibt
sie als gepacktes P010 in den Zielbuffer.

Referenzformel unverändert aus Lauf 08. Eine separate CPU-Referenz wird vor
Submit gesichert. Nach dem Fence erfolgt der Vergleich jedes Y/U/V-Samples
mit dieser Referenz (`word >> 6`), plus vollständige Kontrolle des
Quellbuffers auf Veränderung. Vorher-Referenz, Source nach Fence und Readback
werden gespeichert und gehasht. Shader und Copy liefern je 6.266.880 Bytes.

Pro Prozess genau ein Submit und ein Fence (5 s). Upload-zu-Copy/Sampling-
und Ergebnis-zu-Host-Barriers sind explizit. HOST_COHERENT-Speicher wird
verlangt. Validation wird bei Verfügbarkeit aktiviert; Validierungsfehler,
Vulkanfehler, Mismatch, Sourceveränderung oder Timeout stoppen die Serie.
Keine automatische Wiederholung oder Layoutalternative.

```sh
timeout -k 3s 60s cc -std=c11 -O2 -Wall -Wextra -Werror \
  vulkan-p010-baseline.c -lvulkan -o vulkan-p010-baseline
timeout -k 3s 30s glslc --target-env=vulkan1.1 -fshader-stage=compute \
  p010-planes.comp -o p010-planes.comp.spv
timeout -k 3s 15s ./vulkan-p010-baseline copy \
  copy-source-before.bin copy-readback.bin copy-source-after.bin
# Nur nach erfolgreichem Copy-Vergleich:
timeout -k 3s 15s ./vulkan-p010-baseline sample \
  sample-source-before.bin sample-readback.bin sample-source-after.bin
```

Lokale Syntaxprüfung mit den vorhandenen kopierten Vulkan-Headers:
`cc -std=c11 -O2 -Wall -Wextra -Werror -I/tmp/stereonaut-vulkan-headers -fsyntax-only`.
Shader zusätzlich lokal mit glslc und spirv-val für Vulkan 1.1 geprüft.

## Interpretation

Erfolg würde Copy und per-plane Sampling für Turnips eigenes P010/QCOM-Layout
nachweisen. Die Iris-Q10C-Allokation bleibt ausdrücklich ungeprüft. Ein
Fehlschlag wird pro Stufe dokumentiert, ohne automatisch einen Decoderfehler
oder die Untauglichkeit der jeweils anderen Usage abzuleiten.

Die Quelle ist eine isolierte Ableitung der alten Selbsttestprobe. Der
Playercode wird nicht geändert. Das alte explizite 2560-Byte-Layout wird
wegen seiner ungeklärten Formatsemantik nicht erneut aktiv benutzt.

## Ausführung und Ergebnis

Quellstand `c37d7f5`, Branch `hardware/main10-baseline`:

| Artefakt | SHA-256 |
| --- | --- |
| C-Quelle | `499fb216a3058797484c8fdff4978294fa321e8c5ac55166cd0f941f666b21d3` |
| Shaderquelle | `b88a643cd004390af7c554f6e52e916922dfe6b4f47407dd806a5506df59561e` |
| Frame-Binary | `def246dc3e0399065c0b2b243bb69a7234f66dee650829840ef457785d52a6eb` |
| Auf Frame erzeugtes SPIR-V | `03409ce063934849ebd2a997201a8cd690dd97dae4409fa1a75e5e413f0e80bf` |

Quellhashes auf dem Frame geprüft. C-Build und Shaderbuild Exitcode 0.
Lokaler Shaderbuild/spirv-val ebenfalls erfolgreich; der lokale Compiler
erzeugte einen anderen SPIR-V-Hash, weshalb hier der tatsächliche Frame-Hash
steht. Der Shader wurde noch nicht ausgeführt.

**Genau ein Copy-Lauf, Exitcode 1 wegen Nutzbit-Mismatch.** Kein Timeout,
kein protokollierter Vulkanfehler. Ein Submit; Fence erfolgreich. Die
Samplingprobe wurde entsprechend der Abbruchbedingung nicht gestartet.

Turnip wählte 6.299.648 Byte, Alignment 4096, QCOM-Modifier und für beide
Memory-Planes Row-Pitch 3840. Die gemeldeten Subresource-Offsets sind wie
früher null; sie werden nicht als importierbare Producer-Offsets verwendet.

| Prüfung | Ergebnis |
| --- | --- |
| Source vor/nach GPU | Vollständig identisch und identisch zur deterministischen Referenz |
| Y-Nutzbits | 3.580 / 2.088.960 Samples falsch |
| U-Nutzbits | 0 / 522.240 falsch |
| V-Nutzbits | 0 / 522.240 falsch |
| Erstes falsches Sample | Y(0,0): erwartet 0, erhalten 935 |
| Maximale Nutzbit-Abweichung | 1018 |
| Position der Fehler | Nur x=0..447, y=0..15; sieben betroffene 32×16-Rasterblöcke |
| Vergleich mit Lauf 08 | Alle 3.580 heute falschen Samples besitzen denselben Rohwert wie an derselben Position in Lauf 08 |

Die Rasterblock-Koordinaten (x,y) sind `(0,0), (2,0), (5,0), (7,0),
(8,0), (10,0), (13,0)`. Diese räumliche Beschreibung beweist noch keine
bestimmte Hardware-Tile- oder UBWC-Blocksemantik.

| Daten | SHA-256 |
| --- | --- |
| Source vorher und nachher | `7192b373425caa2c4b0f1e6b3829969716a1b869ff0a39990a6c41818ee9d6c4` |
| Neuer Copy-Readback | `62a2c589de955a95f3e93dd6677f209bb2a54ac0521c866a4824d1b1cceeebe1` |

Je Datei 6.266.880 Bytes. Rohdaten lokal erhalten und über bestehende
`.gitignore` ausgeschlossen. Belege: [Copy-Log](main10-diagnostics/review-20261010/copy.log),
[vollständiger Referenzvergleich](main10-diagnostics/review-20261010/selftest-analysis.json),
[räumlicher Vergleich](main10-diagnostics/review-20261010/copy-spatial-analysis.json).

Offline reproduzierbar:

```sh
python3 tools/vulkan-q10c-probe/analyze-q10c-selftest.py \
  docs/main10-diagnostics/review-20261010/copy-source-after.bin \
  docs/main10-diagnostics/review-20261010/copy-readback.bin \
  docs/main10-diagnostics/review-20261010
python3 tools/vulkan-q10c-probe/analyze-p010-baseline.py \
  docs/main10-diagnostics/review-20261010 \
  --previous docs/vulkan-video-diagnostics/run-08/p010-selftest-readback.bin
```

## Schlussfolgerung und nächste Abgrenzung

Die alten großflächigen Fehler und die Sourceveränderung reproduzieren sich
mit dieser Baseline nicht. Das stützt den Verdacht gegen das alte explizite
2560-Byte-Layout. Es beweist nicht, dass alleine dessen Austausch ursächlich
war: auch Probeinstrumentierung und aktivierte YCbCr-Featuredeklaration
unterscheiden sich vom alten Lauf.

Gleichzeitig bleibt ein exakt reproduzierbarer Restfehler im gemeinsamen
Vulkan-Test-/Upload-/Copy-/Readback-Pfad, selbst bei treibereigenem Layout.
Eine ausschließlich falsche externe Iris-Q10C-Beschreibung kann den heutigen
Befund nicht erklären. Ein isolierter Copy-Fehler, UBWC-Verarbeitung oder
ein noch unerkannter Fehler der Testpipeline bleiben offen. Ein allgemeiner
Hardwaredefekt oder fehlende 10-Bit-Fähigkeit ist nicht nachgewiesen.

Der vorbereitete Samplinglauf wäre die nächste eigenständige Gegenprobe:
er ersetzt den Image-to-Buffer-Copy durch zwei `texelFetch`-Views und SSBO-
Writes. Derselbe Upload bleibt enthalten; identische Fehler könnten daher
bereits beim Upload oder in gemeinsamer Image-Verarbeitung entstehen. Ein
korrektes Ergebnis würde dagegen den Copy-out-Pfad belasten. Der Lauf
verwendet zusätzlich MUTABLE_FORMAT und SAMPLED-Usage; deshalb darf auch
dieser Vergleich nicht als perfekte Ein-Variablen-Isolation ausgegeben werden.
Er ist vorbereitet, aber nach dem heutigen Copy-Mismatch **nicht ausgeführt**.

Kein erneuter Iris-Q10C-Import: Seine Formatsemantik bleibt ungeklärt.
Die bestehenden Playerdateien und die installierte Anwendung wurden durch
diese Untersuchung nicht verändert.

## Erweiterte Fehlerbehebung (anschließende Nutzerfreigabe)

Der Nutzer hat das Finden und Beheben des Fehlers ausdrücklich freigegeben.
Kontrollierte User-Space-/Vergleichsfehler dürfen nun analysiert und mit
begründeten, vorher committeten Folgeproben untersucht werden. Bei GPU-/
Kernelhang oder systemweiter Störung weiterhin stoppen, keine Recovery.

Der ursprüngliche `sample`-Lauf wurde ausgeführt und stoppte vor GPU-Arbeit:
`TRANSFER_DST|SAMPLED` mit `MUTABLE_FORMAT` liefert
`VK_ERROR_FORMAT_NOT_SUPPORTED`. Die einzelnen Plane-Formatfähigkeiten waren
vorhanden. Das widerlegt nicht Sampling ohne MUTABLE_FORMAT.

Die nächste Quellversion enthält deshalb drei isolierte Kontrollmodi:

- `buffer`: identische Allokationsreihenfolge, aber nur `vkCmdCopyBuffer`;
  das angelegte Image wird von der GPU nicht benutzt.
- `linear`: wie `copy`, jedoch DRM-Modifier LINEAR aus der Modifierliste;
  weiterhin keine expliziten Offsets/Pitches.
- `sample-identity`: ohne MUTABLE_FORMAT ein gemeinsamer P010-View und ein
  unveränderlicher YCbCr-Sampler mit RGB_IDENTITY/FULL/NEAREST/MIDPOINT.
  Der Shader liest auf LOD 0 an Pixelzentren und schreibt zehn Nutzbits
  ohne Range-/Farbmatrixumrechnung in das SSBO. Descriptorbedarf wird
  abgefragt. Erst Capability, dann GPU-Arbeit.

Pro Modus ein sequenzieller Prozess mit `timeout -k 3s 15s`, ein Submit und
Fence; Source-Integrity-Prüfung und Referenz unverändert. Keine Iris-Zugriffe.
Die Proben werden anhand ihrer Ergebnisse ausgewertet, nicht automatisch
wiederholt. Der alte Plane-Shader bleibt als historisches Artefakt erhalten.

## Abschluss der Kontrollproben zu Quellstand `1b4bd44`

Die drei vorbereiteten Modi wurden jeweils einmal ausgeführt. Die kleinen
Textlogs bleiben als historische Diagnosebelege versioniert:

| Modus / Log | Ergebnis | Abweichende Y / U / V-Samples |
| --- | --- | --- |
| [buffer](main10-diagnostics/controls-1b4bd44/buffer.log) | Bitgenau, Exitcode 0 | 0 / 0 / 0 |
| [linear](main10-diagnostics/controls-1b4bd44/linear.log) | Bitgenau, Exitcode 0 | 0 / 0 / 0 |
| [sample-identity](main10-diagnostics/controls-1b4bd44/sample-identity.log) | Vergleich fehlgeschlagen, Exitcode 1 | 2.080.782 / 521.777 / 521.592 |

Alle drei Fences signalisierten; die Source blieb unverändert. Beim
Samplinglauf war das erste abweichende Sample 891 statt 0, die maximale
10-Bit-Abweichung betrug 1023. Ein erfolgreiches Fence belegt hier keine
korrekten Bilddaten. Die Proben verwenden synthetisches P010 ohne Iris oder
externen DMA-BUF-Import und beweisen allein keine Ursache im Decoderpfad.

Diese Testreihe ist beendet. Die inzwischen implementierte Lösung verwendet
die Adreno-TP10-Texturformate in einem eigenen Turnip-Build; Nachweise und
Abgrenzung stehen in [Main10 – TP10-Lösung](main10-linear-output.md#durchbruch-q10c-über-gepatchten-turnip-mit-tp10-hardwareformat-2026-10-10)
und im [Player-Handoff](mvp-player-handoff.md#hevc-main10-nativ-2026-10-10).
Die obigen Fehlschläge sind keine Tests dieser neuen Implementierung.
Generierte binäre Readbacks bleiben lokal erhalten und über `.gitignore`
ausgeschlossen. Für diesen Abschluss wurden keine weiteren Hardwaretests
ausgeführt.
