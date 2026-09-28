'use strict';

const crypto = require('crypto');

const SESSION_PROTOCOL = 'codex-creo-session-v2';

function ensureProcessId(value, label = 'Creo process id') {
  const processId = Number(value);
  if (!Number.isInteger(processId) || processId < 1) {
    throw new Error(`${label} must be a positive integer.`);
  }
  return processId;
}

function createSessionBinding(session) {
  if (!session || typeof session !== 'object') {
    throw new Error('A discovered Creo session is required.');
  }
  const processId = ensureProcessId(session.process_id);
  if (typeof session.connect_id !== 'string' || session.connect_id.length < 8) {
    throw new Error('The discovered Creo session has no exact PTC connection id.');
  }
  const display = typeof session.display === 'string' ? session.display : '';
  const nonce = crypto.createHash('sha256')
    .update(SESSION_PROTOCOL)
    .update('\0')
    .update(String(processId))
    .update('\0')
    .update(session.connect_id)
    .update('\0')
    .update(display)
    .digest('hex');
  const shortIdentity = nonce.slice(0, 24);
  return {
    protocol: SESSION_PROTOCOL,
    target_process_id: processId,
    connect_id: session.connect_id,
    display,
    user: typeof session.user === 'string' ? session.user : '',
    session_nonce: nonce,
    session_fingerprint: nonce.slice(0, 32),
    pipe_name: `\\\\.\\pipe\\codex_creo_${processId}_${shortIdentity}`,
    mutex_name: `Local\\CodexCreoResident_${processId}_${shortIdentity}`,
    selection_policy: session.selection_policy || 'single_or_latest_registered_session',
  };
}

function createSpawnedSessionBinding(session, record) {
  if (!record || typeof record !== 'object') {
    throw new Error('A Creo startup-spawn resident registry is required.');
  }
  const processId = ensureProcessId(session.process_id);
  if (Number(record.target_creo_process_id) !== processId) {
    throw new Error('Startup-spawn resident registry targets another Creo process.');
  }
  if (typeof record.session_nonce !== 'string' ||
      !/^[a-f0-9]{64}$/i.test(record.session_nonce) ||
      typeof record.session_fingerprint !== 'string' ||
      !/^[a-f0-9]{32}$/i.test(record.session_fingerprint)) {
    throw new Error('Startup-spawn resident registry has an invalid identity token.');
  }
  const expectedPipePrefix = `\\\\.\\pipe\\codex_creo_spawn_${processId}_`;
  if (typeof record.pipe_name !== 'string' ||
      !record.pipe_name.startsWith(expectedPipePrefix) ||
      !/^[A-Za-z0-9_.\\-]+$/.test(record.pipe_name)) {
    throw new Error('Startup-spawn resident registry has an invalid pipe name.');
  }
  return {
    protocol: SESSION_PROTOCOL,
    target_process_id: processId,
    connect_id: session.connect_id,
    display: typeof session.display === 'string' ? session.display : '',
    user: typeof session.user === 'string' ? session.user : '',
    session_nonce: record.session_nonce.toLowerCase(),
    session_fingerprint: record.session_fingerprint.toLowerCase(),
    pipe_name: record.pipe_name,
    mutex_name: null,
    selection_policy: 'creo_startup_spawn',
    registry_path: record.registry_path || null,
  };
}

function selectTargetSession(sessions, activeBinding, explicitProcessId) {
  if (!Array.isArray(sessions) || sessions.length === 0) {
    throw new Error(
      'No live Creo Parametric session is registered with the PTC name server.');
  }
  const valid = sessions.filter((session) =>
    Number.isInteger(Number(session.process_id)) && Number(session.process_id) > 0 &&
    typeof session.connect_id === 'string' && session.connect_id.length >= 8);
  if (valid.length === 0) {
    throw new Error('PTC name server returned no usable Creo session identity.');
  }

  if (explicitProcessId !== undefined && explicitProcessId !== null &&
      String(explicitProcessId).trim() !== '') {
    const targetProcessId = ensureProcessId(
      explicitProcessId, 'CREO_TARGET_PROCESS_ID');
    const explicit = valid.find(
      (session) => Number(session.process_id) === targetProcessId);
    if (!explicit) {
      throw new Error(
        `CREO_TARGET_PROCESS_ID ${targetProcessId} is not a registered Creo session.`);
    }
    return { ...explicit, selection_policy: 'explicit_process_id' };
  }

  if (activeBinding) {
    const active = valid.find((session) =>
      Number(session.process_id) === activeBinding.target_process_id &&
      session.connect_id === activeBinding.connect_id);
    if (active) {
      return { ...active, selection_policy: 'verified_active_session' };
    }
  }

  if (valid.length === 1) {
    return { ...valid[0], selection_policy: 'only_registered_session' };
  }

  const sorted = [...valid].sort(
    (left, right) => Number(right.process_id) - Number(left.process_id));
  return { ...sorted[0], selection_policy: 'latest_registered_process_id' };
}

function validateResidentPing(ping, binding) {
  if (!ping || ping.ok !== true || ping.persistent !== true ||
      ping.connected !== true || ping.session_bound !== true) {
    throw new Error('Resident bridge did not report a healthy bound Creo session.');
  }
  if (Number(ping.target_creo_process_id) !== binding.target_process_id) {
    throw new Error(
      `Resident bridge is bound to Creo PID ${ping.target_creo_process_id}, ` +
      `expected ${binding.target_process_id}.`);
  }
  if (ping.session_nonce !== binding.session_nonce ||
      ping.session_fingerprint !== binding.session_fingerprint) {
    throw new Error('Resident bridge session nonce or fingerprint does not match.');
  }
  if (!Number.isInteger(Number(ping.worker_process_id)) ||
      Number(ping.worker_process_id) < 1) {
    throw new Error('Resident bridge did not return a valid worker process id.');
  }
  if (ping.target_creo_start_time_100ns !== null &&
      !/^\d+$/.test(String(ping.target_creo_start_time_100ns))) {
    throw new Error('Resident bridge returned an invalid Creo process start time.');
  }
  return ping;
}

function publicSessionIdentity(binding, ping) {
  return {
    protocol: binding.protocol,
    target_creo_process_id: binding.target_process_id,
    target_creo_start_time_100ns:
      ping.target_creo_start_time_100ns ?? null,
    worker_process_id: Number(ping.worker_process_id),
    session_nonce: binding.session_nonce,
    session_fingerprint: binding.session_fingerprint,
    selection_policy: binding.selection_policy,
    pipe_scope: 'one_pipe_per_exact_creo_session',
  };
}

module.exports = {
  SESSION_PROTOCOL,
  createSessionBinding,
  createSpawnedSessionBinding,
  publicSessionIdentity,
  selectTargetSession,
  validateResidentPing,
};
