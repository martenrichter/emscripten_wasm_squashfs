# Copyright 2025 Marten Richter.  All rights reserved.
# This port is - as emscripten - available under two separate licenses, the MIT license and the
# University of Illinois/NCSA Open Source License.  Both these licenses can be
# found in the LICENSE file.
# Note that libsquashfs is licensed under LGPLv3
import os

port_name = 'emscripten_wasmfs_squashfs'

root_path = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))

# contrib port information (required)
URL = 'https://github.com/fails-components/emscripten-wasm-squashfs'
DESCRIPTION = 'This project is an emscripten port to support squashfs through wasmfs'
LICENSE = 'LGPL-3.0-or-later license for necessary libsquashfs, emscripten license for the port '

deps = ['libsquashfs']

def get_lib_name(settings):
    return 'emscripten_wasmfs_squashfs.a'


def get(ports, settings, shared):
    def create(final):
        emscripten_root = shared.path_from_root()
        source_path = os.path.join(root_path, 'src')
        source_include_paths = [os.path.join(root_path, 'include'), os.path.join(emscripten_root, 'system', 'lib', 'wasmfs')]
        ports.install_headers(os.path.join(root_path,'include'))
        srcs = [os.path.join(source_path, "backend.cpp")]
        #flags = [f"--use-port={os.path.abspath(os.path.join(root_path, 'ports', 'libsquashfs.py'))}"]
        ports.build_port(source_path, final, port_name, includes=source_include_paths, srcs=srcs)
    return [shared.cache.get_lib(get_lib_name(settings), create, what='port')]


def clear(ports, settings, shared):
    shared.cache.erase_lib(get_lib_name(settings))


def show():
  return 'emscripten_wasmfs_squashfs (-sUSE_EMSCRIPTEN_WASMFS_SQUASHFS or --use-port=emscripten-wasmfs-squashfs; some files emscripten original license but uses libsquashfs with LGPL-3.0-or-later license)'


