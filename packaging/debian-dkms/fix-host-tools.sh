#!/bin/sh
# fix-host-tools.sh KERNEL_SOURCE_DIR
# DKMS PRE_BUILD step. Kernel headers packaged by a cross "make
# bindeb-pkg" (Linux 6.1, e.g. Mixtile OS: 6.1.172-rockchip-vendor
# built on x86-64) ship fixdep and modpost for the build machine, not
# for the board, and every module build fails. If they are not for
# this machine, rebuild them from the sources in the headers package.
# Headers with native tools (Debian, Ubuntu) are left alone.
set -eu
K=${1:?kernel source dir}
CC=${HOSTCC:-gcc}

# ELF e_machine (2 bytes at offset 18) of a file.
machine() { od -An -tx1 -j18 -N2 "$1" 2>/dev/null | tr -d ' \n'; }

self=$(machine /bin/sh)
[ -n "$self" ] || exit 0

rebuild=0
for t in scripts/basic/fixdep scripts/mod/modpost; do
	[ -f "$K/$t" ] || continue
	m=$(machine "$K/$t")
	[ -z "$m" ] || [ "$m" = "$self" ] || rebuild=1
done
[ "$rebuild" = 1 ] || exit 0

echo "openmiop: host tools in $K are not for $(uname -m), rebuilding fixdep and modpost"
cd "$K"
[ -f scripts/basic/fixdep.c ] && [ -f scripts/mod/modpost.c ] && [ -f scripts/mod/elfconfig.h ] || {
	echo "openmiop: no fixdep/modpost sources in $K, cannot rebuild" >&2
	exit 1
}
"$CC" -O2 -o scripts/basic/fixdep.new scripts/basic/fixdep.c
mv -f scripts/basic/fixdep.new scripts/basic/fixdep
# modpost.c, file2alias.c, sumversion.c (6.1); later kernels add symsearch.c.
src=""
for f in modpost file2alias sumversion symsearch; do
	[ -f "scripts/mod/$f.c" ] && src="$src scripts/mod/$f.c"
done
# shellcheck disable=SC2086
"$CC" -O2 -Iscripts/mod -o scripts/mod/modpost.new $src
mv -f scripts/mod/modpost.new scripts/mod/modpost
