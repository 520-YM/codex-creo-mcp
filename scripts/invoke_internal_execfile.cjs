'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');

const [, , processIdText, executable, ...args] = process.argv;
const processId = Number(processIdText);
if (!Number.isInteger(processId) || processId < 1 ||
    !/^[A-Za-z0-9_.-]+\.exe$/i.test(executable || '')) {
  process.stderr.write(
    'Usage: node invoke_internal_execfile.cjs <creo_pid> <command.exe> [args...]\n');
  process.exit(2);
}
if (args.some((value) => /[\r\n]/.test(value))) {
  throw new Error('Command arguments cannot contain newlines.');
}

const runtimeDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'creo-direct-'));
const resultPath = path.join(runtimeDirectory, 'result.json');
const commandPath = path.join(runtimeDirectory, 'command.txt');
const pipeName = `\\\\.\\pipe\\codex_creo_internal_v11_${processId}`;
const started = process.hrtime.bigint();
let descriptor;

try {
  fs.writeFileSync(
    commandPath,
    [executable, resultPath, ...args].join('\n'),
    'utf8');
  descriptor = fs.openSync(pipeName, 'r+');
  fs.writeSync(descriptor, Buffer.from(`EXECFILE|${commandPath}\n`, 'utf8'));
  const responseBuffer = Buffer.alloc(8192);
  const bytesRead = fs.readSync(
    descriptor, responseBuffer, 0, responseBuffer.length, null);
  const response = JSON.parse(
    responseBuffer.subarray(0, bytesRead).toString('utf8').trim());
  const result = fs.existsSync(resultPath)
    ? JSON.parse(fs.readFileSync(resultPath, 'utf8').replace(/^\uFEFF/, ''))
    : null;
  const elapsedMs = Number(process.hrtime.bigint() - started) / 1e6;
  process.stdout.write(`${JSON.stringify({
    ok: response.ok === true && response.exit_code === 0 && result?.ok === true,
    elapsed_ms: Math.round(elapsedMs * 10) / 10,
    resident_response: response,
    result,
  })}\n`);
  process.exitCode = response.ok === true && response.exit_code === 0 && result?.ok === true
    ? 0 : 1;
} finally {
  if (descriptor !== undefined) fs.closeSync(descriptor);
  fs.rmSync(runtimeDirectory, { recursive: true, force: true });
}
