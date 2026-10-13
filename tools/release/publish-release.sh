#!/bin/sh
# Signs the packages made by src/player/make-release.sh and publishes them
# as a GitHub release:
#
#   tools/release/publish-release.sh DIRECTORY KEY [--draft]
#
# DIRECTORY holds the output of make-release.sh (copied from the Frame),
# KEY is the private SSH key whose public half is in
# src/player/release-signers. The release is named after the version in the
# package's MANIFEST, which must be a git tag already pushed to GitHub
# (gh --verify-tag), so the release points at the commit it was built
# from. Each package gets a .sig (ssh-keygen -Y sign, namespace
# "stereonaut-release"), checked against the release-signers inside the
# package before anything is uploaded. STEREONAUT_REPOSITORY (OWNER/NAME)
# publishes elsewhere, e.g. in a fork.
set -eu

repository=${STEREONAUT_REPOSITORY:-Erdnusschokolade/stereonaut}

[ $# -ge 2 ] || {
    sed -n '2,16s/^# \{0,1\}//p' "$0" >&2
    exit 2
}
directory=$1
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
stable=$1
versioned=
for file in stereonaut-*-steamos-*.tar.gz; do
    [ "$file" != "$stable" ] && versioned=$file
done
[ -n "$versioned" ] || { echo "versioned package missing" >&2; exit 1; }
cmp -s "$stable" "$versioned" || {
    echo "$stable and $versioned differ" >&2
    exit 1
}
sha256sum -c "$stable.sha256" "$versioned.sha256"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tar -C "$work" -xzf "$versioned"
package=$(echo "$work"/stereonaut-*)
tag=$(sed -n 's/^STEREONAUT_VERSION=//p' "$package/MANIFEST")
os=$(sed -n 's/^STEAMOS_VERSION_ID=//p' "$package/MANIFEST")
os_build=$(sed -n 's/^STEAMOS_BUILD_ID=//p' "$package/MANIFEST")
signers="$package/src/player/release-signers"
[ -s "$signers" ] || {
    echo "the package has no release-signers; updates could not verify it" >&2
    exit 1
}

for file in "$stable" "$versioned"; do
    rm -f "$file.sig"
    ssh-keygen -Y sign -f "$key" -n stereonaut-release "$file"
    ssh-keygen -Y verify -f "$signers" -I stereonaut-release \
        -n stereonaut-release -s "$file.sig" <"$file"
done

notes="Built on SteamOS $os ($os_build) for the Steam Frame.

Install (Konsole in desktop mode):

\`\`\`sh
cd \"\$(mktemp -d)\"
curl -LO https://github.com/$repository/releases/latest/download/$stable
curl -LO https://github.com/$repository/releases/latest/download/$stable.sha256
sha256sum -c $stable.sha256 && tar -xzf $stable && ./stereonaut-*/install.sh
\`\`\`

Later updates: Settings → Updates in the headset."
gh release create -R "$repository" "$tag" --verify-tag $draft \
    --title "Stereonaut $tag" --notes "$notes" \
    "$stable" "$stable.sha256" "$stable.sig" \
    "$versioned" "$versioned.sha256" "$versioned.sig"
