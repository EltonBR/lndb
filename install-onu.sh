#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)

if [ "$#" -ne 1 ]; then
    echo "usage: $0 ONU_IPV4" >&2
    exit 2
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "install-onu: python3 is required (run ./install-deps.sh)" >&2
    exit 1
fi

if ! python3 -c 'import ipaddress, sys; ipaddress.IPv4Address(sys.argv[1])' "$1" >/dev/null 2>&1; then
    echo "install-onu: expected a valid IPv4 address" >&2
    exit 2
fi

BUILD_DIR=${BUILD_DIR:-$SCRIPT_DIR/build-mips}
DAEMON=$BUILD_DIR/lndbd
needs_build=0

if [ ! -x "$DAEMON" ]; then
    needs_build=1
else
    for source in "$SCRIPT_DIR"/common/*.c "$SCRIPT_DIR"/common/*.h \
                  "$SCRIPT_DIR"/daemon/*.c "$SCRIPT_DIR"/daemon/*.h \
                  "$SCRIPT_DIR"/include/*.h "$SCRIPT_DIR/Makefile" \
                  "$SCRIPT_DIR/VERSION" "$SCRIPT_DIR/build.sh"; do
        [ -f "$source" ] || continue
        if [ "$source" -nt "$DAEMON" ]; then
            needs_build=1
            break
        fi
    done
fi

if [ "$needs_build" -eq 1 ]; then
    echo "install-onu: building the MIPS daemon..."
    BUILD_DIR=$BUILD_DIR "$SCRIPT_DIR/build.sh" daemon
fi

VERSION=$(cat "$SCRIPT_DIR/VERSION")
exec python3 "$SCRIPT_DIR/scripts/install-onu.py" "$1" "$DAEMON" "$VERSION"
