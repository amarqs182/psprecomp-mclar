#!/bin/sh
# Midnight Club: L.A. Remix (USA) — PortMaster launch script
#
# This script launches the statically-recompiled PSP port on PortMaster.
# The user must supply their own decrypted EBOOT.ELF (see README.md).
#
# The decrypted EBOOT.ELF is expected at one of the following locations
# (first match wins):
#   $PORT_ROOT/../userdata/EBOOT.ELF
#   $PORT_ROOT/../bios/EBOOT.ELF
#   $MC3_EBOOT (environment variable)
#   ~/mc3/EBOOT.ELF
#
# The game is launched with:
#   mc3 -e <path-to-EBOOT.ELF> [-r software|gles32|null]

set -eu

# Resolve the directory containing this script (the port root).
PORT_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

# The recompiled binary lives next to this script.
BINARY="$PORT_ROOT/mc3"

# Optional: override the EBOOT location via environment.
MC3_EBOOT="${MC3_EBOOT:-}"

find_eboot() {
    # 1. Environment variable
    if [ -n "$MC3_EBOOT" ] && [ -f "$MC3_EBOOT" ]; then
        echo "$MC3_EBOOT"
        return 0
    fi
    # 2. Common PortMaster locations (relative to port root)
    for cand in \
        "$PORT_ROOT/../userdata/EBOOT.ELF" \
        "$PORT_ROOT/../bios/EBOOT.ELF" \
        "$PORT_ROOT/EBOOT.ELF" \
        "$HOME/mc3/EBOOT.ELF"; do
        if [ -f "$cand" ]; then
            echo "$cand"
            return 0
        fi
    done
    return 1
}

EBOOT_PATH="$(find_eboot || true)"

if [ -z "$EBOOT_PATH" ]; then
    echo "Error: decrypted EBOOT.ELF not found." >&2
    echo "Place your decrypted EBOOT.ELF at one of:" >&2
    echo "  \$MC3_EBOOT (environment variable)" >&2
    echo "  \$PORT_ROOT/../userdata/EBOOT.ELF" >&2
    echo "  \$PORT_ROOT/../bios/EBOOT.ELF" >&2
    echo "  ~/mc3/EBOOT.ELF" >&2
    echo "See README.md for decryption instructions." >&2
    exit 1
fi

if [ ! -x "$BINARY" ]; then
    echo "Error: recompiled binary not found at $BINARY" >&2
    echo "Build the port first (see README.md)." >&2
    exit 1
fi

# Library path: PortMaster shared runtime (if declared) + port-local libs.
export LD_LIBRARY_PATH="$PORT_ROOT/../libs:$PORT_ROOT/libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Default to the GLES 3.2 backend on RK3326; override with MC3_RENDER.
MC3_RENDER="${MC3_RENDER:-gles32}"

exec "$BINARY" -e "$EBOOT_PATH" -r "$MC3_RENDER"
