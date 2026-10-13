#!/bin/sh
# Registers Stereonaut as a SteamVR application, so it can be started from
# the SteamVR library instead of the desktop. Run after
# install-user-launcher.sh. This changes the persistent SteamVR app list
# (~/.config/openvr/config/appconfig.json via AddApplicationManifest) once;
# running it again only updates the manifest, as SteamVR already lists it. To remove the entry, delete the
# manifest path from "manifest_paths" in appconfig.json while SteamVR is not
# running.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
data_dir="${XDG_DATA_HOME:-$HOME/.local/share}/stereonaut"
manifest="$data_dir/stereonaut.vrmanifest"
vrpath="$HOME/.config/openvr/openvrpaths.vrpath"
launcher="$HOME/.local/bin/stereonaut"

# SteamVR's tool: on the Frame in /opt/steamvr, on a PC in the runtime
# directories listed in openvrpaths.vrpath.
find_vrcmd() {
    if [ -n "${VRCMD:-}" ]; then
        echo "$VRCMD"
        return
    fi
    {
        echo /opt/steamvr
        [ -f "$vrpath" ] &&
            sed -n '/"runtime"/,/]/s/.*"\(\/[^"]*\)".*/\1/p' "$vrpath"
    } | while read -r runtime; do
        for platform in linuxarm64 linux64; do
            if [ -x "$runtime/bin/$platform/vrcmd" ]; then
                echo "$runtime/bin/$platform/vrcmd"
                return
            fi
        done
    done | head -n 1
    command -v vrcmd 2>/dev/null || true
}
vrcmd=$(find_vrcmd | head -n 1)

if [ ! -x "$launcher" ]; then
    echo "launcher missing: run install-user-launcher.sh first" >&2
    exit 1
fi

install -d "$data_dir"
install -m 0644 "$script_dir/assets/stereonaut-library.png" \
    "$data_dir/stereonaut-library.png"
cat >"$manifest" <<EOF
{
  "source": "builtin",
  "applications": [
    {
      "app_key": "stereonaut.player",
      "launch_type": "binary",
      "binary_path_linux_arm": "$launcher",
      "binary_path_linux": "$launcher",
      "is_dashboard_overlay": false,
      "image_path": "$data_dir/stereonaut-library.png",
      "strings": {
        "en_us": {
          "name": "Stereonaut",
          "description": "VR video player"
        },
        "de_de": {
          "name": "Stereonaut",
          "description": "VR-Videoplayer"
        }
      }
    }
  ]
}
EOF
chmod 0644 "$manifest"

# SteamVR keeps the registered manifests in appconfig.json of its config
# directories (listed in openvrpaths.vrpath).
registered() {
    {
        echo "$HOME/.config/openvr/config"
        [ -f "$vrpath" ] &&
            sed -n '/"config"/,/]/s/.*"\(\/[^"]*\)".*/\1/p' "$vrpath"
    } | while read -r directory; do
        if grep -qF "\"$manifest\"" "$directory/appconfig.json" 2>/dev/null
        then
            echo yes
        fi
    done | grep -q yes
}
if registered; then
    printf 'SteamVR app already registered, manifest updated: %s\n' \
        "$manifest"
    exit 0
fi

if [ -z "$vrcmd" ] || [ ! -x "$vrcmd" ]; then
    echo "SteamVR tool not found: $vrcmd" >&2
    exit 1
fi
"$vrcmd" --appmanifest "$manifest"
printf 'registered SteamVR app: %s\n' "$manifest"
