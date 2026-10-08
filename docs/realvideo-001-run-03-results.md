# Realvideo 001: Ergebnis von Lauf 03 mit CAPTURE-Wiederverwendung

Stand: 2026-10-08. Der in `realvideo-001-run-03-test-plan.md` beschriebene
Lauf wurde genau einmal ausgeführt. Er endete kontrolliert mit Exitcode 2 bei
der vorgeschriebenen Abfrage von `V4L2_CID_MIN_BUFFERS_FOR_CAPTURE`. Es gab
keinen Timeout, Hänger, Wiederholungs- oder Recovery-Versuch.

## Reproduzierbarer Stand

| Bestandteil | Wert |
|---|---|
| Quellcommit | `9c48185` |
| ausgeführte C-Quelle | SHA-256 `2036355f803f0ab4cd31b3aaa594f1ad5d030b4bcadd6c40c5738493161f510a` |
| separates Zielbinary | SHA-256 `110ef31e6f082d18e3bb29bf27644f105282a723d868d72a676000a6ac0018af` |
| unveränderter HEVC-Stream | SHA-256 `f78332e37281b24c9c3a3304df785b914d97f84720f3cb5930e3bf0fcadcfa3d` |
| Prozessbegrenzung | `timeout -k 5s 420s` |
| Ergebnis | Exitcode 2 nach 300 Frames, kein Timeout |
| Log | `tests/realvideo-001/realvideo-001-run-03.log` |
| Log-SHA-256 | `56ef23de634cb84d51bb6f4b982207db9dd7c85bd2141ca73b793130060cff6a` |

Quelle und Stream wurden vor dem Build auf dem Steam Frame gegen diese Hashes
geprüft. Das Lauf-03-Binary wurde separat mit `-std=c11 -O2 -Wall -Wextra
-Werror` und den vorhandenen Vulkan-Werkzeugen gebaut. Lauf 01 und Lauf 02
wurden nicht verändert.

## Bestätigter Ablauf

Der Initialpfad bestätigte erneut:

- Turnip unterstützt für den Test lineares NV12 mit zwei Modifier-Memory-
  Planes, `SAMPLED | TRANSFER_SRC`, DMA-BUF-Import und SYNC_FD.
- Iris lieferte NV12 8192×4096, eine V4L2-Memory-Plane, Stride 8192 und
  `sizeimage=50.331.648`.
- Alle zwölf tatsächlichen CAPTURE-Allokationen hatten 50.331.648 Byte und
  wurden genau einmal exportiert und in Vulkan importiert.
- Frame 0 bis Frame 299 wurden verarbeitet.
- Der zweite `SOURCE_CHANGE` wurde erkannt und die alte Generation bis zum
  leeren `LAST` abgearbeitet; OUTPUT blieb aktiv.
- Nach CAPTURE-`STREAMOFF` meldeten `G_FMT` und `G_SELECTION` erneut exakt
  dasselbe Speicherlayout und Compose-Rechteck.
- Der neue Vergleich protokollierte
  `memory_compatible=1`, `compose_unchanged=1` und zwölf bestehende Buffer.

Damit ist erstmals im ausgeführten Wiederverwendungszweig bestätigt, dass der
Speichervertrag am wiederholten SPS/VPS-/IDR-Punkt vollständig kompatibel ist.
Es wurde kein `REQBUFS(0)`, keine Neuanforderung und kein zweiter DMA-BUF- oder
Vulkan-Import ausgeführt.

## Kontrollierter Stopp am Minimum-Control

Bereits beim Initialaufbau ergab die optionale Abfrage:

```text
capture_minimum_buffers=unavailable errno=22
```

Die initialen zwölf Buffer wurden daraufhin wie in den vorherigen Läufen
angefordert, vom Treiber bereitgestellt und erfolgreich verwendet. Nach dem
dynamischen Wechsel verlangte der Lauf-03-Plan dieselbe Abfrage als zwingendes
Gate. Iris antwortete erneut:

```text
VIDIOC_G_CTRL MIN_BUFFERS_FOR_CAPTURE: errno=22 (Invalid argument)
```

Die Probe stoppte deshalb vor `QUERYBUF` der behaltenen Allokationen, vor dem
erneuten `QBUF` und vor CAPTURE-`STREAMON`. Der eigentliche Restart mit den
behaltenen Buffern wurde somit noch nicht nachgewiesen.

Die offizielle
[Capture-Setup-Dokumentation](https://docs.kernel.org/userspace-api/media/v4l/dev-decoder.html#capture-setup)
fordert für die unmittelbare Wiederverwendung ausreichend große Buffer und
eine vorhandene Anzahl mindestens in Höhe des zuvor ermittelten Minimums. Sie
beschreibt die Abfrage von `V4L2_CID_MIN_BUFFERS_FOR_CAPTURE` zugleich als
optionalen Weg, um beim Allokieren zusätzliche Pipeline-Buffer einzuplanen.
Auf diesem installierten Iris-Pfad ist der Control-Zugriff mit `EINVAL` nicht
verfügbar. Das Log erlaubt daher keine numerische Bestätigung eines aktuellen
Minimums.

Dieser Fehler ist ein bewusstes Userspace-Sicherheits-Gate. Er ist kein
Nachweis eines Decoder-, DMA-BUF-, Vulkan- oder Speicherfehlers und sagt noch
nicht aus, ob der anschließende `QBUF`-/`STREAMON`-Restart funktioniert hätte.

## Pixelkorrektheit und Checkpoint-Zeiten

Es entstand nur der vorgesehene Readback von Frame 0. Seine 50.331.648 Byte
haben SHA-256
`775dba0fca937d3be19d607a8a44daff27df4e74ab8b0e22e383ea864795e040`.
Der Offlinevergleich in `tests/realvideo-001/comparison-run-03.json` ergab:

| Plane | Byte | Abweichungen | maximale absolute Abweichung |
|---|---:|---:|---:|
| Y | 33.554.432 | 0 | 0 |
| U | 8.388.608 | 0 | 0 |
| V | 8.388.608 | 0 | 0 |
| Gesamt | 50.331.648 | 0 | 0 |

Frame 0 benötigte 3,943073 ms GPU-Zeit und 4,648202 ms Submit-bis-Fence.
Frame 299 benötigte 4,027500 ms beziehungsweise 4,707054 ms. Der frühe Stopp
verhindert weiterhin aggregierte Langzeitwerte.

## Status und begrenzter nächster Vorschlag

Der vollständige 120-Sekunden-Nachweis und die OpenXR-Bereitschaft bleiben
offen. Lauf 03 bestätigt jedoch alle vorgesehenen Speicherkompatibilitätswerte
vor dem eigentlichen Requeue.

Ein nächster Kandidat müsste die Control-Verfügbarkeit als Eigenschaft des
Treibers behandeln: Wenn `V4L2_CID_MIN_BUFFERS_FOR_CAPTURE` bereits beim
Initialaufbau mit `EINVAL` fehlt und beim Wechsel weiterhin fehlt, könnte er
ohne numerischen Minimumvergleich fortfahren, aber weiterhin den unveränderten
Speichervertrag, das Compose-Rechteck, die gespeicherte Anzahl von zwölf
Buffern und jede per `QUERYBUF` gemeldete Länge zwingend prüfen. Erst danach
dürften dieselben Buffer mit `QBUF` und `STREAMON` wiederverwendet werden.

Diese funktionale Variante ist in Commit `ba240bd` lokal vorbereitet. Der
exakte Diff, die unveränderten Parameter und der einmalige Laufvorschlag stehen
in `realvideo-001-run-04-test-plan.md`. Sie wurde nicht auf dem Steam Frame
ausgeführt und benötigt vor einem aktiven Test eine neue Freigabe. Es wurden
keine Paket-, Kernel-, Mesa-, Treiber-, Firmware- oder Systemänderungen
vorgenommen.
