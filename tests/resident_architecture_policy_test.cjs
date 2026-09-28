'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const read = (relative) => fs.readFileSync(path.join(root, relative), 'utf8');
const server = read('mcp/server.cjs');
const resident = read('native/creo_safe_resident_dll.c');
const loader = read('native/creo_internal_resident_loader.c');
const registry = read('native/codex_external_command_registry.c');
const build = read('build_all.cmd');
const installer = read('install.ps1');

assert.match(server, /creo_safe_resident_internal_v(?:11|12|13|14)\.dll/);
assert.match(server, /creo_internal_resident_loader_v(?:11|12|13|14)\.exe/);
assert.match(server, /function ensureInternalCreoBridge\(/);
assert.match(server, /function tryReuseActiveInternalCreoBridge\(/);
assert.match(server, /fast_cached_in_process_dll/);
assert.match(server, /CREO_ALLOW_DYNAMIC_INTERNAL_LOAD/);
assert.match(server, /connection_mode: 'in_process_dll'/);
assert.match(server, /`EXECFILE\|\$\{commandPath\}`/);
assert.strictEqual(
  (server.match(/ensurePersistentFlatWallBridge\(/g) || []).length,
  1,
  'The external resident function may remain for rollback, but no tool may call it.');
assert.ok(
  (server.match(/ensureInternalCreoBridge\(/g) || []).length >= 7,
  'All generic and specialized runtime paths must enter the internal bridge.');

assert.match(resident, /codex_creo_internal_v11_%lu/);
assert.match(resident, /EXECFILE\|/);
assert.match(resident, /codex_internal_command_file_execute/);
assert.match(resident, /transport\\\":\\\"in_process_dll/);
assert.match(resident, /target_creo_start_time_100ns/);
assert.match(resident, /GetProcessTimes\(/);
assert.match(resident, /PostMessageW\(/);
assert.match(resident, /WM_CODEX_INTERNAL_REQUEST/);
assert.doesNotMatch(resident, /ProUIDialogTimerStart\(/);
assert.doesNotMatch(resident, /resident_timer_action/);
assert.match(loader, /ProToolkitDllLoad\(/);
assert.match(loader, /ProToolkitDllHandleGet\(/);
assert.match(loader, /ProEngineerDisconnect\(/);
assert.match(loader, /ProEngineerConnectionStart\(/);
assert.match(registry, /codex_internal_command_file_execute/);
assert.match(build, /build_internal_bridge_v11\.ps1/);
assert.match(installer, /ResidentMode = 'normal_creo_startup_independent_dat'/);
assert.match(installer, /RegisterConfigProPath/);
assert.match(installer, /codex-before-creo-safe/);
assert.match(installer, /WJT276Changed = \$false/);

process.stdout.write(JSON.stringify({
  ok: true,
  checks: 29,
  default_resident_mode: 'normal_creo_startup_independent_dat',
  config_pro_default_modified: false,
  config_pro_explicit_registration_supported: true,
  wjt276_modified: false,
}, null, 2) + '\n');
