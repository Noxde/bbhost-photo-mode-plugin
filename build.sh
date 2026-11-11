#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds bbphoto.dll (Windows, cross-compiled with Clang) and bbphoto.so (Linux) into build/.
set -e
cd "$(dirname "$0")"
mkdir -p build
CFLAGS="-O2 -Wall -Wno-missing-field-initializers -shared -Iinclude -Isrc"
clang --target=x86_64-w64-mingw32 $CFLAGS -o build/bbphoto.dll src/bbphoto.c
clang $CFLAGS -fPIC -o build/bbphoto.so src/bbphoto.c -lm
ls -l build
