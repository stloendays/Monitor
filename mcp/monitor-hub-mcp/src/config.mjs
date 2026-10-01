import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const EXECUTABLE_ENV = {
  monitor_hub_cli: 'MONITOR_HUB_CLI_EXE',
  monitor_hub_control: 'MONITOR_HUB_CONTROL_EXE',
  monitor_hub_orchestrator: 'MONITOR_HUB_ORCHESTRATOR_EXE'
};

function firstExisting(candidates) {
  for (const candidate of candidates) {
    if (candidate && fs.existsSync(candidate)) return candidate;
  }
  return candidates.find(Boolean) ?? '';
}

function resolveProgram(name, env, platform) {
  const envKey = EXECUTABLE_ENV[name];
  const explicit = envKey ? env[envKey] : '';
  if (explicit) return explicit;

  if (platform === 'win32') {
    const localAppData = env.LOCALAPPDATA ||
      (env.USERPROFILE ? path.join(env.USERPROFILE, 'AppData', 'Local') : '');
    const installBin = env.MONITOR_HUB_BIN_DIR ||
      (localAppData ? path.join(localAppData, 'Programs', 'Monitor Hub', 'bin') : '');
    const exeName = `${name}.exe`;
    return firstExisting([
      installBin ? path.join(installBin, exeName) : '',
      exeName
    ]);
  }

  return name;
}

export function loadConfig(env = process.env, platform = process.platform) {
  const isWindows = platform === 'win32';
  const localAppData = env.LOCALAPPDATA ||
    (env.USERPROFILE ? path.join(env.USERPROFILE, 'AppData', 'Local') : '') ||
    path.join(os.homedir(), '.local', 'share');

  const defaultHubData = isWindows
    ? path.join(localAppData, 'Monitor Hub')
    : (env.XDG_DATA_HOME
        ? path.join(env.XDG_DATA_HOME, 'Monitor Hub')
        : path.join(os.homedir(), '.local', 'share', 'Monitor Hub'));

  return {
    cli: resolveProgram('monitor_hub_cli', env, platform),
    control: resolveProgram('monitor_hub_control', env, platform),
    orchestrator: resolveProgram('monitor_hub_orchestrator', env, platform),
    registry: env.MONITOR_HUB_REGISTRY || '',
    hubData: env.MONITOR_HUB_DATA || defaultHubData,
    jobRoot: env.MONITOR_HUB_JOB_ROOT || '',
    noDiscovery: env.MONITOR_HUB_NO_DISCOVERY === '1',
    timeoutMs: Number(env.MONITOR_HUB_MCP_TIMEOUT_MS || 20000),
    maxOutputBytes: Number(env.MONITOR_HUB_MCP_MAX_OUTPUT_BYTES || 8 * 1024 * 1024)
  };
}

export function runtimeArgs(config, { includeNoDiscovery = false } = {}) {
  const args = [];
  if (config.registry) args.push('--registry', config.registry);
  if (config.hubData) args.push('--hub-data', config.hubData);
  if (config.jobRoot) args.push('--job-root', config.jobRoot);
  if (includeNoDiscovery && config.noDiscovery) args.push('--no-discovery');
  return args;
}
