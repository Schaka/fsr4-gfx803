#!/bin/bash
# Build the frame-generation passthrough proxy in proxies/. Needs mingw64-gcc.
set -e
cd "$(dirname "$0")/../proxies"
x86_64-w64-mingw32-gcc -shared -O2 -o fg_passthrough_proxy.dll fg_passthrough_proxy.c
echo "built:"; ls -la *.dll
