#!/bin/sh
# Installs a Stereonaut release package for the current user (into ~/.local,
# like install-user-launcher.sh) and adds it to the SteamVR library unless
# it is there already or --no-steamvr is given:
#
#   ./install.sh [--no-steamvr] [--rebuild]
#
# On the SteamOS build the package was made on (MANIFEST) the binaries are
# installed as they are. On another build they are kept if every library
# they need is present, otherwise they are rebuilt from the included
# sources (SteamOS on the Frame ships the compiler). --rebuild always
# rebuilds. Copied into the package as install.sh by make-release.sh.
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
        sed -n '2,12s/^# \{0,1\}//p' "$0"
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

if [ ! -f "$here/MANIFEST" ] || [ ! -f "$here/SHA256SUMS" ]; then
    echo "not a release package: MANIFEST or SHA256SUMS missing" >&2
    exit 1
fi
package_arch=$(manifest_value ARCH)
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
printf 'Stereonaut %s, built for %s\n' \
    "$(manifest_value STEREONAUT_VERSION)" "$built_for"
if [ "$rebuild" = 0 ] && [ "$(os_value BUILD_ID)" != "$built_build" ]; then
    missing=$(missing_libraries)
    if [ -n "$missing" ]; then
        printf 'this system is %s and lacks: %s\n' "$running" "$missing"
        rebuild=1
    else
        printf 'this system is %s; all libraries are present\n' "$running"
    fi
fi
if [ "$rebuild" = 1 ]; then
    echo "building from the included sources"
    if ! "$player/build-stereonaut-session.sh"; then
        echo "the build failed; it needs gcc, glslc, pkg-config and the" \
             "headers of the libraries above" >&2
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
