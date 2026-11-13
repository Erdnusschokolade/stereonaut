# Releases

A release carries the package for the Steam Frame,
`stereonaut-steamos-aarch64.tar.gz` (plus a copy named after the version and
SteamOS build): binaries built on the Frame, the sources for rebuilds and
`install.sh`. SteamOS is an atomic image, so on the same build the binaries
install as they are; elsewhere `install.sh` checks the libraries with `ldd`
and rebuilds if one is missing. Linux PCs get the source package
`stereonaut-linux-source.tar.gz` (the sources of the tag and `install.sh`,
which builds and installs into `~/.local`); the updater on a PC fetches and
builds it the same way.

Each package has a `.sha256` and a `.sig` (`ssh-keygen -Y sign`, namespace
`stereonaut-release`). Updates in the headset install only packages signed
by a key in `src/player/release-signers` whose MANIFEST names the requested
version.

## Making a release

1. Render the README pictures again with
   `tools/release/make-readme-pictures.sh` (on a desktop, needs FFmpeg)
   and commit them if they changed, so the README shows the panels of the
   release. They use made-up titles and generated test patterns only.
2. Tag the commit and push the tag (`git tag -a vX.Y.Z`, then push it).
3. On the Frame, from that commit:
   `src/player/make-release.sh vX.Y.Z --turnip LIBRARY OUTPUT_DIR`
   (`--turnip` adds the TP10 Turnip for HEVC Main10).
4. Copy `OUTPUT_DIR` to the machine with the signing key and run
   `tools/release/publish-release.sh OUTPUT_DIR KEY`. It adds the source
   package from the tag (`tools/release/make-source-release.sh`) unless it
   is in `OUTPUT_DIR` already, signs the packages, checks each signature
   against the `release-signers` inside the package and creates the GitHub
   release with `gh` (`--draft` for a draft, which the updater does not
   see). `--notes FILE` puts a Markdown text, such as a "What's new"
   section, before the generated install instructions.

`STEREONAUT_REPOSITORY=OWNER/NAME` publishes to a fork. For tests,
`STEREONAUT_RELEASES` (a base URL) and `STEREONAUT_RELEASE_SIGNERS` point the
updater at another server and key list.
