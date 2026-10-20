#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
DEFAULT_CC=$SCRIPT_DIR/mips32--musl--stable-2018.02-2/bin/mips-buildroot-linux-musl-gcc
CC=${CC:-$DEFAULT_CC}
BUILD_DIR=${BUILD_DIR:-$SCRIPT_DIR/build-mips}
JOBS=${JOBS:-2}
LDFLAGS=${LDFLAGS:--static}

case $CC in
    */*)
        if [ ! -x "$CC" ]; then
            echo "build: compiler is missing or not executable: $CC" >&2
            exit 1
        fi
        ;;
    *)
        if ! command -v "$CC" >/dev/null 2>&1; then
            echo "build: compiler not found in PATH: $CC" >&2
            exit 1
        fi
        ;;
esac

case $JOBS in
    ''|*[!0-9]*|0)
        echo "build: JOBS must be a positive integer" >&2
        exit 2
        ;;
esac

if [ "$#" -eq 0 ]; then
    set -- all
fi

exec make -C "$SCRIPT_DIR" -j"$JOBS" \
    CC="$CC" BUILD_DIR="$BUILD_DIR" LDFLAGS="$LDFLAGS" "$@"
