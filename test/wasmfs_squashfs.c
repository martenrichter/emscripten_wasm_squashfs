/*
 * Copyright 2025 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <dirent.h>
#include <emscripten.h>
#include <emscripten/wasmfs.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <emscripten_squashfs.h>

void printFile(const char* path) {
  FILE* file = fopen(path, "rb");
  printf("Print file: %s\n", path);
  printf("------------\n");
  char buf[4096];
  size_t read = sizeof(buf);
  while (read == sizeof(buf)) {
    read = fread(buf, 1, sizeof(buf), file);
    if (read > 0)
      fwrite(buf, 1, read, stdout);
  }
  fclose(file);
  printf("\n------------\n");
}

void iterateDirs(const char* oldPath) {
  printf("Enter directory: %s\n", oldPath);
  DIR* dir = opendir(oldPath);
  assert(dir != NULL);
  struct dirent* entry = readdir(dir);
  char newPath[PATH_MAX + 1];
  while (entry) {
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
      printf("Process entry: %s\n", entry->d_name);
      snprintf(newPath, sizeof(newPath), "%s/%s", oldPath, entry->d_name);
      printf("New entry: %s\n", newPath);
      struct stat st;
      if (stat(newPath, &st) >= 0) {
        if (S_ISDIR(st.st_mode))
          iterateDirs(newPath);
        else if (S_ISREG(st.st_mode))
          printFile(newPath);
      }
      printf("Finish entry: %s\n", entry->d_name);
    }
    entry = readdir(dir);
  }
  closedir(dir);
  printf("Exit directory: %s\n", oldPath);
}

int main(int argc, char** argv) {
  {
#ifdef TEST_COMPRESSIONS_GZIP
    printf("Create backend from /squashfs_example_gzip.sqshfs...");
    backend_t squashFSBackend =
        wasmfs_create_squashfs_backend("/squashfs_example_gzip.sqshfs");
    if (squashFSBackend == NULL) {
      printf("Backend creation failed\n");
      return 1;
    }
    printf("allocation success!\n");
    // now mount it in the file system
    int ret = wasmfs_create_directory(
        "/squashfs_gzip", S_IRUGO | S_IXUGO | S_IWUGO, squashFSBackend);
    if (ret != 0) {
      printf("Directory creation failed\n");
      return 1;
    }
    printf("mount done!\n");
    printf("Now iterate over all files and print their contents\n");
    iterateDirs("/squashfs_gzip");
    printf("Iteration finished!\n");
#endif
  }
  {
#ifdef TEST_COMPRESSIONS_ZSTD
    printf("Create backend from /squashfs_example_zstd.sqshfs...");fflush(stdout);
    backend_t squashFSBackend =
        wasmfs_create_squashfs_backend("/squashfs_example_zstd.sqshfs");
    if (squashFSBackend == NULL) {
      printf("Backend creation failed\n");
      return 1;
    }
    printf("allocation success!\n");
    // now mount it in the file system
    int ret = wasmfs_create_directory(
        "/squashfs_zstd", S_IRUGO | S_IXUGO | S_IWUGO, squashFSBackend);
    if (ret != 0) {
      printf("Directory creation failed\n");
      return 1;
    }
    printf("mount done!\n");
    printf("Now iterate over all files and print their contents\n");
    iterateDirs("/squashfs_zstd");
    printf("Iteration finished!\n");
#endif
  }
  return 0;
}
