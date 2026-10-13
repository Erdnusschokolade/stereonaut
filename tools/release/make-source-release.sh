#!/bin/sh
# Builds the source package of a release from its git tag:
#
#   tools/release/make-source-release.sh TAG [OUTPUT_DIR]
#
# Linux PCs install Stereonaut from it (its install.sh builds and installs
# into ~/.local), and the updater on a PC builds it. It holds the sources
# of the tag, install.sh and a MANIFEST with
# ARCH=source. Writes stereonaut-linux-source.tar.gz and its .sha256 to
# OUTPUT_DIR (default: src/player/build); sign and publish them with
# publish-release.sh together with the Frame's package.
set -eu

[ $# -ge 1 ] || {
    sed -n '2,11s/^# \{0,1\}//p' "$0" >&2
    exit 2
}
tag=$1
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output_dir=${2:-$project_dir/src/player/build}
commit=$(git -C "$project_dir" rev-parse --verify -q "refs/tags/$tag^{commit}") ||
    { echo "no git tag $tag" >&2; exit 1; }

name="stereonaut-$tag-source"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git -C "$project_dir" archive --format=tar --prefix="$name/" "$tag" \
    src/player tools/openxr-playback LICENSE | tar -C "$work" -xf -
cp "$work/$name/src/player/install-release.sh" "$work/$name/install.sh"
chmod 0755 "$work/$name/install.sh"
cat >"$work/$name/MANIFEST" <<EOF
STEREONAUT_VERSION=$tag
ARCH=source
COMMIT=$commit
BUILT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

mkdir -p "$output_dir"
archive="stereonaut-linux-source.tar.gz"
tar -C "$work" -czf "$output_dir/$archive" "$name"
(cd "$output_dir" && sha256sum "$archive" >"$archive.sha256")
printf 'source package: %s/%s (%s)\n' "$output_dir" "$archive" \
    "$(du -h "$output_dir/$archive" | cut -f1)"
