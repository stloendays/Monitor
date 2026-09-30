"""Monitor Hub bridge for Claude Code statusLine JSON.

Claude Code writes a JSON object to this command's stdin whenever it refreshes
its status line. This bridge copies only a small allow-listed, non-secret subset
into Monitor Hub's local data directory and prints a compact status line back to
Claude Code.

It does not call Claude, read OAuth credentials, or make network requests.
"""
from __future__ import annotations

import datetime as dt
import json
import os
import pathlib
import sys
import tempfile


def _default_output() -> pathlib.Path:
    explicit = os.environ.get("MONITOR_HUB_CLAUDE_STATUS")
    if explicit:
        return pathlib.Path(explicit)

    hub_data = os.environ.get("MONITOR_HUB_DATA")
    if hub_data:
        return pathlib.Path(hub_data) / "claude" / "cli_status.json"

    if os.name == "nt":
        # Preserve the user's established Monitor Hub layout.
        return pathlib.Path(r"D:\Research\monitor-hub\claude\cli_status.json")

    return pathlib.Path.home() / ".local" / "share" / "MonitorHub" / "claude" / "cli_status.json"


def _object(value):
    return value if isinstance(value, dict) else {}


def _number(value):
    return value if isinstance(value, (int, float)) and not isinstance(value, bool) else None


def _usage_window(rate_limits: dict, key: str) -> dict | None:
    source = _object(rate_limits.get(key))
    used = _number(source.get("used_percentage"))
    reset = _number(source.get("resets_at"))
    if used is None and reset is None:
        return None
    out = {}
    if used is not None and 0.0 <= float(used) <= 100.0:
        out["used_percentage"] = float(used)
    if reset is not None:
        out["resets_at"] = float(reset)
    return out


def _sanitize(payload: dict) -> dict:
    model = _object(payload.get("model"))
    workspace = _object(payload.get("workspace"))
    context = _object(payload.get("context_window"))
    cost = _object(payload.get("cost"))
    limits = _object(payload.get("rate_limits"))

    five = _usage_window(limits, "five_hour")
    seven = _usage_window(limits, "seven_day")

    agent = _object(payload.get("agent"))

    snapshot = {
        "schema_version": 1,
        "source": "claude_statusline",
        "captured_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "version": str(payload.get("version") or ""),
        "session": {
            "id": str(payload.get("session_id") or ""),
            "name": str(payload.get("session_name") or ""),
            "prompt_id": str(payload.get("prompt_id") or ""),
            "transcript_path": str(payload.get("transcript_path") or ""),
        },
        "model": {
            "id": str(model.get("id") or ""),
            "display_name": str(model.get("display_name") or model.get("id") or ""),
        },
        "workspace": {
            "current_dir": str(workspace.get("current_dir") or payload.get("cwd") or ""),
            "project_dir": str(workspace.get("project_dir") or ""),
            "git_worktree": str(workspace.get("git_worktree") or ""),
        },
        "agent": {
            "name": str(agent.get("name") or ""),
            "type": str(agent.get("type") or ""),
        },
        "context_window": {},
        "cost": {},
        "rate_limits_available": bool(five or seven),
        "rate_limits": {},
    }

    context_used = _number(context.get("used_percentage"))
    if context_used is not None:
        snapshot["context_window"]["used_percentage"] = float(context_used)

    session_cost = _number(cost.get("total_cost_usd"))
    if session_cost is not None:
        snapshot["cost"]["total_cost_usd"] = float(session_cost)

    if five:
        snapshot["rate_limits"]["five_hour"] = five
    if seven:
        snapshot["rate_limits"]["seven_day"] = seven

    return snapshot


def _atomic_write(path: pathlib.Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(payload, ensure_ascii=False, indent=2) + "\n"

    fd, temp_name = tempfile.mkstemp(
        prefix=path.name + ".",
        suffix=".tmp",
        dir=str(path.parent),
        text=True,
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temp_name, path)
    finally:
        try:
            os.unlink(temp_name)
        except FileNotFoundError:
            pass


def _percent(window: dict | None) -> str:
    if not window or "used_percentage" not in window:
        return "?"
    return f"{window['used_percentage']:.0f}%"


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except Exception:
        # A status line command should fail soft and never disturb Claude Code.
        print("Claude")
        return 0

    snapshot = _sanitize(payload)
    try:
        _atomic_write(_default_output(), snapshot)
    except OSError:
        pass

    model = snapshot["model"].get("display_name") or "Claude"
    five = snapshot["rate_limits"].get("five_hour")
    seven = snapshot["rate_limits"].get("seven_day")
    parts = [model]
    if five or seven:
        parts.append(f"5h {_percent(five)}")
        parts.append(f"7d {_percent(seven)}")
    context = snapshot["context_window"].get("used_percentage")
    if isinstance(context, (int, float)):
        parts.append(f"ctx {context:.0f}%")
    print(" · ".join(parts))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
