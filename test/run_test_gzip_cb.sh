#!/bin/bash

set -e
echo "Clear cache"
emcc --clear-cache
NODE=${EMSDK_NODE:-node}
echo "Check node version..."
"$NODE" --version
echo "Building test..."
emcc wasmfs_squashfs.cc -o wasmfs_squashfs.js -sENVIRONMENT=node -O2 -sWASMFS  -DTEST_COMPRESSIONS_GZIP -sEXPORTED_FUNCTIONS='["_wasmfs_squashfs_init_callback"]' \
                                             -DTEST_CALLBACK -std=c++11 -sASYNCIFY -lembind -sEXPORTED_RUNTIME_METHODS='["HEAPU8"]'\
                                         --use-port=../ports/libsquashfs.py:compressions=zlib --use-port=../ports/emscripten_wasm_squashfs.py
echo "Executing test..."
# Capture output, print it, and compare
if ! "$NODE" wasmfs_test_callback.js | diff -u - wasmfs_squashfs_gzip_cb.out; then
    echo "Output differs!"
    
    exit 1
fi
echo "Success for gzip cb!"
