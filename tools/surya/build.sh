#!/bin/bash
# Build kernel pieces for the phone inside the toolchain container; the tree is mounted, nothing is copied.
#   usage: tools/surya/build.sh TARGET...     e.g. dtbs   drivers/media/i2c/ov02b1b.ko
# Settings (environment or .env at the repository root):
#   SURYA_OUT    build output directory (default $HOME/.cache/surya-build); keep it off tmpfs
#   SURYA_CONFIG kernel config of the running phone kernel, used the first time (default $SURYA_OUT/config-phone)
#   SURYA_IMAGE  container image name (default surya-build)
# Modules are built without vmlinux.o: modpost warns about undefined symbols, the phone resolves them at load time
# (its kernel has CONFIG_MODVERSIONS off), so KBUILD_MODPOST_WARN=1 is set.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
[ -f "$root/.env" ] && . "$root/.env"
out=${SURYA_OUT:-$HOME/.cache/surya-build}
image=${SURYA_IMAGE:-surya-build}
mkdir -p "$out"
if ! podman image exists "$image"; then
    podman build -t "$image" -f "$root/tools/surya/Containerfile" "$root/tools/surya"
fi
if [ ! -f "$out/.config" ]; then
    cfg=${SURYA_CONFIG:-$out/config-phone}
    [ -f "$cfg" ] || { echo "need the phone's kernel config at $cfg" >&2; exit 1; }
    cp "$cfg" "$out/.config"
fi
exec podman run --rm --userns=keep-id -v "$root":/src:Z -v "$out":/out:Z "$image" \
    make KBUILD_MODPOST_WARN=1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION=-surya O=/out -j"$(nproc)" "$@"
