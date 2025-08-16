#include <emscripten/wasmfs.h>
#include <emscripten/val.h>

extern "C"
{
    // creates a squashfs backend, backed by a squashfs file inside the filesystem
    backend_t wasmfs_create_squashfs_backend(const char *squashFSFile __attribute__((nonnull)));
    // init the lib, or prevents the linker from stripping it
    void wasmfs_squashfs_init_callback();
}

// uintptr_t wasmfs_create_squashfs_backend_callback(emscripten::val props);
