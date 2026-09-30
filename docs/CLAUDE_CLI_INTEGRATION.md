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
