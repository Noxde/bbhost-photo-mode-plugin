#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds photo_mode.dll (Windows, cross-compiled with Clang and the MinGW
# headers and libraries) and photo_mode.so (Linux) into build/, as bbhost's own
# build makes them (C++17, symbols hidden, the Windows runtime linked in).
set -e
cd "$(dirname "$0")"
mkdir -p build
CXXFLAGS="-std=c++17 -O2 -Wall -Wno-missing-field-initializers -shared -fvisibility=hidden -Iinclude"
clang++ --target=x86_64-w64-mingw32 $CXXFLAGS -fuse-ld=lld -s -static -static-libgcc -static-libstdc++ -o build/photo_mode.dll src/photo_mode.cpp -lwinpthread
clang++ $CXXFLAGS -fPIC -o build/photo_mode.so src/photo_mode.cpp
ls -l build
