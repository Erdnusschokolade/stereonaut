# OpenXR playback probes

`xr-vulkan-probe.c` validates the SteamVR OpenXR runtime and obtains the
runtime-selected Vulkan physical device through `XR_KHR_vulkan_enable2`. It
does not create an OpenXR session, decode video or submit rendering work.

Build on Steam Frame:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -o xr-vulkan-probe xr-vulkan-probe.c \
  -I/usr/include -lvulkan /opt/steamvr/bin/linuxarm64/libopenxr_loader.so
```
