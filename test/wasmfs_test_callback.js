const Module = require('./wasmfs_squashfs.js');
const fs = require('fs');
const fsprom = require('fs/promises');


Module.onRuntimeInitialized = async () => {
    const props = {};
    try {
        let stats = fs.statSync(Module.sqshfsName);
        props.size = stats.size;
        const fileHandle = fsprom.open(Module.sqshfsName, 'r');
        props.callback = async (offset, buffer, size) => {
            const handle = await fileHandle;
            try {
                await handle.read(Module.HEAPU8, buffer, size, offset);
            }
            catch (error) {
                console.log('Problem reading ', Module.sqshfsName, 'with error', error);
                return -2; // SQFS IO ERROR
            }
            return 0;
        }
    }
    catch (error) {
        console.log('Problem setting up, fs for', Module.sqshfsName, ":", error);
    }
    try {
        console.log("Create backend from", Module.sqshfsName, "using a callback into node js...");
        const backend = await Module.wasmfs_create_squashfs_backend_callback(props);
        await Module.testBackend(backend, Module.mountPoint);
        console.log('Test readDirAsync:', await Module.readDirAsync(Module.mountPoint));
        console.log('Test findObjectAsync:', await Module.findObjectAsync(Module.mountPoint + "/Folder2"));
        console.log('Test findObjectAsync2:', await Module.findObjectAsync(Module.mountPoint + "/roottest1.txt"));
        console.log('Test findObjectAsync2:', await Module.findObjectAsync(Module.mountPoint + "/roottest1.txt"));
        console.log('Test readFileSignAsync:', await Module.readFileSignAsync(Module.mountPoint + "/roottest1.txt"));
    } catch (error) {
        console.log('Problem in test', error);
        return null;
    }
};