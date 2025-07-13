#!/bin/bash

set -e
echo "Building test..."
emcc wasmfs_squashfs.c -o wasmfs_squashfs.js -sENVIRONMENT=node -O2 -sWASMFS --embed-file squashfs_example.sqshfs --use-port=../ports/libsquashfs.py --use-port=../ports/emscripten_wasm_squashfs.py
echo "Executing test..."
# Capture output, print it, and compare
if ! node wasmfs_squashfs.js | diff -u - wasmfs_squashfs.out; then
    echo "Output differs!"
    
    exit 1
fi
echo "Success!"
