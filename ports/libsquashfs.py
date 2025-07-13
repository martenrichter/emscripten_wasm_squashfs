# Copyright 2025 Marten Richter.  All rights reserved.
# This port is - as emscripten - available under two separate licenses, the MIT license and the
# University of Illinois/NCSA Open Source License.  Both these licenses can be
# found in the LICENSE file.
# Note that libsquashfs is licensed under LGPLv3

import os
import shutil
from typing import Dict, Set

TAG = '1.3.2'
HASH = '4a3a194af80aa9ed689cf541106906945f546fa15a5b30feff9df95998105298aa919757b1f17bf8387da0bdb05b70857ce818ce8572411cd5ef25e2b93c2022'

deps = []

# contrib port information (required)
URL = 'https://github.com/fails-components/emscripten-wasm-squashfs'
DESCRIPTION = 'This project is an emscripten port to build libsquashfs'
LICENSE = 'LGPL-3.0-or-later license for libsquashfs, emscripten license for the port'

OPTIONS = {
  'compressions': 'A comma separated list of compressions (ex: --use-port=libsquashfs:compressions=zlib,zstd)',
}

VALID_OPTION_VALUES = {
  'compressions': ['zlib', 'zstd']
}

variants = {
  'libsquashfs-zlib':    {'LIBSQUASHFS_COMPRESSIONS': ["zlib"]},
  'libsquashfs-zstd':    {'LIBSQUASHFS_COMPRESSIONS': ["zstd"]},
  'libsquashfs-zlib-zstd':    {'LIBSQUASHFS_COMPRESSIONS': ["zlib","zstd"]},
}

opts: Dict[str, Set] = {
  'compressions': set(),
}


def get_lib_name(settings):
    return 'libsquashfs.a'

def get_compressions():
    return opts['compressions']

def get_lib_name(settings):
  compressions = '-'.join(sorted(get_compressions()))
  libname = 'libsquashfs'
  if compressions != '':
    libname += '-' + compressions
  return libname + '.a'

def get(ports, settings, shared):
  ports.fetch_project('libsquashfs', f'https://infraroot.at/pub/squashfs/squashfs-tools-ng-{TAG}.tar.gz', sha512hash=HASH)
  if not opts['compressions']:
    raise Exception("Missing compression option for libsquashfs") 

  def create(final):
    compressions = get_compressions()
    source_path = ports.get_dir('libsquashfs', 'squashfs-tools-ng-' + TAG)
    squashfsconf_h = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'include/config.h')
    shutil.copyfile(squashfsconf_h, os.path.join(source_path, 'config.h'))
    ports.install_headers(os.path.join(source_path,'include','sqfs'), target='sqfs')
    flags = [ '-Wno-error=incompatible-pointer-types', '-Wno-error=format', '-D_GNU_SOURCE']
    if 'zlib' in compressions:
      flags.append('-sUSE_ZLIB')
      flags.append('-DWITH_GZIP')
    if 'zstd' in compressions:
      flags.append('-DWITH_ZSTD')
      print("falgs so far", flags)
    print("compressions and flags", compressions, flags)
    ports.make_pkg_config('libsquashfs', TAG, flags)
    includes = [os.path.join(source_path, 'include')]
    exclude_dirs = ['bin', 'extras', 'common', 'compat', 'fstree', 'io', 'tar', 'win32', 'gensquashfs', 'libio', 'libtar', 'tests']
    exclude_files = ['lz4.c', 'lzma.c', 'xz.c']
    if 'zstd' not in compressions:
      exclude_files.append('zstd.c')
    if 'zlib' not in compressions:
      exclude_files.append('gzip.c')

    ports.build_port(source_path, final, 'libsquashfs', flags=flags, includes=includes, exclude_dirs=exclude_dirs, exclude_files=exclude_files)

  return [shared.cache.get_lib('libsquashfs.a', create, what='port')]


def clear(ports, settings, shared):
  shared.cache.erase_lib('libsquashfs.a')

def handle_options(options, error_handler):
    if 'compressions' in options:
        values = options['compressions'].split(',')
        invalid = [v for v in values if v not in VALID_OPTION_VALUES['compressions']]
        if invalid:
            error_handler(f"Invalid compression(s): {', '.join(invalid)}")
        opts['compressions'] = set(values)


def process_dependencies(settings):
  compressions = get_compressions()
  if 'zlib' in compressions:
    deps.append('zlib')
  if 'zstd' in compressions:
    deps.append('libzstd')

def show():
  return 'libsquashfs (; LGPL-3.0-or-later license)'
