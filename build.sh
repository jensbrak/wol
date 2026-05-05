#!/bin/sh
# Copyright 2026 Jens Bråkenhielm
# SPDX-License-Identifier: MIT
#
# ---------------------------------------------------------------------------
# build.sh  -  Builds wol for Linux and macOS
#
# Usage:
#   build.sh            release build   ->  wol  (statically linked on Linux)
#   build.sh debug      debug build     ->  wold (debug symbols, no optimisation)
#   build.sh archive    create release archive  ->  wol-vX.Y.Z-OS-ARCH.tar.gz
#                       (wol must exist; run ./build.sh first)
#
# Build:   C17-capable compiler (gcc or clang) on PATH
# Archive: tar + gzip -9 (max compression); archive name reflects OS and architecture
# Version: extracted automatically from wol.c
# ---------------------------------------------------------------------------

DEBUG=0
ARCHIVE=0

if [ $# -gt 1 ]; then
    echo "Error: too many arguments" >&2; exit 1
fi
ARG=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')
case "$ARG" in
    debug)   DEBUG=1 ;;
    archive) ARCHIVE=1 ;;
    "")      ;;
    *)       echo "Error: unknown argument: $1" >&2; exit 1 ;;
esac

# Build
if [ "$ARCHIVE" = "0" ]; then

    if [ "$DEBUG" = "1" ]; then
        OUT=wold
    else
        OUT=wol
    fi

    if [ ! -f "wol.c" ]; then
        echo "Error: source file not found: wol.c" >&2
        exit 1
    fi

    if [ "$DEBUG" = "1" ]; then
        cc -Wall -Wextra -Werror -g -O0 -std=c17 -DDEBUG -DWOL_SELF_TEST -o "$OUT" wol.c
    elif [ "$(uname -s)" = "Darwin" ]; then
        cc -Wall -Wextra -Werror -O2 -std=c17 -DNDEBUG -o "$OUT" wol.c
    else
        cc -Wall -Wextra -Werror -O2 -std=c17 -static -DNDEBUG -o "$OUT" wol.c
    fi
    RESULT=$?

    echo
    if [ "$RESULT" -eq 0 ]; then
        if [ "$DEBUG" = "1" ]; then
            echo "Build successful: $OUT (debug)"
        else
            echo "Build successful: $OUT"
        fi
    else
        echo "Build FAILED" >&2
        exit 1
    fi

fi

# Archive
if [ "$ARCHIVE" = "1" ]; then

    # Extract version from wol.c (#define WOL_VERSION "X.Y.Z")
    VERSION=$(sed -n 's/^#define WOL_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' wol.c)
    if [ -z "$VERSION" ]; then
        echo "Error: could not extract version from wol.c" >&2
        exit 1
    fi

    if [ ! -f "wol" ]; then
        echo "Error: wol not found -- run ./build.sh first" >&2
        exit 1
    fi

    if [ ! -f "readme.txt" ]; then
        echo "Error: readme.txt not found" >&2
        exit 1
    fi

    OS=$(uname -s | tr '[:upper:]' '[:lower:]')
    ARCH=$(uname -m)
    TARBALL="wol-v${VERSION}-${OS}-${ARCH}.tar.gz"
    tar -cf - wol readme.txt | gzip -9 > "$TARBALL"

    if [ $? -eq 0 ]; then
        echo
        echo "Archive created: $TARBALL"
    else
        echo
        echo "Archive FAILED" >&2
        exit 1
    fi

fi
