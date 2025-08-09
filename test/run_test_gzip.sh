#!/bin/bash

set -e
echo "Clear cache"
emcc --clear-cache
echo "Building test..."
emcc wasmfs_squashfs.cc -o wasmfs_squashfs.js -sENVIRONMENT=node -O2 -sWASMFS --embed-file squashfs_example_gzip.sqshfs -DTEST_COMPRESSIONS_GZIP \
                                        -std=c++11  --use-port=../ports/libsquashfs.py:compressions=zlib --use-port=../ports/emscripten_wasm_squashfs.py
echo "Executing test..."
# Capture output, print it, and compare
if ! node wasmfs_squashfs.js | diff -u - wasmfs_squashfs_gzip.out; then
    echo "Output differs!"
    
    exit 1
fi
echo "Success for gzip!"
