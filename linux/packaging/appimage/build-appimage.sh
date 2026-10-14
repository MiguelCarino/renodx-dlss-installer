#!/usr/bin/env bash
#
# build-appimage.sh - build the "DLSS 5 for Linux" AppImage from the working tree.
#
#   linux/packaging/appimage/build-appimage.sh
#
# Output: linux/packaging/dist/DLSS5_for_Linux-<ver>-x86_64.AppImage
#
# The AppDir mirrors the RPM's /usr layout (usr/bin wrappers, usr/share/...), so
# the same launchers, desktop file and icons work unchanged in both. Python and
# the GTK stack are NOT bundled - see AppRun for why.
#
# Needs only curl, sha256sum and coreutils; no root and no FUSE. appimagetool
# itself is an AppImage, run with --appimage-extract-and-run so it works on
# hosts without libfuse2 and inside containers.
#
# Like build-rpm.sh, this packs the files on disk, not a git checkout, so
# uncommitted edits are what gets built.

set -euo pipefail

readonly HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly PACKAGING_DIR="$(dirname "$HERE")"
readonly LINUX_DIR="$(dirname "$PACKAGING_DIR")"
readonly REPO_DIR="$(dirname "$LINUX_DIR")"
readonly DIST="$PACKAGING_DIR/dist"
readonly APP_ID="io.github.MiguelCarino.DLSS5Linux"
# Not named ARCH: that is the environment variable appimagetool reads, and a
# readonly shell variable of the same name could not be set for its command.
readonly TARGET_ARCH="x86_64"

# Pinned build tools, fetched from their official GitHub releases. The sha256
# values are the ones GitHub publishes for these exact release assets; a
# mismatch aborts the build instead of running whatever was downloaded.
readonly APPIMAGETOOL_TAG="1.9.1"
readonly APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_TAG/appimagetool-$TARGET_ARCH.AppImage"
readonly APPIMAGETOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
# appimagetool embeds a runtime (the ELF stub that mounts the squashfs). Left
# alone it downloads the type2-runtime "continuous" build on every run - a
# moving target with no checksum. Passing a pinned, verified copy keeps the
# build reproducible and offline once the cache is warm.
readonly RUNTIME_TAG="20251108"
readonly RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME_TAG/runtime-$TARGET_ARCH"
readonly RUNTIME_SHA256="2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"

# Same cache root as dlss5-linux.sh, in a build/ subfolder so a "clean cache"
# of the installer's downloads and the build tools stay distinguishable.
readonly BUILD_CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/dlss5-linux/build"

die() { printf 'build-appimage: %s\n' "$*" >&2; exit 1; }
log() { printf 'build-appimage: %s\n' "$*"; }

[[ "$(uname -m)" == "$TARGET_ARCH" ]] || die "this builds an $TARGET_ARCH AppImage and must run on $TARGET_ARCH (got $(uname -m))"
for t in curl sha256sum install; do
    command -v "$t" >/dev/null 2>&1 || die "$t not found"
done

# VERSION in dlss5-linux.sh is the single source of truth, as for the RPM.
VERSION="$(sed -n 's/^readonly VERSION="\([^"]*\)"$/\1/p' "$LINUX_DIR/dlss5-linux.sh")"
[[ -n "$VERSION" ]] || die "could not read VERSION from dlss5-linux.sh"
readonly OUT="$DIST/DLSS5_for_Linux-$VERSION-$TARGET_ARCH.AppImage"
# The AppStream release list is written by hand; a version bump that forgets
# it would ship metadata announcing the previous release.
grep -q "<release version=\"$VERSION\"" "$PACKAGING_DIR/$APP_ID.metainfo.xml" \
    || die "$APP_ID.metainfo.xml has no <release version=\"$VERSION\"> - add one for this version"

# appimagetool's mksquashfs stamps every inode with the build time unless
# SOURCE_DATE_EPOCH is set, so two builds of one tree would differ. Default it
# to the last commit's time, which is stable across rebuilds of a checkout.
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$REPO_DIR" log -1 --format=%ct 2>/dev/null || date +%s)}"
export SOURCE_DATE_EPOCH

# fetch URL SHA256 DEST - download once into the cache, verify every time. The
# check runs on cache hits too, so a corrupted or swapped cached file is
# caught rather than trusted because it was good once.
fetch() {
    local url="$1" sum="$2" dest="$3"
    if [[ ! -f "$dest" ]]; then
        log "downloading $url"
        mkdir -p "$(dirname "$dest")"
        curl -fL --progress-bar --retry 3 --proto '=https' -o "$dest.part" "$url" || { rm -f "$dest.part"; die "download failed: $url"; }
        mv -f "$dest.part" "$dest"
    fi
    if ! printf '%s  %s\n' "$sum" "$dest" | sha256sum -c --status; then
        rm -f "$dest"
        die "sha256 mismatch for $(basename "$dest") (expected $sum) - removed it; re-run to download again"
    fi
}

APPIMAGETOOL="$BUILD_CACHE/appimagetool-$APPIMAGETOOL_TAG-$TARGET_ARCH.AppImage"
RUNTIME="$BUILD_CACHE/type2-runtime-$RUNTIME_TAG-$TARGET_ARCH"
fetch "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA256" "$APPIMAGETOOL"
fetch "$RUNTIME_URL" "$RUNTIME_SHA256" "$RUNTIME"
chmod 0755 "$APPIMAGETOOL"

# ---------------------------------------------------------------------------
# AppDir
# ---------------------------------------------------------------------------

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/dlss5-appimage.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
APPDIR="$STAGE/DLSS5_for_Linux.AppDir"

# src dest mode - every file gets an explicit mode, so what the AppImage ships
# does not depend on the umask or on how the checkout set the exec bits.
put() {
    [[ -f "$1" ]] || die "missing $1"
    install -D -m "$3" "$1" "$APPDIR/$2"
}

# Contract layout, identical to the RPM's.
put "$LINUX_DIR/dlss5-linux.sh"  usr/share/dlss5-linux/dlss5-linux.sh 0755
put "$LINUX_DIR/dlss5-gui.py"    usr/share/dlss5-linux/dlss5-gui.py   0755
put "$LINUX_DIR/dlss5-app.py"    usr/share/dlss5-linux/dlss5-app.py   0755
put "$LINUX_DIR/still/nr-runtime.sh"            usr/share/dlss5-linux/still/nr-runtime.sh            0755
put "$LINUX_DIR/still/bin/nrstill.exe"          usr/share/dlss5-linux/still/bin/nrstill.exe          0644
put "$LINUX_DIR/still/bin/nvngx.dll_nrshim.dll" usr/share/dlss5-linux/still/bin/nvngx.dll_nrshim.dll 0644
put "$LINUX_DIR/still/native/nrstill.cpp"       usr/share/dlss5-linux/still/native/nrstill.cpp       0644
put "$LINUX_DIR/still/native/nrshim.cpp"        usr/share/dlss5-linux/still/native/nrshim.cpp        0644
put "$LINUX_DIR/still/native/build.sh"          usr/share/dlss5-linux/still/native/build.sh          0755
put "$PACKAGING_DIR/bin/dlss5-linux"     usr/bin/dlss5-linux     0755
put "$PACKAGING_DIR/bin/dlss5-linux-gui" usr/bin/dlss5-linux-gui 0755
put "$PACKAGING_DIR/$APP_ID.desktop"      "usr/share/applications/$APP_ID.desktop"                0644
put "$PACKAGING_DIR/$APP_ID.svg"          "usr/share/icons/hicolor/scalable/apps/$APP_ID.svg"     0644
put "$PACKAGING_DIR/$APP_ID.png"          "usr/share/icons/hicolor/256x256/apps/$APP_ID.png"      0644
put "$PACKAGING_DIR/$APP_ID.metainfo.xml" "usr/share/metainfo/$APP_ID.metainfo.xml"               0644
put "$LINUX_DIR/README.md"       usr/share/doc/dlss5-linux/README.md  0644
put "$LINUX_DIR/LICENSE"         usr/share/licenses/dlss5-linux/LICENSE 0644
put "$HERE/AppRun" AppRun 0755

# appimagetool wants exactly one .desktop file at the AppDir root, an icon there
# named after its Icon= key, and .DirIcon (what file managers show for the
# AppImage). Relative symlinks keep a single copy of each and survive the
# squashfs packing.
ln -s "usr/share/applications/$APP_ID.desktop" "$APPDIR/$APP_ID.desktop"
ln -s "usr/share/icons/hicolor/256x256/apps/$APP_ID.png" "$APPDIR/$APP_ID.png"
ln -s "$APP_ID.png" "$APPDIR/.DirIcon"

# ---------------------------------------------------------------------------
# Pack
# ---------------------------------------------------------------------------

mkdir -p "$DIST"
rm -f "$OUT"

# appimagetool 1.9.1 only validates AppStream data named <id>.appdata.xml (the
# legacy name) and skips ours, which uses the current .metainfo.xml name. An
# .appdata.xml symlink would make software centres see the component twice, so
# validate it here instead, when the host has appstreamcli. --no-net keeps the
# build offline; the screenshot URLs are checked on release, not every build.
if command -v appstreamcli >/dev/null 2>&1; then
    appstreamcli validate --no-net "$APPDIR/usr/share/metainfo/$APP_ID.metainfo.xml" \
        || die "metainfo failed appstreamcli validation"
else
    log "appstreamcli not found - metainfo not validated"
fi

# No update information is embedded: gh-releases-zsync needs a .zsync published
# next to every release asset, and this project does not publish AppImages to
# GitHub releases yet. Once it does, add
#   -u "gh-releases-zsync|MiguelCarino|renodx-dlss-installer|latest|DLSS5_for_Linux-*-x86_64.AppImage.zsync"
#
# --appimage-extract-and-run is consumed by the AppImage runtime (it must come
# first), not by appimagetool: the tool unpacks to a temp dir, runs from there
# and removes it, so no FUSE is needed. ARCH tells appimagetool what it packs;
# with only shell and Python inside, it cannot infer that from an ELF.
#
# TMPDIR points into the staging dir because the extract is not reliably
# removed by the runtime (1.9.1 leaves appimage_extracted_<md5> behind in
# $TMPDIR); inside $STAGE the EXIT trap takes it away with everything else.
log "packing $OUT"
mkdir -p "$STAGE/tmp"
TMPDIR="$STAGE/tmp" ARCH="$TARGET_ARCH" "$APPIMAGETOOL" --appimage-extract-and-run \
    --runtime-file "$RUNTIME" "$APPDIR" "$OUT"

[[ -s "$OUT" ]] || die "appimagetool reported success but $OUT is missing"
chmod 0755 "$OUT"
printf '\nbuilt:\n'
ls -lh "$OUT"
sha256sum "$OUT"
