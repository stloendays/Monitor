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
        "model": {"id": "claude-sonnet-5", "display_name": "Claude Sonnet 5"},
        "workspace": {"current_dir": r"D:\Research\Monitor"},
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
        serialized = json.dumps(saved)
        assert "must-not-leak" not in serialized
        assert "oauth_token" not in serialized
        assert "api_key" not in serialized

    print("claude statusline bridge test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
