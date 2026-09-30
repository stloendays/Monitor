# Claude CLI integration

Monitor Hub can display local Claude Code status without reading Claude credentials or making a separate model request.

## What is shown

The Qt sidebar card can show:

- whether Claude CLI was found;
- how many local Claude CLI processes are currently visible through WMI;
- Claude Code version and active model when available;
- 5-hour usage percentage;
- 5-hour reset time;
- 7-day usage percentage;
- 7-day reset time;
- context-window usage percentage;
- when Monitor Hub last received a usage snapshot.

The card refreshes with the normal Monitor Hub refresh cycle. Reset times are rendered as both an absolute local time and, when close enough, a human-readable countdown such as `今天 15:00（约 2 小时 10 分钟后）`.

## Data sources

Monitor Hub uses two sources, in priority order.

### 1. Claude Code statusLine bridge

This is the preferred source.

Claude Code already provides rate-limit data to status-line commands. Monitor Hub ships:

`hub/claude_statusline_bridge.py`

The bridge:

1. reads the statusLine JSON from stdin;
2. copies only an allow-listed subset;
3. atomically writes a local snapshot;
4. prints a compact status line back to Claude Code.

It does **not**:

- call Claude;
- spend an extra model turn;
- read OAuth tokens;
- read API keys;
- call Anthropic's internal usage endpoints;
- write credentials to Monitor Hub data.

The Qt **接入用量** button copies a `/statusline` instruction pointing Claude Code at the bridge. The instruction explicitly asks Claude Code to preserve/merge an existing statusLine instead of silently replacing it. The user still pastes and accepts that configuration inside Claude Code; Monitor Hub does not write `settings.json` itself.

### 2. Existing cdesktop-detach stream-json logs

If the statusLine bridge has not been configured, Monitor Hub scans recent:

`%LOCALAPPDATA%\cdesktop-jobs\*\output.log`

for Claude `rate_limit_event` records.

This preserves compatibility with the user's existing detached Claude workflow. When a recent Claude Code version includes `unifiedWindows`, Monitor Hub can display both utilization and reset times. Older events may provide reset time without a percentage.

## Snapshot path

Priority:

1. `MONITOR_HUB_CLAUDE_STATUS`
2. `MONITOR_HUB_DATA\claude\cli_status.json`
3. the user's established Windows default:
   `D:\Research\monitor-hub\claude\cli_status.json`

The bridge and Qt reader use the same contract.

Example:

```json
{
  "schema_version": 1,
  "source": "claude_statusline",
  "captured_at": "2026-09-30T10:15:00+08:00",
  "version": "2.1.259",
  "model": {
    "id": "claude-sonnet-5",
    "display_name": "Claude Sonnet 5"
  },
  "context_window": {
    "used_percentage": 31.5
  },
  "rate_limits_available": true,
  "rate_limits": {
    "five_hour": {
      "used_percentage": 24,
      "resets_at": 1788062400
    },
    "seven_day": {
      "used_percentage": 13,
      "resets_at": 1788580800
    }
  }
}
```

## Personal Windows compatibility

The existing machine-specific defaults remain supported.

### Monitor Hub data

Existing default:

`D:\Research\monitor-hub`

Override:

`MONITOR_HUB_DATA`

### Claude executable

Detection order:

1. `MONITOR_HUB_CLAUDE_EXE`
2. `D:\Download\npm-global\node_modules\@anthropic-ai\claude-code\bin\claude.exe`
3. `%USERPROFILE%\.local\bin\claude.exe`
4. `%APPDATA%\npm\claude.cmd`

### Claude configuration

Detection order:

1. `CLAUDE_CONFIG_DIR`
2. `%USERPROFILE%\.claude`

The user's established configuration therefore continues to resolve to:

`C:\Users\ASUS\.claude`

when no override is set.

### Detached Claude helper

Existing default:

`C:\Users\ASUS\.claude\tools\cdesktop-detach.ps1`

Override:

`MONITOR_HUB_DETACH`

### PowerShell

Existing preferred executable:

`D:\Tools\PowerShell\7.6.3\pwsh.exe`

Override:

`MONITOR_HUB_PWSH`

If that executable is unavailable and no override is set, the legacy Python hub falls back to `pwsh` from PATH.

### Detached jobs

Existing root remains:

`%LOCALAPPDATA%\cdesktop-jobs`

On the user's current machine this resolves under:

`C:\Users\ASUS\AppData\Local\cdesktop-jobs`

## Installed application

CMake installs the bridge under:

`share\monitor_hub\claude_statusline_bridge.py`

The Qt app searches both:

- the user's development checkout at `D:\Research\Monitor\hub\claude_statusline_bridge.py`;
- the installed `share\monitor_hub` location.

This allows the same UI to work from a development checkout and from a packaged release.

## Manual usage check

The **复制 /usage** button copies Claude Code's interactive `/usage` command.

Use that when you want Claude Code's full native usage screen. Monitor Hub does not attempt to scrape the interactive TUI.

## Known scope

The portable status-line contract exposes the common 5-hour and 7-day windows. Claude Code may internally track additional model-scoped or credit-specific limits. Monitor Hub does not fabricate those values when they are not present in the status-line payload.

If a usage window is absent, the UI shows it as unknown instead of displaying `0%`.


## Session observability

When Claude Code's statusLine payload includes session metadata, Monitor Hub also projects a small read-only session view.

Displayed metadata can include:

- `session_id`;
- `session_name`;
- `prompt_id`;
- current workspace / project directory;
- current git worktree;
- transcript file path;
- current `--agent` name/type when present;
- most recent tool name seen near the end of the transcript;
- most recent Agent/Task tool call and its subagent type;
- freshness of the last statusLine snapshot.

The bridge only stores metadata. It does not store prompt text, assistant text, tool inputs, tool outputs, OAuth credentials or API keys.

### Transcript inspection

If a transcript path is available, the C++ adapter reads only a bounded tail of the local JSONL file and extracts tool-call metadata.

For generic tools, Monitor Hub records only the tool name.

For Agent/Task calls, Monitor Hub records only:

- the tool name;
- optional `subagent_type`;
- event timestamp when available.

It deliberately ignores fields such as `prompt`, `description`, command contents and tool results.

The Qt **会话记录** button opens the original local transcript in the user's normal file handler; Monitor Hub does not render the conversation body inside the monitoring UI.

### Session activity state

The sidebar state is conservative:

- **正在工作**: a Claude process is visible and a statusLine snapshot is very recent;
- **最近活动**: a recent statusLine snapshot exists;
- **CLI 运行中**: a Claude process is visible but no fresh snapshot is available;
- **已缓存**: metadata exists but no local Claude process is currently visible;
- **待接用量**: Claude CLI is installed but statusLine data is not connected;
- **未发现**: no configured/install candidate is found.

A visible Claude process is not treated as proof that a specific cached session is alive.


### Monitor project linking

The Qt Claude card attempts a local path-only association between the current Claude workspace and registered Monitor Hub projects.

Claude-side candidates:

- `workspace.project_dir`;
- `workspace.current_dir` / `cwd`.

Monitor project candidates:

- `dir`;
- `qa_cwd`;
- `runner.workdir`;
- parent directory of `status_json`;
- parent directory of `status_md`.

Matching is path-based and case-insensitive for Windows-friendly behavior. Exact matches and ancestor/descendant relationships are accepted; when several projects match, the most specific (longest) project path wins.

Monitor Hub does not link projects by display name.

When a match exists, the Claude card shows the associated Monitor project and enables **关联项目**, which selects that project and opens its project view. When no path match exists, the button remains disabled.


## Native statusLine bridge

Packaged Monitor Hub now prefers a native bridge implemented by `monitor_hub_cli`:

```powershell
monitor_hub_cli.exe --claude-statusline
```

The command:

1. reads Claude Code statusLine JSON from stdin;
2. applies the same allow-list policy as the Python compatibility bridge;
3. atomically replaces the local `cli_status.json` snapshot;
4. prints the compact visible status line to stdout;
5. exits without making a Claude/model/network request.

This removes Python as a runtime requirement for an installed Monitor Hub package.

### Bridge selection in the Qt UI

The **接入用量** action chooses the bridge in this order:

1. `MONITOR_HUB_CLAUDE_BRIDGE` when it points to an existing custom bridge;
2. `monitor_hub_cli.exe --claude-statusline` beside the running Qt executable;
3. the installed/development `claude_statusline_bridge.py`;
4. the user's established development path:
   `D:\Research\Monitor\hub\claude_statusline_bridge.py`.

If the explicit override ends in `.py`, Qt prefixes it with `python`. Other explicit overrides are treated as directly executable bridge commands.

### Native snapshot safety

The native bridge persists only:

- version;
- session id/name/prompt id/transcript path;
- model id/display name;
- workspace current/project/worktree paths;
- Agent name/type;
- context-window percentage;
- session cost;
- validated 5-hour / 7-day usage percentages and reset timestamps.

It does not persist arbitrary statusLine fields, OAuth/API credentials, Agent prompts, tool input/output, or conversation text.

The native write uses a temporary file and an atomic replace operation on Windows so the Qt reader does not observe a partially written JSON document.

The Python bridge remains installed as a compatibility/debug fallback; both bridges write the same snapshot contract.
