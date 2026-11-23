// Tests for crash-handler-module/main.js (the JS client Electron main uses to talk to
// crash-handler-process). Run with: node --test tests/crash-handler-module.test.js
//
// main.js is loaded unchanged. Two things are stubbed so no native build or real
// crash-handler process is needed:
//   - './crash-handler.node' (the native addon, which main.js loads but never uses)
//   - child_process.spawn (so startCrashHandler() sets the pipe name without launching anything)
// A Node pipe server stands in for crash-handler-process.

'use strict';

const { test, describe, before, after } = require('node:test');
const assert = require('node:assert');
const Module = require('node:module');
const net = require('node:net');
const path = require('node:path');
const { EventEmitter } = require('node:events');

const isWindows = process.platform === 'win32';
const MAIN_JS = path.join(__dirname, '..', 'crash-handler-module', 'main.js');

let crashHandler;
let pipeCounter = 0;

function loadMainJs() {
  const originalLoad = Module._load;
  Module._load = function (request, parent, isMain) {
    if (request === './crash-handler.node') return {};
    if (request === 'child_process') {
      return {
        spawn() {
          const fake = new EventEmitter();
          fake.unref = () => {};
          return fake;
        },
      };
    }
    return originalLoad.call(this, request, parent, isMain);
  };
  try {
    delete require.cache[require.resolve(MAIN_JS)];
    return require(MAIN_JS);
  } finally {
    Module._load = originalLoad;
  }
}

// Points main.js at a fresh, unique pipe and returns that pipe's full path.
function usePipe() {
  const prefix = `crash-handler-js-test-${process.pid}-${++pipeCounter}`;
  crashHandler.startCrashHandler('C:\\fake\\app.asar', '0.0.0', 'false', '', prefix);
  return `\\\\.\\pipe\\${prefix}-crash-handler`;
}

// A stand-in for crash-handler-process that records what each client connection sent
// and whether the client closed its end.
function startServer(pipePath, { onData } = {}) {
  const connections = [];
  const sockets = new Set();
  const server = net.createServer(socket => {
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    const conn = { data: Buffer.alloc(0), clientClosed: false };
    connections.push(conn);
    socket.on('data', chunk => {
      conn.data = Buffer.concat([conn.data, chunk]);
      if (onData) onData(socket, conn);
    });
    socket.on('end', () => {
      conn.clientClosed = true;
    });
    socket.on('error', () => {});
  });
  server.sockets = sockets;
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(pipePath, () => resolve({ server, connections }));
  });
}

// Force-closes any connection a client left open, so a regression fails the test
// instead of hanging it (server.close() waits for every connection to end).
function closeServer(server) {
  for (const socket of server.sockets) socket.destroy();
  return new Promise(resolve => server.close(() => resolve()));
}

// Polls until check() is true or the timeout passes. Returns the final check() result.
async function waitFor(check, timeoutMs = 3000) {
  const deadline = Date.now() + timeoutMs;
  while (!check() && Date.now() < deadline) {
    await new Promise(r => setTimeout(r, 20));
  }
  return check();
}

const sleep = ms => new Promise(r => setTimeout(r, ms));

function registerBytes(pid, critical) {
  const b = Buffer.alloc(6);
  b.writeUInt8(0, 0);
  b.writeUInt8(critical ? 1 : 0, 1);
  b.writeUInt32LE(pid, 2);
  return b;
}

function unregisterBytes(pid) {
  const b = Buffer.alloc(5);
  b.writeUInt8(1, 0);
  b.writeUInt32LE(pid, 1);
  return b;
}

describe('crash-handler-module main.js', { skip: !isWindows && 'named pipe client is Windows-only', timeout: 15000 }, () => {
  let originalLog;

  before(() => {
    originalLog = console.log;
    console.log = () => {}; // main.js logs every call
    crashHandler = loadMainJs();
  });

  after(() => {
    console.log = originalLog;
  });

  test('sends a complete register and unregister message', async () => {
    const pipePath = usePipe();
    const { server, connections } = await startServer(pipePath);
    try {
      crashHandler.registerProcess(1234, true);
      crashHandler.unregisterProcess(5678);

      assert.ok(await waitFor(() => connections.length === 2 && connections.every(c => c.clientClosed)),
        'both messages should arrive and their connections close');

      const received = connections.map(c => c.data.toString('hex')).sort();
      const expected = [registerBytes(1234, true), unregisterBytes(5678)].map(b => b.toString('hex')).sort();
      assert.deepStrictEqual(received, expected);
    } finally {
      await closeServer(server);
    }
  });

  test('closes its connection after writing the message', async () => {
    const pipePath = usePipe();
    const { server, connections } = await startServer(pipePath);
    try {
      crashHandler.registerProcess(42, false);

      assert.ok(await waitFor(() => connections.length === 1 && connections[0].data.length === 6),
        'message should arrive');
      // The crash handler only frees a pipe instance once the client closes its end.
      assert.ok(await waitFor(() => connections[0].clientClosed),
        'client should close its connection once the message is written');
    } finally {
      await closeServer(server);
    }
  });

  test('leaves no connections open after many messages', async () => {
    const pipePath = usePipe();
    const { server, connections } = await startServer(pipePath);
    const count = 20; // well above the crash handler's 8 pipe instances
    try {
      for (let i = 0; i < count; i++) crashHandler.registerProcess(1000 + i, false);

      assert.ok(await waitFor(() => connections.length === count && connections.every(c => c.clientClosed)),
        `all ${count} connections should close (still open: ${connections.filter(c => !c.clientClosed).length})`);
      assert.ok(connections.every(c => c.data.length === 6), 'every message should arrive complete');
    } finally {
      await closeServer(server);
    }
  });

  test('retries until the pipe exists and delivers exactly once', async () => {
    // At startup Electron main registers right after launching crash-handler-process,
    // before its pipe exists. The first attempts fail and must be retried.
    const pipePath = usePipe();
    crashHandler.registerProcess(777, false);

    await sleep(150);
    const { server, connections } = await startServer(pipePath);
    try {
      assert.ok(await waitFor(() => connections.length >= 1 && connections[0].clientClosed),
        'message should arrive once the pipe exists');
      await sleep(1000); // give any stray retry time to show up
      assert.strictEqual(connections.length, 1, 'message should be delivered exactly once');
      assert.deepStrictEqual(connections[0].data, registerBytes(777, false));
    } finally {
      await closeServer(server);
    }
  });

  test('does not resend a message the server already received', async () => {
    // The 'error' handler retries on any error. A retry after a successful delivery would
    // send the message twice; a late duplicate "register" arriving after an "unregister"
    // would make the crash handler watch that process again.
    const pipePath = usePipe();
    const { server, connections } = await startServer(pipePath, {
      // Like crash-handler-process cleaning up: drop the connection once the message is read.
      onData: (socket, conn) => {
        if (conn.data.length >= 6) socket.destroy();
      },
    });
    try {
      crashHandler.registerProcess(999, false);

      assert.ok(await waitFor(() => connections.length >= 1 && connections[0].data.length === 6),
        'message should arrive');
      await sleep(1500); // the retry back-off starts at 100ms, so a retry would show up by now
      assert.strictEqual(connections.length, 1, 'message should not be sent again');
    } finally {
      await closeServer(server);
    }
  });
});
