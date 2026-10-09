# Realvideo 002 – OpenXR-Vulkan-Grundlage

Stand: 2026-10-08. Dieser Zwischenstand führt noch **keine** Videodekodierung
oder DMA-BUF-Übertragung aus. Er grenzt die OpenXR-/Vulkan-Basis ab, damit der
bestehende, erfolgreiche Realvideo-001-Decoderpfad anschließend ohne eine
zweite unbekannte Variable eingebunden werden kann.

## Bestätigte Fakten

Der SteamVR-OpenXR-Loader liegt unter
`/opt/steamvr/bin/linuxarm64/libopenxr_loader.so`; die aktive Runtime ist
SteamVR. Die Runtime stellt `XR_KHR_vulkan_enable2` (Version 4) bereit.

`xrCreateInstance` mit der neueren Version aus den installierten Headern
scheiterte mit `XR_ERROR_INITIALIZATION_FAILED`. Dieselbe Probe funktioniert
mit der angeforderten OpenXR-API-Version 1.0. Das ist eine Runtime-Eigenheit
dieser Installation; die verwendete Vulkan-enable2-Erweiterung ist dabei
verfügbar.

Die erfolgreiche Capability-Probe meldete folgende Runtime-Auswahl:

```
openxr_runtime name=SteamVR/OpenXR
openxr_system ... max_swapchain=8192x8192
openxr_vulkan_device name=Turnip Adreno (TM) 750 vendor=0x5143 device=0x43051401
```

Die Runtime-eigene Vulkan-Gerätewahl wird somit übernommen, statt das frühere
separat erzeugte Decodergerät ungeprüft an eine OpenXR-Session zu binden.

## Quad-Probe

`tools/openxr-playback/xr-quad-probe.c` erstellt über
`XR_KHR_vulkan_enable2` Instanz, System, Vulkan-Instanz/-Gerät, Session,
LOCAL-Reference-Space und einen Stereo-Array-Swapchain. Die beiden Augen
verwenden die von `xrLocateViews` gelieferten Pose- und FOV-Daten. Die Probe
rendert ein asymmetrisches blau/rotes 2:1-Quad in zwei Metern Entfernung. Das
gelbe Feld oben links dient zur Orientierungsprüfung.

Build auf dem Frame im bestehenden Testverzeichnis:

```
sh build-quad-probe.sh
```

Der begrenzte Lauf verwendete dieselbe Sitzungsumgebung wie SteamVR:

```
timeout -k 3s 45s env DISPLAY=:0 STEAM_GAME_DISPLAY_0=:1 \
  XDG_RUNTIME_DIR=/run/user/1000 \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
  ./xr-quad-probe
```

SteamVR protokollierte dafür die Registrierung als OpenXR-Instanz und den
Übergang zu `VRApplication_OpenXRScene`; die Runtime legte einen
1728-Pixel-Swapchain an. Bei einem Lauf mit getragenem Headset wechselte die
Session nach `VISIBLE` und `FOCUSED`. Der Compositor verzeichnete 2.158
Presents ohne Drops; die App-GPU-Zeit lag bei 1,064 ms. Der Prozess wurde
nach seiner absichtlich auf rund 30 Sekunden begrenzten Laufzeit regulär
abgemeldet und hinterließ keinen Prozess.

Der Sichtnachweis ist erbracht: Im Headset war das 2:1-Quad mit gelbem Feld
oben links sowie einem rot-blauen vertikalen Verlauf und Schachbrettmuster
sichtbar. Die Orientierung war damit korrekt; die Darstellung blieb bei
Kopfbewegungen stabil.

## Noch offen

Der nächste Implementierungsschritt muss die Realvideo-001-Komponenten
semantisch unverändert in dieselbe, von OpenXR erzeugte Vulkan-Device
integrieren:

1. die zwölf Iris-NV12-CAPTURE-Buffers und ihre linearen DMA-BUF-Imports;
2. einen YCbCr-Conversion-Sampler für `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`;
3. pro Bild das Warten auf den Producer-Sync-FD und die Rückgabe des
   CAPTURE-Buffers erst nach dem Render-Fence;
4. einen Decoderpoll, der vom `xrWaitFrame` getrennt bleibt, sowie die
   Wiederverwendung des letzten vollständigen Bildes;
5. einen mehrminütigen Lauf mit dem echten HEVC-Main-8-Bit-Teststream.

Bis dahin darf die Quad-Probe nicht als Realvideo-Wiedergabe oder als
Nachweis der NV12-Farbkonversion gelten.
