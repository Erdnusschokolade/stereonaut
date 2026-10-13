#!/bin/sh
# Builds a release package on the Steam Frame:
#
#   src/player/make-release.sh VERSION [--turnip LIBRARY] [OUTPUT_DIR]
#
# VERSION names the package (a commit or tag; inside a git checkout it
# defaults to the current commit, which must be clean). The package holds
# the binaries and shaders built here, the sources for rebuilds, install.sh
# (install-release.sh), a MANIFEST with the SteamOS build it was made on and
# SHA256SUMS of the binaries. SteamOS is an atomic image: the same BUILD_ID
# means the same libraries, so install.sh installs the binaries as they are
# there and checks the libraries (rebuilding if needed) elsewhere.
# --turnip adds the TP10 Turnip (tools/mesa-tp10) for HEVC Main10.
# Writes NAME.tar.gz, a copy named stereonaut-steamos-ARCH.tar.gz and their
# .sha256 files to OUTPUT_DIR (default: build); sign and publish them with
# tools/release/publish-release.sh.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)

version=
turnip=
output_dir="$script_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
    --turnip)
        [ $# -ge 2 ] || { echo "--turnip needs a file" >&2; exit 2; }
        turnip=$2
        shift 2
        ;;
    -h | --help)
        sed -n '2,17s/^# \{0,1\}//p' "$0"
        exit 0
        ;;
    *)
        if [ -z "$version" ]; then
            version=$1
        else
            output_dir=$1
        fi
        shift
        ;;
    esac
done

if [ -z "$version" ] && git -C "$project_dir" rev-parse --git-dir \
        >/dev/null 2>&1; then
    if [ -n "$(git -C "$project_dir" status --porcelain -- src tools)" ]; then
        echo "uncommitted changes; commit them or pass a VERSION" >&2
        exit 1
    fi
    version=$(git -C "$project_dir" rev-parse --short HEAD)
fi
if [ -z "$version" ]; then
    echo "usage: $0 VERSION [--turnip LIBRARY] [OUTPUT_DIR]" >&2
    exit 2
fi

os_value() {
    sed -n "s/^$1=//p" /etc/os-release | tr -d '"' | head -n 1
}
if [ "$(os_value ID)" != steamos ]; then
    echo "release packages are built on SteamOS (the Steam Frame)" >&2
    exit 1
fi
os_version=$(os_value VERSION_ID)
os_build=$(os_value BUILD_ID)
arch=$(uname -m)
if [ -z "$os_version" ] || [ -z "$os_build" ]; then
    echo "no VERSION_ID/BUILD_ID in /etc/os-release" >&2
    exit 1
fi
if [ -n "$turnip" ] && [ ! -f "$turnip" ]; then
    echo "Turnip library not found: $turnip" >&2
    exit 1
fi

"$script_dir/build-stereonaut-session.sh"

name="stereonaut-$version-steamos-$os_version-$os_build-$arch"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
stage="$work/$name"
mkdir -p "$stage/src/player/build"

# The sources as the installer copies them for rebuilds.
(cd "$project_dir" && tar -cf - --exclude=src/player/build \
    src/player tools/openxr-playback) | tar -C "$stage" -xf -

# Only what install-user-launcher.sh installs.
for file in stereonaut-launcher stereonaut-session stereonaut-renderer \
            video-projection.vert.spv nv12-projection.frag.spv \
            tp10-projection.frag.spv stereo-disparity.comp.spv \
            stereo-disparity-tp10.comp.spv ui-panel.vert.spv \
            ui-panel.frag.spv controller-model.vert.spv \
            controller-model.frag.spv laser.vert.spv laser.frag.spv; do
    cp "$script_dir/build/$file" "$stage/src/player/build/"
done
if [ -n "$turnip" ]; then
    cp "$turnip" "$stage/src/player/build/libvulkan_freedreno.so"
fi

cp "$script_dir/install-release.sh" "$stage/install.sh"
chmod 0755 "$stage/install.sh"
cat >"$stage/MANIFEST" <<EOF
STEREONAUT_VERSION=$version
ARCH=$arch
STEAMOS_VERSION_ID=$os_version
STEAMOS_BUILD_ID=$os_build
STEAMOS_VARIANT_ID=$(os_value VARIANT_ID)
BUILT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
TP10_TURNIP=$([ -n "$turnip" ] && echo yes || echo no)
EOF
(cd "$stage/src/player/build" && sha256sum -- *) >"$stage/SHA256SUMS"

mkdir -p "$output_dir"
archive="$output_dir/$name.tar.gz"
tar -C "$work" -czf "$archive" "$name"
# The same package under a fixed name, so that
# releases/latest/download/stereonaut-steamos-ARCH.tar.gz always works.
stable="stereonaut-steamos-$arch.tar.gz"
cp "$archive" "$output_dir/$stable"
(cd "$output_dir" && sha256sum "$name.tar.gz" >"$name.tar.gz.sha256" &&
    sha256sum "$stable" >"$stable.sha256")
printf 'release package: %s (%s)\n' "$archive" "$(du -h "$archive" | cut -f1)"
printf 'fixed name: %s/%s\n' "$output_dir" "$stable"
