#include <emscripten/wasmfs.h>
#include <emscripten/val.h>

// creates a squashfs backend, backed by a squashfs file inside the filesystem
backend_t wasmfs_create_squashfs_backend(const char* squashFSFile __attribute__((nonnull)));
// creates a squashfs backend, backed by a squashfs served via callbacks
backend_t wasmfs_create_squashfs_backend_callback(emscripten::val props);
