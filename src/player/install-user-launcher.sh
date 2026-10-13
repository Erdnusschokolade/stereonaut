#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$script_dir/build"
install_dir="$HOME/.local/lib/stereonaut"
bin_dir="$HOME/.local/bin"
applications_dir="$HOME/.local/share/applications"

for file in stereonaut-launcher stereonaut-session \
            stereonaut-renderer video-projection.vert.spv \
            nv12-projection.frag.spv tp10-projection.frag.spv \
            stereo-disparity.comp.spv stereo-disparity-tp10.comp.spv \
            ui-panel.vert.spv ui-panel.frag.spv \
            controller-model.vert.spv controller-model.frag.spv \
            laser.vert.spv laser.frag.spv; do
    if [ ! -f "$build_dir/$file" ]; then
        echo "missing build artifact: $build_dir/$file" >&2
        exit 1
    fi
done

install -d "$install_dir" "$bin_dir" "$applications_dir"
install -m 0755 \
    "$build_dir/stereonaut-launcher" \
    "$build_dir/stereonaut-session" \
    "$build_dir/stereonaut-renderer" \
    "$install_dir/"
install -m 0644 \
    "$build_dir/video-projection.vert.spv" \
    "$build_dir/nv12-projection.frag.spv" \
    "$build_dir/tp10-projection.frag.spv" \
    "$build_dir/stereo-disparity.comp.spv" \
    "$build_dir/stereo-disparity-tp10.comp.spv" \
    "$build_dir/ui-panel.vert.spv" \
    "$build_dir/ui-panel.frag.spv" \
    "$build_dir/controller-model.vert.spv" \
    "$build_dir/controller-model.frag.spv" \
    "$build_dir/laser.vert.spv" \
    "$build_dir/laser.frag.spv" \
    "$install_dir/"

# Updates from the release page (settings page): the script, the keys a
# release must be signed with and, for a release package, its MANIFEST with
# the installed version. A build from the sources has no version.
project_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
install -m 0755 "$script_dir/stereonaut-update.sh" "$install_dir/"
if [ -f "$script_dir/release-signers" ]; then
    install -m 0644 "$script_dir/release-signers" "$install_dir/"
else
    rm -f "$install_dir/release-signers"
fi
if [ -f "$project_dir/MANIFEST" ]; then
    install -m 0644 "$project_dir/MANIFEST" "$install_dir/"
else
    rm -f "$install_dir/MANIFEST"
fi

# Optional: the Turnip build with TP10 sampling (tools/mesa-tp10), copied to
# build/libvulkan_freedreno.so. Only Stereonaut loads it; it enables native
# HEVC Main10. Without it Main10 is transcoded by Jellyfin as before.
turnip_dir="$install_dir/turnip"
if [ -f "$build_dir/libvulkan_freedreno.so" ]; then
    install -d "$turnip_dir"
    install -m 0755 "$build_dir/libvulkan_freedreno.so" "$turnip_dir/"
    printf '{"file_format_version": "1.0.1", "ICD": {"library_path": "%s", "api_version": "1.4.0"}}\n' \
        "$turnip_dir/libvulkan_freedreno.so" >"$turnip_dir/icd.json"
    chmod 0644 "$turnip_dir/icd.json"
    printf 'installed TP10 Turnip: %s\n' "$turnip_dir"
fi

# A copy of the sources lets the launcher rebuild itself after a SteamOS
# update has replaced a library it was linked against (see below). Skipped
# when this script already runs from that copy.
source_dir="${XDG_DATA_HOME:-$HOME/.local/share}/stereonaut/source"
if [ "$project_dir" != "$source_dir" ]; then
    rm -rf "$source_dir"
    install -d "$source_dir/src" "$source_dir/tools"
    manifest=
    [ -f "$project_dir/MANIFEST" ] && manifest=MANIFEST
    (cd "$project_dir" && tar -cf - --exclude=src/player/build \
        src/player tools/openxr-playback $manifest) |
        tar -C "$source_dir" -xf -
    printf 'installed sources for rebuilds: %s\n' "$source_dir"
fi

# Copied to a temporary file and moved into place: the start script runs
# this one for a rebuild and must not see its own file change.
install -m 0755 "$script_dir/stereonaut-start.sh" "$bin_dir/stereonaut.new"
mv "$bin_dir/stereonaut.new" "$bin_dir/stereonaut"

cat >"$applications_dir/stereonaut.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Stereonaut
Comment=Native OpenXR video player
Exec=$bin_dir/stereonaut
Terminal=false
Categories=AudioVideo;Player;
StartupNotify=false
EOF
chmod 0644 "$applications_dir/stereonaut.desktop"

printf 'installed launcher: %s\n' "$bin_dir/stereonaut"
printf 'installed desktop entry: %s\n' \
       "$applications_dir/stereonaut.desktop"
