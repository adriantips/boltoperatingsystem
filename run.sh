#!/usr/bin/env bash
# Boot BoltOS in QEMU. Serial -> stdio so kernel output shows in the terminal.
# Extra args are passed through (e.g. -display sdl for a real window).
ROOT="$(cd "$(dirname "$0")" && pwd)"
QEMU=/c/Program\ Files/qemu/qemu-system-x86_64.exe
exec "$QEMU" \
    -drive file="$ROOT/iso/os.img",format=raw,if=ide \
    -drive file="$ROOT/iso/boltos.iso",if=ide,media=cdrom \
    -boot order=c \
    -m 512M \
    -serial stdio \
    -no-reboot -no-shutdown \
    "$@"
