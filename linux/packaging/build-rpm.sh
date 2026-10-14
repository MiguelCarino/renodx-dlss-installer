#!/usr/bin/env bash
#
# build-rpm.sh - build the dlss5-linux RPM and SRPM from the working tree.
#
#   linux/packaging/build-rpm.sh
#
# Output: linux/packaging/dist/dlss5-linux-<ver>-1.<dist>.noarch.rpm and the
# matching .src.rpm. Needs rpm-build and desktop-file-utils (for %check); no
# root. Everything rpmbuild touches lives under dist/rpmbuild, not ~/rpmbuild,
# so a build never mixes with whatever else the user packages.
#
# The tarball is made from the files on disk, not from git, so uncommitted edits
# are what gets packaged - this is for testing a change before committing it.

set -euo pipefail

readonly HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly LINUX_DIR="$(dirname "$HERE")"
readonly REPO_DIR="$(dirname "$LINUX_DIR")"
readonly DIST="$HERE/dist"
readonly TOPDIR="$DIST/rpmbuild"
readonly NAME="dlss5-linux"
readonly APP_ID="io.github.MiguelCarino.DLSS5Linux"

die() { printf 'build-rpm: %s\n' "$*" >&2; exit 1; }

for t in rpmbuild tar desktop-file-validate; do
    command -v "$t" >/dev/null 2>&1 || die "$t not found (dnf install rpm-build desktop-file-utils)"
done

# VERSION in dlss5-linux.sh is the single source of truth; it is written into
# the spec copy below so the two can never disagree.
VERSION="$(sed -n 's/^readonly VERSION="\([^"]*\)"$/\1/p' "$LINUX_DIR/dlss5-linux.sh")"
[[ -n "$VERSION" ]] || die "could not read VERSION from dlss5-linux.sh"
# The AppStream release list is written by hand; a version bump that forgets
# it would ship metadata announcing the previous release.
grep -q "<release version=\"$VERSION\"" "$HERE/$APP_ID.metainfo.xml" \
    || die "$APP_ID.metainfo.xml has no <release version=\"$VERSION\"> - add one for this version"

# One timestamp for everything the build stamps (tar entries, the RPM's build
# time, file mtimes in the payload), so the same tree gives the same Source0.
# The last commit's time is stable across rebuilds of one checkout.
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$REPO_DIR" log -1 --format=%ct 2>/dev/null || date +%s)}"
export SOURCE_DATE_EPOCH

# Only what the spec installs. Paths are repository-relative so the spec's
# %doc linux/README.md resolves inside the tarball.
readonly FILES=(
    linux/dlss5-linux.sh
    linux/dlss5-gui.py
    linux/dlss5-app.py
    linux/still/nr-runtime.sh
    linux/still/bin/nrstill.exe
    linux/still/bin/nvngx.dll_nrshim.dll
    linux/still/native/nrstill.cpp
    linux/still/native/nrshim.cpp
    linux/still/native/build.sh
    linux/README.md
    linux/LICENSE
    linux/packaging/bin/dlss5-linux
    linux/packaging/bin/dlss5-linux-gui
    "linux/packaging/$APP_ID.desktop"
    "linux/packaging/$APP_ID.svg"
    "linux/packaging/$APP_ID.png"
    "linux/packaging/$APP_ID.metainfo.xml"
)

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/dlss5-rpm.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
SRC="$STAGE/$NAME-$VERSION"

for f in "${FILES[@]}"; do
    if [[ -e "$REPO_DIR/$f" ]]; then
        # Keep the source mode: the spec installs with explicit modes anyway,
        # but the SRPM should carry the scripts as executable as they are here.
        install -D -p -m "$(stat -c %a "$REPO_DIR/$f")" "$REPO_DIR/$f" "$SRC/$f"
    elif [[ "$f" == linux/dlss5-app.py ]]; then
        # dlss5-app.py is developed alongside this script. Until it lands, a
        # stub in the staging copy (never in linux/) lets the packaging itself
        # be tested. The warning keeps such a build from passing unnoticed.
        printf 'build-rpm: WARNING: %s missing, packaging a placeholder\n' "$f" >&2
        printf '#!/usr/bin/env python3\nimport sys\nsys.exit("dlss5-app.py placeholder from build-rpm.sh")\n' \
            > "$SRC/$f"
        chmod 0755 "$SRC/$f"
    else
        die "missing $f"
    fi
done

mkdir -p "$TOPDIR"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
# Reproducible Source0: fixed owner, sorted entries, every mtime (including
# the staging directories mktemp and install -D just made) set to
# SOURCE_DATE_EPOCH, no atime/ctime pax records, and gzip -n so the gzip header
# carries no name or time. The same tree gives a byte-identical tarball.
tar -C "$STAGE" --sort=name --mtime="@$SOURCE_DATE_EPOCH" \
    --owner=0 --group=0 --numeric-owner \
    --pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
    -cf - "$NAME-$VERSION" | gzip -n -9 > "$TOPDIR/SOURCES/$NAME-$VERSION.tar.gz"

# A literal Version in the spec copy, not a --define: the SRPM ships this file,
# and a rebuild of it elsewhere has no build-rpm.sh to pass the version in.
sed "s/^Version:.*/Version:        $VERSION/" "$HERE/rpm/$NAME.spec" > "$TOPDIR/SPECS/$NAME.spec"
grep -q "^Version:        $VERSION\$" "$TOPDIR/SPECS/$NAME.spec" || die "could not set Version in the spec"
touch -d "@$SOURCE_DATE_EPOCH" "$TOPDIR/SPECS/$NAME.spec"

# The two use_/clamp_ switches make rpm stamp the build time and payload mtimes
# from SOURCE_DATE_EPOCH too, and _buildhost keeps the machine name out.
rpmbuild -ba \
    --define "_topdir $TOPDIR" \
    --define "use_source_date_epoch_as_buildtime 1" \
    --define "clamp_mtime_to_source_date_epoch 1" \
    --define "_buildhost reproducible" \
    "$TOPDIR/SPECS/$NAME.spec"

# Flatten the results next to dist/ so the install command in the README is a
# fixed path. Clear the previous build's packages first so a version bump does
# not leave two RPMs side by side.
rm -f "$DIST"/*.rpm
find "$TOPDIR/RPMS" "$TOPDIR/SRPMS" -name "$NAME-$VERSION-*.rpm" -exec cp -p {} "$DIST/" \;
printf '\nbuilt:\n'
ls -1 "$DIST"/*.rpm
