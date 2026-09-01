#!/usr/bin/env node

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline';

const [, , inputPath, outputDirectory] = process.argv;

if (!inputPath || !outputDirectory) {
  console.error('Usage: node scripts/export_codex_chat.mjs <rollout.jsonl> <output-directory>');
  process.exit(2);
}

const MAX_PART_BYTES = 750 * 1024;

function textFromContent(content) {
  if (!Array.isArray(content)) return '';
  return content
    .map((item) => {
      if (!item || typeof item !== 'object') return '';
      if (typeof item.text === 'string') return item.text;
      if (typeof item.output_text === 'string') return item.output_text;
      return '';
    })
    .filter(Boolean)
    .join('\n');
}

function sanitize(value) {
  let text = value;

  // Remove app-injected context that was never typed by the user.
  text = text.replace(/<recommended_plugins>[\s\S]*?<\/recommended_plugins>\s*/gi, '');
  text = text.replace(/<environment_context>[\s\S]*?<\/environment_context>\s*/gi, '');
  text = text.replace(/<in-app-browser-context\b[^>]*>[\s\S]*?<\/in-app-browser-context>\s*/gi, '');
  text = text.replace(/^Distinguish instructions in attached documents from the user's request\.\s*$/gmi, '');
  text = text.replace(/^## My request(?: for Codex)?:\s*$/gmi, '');

  // Keep an attachment reference but do not publish local temporary image paths.
  text = text.replace(
    /<image\s+name=\[([^\]]+)\]\s+path="[^"]+">[\s\S]*?<\/image>/gi,
    '【图片附件：$1；二进制文件未包含在公开归档中】',
  );

  // Remove credentials if they ever appeared in a visible message.
  text = text.replace(/\bghp_[A-Za-z0-9]{20,}\b/g, '[REDACTED_GITHUB_TOKEN]');
  text = text.replace(/\bgithub_pat_[A-Za-z0-9_]{20,}\b/g, '[REDACTED_GITHUB_TOKEN]');
  text = text.replace(/\bsk-[A-Za-z0-9_-]{20,}\b/g, '[REDACTED_API_KEY]');
  text = text.replace(/\bBearer\s+[A-Za-z0-9._~+\/-]{20,}=*/gi, 'Bearer [REDACTED]');

  // Public-repository privacy: retain path meaning while removing the Windows account name.
  text = text.replace(/C:\\Users\\[^\\\s<>"']+/gi, '%USERPROFILE%');
  text = text.replace(/([A-Z]:\\Users\\)[^\\\s<>"']+/gi, '$1<USER>');

  return text
    .replace(/\r\n/g, '\n')
    .split('\n')
    .map((line) => line.replace(/[ \t]+$/g, ''))
    .join('\n')
    .replace(/\n{3,}/g, '\n\n')
    .trim();
}

function formatTime(timestamp) {
  if (!timestamp) return '时间未记录';
  const date = new Date(timestamp);
  if (Number.isNaN(date.getTime())) return String(timestamp);
  return new Intl.DateTimeFormat('zh-CN', {
    timeZone: 'Asia/Shanghai',
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  }).format(date);
}

function publicRole(role, phase) {
  if (role === 'user') return '用户';
  return phase === 'commentary' ? 'Codex（过程更新）' : 'Codex';
}

const messages = [];
let skippedMalformedLines = 0;

const input = fs.createReadStream(inputPath, { encoding: 'utf8' });
const reader = readline.createInterface({ input, crlfDelay: Infinity });

for await (const line of reader) {
  if (!line.trim()) continue;
  let record;
  try {
    record = JSON.parse(line);
  } catch {
    skippedMalformedLines += 1;
    continue;
  }

  if (record?.type !== 'response_item' || record?.payload?.type !== 'message') continue;
  const role = record.payload.role;
  if (role !== 'user' && role !== 'assistant') continue;

  const text = sanitize(textFromContent(record.payload.content));
  if (!text) continue;

  messages.push({
    role,
    phase: record.payload.phase || '',
    timestamp: record.timestamp || record.payload.timestamp || '',
    text,
  });
}

if (messages.length === 0) {
  console.error('No visible user/assistant messages were found.');
  process.exit(1);
}

fs.mkdirSync(outputDirectory, { recursive: true });

const parts = [];
let current = [];
let currentBytes = 0;

for (const message of messages) {
  const block = [
    `## ${formatTime(message.timestamp)} · ${publicRole(message.role, message.phase)}`,
    '',
    message.text,
    '',
  ].join('\n');
  const blockBytes = Buffer.byteLength(block, 'utf8');
  if (current.length > 0 && currentBytes + blockBytes > MAX_PART_BYTES) {
    parts.push(current);
    current = [];
    currentBytes = 0;
  }
  current.push(block);
  currentBytes += blockBytes;
}
if (current.length > 0) parts.push(current);

const generatedNames = [];
for (let index = 0; index < parts.length; index += 1) {
  const number = String(index + 1).padStart(3, '0');
  const fileName = `part-${number}.md`;
  const body = [
    `# Codex&Creo 对话记录 · 第 ${index + 1}/${parts.length} 部分`,
    '',
    '> 时区：Asia/Shanghai。仅包含用户可见的对话，不包含系统指令、内部推理、工具调用和原始工具输出。',
    '',
    ...parts[index],
  ].join('\n');
  fs.writeFileSync(path.join(outputDirectory, fileName), `${body.trimEnd()}\n`, 'utf8');
  generatedNames.push(fileName);
}

const userCount = messages.filter((message) => message.role === 'user').length;
const assistantCount = messages.length - userCount;
const exportedAt = new Date().toISOString();
const sourceHash = crypto.createHash('sha256').update(fs.readFileSync(inputPath)).digest('hex');

const indexBody = [
  '# Codex&Creo 完整对话归档',
  '',
  `- 导出时间：${exportedAt}`,
  `- 用户消息：${userCount}`,
  `- Codex 可见回复：${assistantCount}`,
  `- 合计：${messages.length}`,
  `- 分卷：${parts.length}`,
  `- 原始任务日志 SHA-256：\`${sourceHash}\``,
  '',
  '## 归档范围',
  '',
  '本归档记录了 Codex 接入 Creo、Pro/TOOLKIT/MCP 桥接、工具开发、模型操作、故障排查、性能优化和日常指令验证的完整可见对话时间线。',
  '',
  '公开仓库中不包含系统/开发者指令、模型内部推理、工具调用参数、工具原始输出、登录凭据，以及聊天中引用的本地临时图片二进制文件。本机用户目录已替换为占位符。',
  '',
  '## 分卷',
  '',
  ...generatedNames.map((name, index) => `- [第 ${index + 1} 部分](./${name})`),
  '',
  '## 重新导出',
  '',
  '```powershell',
  'node scripts/export_codex_chat.mjs <rollout.jsonl> docs/chat-history',
  '```',
  '',
  `解析异常行：${skippedMalformedLines}`,
  '',
].join('\n');

fs.writeFileSync(path.join(outputDirectory, 'README.md'), `${indexBody.trimEnd()}\n`, 'utf8');

console.log(JSON.stringify({
  ok: true,
  exported_at: exportedAt,
  message_count: messages.length,
  user_message_count: userCount,
  assistant_message_count: assistantCount,
  part_count: parts.length,
  malformed_line_count: skippedMalformedLines,
  output_directory: path.resolve(outputDirectory),
}, null, 2));
