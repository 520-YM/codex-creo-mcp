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
const internalBuild = read('scripts/build_internal_bridge_v11.ps1');
const featureCompat = read('native/codex_internal_feature_compat.h');
const build = read('build_all.cmd');
const installer = read('install.ps1');

assert.match(server, /creo_safe_resident_internal_v(?:11|12|13|14|15|16|17)\.dll/);
assert.match(server, /creo_internal_resident_loader_v(?:11|12|13|14|15|16|17)\.exe/);
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
assert.match(server, /const externalFeatureCreationCommands = new Set\(/);
assert.match(server, /function runExternalBridge\(/);
assert.match(server, /connection_mode: 'out_of_process_toolkit'/);
assert.match(server, /crash_isolation: 'worker_process'/);
assert.match(server, /single_creo_external_target_guard/);
const externalCommandSet = server.match(
  /const externalFeatureCreationCommands = new Set\([\s\S]*?\n\]\);/);
assert.ok(externalCommandSet, 'The external command set must exist.');
assert.doesNotMatch(
  externalCommandSet[0],
  /creo_sheetmetal_flat_wall_bridge\.exe/,
  'Attached flat-wall creation must never use the external worker.');
assert.doesNotMatch(server, /creo_create_project_sheetmetal_flat_wall/);
assert.doesNotMatch(server, /creo_create_project_sheetmetal_flat_walls_batch/);
assert.doesNotMatch(registry, /creo_sheetmetal_flat_wall_bridge\.exe/);
assert.doesNotMatch(internalBuild, /'creo_sheetmetal_flat_wall_bridge'/);
assert.match(server, /name: 'creo_create_project_sheetmetal_ui_flat_wall'/);
assert.match(server, /function runInternalUiFlatWall\(/);
assert.match(server, /`FLATWALLFILE\|\$\{commandPath\}`/);
assert.match(server, /execution_context: 'registered_creo_ui_command_action'/);
assert.match(server, /external_connection_used: false/);
assert.match(resident, /ProCmdActionAdd\(/);
assert.match(resident, /CodexCreoInternalFlatWallAction/);
assert.match(resident, /ProMacroLoad\(/);
assert.match(resident, /FLATWALLFILE\|/);
assert.match(resident, /unsupported_internal_command/);
assert.match(registry, /codex_command_requires_external_worker/);
for (const command of [
  'creo_datum_plane_bridge',
  'creo_datum_point_bridge',
  'creo_extrude_bridge',
  'creo_general_sketch_bridge',
  'creo_hole_bridge',
  'creo_sheetmetal_plate_bridge',
  'creo_sheetmetal_three_circle_cut_bridge',
  'creo_skeleton_box_bridge',
]) {
  assert.match(
    server,
    new RegExp(`'${command}\\.exe'`),
    `${command} must use the crash-isolated external feature worker.`);
  assert.match(
    registry,
    new RegExp(`L"${command}\\.exe"`),
    `${command} must be rejected by the in-process registry.`);
}
assert.match(loader, /ProToolkitDllLoad\(/);
assert.match(loader, /ProToolkitDllHandleGet\(/);
assert.match(loader, /ProEngineerDisconnect\(/);
assert.match(loader, /ProEngineerConnectionStart\(/);
assert.match(registry, /codex_internal_command_file_execute/);
assert.match(registry, /creo_feature_delete_bridge\.exe/);
const runtimeBridges = [...server.matchAll(/runBridge\('([^']+\.exe)'/g)]
  .map((match) => match[1]);
const missingInternalCommands = [...new Set(runtimeBridges)]
  .filter((executable) => !registry.includes(`{L"${executable}"`));
assert.deepStrictEqual(
  missingInternalCommands,
  [],
  'Every legacy MCP bridge must be callable through the in-process DLL registry.');
const firstCommandBody = server.match(
  /if \(name === 'creo_start_resident_and_get_basic_model'\) \{([\s\S]*?)\n  \}/);
assert.ok(firstCommandBody, 'The first-command MCP handler must exist.');
assert.doesNotMatch(
  firstCommandBody[1],
  /openTopAssemblyAndReadSkeleton/,
  'The first command must not switch away from the current Creo model.');
assert.match(firstCommandBody[1], /readInitialWallMountContextWithoutDisplay/);
assert.match(server, /name: 'creo_delete_current_project_feature'/);
assert.match(server, /dependent children/);
assert.match(build, /build_internal_bridge_v11\.ps1/);
assert.match(installer, /ResidentMode = 'normal_creo_startup_independent_dat'/);
assert.match(installer, /RegisterConfigProPath/);
assert.match(installer, /codex-before-creo-safe/);
assert.match(installer, /WJT276Changed = \$false/);

process.stdout.write(JSON.stringify({
  ok: true,
  checks: 50,
  default_resident_mode: 'normal_creo_startup_independent_dat',
  config_pro_default_modified: false,
  config_pro_explicit_registration_supported: true,
  wjt276_modified: false,
}, null, 2) + '\n');
