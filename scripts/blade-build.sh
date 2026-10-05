#!/bin/sh
# Build openmiop-ep.ko natively on a Blade against its own kernel
# headers and copy the result back. The workstation has no aarch64
# toolchain; the Blade has the exact vendor headers.
#
#   scripts/blade-build.sh [OUT_DIR]      (default: ./_out/debian)
set -eu

BUILDER=${BUILDER:-blade173}
KDIR=${KDIR:-/usr/src/linux-headers-6.1-rockchip}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-"$ROOT/_out/debian"}
REMOTE=pcie-ep-net-dev

mkdir -p "$OUT"
ssh "$BUILDER" "rm -rf ~/$REMOTE && mkdir -p ~/$REMOTE"
tar -C "$ROOT" -cf - drivers | ssh "$BUILDER" "tar -C ~/$REMOTE -xf -"
ssh "$BUILDER" "make -C ~/$REMOTE/drivers/openmiop KDIR=$KDIR 2>&1 |
	grep -vE 'compiler differs|The kernel was built by|You are using'"
scp -q "$BUILDER:$REMOTE/drivers/openmiop/openmiop-ep.ko" "$OUT/"
sha256sum "$OUT/openmiop-ep.ko"
