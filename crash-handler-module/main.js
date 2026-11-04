"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
const crash_handler = require('./crash-handler.node');
const net = require('net');
const fs = require('fs');

let socket_name = '';

function tryConnect(buffer, attempt = 5, waitMs = 100) {
  try {
    if (process.platform === "win32") {
      const socket = net.createConnection({
        path: socket_name,
        readable: false,
        writable: true });
      socket.on('connect', function () {
        socket.write(buffer);
      });
      socket.on('error', function () {
        if (attempt > 0) {
          setTimeout(() => {
            tryConnect(buffer, attempt - 1, waitMs * 2);
          }, waitMs);
        }
      });
    } else {
      if (fs.existsSync(socket_name)) {
        fs.writeFileSync(socket_name, buffer);
      } else {
        throw "crash handler pipe not exists";
      }
    }
  } catch (error) {
    if (attempt > 0) {
      setTimeout(() => {
        tryConnect(buffer, attempt - 1, waitMs * 2);
      }, waitMs);
    }
  }
}

function registerProcess(pid, isCritial = false) {
    console.log('[crash-handler] Register process ' + pid);
    const buffer = new Buffer.alloc(6);
    let offset = 0;
    buffer.writeUInt8(0, offset++);
    buffer.writeUInt8(isCritial, offset++);
    buffer.writeUInt32LE(pid, offset++);

    tryConnect(buffer);
}

function unregisterProcess(pid) {
    console.log('[crash-handler] Unregister process' + pid);
    const buffer = new Buffer.alloc(5);
    let offset = 0;
    buffer.writeUInt8(1, offset++);
    buffer.writeUInt32LE(pid, offset++);
    
    tryConnect(buffer);
}

// Synchronous so the message is delivered even if the caller exits right after.
function markShutdownIntent(pid) {
    console.log('[crash-handler] Mark shutdown intent ' + pid);
    const buffer = new Buffer.alloc(5);
    let offset = 0;
    buffer.writeUInt8(7, offset++);
    buffer.writeUInt32LE(pid, offset++);

    const attempts = 5;
    for (let i = 0; i < attempts; i++) {
      try {
        const fd = fs.openSync(socket_name, 'r+');
        try {
          fs.writeSync(fd, buffer);
        } finally {
          fs.closeSync(fd);
        }
        return true;
      } catch (error) {
        if (i < attempts - 1) {
          Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 10);
        }
      }
    }
    return false;
}

function startCrashHandler(workingDirectory, version, isDevEnv, cachePath = "", socket_prefix = "") {
    console.log('[crash-handler] Spawn crash handler');
    const { spawn } = require('child_process');

    if (socket_prefix.length > 0 ) {
      socket_name = process.platform === "win32" ?
      '\\\\.\\pipe\\'+socket_prefix+'-crash-handler' :
      '/tmp/'+socket_prefix+'-crash-handler'
    } else {
      socket_name = process.platform === "win32" ?
      '\\\\.\\pipe\\slobs-crash-handler' :
      '/tmp/slobs-crash-handler'
    }

    try {
      fs.unlinkSync(socket_name);
    } catch (error) {}

    const processPath = workingDirectory.replace('app.asar', 'app.asar.unpacked') +
    '/node_modules/crash-handler';

    let spawnArguments = [workingDirectory, version, isDevEnv];

    if ( cachePath.length > 0 ) 
      spawnArguments.push( cachePath );

    spawnArguments.push( socket_name );

    const subprocess = spawn(processPath +
      '/crash-handler-process', spawnArguments, {
      detached: true,
      stdio: 'ignore'
    });

    subprocess.on('error', (error) => {
      console.log('Error spawning');
      console.log('Error : ' + error)
    });

    subprocess.unref();
}

exports.startCrashHandler = startCrashHandler;
exports.registerProcess = registerProcess;
exports.unregisterProcess = unregisterProcess;
exports.markShutdownIntent = markShutdownIntent;

exports = crash_handler;