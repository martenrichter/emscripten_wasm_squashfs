// Copyright 2025 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include "backend.h"
#include "sqfs/predef.h"
#include "sqfs/compressor.h"
#include "sqfs/data_reader.h"
#include "sqfs/dir.h"
#include "sqfs/dir_reader.h"
#include "sqfs/id_table.h"
#include "sqfs/inode.h"
#include "sqfs/io.h"
#include "sqfs/super.h"
#include "sqfs/error.h"
#include "wasmfs.h"
#include <emscripten/val.h>
#include <emscripten/wire.h>

using namespace wasmfs;

namespace
{

  extern "C"
  {
    // helper functions for js based sqfs file, all heavily inspired by the original implementation of libsqfs
    typedef struct
    {
      sqfs_file_t base;
      sqfs_u64 size;
      emscripten::val callback; // index to js reading function
    } sqfs_file_js_t;

    static void js_destroy(sqfs_object_t *base)
    {
      sqfs_file_js_t *file = (sqfs_file_js_t *)base;
      free(file);
    }

    static sqfs_object_t *js_copy(const sqfs_object_t *base)
    {
      const sqfs_file_js_t *file = (const sqfs_file_js_t *)base;
      sqfs_file_js_t *copy;
      size_t size;
      copy = (sqfs_file_js_t *)calloc(1, sizeof(*file));
      if (copy == NULL)
        return NULL;

      *copy = *file;
      return (sqfs_object_t *)copy;
    }

    static int js_read_at(sqfs_file_t *base, sqfs_u64 offset,
                          void *buffer, size_t size)
    {
      sqfs_file_js_t *file = (sqfs_file_js_t *)base;

      if (offset + size > file->size)
      {
        return SQFS_ERROR_OUT_OF_BOUNDS;
      }

      int ret = file->callback(offset, (uintptr_t)buffer, size).await().as<int>();
      return ret;
    }

    static int js_write_at(sqfs_file_t *base, sqfs_u64 offset,
                           const void *buffer, size_t size)
    {
      return SQFS_ERROR_IO; // only reading
    }

    static sqfs_u64 js_get_size(const sqfs_file_t *base)
    {
      const sqfs_file_js_t *file = (const sqfs_file_js_t *)base;

      return file->size;
    }

    static int js_truncate(sqfs_file_t *base, sqfs_u64 size)
    {
      return SQFS_ERROR_UNSUPPORTED;
    }
  }

  class SquashFSFile;
  class SquashFSDirectory;
  class SquashFSSymlink;

  class SquashFSBackend : public Backend
  {
    friend SquashFSFile;
    friend SquashFSDirectory;
    friend SquashFSSymlink;

  public:
    SquashFSBackend(const std::string &squashFSFile)
    {
      inited = false;
      fsFile = nullptr;
      compressor = nullptr;
      idTable = nullptr;
      dirReader = nullptr;
      inited = false;
      sqfs_file_t *file = sqfs_open_file(squashFSFile.c_str(), SQFS_FILE_OPEN_READ_ONLY);
      if (file == nullptr)
      {
        // TODO: How can I pass errors from the backend
        return; // stop init we failed
      }
      init(file);
    }

    SquashFSBackend(emscripten::val props)
    {
      inited = false;
      fsFile = nullptr;
      compressor = nullptr;
      idTable = nullptr;
      dirReader = nullptr;
      inited = false;
      sqfs_file_t *file;
      int ret = open_js(&file, props);
      if (ret != 0)
      {
        // TODO: How can I pass errors from the backend
        return; // stop init we failed
      }
      init(file);
    }

    void init(sqfs_file_t *file)
    {

      fsFile = file;

      /* read super block*/
      if (sqfs_super_read(&superBlock, fsFile))
      {
        // TODO: How can I pass errors from the backend
        return;
      }

      sqfs_compressor_config_init(
          &compressorCfg,
          static_cast<SQFS_COMPRESSOR>(superBlock.compression_id),
          superBlock.block_size,
          SQFS_COMP_FLAG_UNCOMPRESS);

      int ret = sqfs_compressor_create(&compressorCfg, &compressor);
      if (ret != 0)
      {
        // TODO: How can I pass errors from the backend
        return;
      }
      idTable = sqfs_id_table_create(0);
      if (idTable == nullptr)
      {
        // TODO: How can I pass errors from the backend
        return;
      }

      if ((ret = sqfs_id_table_read(idTable, fsFile, &superBlock, compressor)))
      {
        // TODO: How can I pass errors from the backend
        return;
      }
      dirReader = sqfs_dir_reader_create(&superBlock, compressor, fsFile, 0);
      if (dirReader == nullptr)
      {
        // TODO: How can I pass errors from the backend
        return;
      }
      dataReader =
          sqfs_data_reader_create(fsFile, superBlock.block_size, compressor, 0);
      if (dataReader == nullptr)
      {
        // TODO: How can I pass errors from the backend
        return;
      }

      ret = sqfs_data_reader_load_fragment_table(dataReader, &superBlock);
      if (ret != 0)
      {
        // TODO: How can I pass errors from the backend
        return;
      }
      inited = true;
    }

    virtual ~SquashFSBackend()
    {
      if (fsFile)
        sqfs_destroy(fsFile);
      if (compressor)
        sqfs_destroy(compressor);
      if (idTable)
        sqfs_destroy(idTable);
      if (dirReader)
        sqfs_destroy(dirReader);
      if (dataReader)
        sqfs_destroy(dataReader);
    }

    std::shared_ptr<DataFile> createFile(mode_t mode) override
    {
      // honestly, this is readonly FS, and without a dir how should this work
      return nullptr;
    }

    std::shared_ptr<Directory> createDirectory(mode_t mode) override;

    std::shared_ptr<Symlink> createSymlink(std::string target) override
    {
      // Symlinks not supported.
      return nullptr;
    }

    bool isInited() { return inited; }

  private:
    sqfs_file_t *fsFile;
    sqfs_super_t superBlock;
    sqfs_compressor_config_t compressorCfg;
    sqfs_compressor_t *compressor;
    sqfs_id_table_t *idTable;
    sqfs_dir_reader_t *dirReader;
    sqfs_data_reader_t *dataReader;
    bool inited;

    // again a rewritten version of the squashfs original
    int open_js(sqfs_file_t **out, emscripten::val props)
    {
      sqfs_file_js_t *file;
      size_t size, namelen;
      sqfs_file_t *base;
      int ret;

      *out = nullptr;

      file = (sqfs_file_js_t *)calloc(1, sizeof(*file));
      base = (sqfs_file_t *)file;
      if (file == nullptr)
      {
        return SQFS_ERROR_ALLOC;
      }
      file->size = props["size"].as<size_t>();
      file->callback = props["callback"];

      base->read_at = js_read_at;
      base->write_at = js_write_at;
      base->get_size = js_get_size;
      base->truncate = js_truncate;

      ((sqfs_object_t *)base)->copy = js_copy;
      ((sqfs_object_t *)base)->destroy = js_destroy;

      *out = base;
      return 0;
    }
  };

  class SquashFSFile : public DataFile
  {
  public:
    SquashFSFile(mode_t mode,
                 SquashFSBackend *backend,
                 std::shared_ptr<sqfs_inode_generic_t> inode)
        : DataFile(mode, backend), inodeGeneric(inode) {}

    int open(oflags_t flags) override { return 0; };
    int close() override { return 0; }

    ssize_t read(uint8_t *buf, size_t len, off_t offset) override
    {
      SquashFSBackend *sbackend = static_cast<SquashFSBackend *>(getBackend());
      sqfs_s32 res = sqfs_data_reader_read(
          sbackend->dataReader, inodeGeneric.get(), offset, buf, len);
      return res;
    }
    ssize_t write(const uint8_t *buf, size_t len, off_t offset) override
    {
      return -EROFS;
    }

    int setSize(off_t size) override { return -EROFS; }

    int flush() override { return 0; }

    off_t getSize() override { return inodeGeneric->data.file.file_size; }

  private:
    std::shared_ptr<sqfs_inode_generic_t> inodeGeneric;
  };

  class SquashFSDirectory : public Directory
  {
    // Use a vector instead of a map to save code size.
    struct ChildEntry
    {
      ChildEntry(std::string _name, std::shared_ptr<File> _child)
          : name(_name), child(_child) {};
      std::string name;
      std::shared_ptr<File> child;
    };

    std::vector<ChildEntry> entries;

  public:
    SquashFSDirectory(mode_t mode,
                      SquashFSBackend *backend,
                      std::shared_ptr<sqfs_inode_generic_t> inode)
        : Directory(mode, backend), dirRead(false), inodeGeneric(inode) {}

  protected:
    // From memory backend
    std::vector<ChildEntry>::iterator findEntry(const std::string &name)
    {
      if (!dirRead)
      {
        getEntries();
      }
      return std::find_if(entries.begin(), entries.end(), [&](const auto &entry)
                          { return entry.name == name; });
    }
    // From memory backend
    std::shared_ptr<File> getChild(const std::string &name) override
    {
      if (auto entry = findEntry(name); entry != entries.end())
      {
        return entry->child;
      }
      return nullptr;
    }

    std::shared_ptr<DataFile> insertDataFile(const std::string &name,
                                             mode_t mode) override
    {
      // we are readonly
      return nullptr;
    }
    std::shared_ptr<Directory> insertDirectory(const std::string &name,
                                               mode_t mode) override
    {
      // we are readonly
      return nullptr;
    }
    std::shared_ptr<Symlink> insertSymlink(const std::string &name,
                                           const std::string &target) override
    {
      // we are readonly
      return nullptr;
    }

    int insertMove(const std::string &name, std::shared_ptr<File> file) override
    {
      return -EROFS; // we are readonly
    }

    int removeChild(const std::string &name) override
    {
      return -EROFS; // we are readonly
    }

    // The number of entries in this directory. Returns the number of entries or a
    // negative error code.
    ssize_t getNumEntries() override
    {
      if (!dirRead)
      {
        getEntries();
      }
      return entries.size();
    }

    // The list of entries in this directory or a negative error code.
    MaybeEntries getEntries() override
    {
      if (!dirRead)
      { // dir was not read, so we read in know
        SquashFSBackend *sbackend = static_cast<SquashFSBackend *>(getBackend());

        int result =
            sqfs_dir_reader_open_dir(sbackend->dirReader, inodeGeneric.get(), 0);
        if (result != 0)
        {
          return {EIO};
        }
        while (result == 0)
        {
          sqfs_dir_entry_t *curEntry;
          result = sqfs_dir_reader_read(sbackend->dirReader, &curEntry);
          if (result != 0)
            break;
          sqfs_inode_generic_t *cinode;
          result = sqfs_dir_reader_get_inode(sbackend->dirReader, &cinode);
          if (result != 0)
          {
            sqfs_destroy(&curEntry);
            break;
          }

          switch (curEntry->type)
          {
          case SQFS_INODE_DIR:
            entries.emplace_back(
                std::string(reinterpret_cast<char *>(curEntry->name),
                            curEntry->size + 1),
                std::make_shared<SquashFSDirectory>(
                    cinode->base.mode,
                    sbackend,
                    std::shared_ptr<sqfs_inode_generic_t>(cinode, sqfs_free)));
            break;

          case SQFS_INODE_FILE:
            entries.emplace_back(
                std::string(reinterpret_cast<char *>(curEntry->name),
                            curEntry->size + 1),
                std::make_shared<SquashFSFile>(
                    cinode->base.mode,
                    sbackend,
                    std::shared_ptr<sqfs_inode_generic_t>(cinode, sqfs_free)));
            break;

          case SQFS_INODE_SLINK:
            entries.emplace_back(
                std::string(reinterpret_cast<char *>(curEntry->name),
                            curEntry->size + 1),
                std::make_shared<SquashFSSymlink>(
                    sbackend,
                    std::shared_ptr<sqfs_inode_generic_t>(cinode, sqfs_free)));
            break;
          default:
            sqfs_free(cinode);
            // do not add device file etc.
          };
          sqfs_free(curEntry);
        };
        if (result < 0)
        {
          return {EIO};
        }
        dirRead = true;
      }

      // from MemoryDirectory
      std::vector<Directory::Entry> result;
      result.reserve(entries.size());
      for (auto &[name, child] : entries)
      {
        result.push_back({name, child->kind, child->getIno()});
      }
      return {result};
    }

    // from MemoryDirectory
    std::string getName(std::shared_ptr<File> file) override
    {
      auto it =
          std::find_if(entries.begin(), entries.end(), [&](const auto &entry)
                       { return entry.child == file; });
      if (it != entries.end())
      {
        return it->name;
      }
      return "";
    }

    bool maintainsFileIdentity() override { return true; }

  private:
    bool dirRead;
    std::shared_ptr<sqfs_inode_generic_t> inodeGeneric;
  };

  class SquashFSSymlink : public Symlink
  {
  public:
    SquashFSSymlink(SquashFSBackend *backend,
                    std::shared_ptr<sqfs_inode_generic_t> inode)
        : Symlink(backend), inodeGeneric(inode) {}

    std::string getTarget() const override
    {
      return std::string(reinterpret_cast<char *>(inodeGeneric->extra),
                         inodeGeneric->data.slink.target_size);
    }

  private:
    std::shared_ptr<sqfs_inode_generic_t> inodeGeneric;
  };

  std::shared_ptr<Directory> SquashFSBackend::createDirectory(mode_t mode)
  {
    sqfs_inode_generic_t *inode;
    int result = sqfs_dir_reader_get_root_inode(dirReader, &inode);
    if (result != 0)
      return nullptr;

    return std::make_shared<SquashFSDirectory>(
        mode, this, std::shared_ptr<sqfs_inode_generic_t>(inode, sqfs_free));
  }

} // anonymous namespace

backend_t wasmfs_create_squashfs_backend(const char *squashFSFile)
{
  std::unique_ptr<SquashFSBackend> sqFSBackend =
      std::make_unique<SquashFSBackend>(squashFSFile);
  if (sqFSBackend->isInited())
  {
    return wasmFS.addBackend(std::move(sqFSBackend));
  }
  else
  {
    return nullptr;
  }
}

backend_t wasmfs_create_squashfs_backend_callback(emscripten::val props)
{
  std::unique_ptr<SquashFSBackend> sqFSBackend =
      std::make_unique<SquashFSBackend>(props);
  if (sqFSBackend->isInited())
  {
    return wasmFS.addBackend(std::move(sqFSBackend));
  }
  else
  {
    return nullptr;
  }
}
