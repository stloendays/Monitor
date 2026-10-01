import assert from 'node:assert/strict';
import path from 'node:path';
import test from 'node:test';

import { loadConfig, runtimeArgs } from '../src/config.mjs';

test('Windows executable overrides use stable environment variable names', () => {
  const config = loadConfig({
    LOCALAPPDATA: 'C:\\Users\\Tony\\AppData\\Local',
    MONITOR_HUB_CLI_EXE: 'D:\\bin\\cli.exe',
    MONITOR_HUB_CONTROL_EXE: 'D:\\bin\\control.exe',
    MONITOR_HUB_ORCHESTRATOR_EXE: 'D:\\bin\\orchestrator.exe'
  }, 'win32');

  assert.equal(config.cli, 'D:\\bin\\cli.exe');
  assert.equal(config.control, 'D:\\bin\\control.exe');
  assert.equal(config.orchestrator, 'D:\\bin\\orchestrator.exe');
});

test('Windows default hub data follows Monitor Hub per-user data path', () => {
  const local = 'C:\\Users\\Tony\\AppData\\Local';
  const config = loadConfig({ LOCALAPPDATA: local }, 'win32');
  assert.equal(config.hubData, path.join(local, 'Monitor Hub'));
});

test('no-discovery is only forwarded when the target CLI supports it', () => {
  const config = {
    registry: 'registry.json',
    hubData: 'hubdata',
    jobRoot: 'jobs',
    noDiscovery: true
  };
  assert.deepEqual(runtimeArgs(config), [
    '--registry', 'registry.json',
    '--hub-data', 'hubdata',
    '--job-root', 'jobs'
  ]);
  assert.deepEqual(runtimeArgs(config, { includeNoDiscovery: true }), [
    '--registry', 'registry.json',
    '--hub-data', 'hubdata',
    '--job-root', 'jobs',
    '--no-discovery'
  ]);
});
