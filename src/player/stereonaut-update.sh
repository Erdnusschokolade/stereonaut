#!/bin/sh
# Updates an installed Stereonaut from the project's GitHub releases. The
# XR host runs it and reads the lines it prints:
#
#   stereonaut-update.sh check         prints "latest TAG"
#   stereonaut-update.sh install TAG   prints "phase download|verify|unpack|
#                                      install" and finally "done"
#
# Errors end with "error TEXT" and a non-zero exit. A package must carry a
# signature (ssh-keygen -Y sign, namespace "stereonaut-release") by a key
# in release-signers next to this script. Details of the installation go to
# stderr (the launcher log). STEREONAUT_RELEASES and
# STEREONAUT_RELEASE_SIGNERS replace the repository and the key list for
# tests.
set -eu

repository=${STEREONAUT_RELEASES:-https://github.com/Erdnusschokolade/stereonaut}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
signers=${STEREONAUT_RELEASE_SIGNERS:-$here/release-signers}
asset="stereonaut-steamos-$(uname -m).tar.gz"

fail() {
    echo "error $*"
    exit 1
}

# The installation replaces this file while it runs: everything happens in
# main, which the shell has read completely before it starts.
main() {
case "${1:-}" in
check)
    # The latest release redirects to its tag; no API, no JSON.
    url=$(curl -fsS --max-time 20 -o /dev/null -w '%{redirect_url}' \
        "$repository/releases/latest") || fail "no connection to the server"
    tag=${url##*/releases/tag/}
    if [ -z "$url" ] || [ "$tag" = "$url" ] || [ -z "$tag" ]; then
        fail "no release found"
    fi
    echo "latest $tag"
    ;;
install)
    tag=${2:-}
    [ -n "$tag" ] || fail "no version given"
    case "$tag" in
    */* | *..*) fail "invalid version" ;;
    esac
    [ -s "$signers" ] || fail "no release key installed"
    command -v ssh-keygen >/dev/null 2>&1 || fail "ssh-keygen is missing"
    work="${XDG_CACHE_HOME:-$HOME/.cache}/stereonaut/update"
    rm -rf "$work"
    mkdir -p "$work"
    cd "$work"

    echo "phase download"
    base="$repository/releases/download/$tag/$asset"
    curl -fsSL --max-time 600 -o "$asset" "$base" >&2 ||
        fail "download failed"
    curl -fsSL --max-time 60 -o "$asset.sig" "$base.sig" >&2 ||
        fail "the release has no signature"

    echo "phase verify"
    ssh-keygen -Y verify -f "$signers" -I stereonaut-release \
        -n stereonaut-release -s "$asset.sig" <"$asset" >&2 ||
        fail "invalid signature"

    echo "phase unpack"
    mkdir package
    tar -C package -xzf "$asset" >&2 || fail "damaged package"
    set -- package/stereonaut-*/install.sh
    [ $# -eq 1 ] && [ -f "$1" ] || fail "damaged package"
    package=$(dirname -- "$1")
    # A signed older package must not pass for the requested version.
    version=$(sed -n 's/^STEREONAUT_VERSION=//p' "$package/MANIFEST")
    [ "$version" = "$tag" ] || fail "the package holds version $version"

    echo "phase install"
    "$package/install.sh" >&2 || fail "installation failed"
    cd /
    rm -rf "$work"
    echo "done"
    ;;
*)
    echo "usage: $0 check | install TAG" >&2
    exit 2
    ;;
esac
}

main "$@"; exit
