const Module = require("./wasmfs_squashfs.js");
const fs = require("fs");
const fsprom = require("fs/promises");
const { Worker } = require("worker_threads");

Module.onRuntimeInitialized = async () => {
  let backend;

  try {
    const sharedMemMain = Module._malloc(5); // 5 bytes
    const sharedMemMainDv = new DataView(
      Module.HEAPU8.buffer,
      sharedMemMain,
      5
    );
    sharedMemMainDv.setUint32(0, 0, true); // nextFileIdToGet
    sharedMemMainDv.setUint8(4, 1, true); // should be true for the test, crossOriginIsolated
    let curId = 1; // firstId

    const sharedMemFile = Module._malloc(8 + 4 + 4 + 4 + 4 + 4 + 4);
    const sharedMemFileDv = new DataView(
      Module.HEAPU8.buffer,
      sharedMemFile,
      8 + 4 + 4 + 4 + 4 + 4 + 4
    );
    sharedMemFileDv.setUint32(0, 0, true); // fileSize
    sharedMemFileDv.setUint32(4, curId, true); // fileId
    sharedMemFileDv.setUint32(8, 0, true); // chunksize
    sharedMemFileDv.setUint32(12, 0, true); // tiggertChunkStart
    sharedMemFileDv.setUint32(16, 0, true); // tiggertChunkEnd
    sharedMemFileDv.setUint32(20, sharedMemMain, true); // mainStruct
    sharedMemFileDv.setUint32(24, 0, true); // chunks, not allocated yet

    const worker = new Worker("./wasmfs_test_sharedmem_worker.js");

    worker.postMessage({
      task: "init",
      heap: Module.HEAPU8.buffer,
      sharedMemMain,
    });

    worker.postMessage({
      task: "addFile",
      sharedMemFile,
      fileName: Module.sqshfsName,
    });
    console.log(
      "Create backend from",
      Module.sqshfsName,
      "using a callback into node js..."
    );
    await new Promise((resolve) => setTimeout(resolve, 1000)); // TODO wait for worker startuo
    backend = Module._wasmfs_create_squashfs_backend_memfile(sharedMemFile);
  } catch (error) {
    console.log("Problem setting up, fs for", Module.sqshfsName, ":", error);
  }
  try {
    await Module.testBackend(backend, Module.mountPoint);
  } catch (error) {
    console.log("Problem in test", error);
    return null;
  }
  process.exit(0);
};
