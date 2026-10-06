#!/bin/sh
# build-dkms-deb.sh VERSION OUTDIR
# Build openmiop-dkms_<VERSION>_all.deb from this tree. VERSION is the
# release without the "v" (0.1.0-rc.2) or a git describe string. The
# Debian package version uses "~" for pre-releases so 0.1.0~rc.2 sorts
# before 0.1.0; the file name keeps "-" (GitHub renames "~" in assets).
set -eu
VERSION=${1:?version}
OUT=${2:?outdir}
TOP=$(cd "$(dirname "$0")/.." && pwd)
DEBVERSION=$(echo "$VERSION" | sed 's/-rc/~rc/; s/-\([0-9][0-9]*\)-g\([0-9a-f]*\)$/+\1.g\2/')-1
P=$TOP/packaging/debian-dkms
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

sub() { sed -e "s/@VERSION@/$VERSION/g" -e "s/@DEBVERSION@/$DEBVERSION/g" "$1"; }

src=$W/root/usr/src/openmiop-$VERSION
mkdir -p "$src" "$W/root/usr/sbin" "$W/root/lib/systemd/system" \
	"$W/root/usr/share/doc/openmiop-dkms" "$W/root/DEBIAN"
cp "$TOP/drivers/openmiop/openmiop-ep.c" "$TOP/drivers/openmiop/openmiop.h" "$src/"
cp "$P/Kbuild" "$src/Kbuild"
sub "$P/dkms.conf" > "$src/dkms.conf"
install -m 0755 "$P/openmiop-ep-start" "$W/root/usr/sbin/openmiop-ep-start"
install -m 0644 "$P/openmiop.service" "$W/root/lib/systemd/system/openmiop.service"
cp "$TOP/LICENSE" "$W/root/usr/share/doc/openmiop-dkms/copyright"
sub "$P/control" > "$W/root/DEBIAN/control"
sub "$P/postinst" > "$W/root/DEBIAN/postinst"
sub "$P/prerm" > "$W/root/DEBIAN/prerm"
chmod -R u=rwX,go=rX "$W/root"
chmod 0755 "$W/root/DEBIAN/postinst" "$W/root/DEBIAN/prerm" "$W/root/usr/sbin/openmiop-ep-start"
echo "Installed-Size: $(du -sk "$W/root" | cut -f1)" >> "$W/root/DEBIAN/control"

mkdir -p "$OUT"
deb=$OUT/openmiop-dkms_${VERSION}_all.deb
dpkg-deb --root-owner-group -Zxz --build "$W/root" "$deb" >/dev/null
echo "$deb ($DEBVERSION)"
