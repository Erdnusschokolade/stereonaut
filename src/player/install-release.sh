#!/bin/sh
# Installs a Stereonaut release package for the current user (into ~/.local,
# like install-user-launcher.sh) and adds it to the SteamVR library unless
# it is there already or --no-steamvr is given:
#
#   ./install.sh [--no-steamvr] [--rebuild]
#
# A package for the Steam Frame holds binaries: on the SteamOS build it was
# made on (MANIFEST) they are installed as they are, on another build they
# are kept if every library they need is present, otherwise they are
# rebuilt from the included sources (SteamOS on the Frame ships the
# compiler). A source package (for Linux PCs) is always built; missing
# build tools and libraries are named with the command that installs them.
# --rebuild always rebuilds. Copied into the packages as install.sh by
# make-release.sh and tools/release/make-source-release.sh.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
player="$here/src/player"
register=1
rebuild=0
for argument in "$@"; do
    case "$argument" in
    --no-steamvr) register=0 ;;
    --rebuild) rebuild=1 ;;
    -h | --help)
        sed -n '2,15s/^# \{0,1\}//p' "$0"
        exit 0
        ;;
    *)
        echo "unknown option: $argument" >&2
        exit 2
        ;;
    esac
done

manifest_value() {
    sed -n "s/^$1=//p" "$here/MANIFEST" | head -n 1
}
os_value() {
    sed -n "s/^$1=//p" /etc/os-release | tr -d '"' | head -n 1
}
# Libraries (or symbol versions) the binaries need but this system lacks.
missing_libraries() {
    (cd "$player/build" &&
        ldd stereonaut-launcher stereonaut-session stereonaut-renderer 2>&1) |
        sed -n -e 's/^[[:space:]]*\([^[:space:]]*\) => not found.*/\1/p' \
            -e "s/.*version \`\([^']*\)' not found.*/\1/p" |
        sort -u | tr '\n' ' '
}

# The command that installs the build dependencies on this distribution.
install_hint() {
    family=" $(os_value ID) $(os_value ID_LIKE) "
    case "$family" in
    *" arch "*)
        echo "sudo pacman -S --needed gcc pkgconf shaderc ffmpeg alsa-lib" \
             "curl json-c libjpeg-turbo freetype2 libpng vulkan-icd-loader" \
             "vulkan-headers openxr"
        ;;
    *" debian "* | *" ubuntu "*)
        echo "sudo apt install gcc pkgconf glslc libavformat-dev" \
             "libavcodec-dev libavutil-dev libswresample-dev libasound2-dev" \
             "libcurl4-openssl-dev libjson-c-dev libjpeg-dev libfreetype-dev" \
             "libpng-dev libvulkan-dev libopenxr-dev"
        ;;
    *" fedora "*)
        echo "sudo dnf install gcc pkgconf-pkg-config glslc ffmpeg-free-devel" \
             "alsa-lib-devel libcurl-devel json-c-devel libjpeg-turbo-devel" \
             "freetype-devel libpng-devel vulkan-loader-devel vulkan-headers" \
             "openxr-devel"
        echo "(for decoding on NVIDIA GPUs: ffmpeg-devel from RPM Fusion" \
             "instead of ffmpeg-free-devel)"
        ;;
    *)
        echo "the development files of these pkg-config modules:" \
             "libavformat libavcodec libavutil libswresample alsa libcurl" \
             "json-c libjpeg freetype2 libpng vulkan openxr, and gcc," \
             "pkg-config and glslc"
        ;;
    esac
}

# Build tools and libraries a PC build needs (the Frame's SteamOS has them,
# with its own OpenXR loader); false after naming what is missing.
check_build_dependencies() {
    missing=
    for tool in cc pkg-config glslc; do
        command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
    done
    if command -v pkg-config >/dev/null 2>&1; then
        for module in libavformat libavcodec libavutil libswresample alsa \
                      libcurl json-c libjpeg freetype2 libpng vulkan openxr; do
            pkg-config --exists "$module" || missing="$missing $module"
        done
    fi
    [ -z "$missing" ] && return 0
    echo "missing for the build:$missing" >&2
    echo "install them with:" >&2
    install_hint >&2
    return 1
}

if [ ! -f "$here/MANIFEST" ]; then
    echo "not a release package: MANIFEST missing" >&2
    exit 1
fi
version=$(manifest_value STEREONAUT_VERSION)
package_arch=$(manifest_value ARCH)

if [ "$package_arch" = source ]; then
    printf 'Stereonaut %s from the sources\n' "$version"
    rebuild=1
else
    if [ ! -f "$here/SHA256SUMS" ]; then
        echo "not a release package: SHA256SUMS missing" >&2
        exit 1
    fi
    if [ "$(uname -m)" != "$package_arch" ]; then
        echo "this package is for $package_arch, this system is $(uname -m)" >&2
        exit 1
    fi
    if ! (cd "$player/build" && sha256sum -c --quiet "$here/SHA256SUMS"); then
        echo "the package is damaged (checksums differ)" >&2
        exit 1
    fi
    built_build=$(manifest_value STEAMOS_BUILD_ID)
    built_for="SteamOS $(manifest_value STEAMOS_VERSION_ID) ($built_build)"
    running="$(os_value NAME) $(os_value VERSION_ID) ($(os_value BUILD_ID))"
    printf 'Stereonaut %s, built for %s\n' "$version" "$built_for"
    if [ "$rebuild" = 0 ] && [ "$(os_value BUILD_ID)" != "$built_build" ]; then
        missing=$(missing_libraries)
        if [ -n "$missing" ]; then
            printf 'this system is %s and lacks: %s\n' "$running" "$missing"
            rebuild=1
        else
            printf 'this system is %s; all libraries are present\n' "$running"
        fi
    fi
fi
if [ "$rebuild" = 1 ]; then
    if [ "$(os_value ID)" != steamos ] && ! check_build_dependencies; then
        exit 1
    fi
    echo "building from the included sources"
    if ! "$player/build-stereonaut-session.sh"; then
        echo "the build failed; it needs gcc, glslc, pkg-config and the" \
             "development files of the libraries" >&2
        exit 1
    fi
    missing=$(missing_libraries)
    if [ -n "$missing" ]; then
        echo "still missing after the build: $missing" >&2
        exit 1
    fi
fi

"$player/install-user-launcher.sh"

if [ "$register" = 1 ]; then
    if ! "$player/register-steamvr-app.sh"; then
        echo "SteamVR library entry not added; run" \
             "$player/register-steamvr-app.sh later" >&2
    fi
fi
printf 'installed; start Stereonaut from the SteamVR library or with %s\n' \
    "$HOME/.local/bin/stereonaut"
