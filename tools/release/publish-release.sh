#!/bin/sh
# Signs the packages of a release and publishes them on GitHub:
#
#   tools/release/publish-release.sh DIRECTORY KEY [--draft]
#
# DIRECTORY holds the output of src/player/make-release.sh (copied from the
# Frame); the source package for Linux PCs is added from the same git tag
# (make-source-release.sh) unless it is there already. KEY is the private
# SSH key whose public half is in src/player/release-signers. The release
# is named after the version in the packages' MANIFESTs, which must be a git
# tag already pushed to GitHub (gh --verify-tag), so the release points at
# the commit it was built from. Each package gets a .sig (ssh-keygen -Y
# sign, namespace "stereonaut-release"), checked against the
# release-signers inside the package before anything is uploaded.
# STEREONAUT_REPOSITORY (OWNER/NAME) publishes elsewhere, e.g. in a fork.
set -eu

repository=${STEREONAUT_REPOSITORY:-Erdnusschokolade/stereonaut}
tools_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

[ $# -ge 2 ] || {
    sed -n '2,16s/^# \{0,1\}//p' "$0" >&2
    exit 2
}
directory=$(CDPATH= cd -- "$1" && pwd)
key=$2
draft=
[ "${3:-}" = --draft ] && draft=--draft
command -v gh >/dev/null 2>&1 || { echo "gh is missing" >&2; exit 1; }

cd "$directory"
set -- stereonaut-steamos-*.tar.gz
[ $# -eq 1 ] && [ -f "$1" ] || {
    echo "expected one stereonaut-steamos-ARCH.tar.gz in $directory" >&2
    exit 1
}
frame=$1

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# The MANIFEST of a package.
manifest() {
    rm -rf "$work/package"
    mkdir "$work/package"
    tar -C "$work/package" -xzf "$1"
    echo "$work"/package/stereonaut-*/MANIFEST
}
value() {
    sed -n "s/^$2=//p" "$1"
}

frame_manifest=$(manifest "$frame")
tag=$(value "$frame_manifest" STEREONAUT_VERSION)
os=$(value "$frame_manifest" STEAMOS_VERSION_ID)
os_build=$(value "$frame_manifest" STEAMOS_BUILD_ID)
[ -f stereonaut-linux-source.tar.gz ] ||
    "$tools_dir/make-source-release.sh" "$tag" "$directory"

for file in stereonaut-*.tar.gz; do
    sha256sum -c "$file.sha256"
    package_manifest=$(manifest "$file")
    version=$(value "$package_manifest" STEREONAUT_VERSION)
    [ "$version" = "$tag" ] || {
        echo "$file holds version $version, not $tag" >&2
        exit 1
    }
    signers="$(dirname "$package_manifest")/src/player/release-signers"
    [ -s "$signers" ] || {
        echo "$file has no release-signers; updates could not verify it" >&2
        exit 1
    }
    rm -f "$file.sig"
    ssh-keygen -Y sign -f "$key" -n stereonaut-release "$file"
    ssh-keygen -Y verify -f "$signers" -I stereonaut-release \
        -n stereonaut-release -s "$file.sig" <"$file"
done

base="https://github.com/$repository/releases/latest/download"
notes="The Steam Frame package was built on SteamOS $os ($os_build); on a
Linux PC with SteamVR the source package is built on installation.

Install on the Steam Frame (Konsole in desktop mode):

\`\`\`sh
cd \"\$(mktemp -d)\"
curl -LO $base/$frame
curl -LO $base/$frame.sha256
sha256sum -c $frame.sha256 && tar -xzf $frame && ./stereonaut-*/install.sh
\`\`\`

Install on a Linux PC (it names missing build dependencies):

\`\`\`sh
cd \"\$(mktemp -d)\"
curl -LO $base/stereonaut-linux-source.tar.gz
curl -LO $base/stereonaut-linux-source.tar.gz.sha256
sha256sum -c stereonaut-linux-source.tar.gz.sha256 &&
  tar -xzf stereonaut-linux-source.tar.gz && ./stereonaut-*/install.sh
\`\`\`

Later updates: Settings → Updates in the headset."
set --
for file in stereonaut-*.tar.gz; do
    set -- "$@" "$file" "$file.sha256" "$file.sig"
done
gh release create -R "$repository" "$tag" --verify-tag $draft \
    --title "Stereonaut $tag" --notes "$notes" "$@"
