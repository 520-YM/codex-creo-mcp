'use strict';

const fs = require('fs');
const path = require('path');
const readline = require('readline');
const { spawn } = require('child_process');

const serverPath = path.join(__dirname, '..', 'mcp', 'server.cjs');
const source = fs.readFileSync(serverPath, 'utf8');
const forbiddenSourceTokens = [
  "process.env.CREO_PROJECT_ROOT",
  "resolveProjectDirectory(",
  "resolveProjectModelFile(",
  "E:\\\\project",
];
const sourceViolations = forbiddenSourceTokens.filter((token) => source.includes(token));

const child = spawn(process.execPath, [serverPath], {
  cwd: __dirname,
  windowsHide: true,
  stdio: ['pipe', 'pipe', 'pipe'],
});
const output = readline.createInterface({ input: child.stdout, crlfDelay: Infinity });
const timeout = setTimeout(() => {
  child.kill();
  throw new Error('Working-directory policy test timed out.');
}, 10000);

function send(id, method, params) {
  child.stdin.write(`${JSON.stringify({ jsonrpc: '2.0', id, method, params })}\n`);
}

output.on('line', (line) => {
  const response = JSON.parse(line);
  if (response.id === 1) {
    child.stdin.write(`${JSON.stringify({
      jsonrpc: '2.0', method: 'notifications/initialized',
    })}\n`);
    send(2, 'tools/list', {});
    return;
  }
  if (response.id !== 2) return;

  clearTimeout(timeout);
  const tools = response.result.tools;
  const schemaViolations = tools
    .filter((tool) => Object.prototype.hasOwnProperty.call(
      tool.inputSchema?.properties || {}, 'project_name'))
    .map((tool) => tool.name);
  const ok = sourceViolations.length === 0 && schemaViolations.length === 0;
  console.log(JSON.stringify({
    ok,
    tool_count: tools.length,
    source_violations: sourceViolations,
    project_name_schema_violations: schemaViolations,
    directory_policy: 'current_creo_session_only',
  }, null, 2));
  child.stdin.end();
  if (!ok) process.exitCode = 1;
});

child.stderr.on('data', (chunk) => process.stderr.write(chunk));
send(1, 'initialize', {
  protocolVersion: '2025-06-18',
  capabilities: {},
  clientInfo: { name: 'working-directory-policy-test', version: '1.0.0' },
});
