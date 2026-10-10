#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$script_dir/build"
install_dir="$HOME/.local/lib/stereonaut"
bin_dir="$HOME/.local/bin"
applications_dir="$HOME/.local/share/applications"

for file in stereonaut-launcher stereonaut-session \
            stereonaut-renderer video-projection.vert.spv \
            nv12-projection.frag.spv tp10-projection.frag.spv; do
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
    "$install_dir/"

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

cat >"$bin_dir/stereonaut" <<'EOF'
#!/bin/sh
set -eu
# Without an argument the launcher opens the start page (local files and
# Jellyfin). The log of the last run helps to diagnose stutter and failures.
state_dir="${XDG_STATE_HOME:-$HOME/.local/state}/stereonaut"
mkdir -p "$state_dir"
exec >"$state_dir/launcher.log" 2>&1
cd "$HOME/.local/lib/stereonaut"
# Own Turnip with TP10 sampling for HEVC Main10, if installed.
if [ -f turnip/icd.json ]; then
    export VK_ICD_FILENAMES="$PWD/turnip/icd.json" STEREONAUT_TP10=1
fi
exec ./stereonaut-launcher ${1:+"$1"}
EOF
chmod 0755 "$bin_dir/stereonaut"

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
