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
