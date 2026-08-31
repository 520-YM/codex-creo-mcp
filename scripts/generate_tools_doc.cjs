'use strict';

const fs = require('fs');
const path = require('path');
const readline = require('readline');
const { spawn } = require('child_process');

const root = path.resolve(__dirname, '..');
const server = path.join(root, 'mcp', 'server.cjs');
const outputPath = path.join(root, 'docs', 'TOOLS.md');
const child = spawn(process.execPath, [server], {
  cwd: path.dirname(server),
  stdio: ['pipe', 'pipe', 'inherit'],
  windowsHide: true,
});
const lines = readline.createInterface({ input: child.stdout, crlfDelay: Infinity });

function send(id, method, params) {
  child.stdin.write(`${JSON.stringify({ jsonrpc: '2.0', id, method, params })}\n`);
}

lines.on('line', (line) => {
  const message = JSON.parse(line);
  if (message.id === 1) {
    child.stdin.write(`${JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' })}\n`);
    send(2, 'tools/list', {});
    return;
  }
  if (message.id !== 2) return;
  const tools = message.result.tools;
  const body = [
    '# MCP 工具清单',
    '',
    `正式工具数量：**${tools.length}**。`,
    '',
    '所有工具均不得接收 `project_name` 或固定项目目录。涉及模型文件时，目录来源必须是当前 Creo 会话。',
    '',
    ...tools.flatMap((tool, index) => [
      `## ${index + 1}. \`${tool.name}\``,
      '',
      tool.description,
      '',
    ]),
  ].join('\n');
  fs.writeFileSync(outputPath, body, 'utf8');
  console.log(`Wrote ${tools.length} tools to ${outputPath}`);
  child.stdin.end();
});

send(1, 'initialize', {
  protocolVersion: '2025-06-18',
  capabilities: {},
  clientInfo: { name: 'tools-doc-generator', version: '1.0.0' },
});
