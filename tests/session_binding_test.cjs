'use strict';

const assert = require('assert');
const {
  createSessionBinding,
  createSpawnedSessionBinding,
  publicSessionIdentity,
  selectTargetSession,
  validateResidentPing,
} = require('../mcp/session-binding.cjs');

const first = {
  process_id: 101,
  connect_id: 'host:a:rpcnum:1:netaddr:127.0.0.1',
  display: 'a',
  user: 'tester',
};
const second = {
  process_id: 202,
  connect_id: 'host:a:rpcnum:2:netaddr:127.0.0.1',
  display: 'a',
  user: 'tester',
};

const firstBinding = createSessionBinding(first);
const firstBindingAgain = createSessionBinding(first);
const secondBinding = createSessionBinding(second);
assert.deepStrictEqual(firstBinding, firstBindingAgain);
assert.notStrictEqual(firstBinding.pipe_name, secondBinding.pipe_name);
assert.notStrictEqual(firstBinding.session_nonce, secondBinding.session_nonce);
assert.match(firstBinding.pipe_name, /^\\\\\.\\pipe\\codex_creo_101_[a-f0-9]{24}$/);

const spawnedBinding = createSpawnedSessionBinding(first, {
  target_creo_process_id: 101,
  session_nonce: 'a'.repeat(64),
  session_fingerprint: 'b'.repeat(32),
  pipe_name: `\\\\.\\pipe\\codex_creo_spawn_101_${'b'.repeat(24)}`,
});
assert.strictEqual(spawnedBinding.target_process_id, 101);
assert.strictEqual(spawnedBinding.selection_policy, 'creo_startup_spawn');
assert.throws(() => createSpawnedSessionBinding(second, {
  target_creo_process_id: 101,
  session_nonce: 'a'.repeat(64),
  session_fingerprint: 'b'.repeat(32),
  pipe_name: `\\\\.\\pipe\\codex_creo_spawn_101_${'b'.repeat(24)}`,
}), /another Creo process/);

assert.strictEqual(
  selectTargetSession([first, second], firstBinding).process_id, 101);
assert.strictEqual(
  selectTargetSession([first, second], null, '202').process_id, 202);
assert.strictEqual(
  selectTargetSession([first, second], null).process_id, 202);

const ping = {
  ok: true,
  persistent: true,
  connected: true,
  session_bound: true,
  target_creo_process_id: 101,
  target_creo_start_time_100ns: '133700000000000000',
  worker_process_id: 303,
  session_nonce: firstBinding.session_nonce,
  session_fingerprint: firstBinding.session_fingerprint,
};
assert.strictEqual(validateResidentPing(ping, firstBinding), ping);
assert.throws(
  () => validateResidentPing({ ...ping, target_creo_process_id: 202 }, firstBinding),
  /expected 101/);
assert.throws(
  () => validateResidentPing({ ...ping, session_nonce: 'wrong' }, firstBinding),
  /nonce or fingerprint/);

const identity = publicSessionIdentity(firstBinding, ping);
assert.strictEqual(identity.target_creo_process_id, 101);
assert.strictEqual(identity.worker_process_id, 303);
assert.strictEqual(identity.pipe_scope, 'one_pipe_per_exact_creo_session');

process.stdout.write(JSON.stringify({
  ok: true,
  tests: 17,
  protocol: identity.protocol,
  pipe_scope: identity.pipe_scope,
}, null, 2) + '\n');
