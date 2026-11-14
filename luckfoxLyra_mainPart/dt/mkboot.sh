#!/bin/sh
# Build a Lyra boot partition image from a patched device tree source, keeping
# everything else from the stock image: its kernel (so all out-of-tree modules
# still match) and its boot logos. An optional zImage replaces the stock kernel
# (the -erl NAT kernel; its modules then live in /usr/lib/modules/6.1.99-erl). The DTB goes into both places U-Boot may take
# it from: the FIT "fdt" image and rk-kernel.dtb inside the resource image.
#
#   ./mkboot.sh <board.dts> <out.img> [zImage]   (needs orig/ from the stock image)
set -e

cd "$(dirname "$0")"
DTS=${1:?usage: $0 <board.dts> <out.img> [zImage]}
OUT=${2:?usage: $0 <board.dts> <out.img> [zImage]}
KERNEL=${3:-orig/kernel}
SDK=../../Lyra-sdk
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for f in "$KERNEL" orig/res/out/logo.bmp orig/res/out/logo_kernel.bmp; do
	[ -f "$f" ] || { echo "missing $f: extract the stock image first" >&2; exit 1; }
done

dtc -q -I dts -O dtb -o "$WORK/fdt" "$DTS"

mkdir "$WORK/res"
cp "$WORK/fdt" "$WORK/res/rk-kernel.dtb"
cp orig/res/out/logo.bmp orig/res/out/logo_kernel.bmp "$WORK/res/"
(cd "$WORK/res" && "$OLDPWD/$SDK/kernel/scripts/resource_tool" rk-kernel.dtb logo.bmp logo_kernel.bmp >/dev/null)
cp "$WORK/res/resource.img" "$WORK/resource"

cp "$KERNEL" "$WORK/kernel"
cp boot.its "$WORK/"
(cd "$WORK" && "$OLDPWD/$SDK/rkbin/tools/mkimage" -f boot.its -E -p 0x800 boot.img >/dev/null)
cp "$WORK/boot.img" "$OUT"

echo "$OUT: $(stat -c%s "$OUT") bytes, md5 $(md5sum "$OUT" | cut -d' ' -f1)"
