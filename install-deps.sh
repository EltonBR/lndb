#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
SDK_DIR=$SCRIPT_DIR/mips32--musl--stable-2018.02-2
SDK_CC=$SDK_DIR/bin/mips-buildroot-linux-musl-gcc

if [ ! -x "$SDK_CC" ]; then
    echo "install-deps: bundled MIPS toolchain is missing: $SDK_CC" >&2
    exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo "install-deps: this script currently supports Debian/Ubuntu (apt-get)." >&2
    echo "Install build-essential, make, file, grep, sed, coreutils, libc6, libstdc++6 and libgcc-s1 with your package manager." >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "install-deps: run as root or install sudo to install host packages." >&2
        exit 1
    fi
    run_apt() { sudo apt-get "$@"; }
else
    run_apt() { apt-get "$@"; }
fi

echo "Installing host build tools and runtime libraries for the bundled toolchain..."
run_apt update
run_apt install -y build-essential make file grep sed coreutils libc6 libstdc++6 libgcc-s1

SDK_LOCATION_FILE=$SDK_DIR/share/buildroot/sdk-location
if [ ! -f "$SDK_LOCATION_FILE" ]; then
    echo "install-deps: SDK location metadata is missing: $SDK_LOCATION_FILE" >&2
    exit 1
fi

SDK_OLD_LOCATION=$(cat "$SDK_LOCATION_FILE")
if [ "$SDK_OLD_LOCATION" != "$SDK_DIR" ]; then
    echo "Relocating the bundled MIPS SDK to $SDK_DIR..."
    (CDPATH= cd -- "$SDK_DIR" && ./relocate-sdk.sh)
fi

SDK_TARGET=$("$SDK_CC" -dumpmachine)
case $SDK_TARGET in
    mips*-linux-musl*) ;;
    *) echo "install-deps: unexpected toolchain target: $SDK_TARGET" >&2; exit 1 ;;
esac

echo "Dependencies are ready. MIPS compiler target: $SDK_TARGET"
echo "Build with: make CC=./mips32--musl--stable-2018.02-2/bin/mips-buildroot-linux-musl-gcc BUILD_DIR=build-mips"
echo "The target ONU must provide musl; this host script does not install software on the device."
