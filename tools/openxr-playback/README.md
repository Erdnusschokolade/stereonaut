# OpenXR playback probes

`xr-vulkan-probe.c` validates the SteamVR OpenXR runtime and obtains the
runtime-selected Vulkan physical device through `XR_KHR_vulkan_enable2`. It
does not create an OpenXR session, decode video or submit rendering work.

Build on Steam Frame:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -o xr-vulkan-probe xr-vulkan-probe.c \
  -I/usr/include -lvulkan /opt/steamvr/bin/linuxarm64/libopenxr_loader.so
```

`xr-nv12-playback-probe.c` combines the proven Iris/NV12 path with the
OpenXR renderer. Its optional projection arguments are:

```text
--projection flat|180|360
--stereo mono|sbs|tb
--swap-eyes
--frames N
```

Defaults are `flat`, `mono`, normal eye order and the full input stream.
Projection and stereo layout are independent. In OpenXR primary stereo View 0
is treated as the left eye and View 1 as the right eye. Equirectangular modes
use view orientation but deliberately ignore view position.

Build the combined probe and shaders on Steam Frame with:

```sh
sh build-nv12-playback-probe.sh
```

Run host-side mapping checks with:

```sh
python3 test-projection-math.py
```

`xr-input-profile-probe.c` creates only an OpenXR instance and action set and
reports which input paths the runtime accepts as suggested bindings for the
Steam Frame, simple, generic and Index controller profiles. It needs no
session. The paths in use are listed in `button_inputs` in
`src/player/stereonaut-renderer.c`.

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -I/usr/include -o xr-input-profile-probe \
  xr-input-profile-probe.c /opt/steamvr/bin/linuxarm64/libopenxr_loader.so
```
