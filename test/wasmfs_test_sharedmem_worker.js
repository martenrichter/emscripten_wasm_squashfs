let heap;
let heapU8;
let heapI32;
let sharedMemMainDv;
let sharedMemMain;
let files = {};
const fsprom = require("fs/promises");
const { parentPort } = require("worker_threads");

const waitAsync = async (pointer, expect) => {
  const { value, async } = Atomics.waitAsync(heapI32, pointer / 4, expect);
  if (async) {
    await value;
  }
};

const fetchLoop = async () => {
  while (true) {
    try {
      await new Promise((resolve) => setTimeout(resolve, 1));
      await waitAsync(sharedMemMain, 0); // TODO polyfill for older Firefoxes
      const nextFileIdToGet = sharedMemMainDv.getUint32(0, true);
      await new Promise((resolve) => setTimeout(resolve, 1));

      sharedMemMainDv.setUint32(0, 0, true);
      const file = files[nextFileIdToGet];
      if (file) {
        // now decide what to do
        // check if fileSize is set
        const { sharedMemFile, sharedMemFileDv, fileName, fileHandle } = file;
        if (sharedMemFileDv.getUint32(0, true) === 0) {
          // ok we need to get the fileSize
          let stats = await fsprom.stat(fileName);
          sharedMemFileDv.setUint32(0, stats.size, true);
          Atomics.store(heapI32, (sharedMemFile + 0) / 4, stats.size);
          Atomics.notify(heapI32, (sharedMemFile + 0) / 4, 1);
          // now we need to read the header to get the chunksize
          const fileHandle = await fsprom.open(fileName, "r");
          file.fileHandle = fileHandle;
          // readHeader
          const header = new Uint8Array(16);
          const readRes = await fileHandle.read(header, 0, 16, 0);
          const headerDv = new DataView(header.buffer);
          const blockSize = headerDv.getUint32(12, true); // is equal to chunkSize
          await new Promise((resolve) => setTimeout(resolve, 1));
          Atomics.store(heapI32, (sharedMemFile + 8) / 4, blockSize);
          Atomics.notify(heapI32, (sharedMemFile + 8) / 4, 1);
          await new Promise((resolve) => setTimeout(resolve, 1));
          // now we have to wait, that chunks is set
          await waitAsync(sharedMemFile + 6 * 4);
          continue; // ok, done, the next fetch has to reset nextFileIdToGet
        } else {
          // init done now do the data fetch
          // ok let's get the startChunk and EndChunk
          const startChunk = sharedMemFileDv.getUint32(12, true);
          const endChunk = sharedMemFileDv.getUint32(16, true);
          const chunkSize = sharedMemFileDv.getUint32(8, true);
          // Reset
          sharedMemFileDv.setUint32(12, 0, true);
          sharedMemFileDv.setUint32(16, 0, true);
          const chunksPtr = sharedMemFileDv.getUint32(24, true); // pointers seem to be different ?
          await new Promise((resolve) => setTimeout(resolve, 1));
          // now do the fetching
          for (let chunk = startChunk; chunk <= endChunk; chunk++) {
            await new Promise((resolve) => setTimeout(resolve, 1));
            const chunkPtr = chunk * 3 * 4 + chunksPtr;
            const chunkDv = new DataView(heap, chunkPtr, 3 * 4);
            const dataPtr = chunkDv.getUint32(8, true);
            // now fetch the chunk and store it
            const readin = await fileHandle.read(
              heapU8,
              dataPtr,
              chunkSize,
              chunk * chunkSize
            );
            // now update read
            Atomics.store(heapI32, chunkPtr / 4, chunkSize);
            Atomics.notify(heapI32, chunkPtr / 4);
          }
        }
      }
    } catch (error) {
      console.log("error:", error);
      await new Promise((resolve) => setTimeout(resolve, 1)); // important to not block console.log by a busy loop
    }
  }
};

parentPort.on("message", (message) => {
  switch (message.task) {
    case "init":
      {
        heap = message.heap;
        heapU8 = new Uint8Array(heap);
        heapI32 = new Int32Array(heap);
        sharedMemMain = message.sharedMemMain;
        sharedMemMainDv = new DataView(heap, sharedMemMain, 5);
        // TODO init task queue
        try {
          fetchLoop().catch((error) =>
            console.log("Error launching main:", error)
          );
        } catch (error) {
          console.log("mainFunc error", error);
        }
      }
      break;
    case "addFile":
      {
        const sharedMemFile = message.sharedMemFile;
        const sharedMemFileDv = new DataView(
          heap,
          sharedMemFile,
          8 + 4 + 4 + 4 + 4 + 4 + 4
        );
        const fileId = sharedMemFileDv.getUint32(4, true);
        files[fileId] = {
          fileId,
          sharedMemFile,
          sharedMemFileDv,
          fileName: message.fileName,
          fileHandle: undefined,
        };
      }
      break;
  }
});
