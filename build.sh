#!/bin/sh
# Copyright 2026 Jens Bråkenhielm
# SPDX-License-Identifier: MIT
#
# ---------------------------------------------------------------------------
# build.sh  -  Builds wol for Linux
#
# Usage:
#   build.sh          release build  ->  wol   (statically linked)
#   build.sh debug    debug build    ->  wold  (debug symbols, no optimisation)
#
# Requires a C17-capable compiler (gcc or clang) on PATH.
# ---------------------------------------------------------------------------

DEBUG=0

for arg in "$@"; do
    arg_lower=$(printf '%s' "$arg" | tr '[:upper:]' '[:lower:]')
    case "$arg_lower" in
        debug) DEBUG=1 ;;
        *) echo "Error: unknown argument: $arg" >&2; exit 1 ;;
    esac
done

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
    cc -Wall -Wextra -Werror -g -O0 -std=c17 -DDEBUG -o "$OUT" wol.c
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
    echo "Build FAILED"
    exit 1
fi
