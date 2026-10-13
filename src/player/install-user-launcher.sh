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

# Written to a temporary file and moved into place: this script may be run by
# the launcher itself, which must not see its file change while it runs.
cat >"$bin_dir/stereonaut.new" <<'EOF'
#!/bin/sh
set -eu
# Without an argument the launcher opens the start page (local files and
# Jellyfin). The log of the last run helps to diagnose stutter and failures.
state_dir="${XDG_STATE_HOME:-$HOME/.local/state}/stereonaut"
mkdir -p "$state_dir"
log="$state_dir/launcher.log"
# The launcher runs this again in its own process after an update and when
# the XR host failed: keep the lines before.
if [ -n "${STEREONAUT_RESTART:-}${STEREONAUT_HOST_FAILED:-}" ]; then
    exec >>"$log" 2>&1
    echo "--- start script again"
else
    exec >"$log" 2>&1
fi
source_dir="${XDG_DATA_HOME:-$HOME/.local/share}/stereonaut/source"
initial=${1:-}
cd "$HOME/.local/lib/stereonaut"

# Dialogs in the language of the interface (English or German); the
# launcher only needs the C library and answers even when the rest broke.
language=$(./stereonaut-launcher --language 2>/dev/null || echo en)
text() {
    if [ "$language" = de ]; then
        printf '%s' "$2"
    else
        printf '%s' "$1"
    fi
}

# Libraries the given files need but the system no longer has (a SteamOS
# update can replace a library by a new major version).
missing_libraries() {
    ldd "$@" 2>/dev/null | sed -n 's/^[[:space:]]*\([^[:space:]]*\).*not found.*/\1/p' |
        sort -u | tr '\n' ' '
}

# Dialogs appear on the SteamVR desktop (visible in the headset) or the
# desktop session, dark like the player (under gamescope libadwaita gets no
# usable colour scheme and falls back to light); without zenity the answer
# is "no".
has_dialogs() {
    command -v zenity >/dev/null 2>&1
}

dialog() {
    ADW_DEBUG_COLOR_SCHEME=prefer-dark GTK_THEME=Adwaita:dark zenity "$@"
}

# A question ($1) with the label of its yes button ($2).
ask() {
    has_dialogs &&
        dialog --question --title=Stereonaut --text="$1" \
            --ok-label="$2" --cancel-label="$(text Cancel Abbrechen)" \
            --timeout=120
}

report_error() {
    echo "$1"
    if has_dialogs; then
        dialog --error --title=Stereonaut --timeout=120 \
            --text="$1

$(text Details Details): $log" || true
    fi
}

# Runs a command (its output to the log) under a pulsing progress bar with
# the text $1; succeeds if the command does.
with_progress() {
    message=$1
    shift
    status="$state_dir/progress.status"
    echo 1 >"$status"
    {
        if "$@" >&2; then
            echo 0 >"$status"
        fi
        echo 100
    } | if has_dialogs; then
        dialog --progress --pulsate --no-cancel --auto-close \
            --title=Stereonaut --text="$message" || true
    else
        cat >/dev/null
    fi
    [ "$(cat "$status")" = 0 ]
}

rebuild() {
    "$source_dir/src/player/build-stereonaut-session.sh" &&
        "$source_dir/src/player/install-user-launcher.sh"
}

# A release newer than this installation (any release for a build from the
# sources), or nothing.
newer_release() {
    [ -x ./stereonaut-update.sh ] && [ -s release-signers ] || return 1
    latest=$(sh ./stereonaut-update.sh check 2>/dev/null |
        sed -n 's/^latest //p')
    installed=$(sed -n 's/^STEREONAUT_VERSION=//p' MANIFEST 2>/dev/null ||
        true)
    [ -n "$latest" ] && [ "$latest" != "$installed" ] || return 1
    echo "$latest"
}

# After the problem $1 a SteamOS update may have caused: offers a newer
# release, which may be built for that SteamOS, and starts it. Returns if
# there is none or the viewer declines.
offer_update() {
    tag=$(newer_release) || return 1
    echo "offering release $tag"
    ask "$1

$(text "Version $tag is available. Install it now?" \
       "Version $tag ist verfügbar. Jetzt installieren?")" \
        "$(text Install Installieren)" || return 1
    if ! with_progress "$(text "Installing Stereonaut $tag …" \
                               "Stereonaut $tag wird installiert …")" \
            sh ./stereonaut-update.sh install "$tag"; then
        report_error "$(text "The update failed." \
                             "Das Update ist fehlgeschlagen.")"
        exit 1
    fi
    echo "update installed; starting it"
    unset STEREONAUT_HOST_FAILED
    STEREONAUT_RESTART=1 exec "$0" ${initial:+"$initial"}
}

if [ -n "${STEREONAUT_HOST_FAILED:-}" ]; then
    unset STEREONAUT_HOST_FAILED
    problem=$(text "The VR view of Stereonaut could not start or ended unexpectedly." \
                   "Die VR-Ansicht von Stereonaut konnte nicht starten oder wurde unerwartet beendet.")
    offer_update "$problem" || report_error "$problem"
    exit 1
fi

missing=$(missing_libraries stereonaut-launcher stereonaut-session \
                            stereonaut-renderer)
if [ -n "$missing" ]; then
    echo "missing libraries: $missing"
    problem=$(text "After a SteamOS update libraries are missing: $missing" \
                   "Nach einem SteamOS-Update fehlen Bibliotheken: $missing")
    if [ ! -x "$source_dir/src/player/build-stereonaut-session.sh" ]; then
        offer_update "$problem" ||
            report_error "$problem

$(text "There are no sources to rebuild from; please reinstall Stereonaut." \
       "Es liegt kein Quelltext zum Neubauen vor; bitte Stereonaut neu installieren.")"
        exit 1
    fi
    if ! ask "$problem

$(text "Rebuild Stereonaut now? It takes about 10 seconds." \
       "Stereonaut jetzt neu bauen? Das dauert etwa 10 Sekunden.")" \
            "$(text Rebuild "Neu bauen")"; then
        echo "rebuild declined or not answered"
        exit 1
    fi
    if ! with_progress "$(text "Rebuilding Stereonaut …" \
                               "Stereonaut wird neu gebaut …")" rebuild; then
        problem=$(text "Rebuilding Stereonaut failed." \
                       "Der Neubau von Stereonaut ist fehlgeschlagen.")
        offer_update "$problem" || report_error "$problem"
        exit 1
    fi
    missing=$(missing_libraries stereonaut-launcher stereonaut-session \
                                stereonaut-renderer)
    if [ -n "$missing" ]; then
        problem=$(text "Libraries are still missing after the rebuild: $missing" \
                       "Auch nach dem Neubau fehlen Bibliotheken: $missing")
        offer_update "$problem" || report_error "$problem"
        exit 1
    fi
    echo "rebuild finished"
fi

# Own Turnip with TP10 sampling for HEVC Main10, if installed and still
# loadable; otherwise Main10 is transcoded by Jellyfin.
if [ -f turnip/icd.json ]; then
    turnip_missing=$(missing_libraries turnip/libvulkan_freedreno.so)
    if [ -z "$turnip_missing" ]; then
        export VK_ICD_FILENAMES="$PWD/turnip/icd.json" STEREONAUT_TP10=1
    else
        echo "TP10 Turnip disabled, missing libraries: $turnip_missing"
    fi
fi
exec ./stereonaut-launcher ${1:+"$1"}
EOF
chmod 0755 "$bin_dir/stereonaut.new"
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
