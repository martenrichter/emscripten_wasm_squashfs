# Copyright 2025 Marten Richter.  All rights reserved.
# This port is - as emscripten - available under two separate licenses, the MIT license and the
# University of Illinois/NCSA Open Source License.  Both these licenses can be
# found in the LICENSE file.
# Note that libsquashfs is licensed under LGPLv3

import os
import shutil

TAG = '1.5.7'
HASH = 'b4de208f179b68d4c6454139ca60d66ed3ef3893a560d6159a056640f83d3ee67cdf6ffb88971cdba35449dba4b597eaa8b4ae908127ef7fd58c89f40bf9a705'


# contrib port information (required)
URL = 'https://github.com/fails-components/emscripten-wasm-squashfs'
DESCRIPTION = 'This project is an emscripten port to build libzstd'
LICENSE = 'BSD or GPLv2 license for libzstd, emscripten license for the port'


def get(ports, settings, shared):
  # TODO: This is an emscripten-hosted mirror of the libsquashfs repo from Sourceforge.
  ports.fetch_project('libzstd', f'https://github.com/facebook/zstd/releases/download/v{TAG}/zstd-{TAG}.tar.gz', sha512hash=HASH)

  def create(final):
    source_path = ports.get_dir('libzstd', 'zstd-' + TAG, 'lib')
#    squashfsconf_h = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'include/config.h')
#    shutil.copyfile(squashfsconf_h, os.path.join(source_path, 'config.h'))
    ports.install_headers(source_path)
#    flags = ['-sUSE_ZLIB', '-DWITH_GZIP', '-Wno-error=incompatible-pointer-types', '-Wno-error=format', '-D_GNU_SOURCE']
    flags = ['-DZSTD_LIB_COMPRESSION=0']
    if hasattr(ports, 'make_pkg_config'):
      ports.make_pkg_config('libzstd', TAG, flags)
    includes = []
    exclude_dirs = []
    exclude_files = []
    ports.build_port(source_path, final, 'libzstd', includes=includes, exclude_dirs=exclude_dirs, exclude_files=exclude_files)

  return [shared.cache.get_lib('libzstd.a', create, what='port')]


def clear(ports, settings, shared):
  shared.cache.erase_lib('libzstd.a')


#def process_dependencies(settings):
#  settings.USE_ZLIB = 1


def show():
  return 'libzstd (; LGPL-3.0-or-later license)'
