from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
BRIDGE = ROOT / "hub" / "claude_statusline_bridge.py"


def main() -> int:
    payload = {
        "version": "2.1.259",
        "session_id": "sess-123",
        "session_name": "monitor-work",
        "prompt_id": "prompt-456",
        "transcript_path": r"D:\Research\Monitor\.claude\session.jsonl",
        "model": {"id": "claude-sonnet-5", "display_name": "Claude Sonnet 5"},
        "cwd": r"D:\Research\Monitor",
        "workspace": {
            "project_dir": r"D:\Research\Monitor",
            "git_worktree": "feature-monitor",
            "secret": "must-not-leak-workspace",
        },
        "agent": {
            "name": "monitor-agent",
            "type": "general-purpose",
            "prompt": "must-not-leak-agent-prompt",
        },
        "context_window": {"used_percentage": 37.5},
        "cost": {"total_cost_usd": 0.42},
        "rate_limits": {
            "five_hour": {"used_percentage": 24.0, "resets_at": 1788062400},
            "seven_day": {"used_percentage": 13.0, "resets_at": 1788580800},
        },
        # Deliberately present sensitive-looking fields: the bridge must not copy them.
        "oauth_token": "must-not-leak",
        "api_key": "must-not-leak",
    }

    with tempfile.TemporaryDirectory() as td:
        output = pathlib.Path(td) / "cli_status.json"
        env = dict(os.environ)
        env["MONITOR_HUB_CLAUDE_STATUS"] = str(output)

        result = subprocess.run(
            [sys.executable, str(BRIDGE)],
            input=json.dumps(payload),
            text=True,
            capture_output=True,
            env=env,
            check=True,
        )

        assert "Claude Sonnet 5" in result.stdout
        assert "5h 24%" in result.stdout
        assert "7d 13%" in result.stdout

        saved = json.loads(output.read_text(encoding="utf-8"))
        assert saved["version"] == "2.1.259"
        assert saved["rate_limits"]["five_hour"]["used_percentage"] == 24.0
        assert saved["rate_limits"]["seven_day"]["used_percentage"] == 13.0
        assert saved["context_window"]["used_percentage"] == 37.5
        assert saved["workspace"]["current_dir"] == r"D:\Research\Monitor"
        assert saved["workspace"]["project_dir"] == r"D:\Research\Monitor"
        assert saved["workspace"]["git_worktree"] == "feature-monitor"
        assert saved["session"]["id"] == "sess-123"
        assert saved["session"]["name"] == "monitor-work"
        assert saved["session"]["prompt_id"] == "prompt-456"
        assert saved["session"]["transcript_path"] == r"D:\Research\Monitor\.claude\session.jsonl"
        assert saved["agent"]["name"] == "monitor-agent"
        assert saved["agent"]["type"] == "general-purpose"
        serialized = json.dumps(saved)
        assert "must-not-leak" not in serialized
        assert "must-not-leak-workspace" not in serialized
        assert "must-not-leak-agent-prompt" not in serialized
        assert "oauth_token" not in serialized
        assert "api_key" not in serialized

        payload["rate_limits"]["five_hour"]["used_percentage"] = 1788062400
        subprocess.run(
            [sys.executable, str(BRIDGE)],
            input=json.dumps(payload),
            text=True,
            capture_output=True,
            env=env,
            check=True,
        )
        invalid = json.loads(output.read_text(encoding="utf-8"))
        assert "used_percentage" not in invalid["rate_limits"]["five_hour"]
        assert invalid["rate_limits"]["five_hour"]["resets_at"] == 1788062400

    print("claude statusline bridge test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
