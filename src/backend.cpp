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
#include <emscripten/atomic.h>
#include <dirent.h>
#include <unistd.h>
#include <syscall_arch.h>
#include <list>

using namespace wasmfs;

namespace
{

  struct SharedMemMain
  {
    uint32_t nextFileIdToGet; // used as Mutex
    bool crossOriginIsolated; // if not true, everything is preloaded.
  };

  struct SharedMemChunk
  {
    uint32_t read;    // already available data, used as Mutex
    uint32_t trigger; // position the fs is waiting for
    uint8_t *data;    // nullptr if present
  };

  struct SharedMemFile
  {
    uint32_t fileSize;          // also used as Mutex, 0 is not yet known
    uint32_t fileId;            // id to identify the File
    uint32_t chunkSize;         // usually set by C++ side, only in preloaded case not, also as Mutex, 0 is not yet known
    uint32_t triggerChunkStart; // position the fs is waiting for
    uint32_t triggerChunkEnd;   // position the fs is waiting for
    SharedMemMain *mainStruct;  // pointer to main struct
    SharedMemChunk *chunks;
  };

  class MemBuffer
  {
  public:
    MemBuffer(size_t desiredSize) : size(desiredSize), assignedChunk(nullptr)
    {
      data = (uint8_t *)malloc(desiredSize);
      if (data == nullptr)
        size = 0;
    }

    void unAssign()
    {
      SharedMemChunk *oldChunk = assignedChunk;
      assignedChunk = nullptr;
      oldChunk->data = nullptr; // do me need to add atomics
      oldChunk->read = 0; // which does not mean, it is not buffered on the other side
    }

    bool
    Assign(SharedMemChunk *newChunk)
    {
      if (size == 0 || data == nullptr)
        return false;
      if (assignedChunk)
        unAssign();
      assignedChunk = newChunk;
#ifdef __wasm64__
      emscripten_atomic_store_u64((uint64_t *)&assignedChunk->data, (uint64_t)data);
#else
      emscripten_atomic_store_u32((uint32_t *)&assignedChunk->data, (uint32_t)data);
#endif
      emscripten_atomic_notify(&assignedChunk->data, 1);
      return true;
    }

    ~MemBuffer()
    {
      if (assignedChunk)
        unAssign();
      if (data)
        free(data);
    }

    size_t Size()
    {
      return size;
    }

  protected:
    uint8_t *data;
    SharedMemChunk *assignedChunk;
    size_t size;
  };

  class MemBuffers
  {
  public:
    MemBuffers(size_t maxmem) : cache_mem(0), max_cache_mem(maxmem)
    {
    }

    void tryGetBuffers(std::list<SharedMemChunk *> demand, size_t sizeBuf)
    {
      // First check, if can just create new buffers

      // ok, we recycle and free
      while (demand.size() * sizeBuf + cache_mem > max_cache_mem)
      {
        std::shared_ptr<MemBuffer> buffer = buffers.front();
        buffers.pop_front();
        if (buffer->Size() == sizeBuf)
        { // ok, we recycle
          SharedMemChunk *chunk = demand.front();
          demand.pop_front();
          buffer->Assign(chunk);
          buffers.push_back(buffer);
        }
        else
        {
          cache_mem -= buffer->Size(); // just let it free
        }
      }

      while (demand.size() > 0)
      {
        if (sizeBuf + cache_mem > max_cache_mem)
          return; // sorry this is too much
        std::shared_ptr<MemBuffer> buffer = std::make_shared<MemBuffer>(sizeBuf);
        cache_mem += sizeBuf;
        SharedMemChunk *chunk = demand.front();
        demand.pop_front();
        buffer->Assign(chunk);
        buffers.push_back(buffer);
      }
    }

  protected:
    size_t cache_mem;
    size_t max_cache_mem;
    std::list<std::shared_ptr<MemBuffer>> buffers;
  };

  MemBuffers bufferPool(1024 * 1024); // one MByte buffer ?

  extern "C"
  {
    // helper functions for js based sqfs file, all heavily inspired by the original implementation of libsqfs

    typedef struct
    {
      sqfs_file_t base;
      SharedMemFile *memfile; // index to the memfile
    } sqfs_file_memfile_t;

    static void mem_destroy(sqfs_object_t *base)
    {
      sqfs_file_memfile_t *file = (sqfs_file_memfile_t *)base;
      free(file);
    }

    static sqfs_object_t *mem_copy(const sqfs_object_t *base)
    {
      const sqfs_file_memfile_t *file = (const sqfs_file_memfile_t *)base;
      sqfs_file_memfile_t *copy;
      size_t size;
      copy = (sqfs_file_memfile_t *)calloc(1, sizeof(*file));
      if (copy == NULL)
        return NULL;

      *copy = *file;
      return (sqfs_object_t *)copy;
    }

    static sqfs_u64 mem_get_size(const sqfs_file_t *base)
    {
      const sqfs_file_memfile_t *file = (const sqfs_file_memfile_t *)base;
      SharedMemFile *memFile = file->memfile;
      if (memFile->fileSize == 0)
      {
        // unknown, we need to trigger a read and a wait
        memFile->triggerChunkStart = 0; // we are only waiting for fileSize
        memFile->triggerChunkEnd = 0;
        // set the new val
        emscripten_atomic_store_u32(&memFile->mainStruct->nextFileIdToGet, memFile->fileId);
        emscripten_atomic_notify(&memFile->mainStruct->nextFileIdToGet, 1);
        // now we wait for fileSize
        emscripten_atomic_wait_u32(&memFile->fileSize, 0, -1);
      }
      return memFile->fileSize;
    }

    static int mem_read_at(sqfs_file_t *base, sqfs_u64 offset,
                           void *buffer, size_t size)
    {
      sqfs_file_memfile_t *file = (sqfs_file_memfile_t *)base;
      SharedMemFile *memFile = file->memfile;

      sqfs_u64 fileSize = mem_get_size(base);

      if (offset + size > fileSize)
      {
        return SQFS_ERROR_OUT_OF_BOUNDS;
      }
      if (memFile->chunkSize == 0)
      {
        // per convention the js side will always read the first  chunk and the ChunkSize
        // we just need to wait
        emscripten_atomic_wait_u32(&memFile->chunkSize, 0, -1);
      }
      uint32_t chunkSize = emscripten_atomic_load_u32(&memFile->chunkSize);
      if (memFile->chunks == nullptr)
      {
        // ok wow got it, now we alloc the chunks
        memFile->chunks = (SharedMemChunk *)calloc(fileSize / chunkSize, sizeof(SharedMemChunk));
        emscripten_atomic_notify(&memFile->chunks, 1); // notify, if necessary
      }
      // now we figure out the chunks we need
      uint32_t startChunk = offset / chunkSize;
      uint32_t endChunk = (offset + size) / chunkSize;
      uint32_t lastChunkBytes = (offset + size) % chunkSize;
      if (lastChunkBytes == 0)
      {
        endChunk--;
        lastChunkBytes = chunkSize;
      }
      // ok, now go through all chunks and see if all data is there
      for (uint32_t chunk = startChunk; chunk < endChunk; chunk++)
      {
        // check if chunk has data
        if (memFile->chunks[chunk].read != chunkSize)
        {
          // ok, we do not have data yet, figure out which chunks also need a fetch
          uint32_t endfchunk = chunk;
          for (uint32_t fchunk = chunk + 1; fchunk < endChunk; fchunk++)
          {
            if (memFile->chunks[fchunk].read == chunkSize)
              break; // ok, this one was read
            endfchunk = fchunk;
          }
          if (endfchunk == endChunk - 1)
          {
            if (memFile->chunks[endChunk].read < lastChunkBytes)
            {
              endfchunk = endChunk;
            }
          }
          // now we got the area we need to fetch
          // first set our desired ranges
          for (uint32_t gchunk = chunk; gchunk < endfchunk; gchunk++)
          {
            memFile->chunks[gchunk].trigger = chunkSize;
          }
          // second get buffers
          std::list<SharedMemChunk *> toAlloc;
          for (uint32_t gchunk = chunk; gchunk < endfchunk; gchunk++)
          {
            if (memFile->chunks[gchunk].data == nullptr)
            {
              toAlloc.push_back(&memFile->chunks[gchunk]);
            }
          }
          bufferPool.tryGetBuffers(toAlloc, chunkSize);
          assert(memFile->chunks[chunk].data != nullptr);
          // now adjust end fchunk
          for (uint32_t gchunk = chunk; gchunk < endfchunk; gchunk++)
          {
            if (memFile->chunks[gchunk].data == nullptr) {
              endfchunk = gchunk;
              break;
            }
          }
          memFile->chunks[endfchunk].trigger = endfchunk == endChunk ? lastChunkBytes : chunkSize;

          // now tell, which the chunks are
          memFile->triggerChunkStart = chunk;
          memFile->triggerChunkEnd = endfchunk;
          emscripten_atomic_store_u32(&memFile->mainStruct->nextFileIdToGet, memFile->fileId);
          emscripten_atomic_notify(&memFile->mainStruct->nextFileIdToGet, 1);
          // now we wait for the trigger at the last chunk
          uint32_t read = memFile->chunks[endfchunk].read;
          while (read < memFile->chunks[endfchunk].trigger)
          {
            emscripten_atomic_wait_u32(&memFile->chunks[endfchunk].read, read, -1);
            read = memFile->chunks[endfchunk].read;
          }
        }
        uint32_t start = std::max((uint32_t)(offset - chunk * chunkSize), (uint32_t)0);
        uint32_t cpysize = chunkSize - start;
        memcpy(((uint8_t *)buffer), memFile->chunks[chunk].data + start, cpysize); // note that the loop runs < endChunk, so the last step is outside the loop
      }
      // fetch the last block
      if (memFile->chunks[endChunk].read < lastChunkBytes) // note is uint, so zero or positive
      {
        memFile->chunks[endChunk].trigger = lastChunkBytes;
        memFile->triggerChunkStart = endChunk;
        memFile->triggerChunkEnd = endChunk;
        if (memFile->chunks[endChunk].data == nullptr)
        {
          std::list<SharedMemChunk *> toAlloc;
          toAlloc.push_back(&memFile->chunks[endChunk]);
          bufferPool.tryGetBuffers(toAlloc, chunkSize);
        
          assert(memFile->chunks[endChunk].data != nullptr);
          memFile->chunks[endChunk].data = (uint8_t *)malloc(chunkSize);
        }
        emscripten_atomic_store_u32(&memFile->mainStruct->nextFileIdToGet, memFile->fileId);
        emscripten_atomic_notify(&memFile->mainStruct->nextFileIdToGet, 1);
        uint32_t read = memFile->chunks[endChunk].read;
        while (read < memFile->chunks[endChunk].trigger)
        {
          emscripten_atomic_wait_u32(&memFile->chunks[endChunk].read, read, -1);
          read = memFile->chunks[endChunk].read;
          break;
        }
      }
      {
        uint32_t start = std::max((uint32_t)(offset - endChunk * chunkSize), (uint32_t)0);
        uint32_t cpysize = lastChunkBytes - start;
        memcpy(((uint8_t *)buffer), memFile->chunks[endChunk].data + start, cpysize);
      }

      return 0;
    }

    static int mem_write_at(sqfs_file_t *base, sqfs_u64 offset,
                            const void *buffer, size_t size)
    {
      return SQFS_ERROR_IO; // only reading
    }

    static int mem_truncate(sqfs_file_t *base, sqfs_u64 size)
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

    SquashFSBackend(SharedMemFile *memFile)
    {
      inited = false;
      fsFile = nullptr;
      compressor = nullptr;
      idTable = nullptr;
      dirReader = nullptr;
      inited = false;
      sqfs_file_t *file;
      int ret = openMemFile(&file, memFile);
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
    int openMemFile(sqfs_file_t **out, SharedMemFile *memfile)
    {
      sqfs_file_memfile_t *file;
      size_t size, namelen;
      sqfs_file_t *base;
      int ret;

      *out = nullptr;

      file = (sqfs_file_memfile_t *)calloc(1, sizeof(*file));
      base = (sqfs_file_t *)file;
      if (file == nullptr)
      {
        return SQFS_ERROR_ALLOC;
      }
      file->memfile = memfile;
      base->read_at = mem_read_at;
      base->write_at = mem_write_at;
      base->get_size = mem_get_size;
      base->truncate = mem_truncate;

      ((sqfs_object_t *)base)->copy = mem_copy;
      ((sqfs_object_t *)base)->destroy = mem_destroy;

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

extern "C"
{
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
  void wasmfs_squashfs_init_callback()
  {
    // we do nothing, it is just ensures, that the lib is not stripped
  }

  // forward declaration
  int wasmfs_create_directory(char *path, int mode, backend_t backend);

  EMSCRIPTEN_KEEPALIVE uintptr_t wasmfs_create_squashfs_backend_memfile(SharedMemFile *memfile)
  {
    std::unique_ptr<SquashFSBackend> sqFSBackend =
        std::make_unique<SquashFSBackend>(memfile);
    if (sqFSBackend->isInited())
    {
      return (uintptr_t)wasmFS.addBackend(std::move(sqFSBackend)); //(uintptr_t)
    }
    else
    {
      return (uintptr_t)nullptr;
    }
  }

  // may be strip if not necessary
  EMSCRIPTEN_KEEPALIVE bool wasmfs_create_squashfs_backend_memfile_and_mount(SharedMemFile *memfile,
                                                                             std::string path)
  {
    std::unique_ptr<SquashFSBackend> sqFSBackend =
        std::make_unique<SquashFSBackend>(memfile);
    if (sqFSBackend->isInited())
    {
      backend_t backend = wasmFS.addBackend(std::move(sqFSBackend));
      int ret = wasmfs_create_directory(const_cast<char *>(path.c_str()), 0777, backend);
      if (ret != 0)
        return false;
      else
        return true;
    }
    else
    {
      return false;
    }
  }
}
