#!/bin/bash

set -e
echo "Clear cache"
emcc --clear-cache
echo "Building test..."
emcc wasmfs_squashfs.c -o wasmfs_squashfs.js -sENVIRONMENT=node -O2 -sWASMFS --embed-file squashfs_example_gzip.sqshfs --embed-file squashfs_example_zstd.sqshfs \
                                         -DTEST_COMPRESSIONS_ZSTD  --use-port=../ports/libzstd.py \
                                         --use-port=../ports/libsquashfs.py:compressions=zstd --use-port=../ports/emscripten_wasm_squashfs.py
#emcc wasmfs_squashfs.c -o wasmfs_squashfs.js -sENVIRONMENT=node -O2 -sWASMFS --embed-file squashfs_example_zstd.sqshfs \
#                        -DTEST_COMPRESSIONS_ZSTD --use-port=../ports/libzstd.py \
#                        --use-port=../ports/libsquashfs.py:compressions=zstd \
#                        --use-port=../ports/emscripten_wasm_squashfs.py

echo "Executing test..."
# Capture output, print it, and compare
if ! node wasmfs_squashfs.js | diff -u - wasmfs_squashfs_zstd.out; then
    echo "Output differs!"
    
    exit 1
fi
echo "Success for zstd!"
