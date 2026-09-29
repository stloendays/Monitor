"""Machine-readable naming convention for monitor scripts and runners.

Canonical forms:
  monitor__<project>__<scope>__<interval>.py
  <project>__monitor__<scope>__<interval>

Examples:
  monitor__ceox-rh__hpc__300m.py
  ceox-rh__monitor__hpc__300m
"""
from __future__ import annotations

import ast
import os
import re

SLUG = r"[a-z0-9][a-z0-9-]*"
INTERVAL = r"(?:\d+m|\d+h|\d+d)"
SCRIPT_RE = re.compile(rf"^monitor__(?P<project>{SLUG})__(?P<scope>{SLUG})__(?P<interval>{INTERVAL})\.py$", re.I)
RUNNER_RE = re.compile(rf"^(?P<project>{SLUG})__monitor__(?P<scope>{SLUG})__(?P<interval>{INTERVAL})$", re.I)


def interval_minutes(token):
    m = re.fullmatch(r"(\d+)([mhd])", str(token or ""), re.I)
    if not m:
        return None
    n = int(m.group(1))
    return n * {"m": 1, "h": 60, "d": 1440}[m.group(2).lower()]


def parse_monitor_name(name):
    raw = str(name or "")
    base = os.path.basename(raw.replace("\\", "/"))
    m = SCRIPT_RE.fullmatch(base) or RUNNER_RE.fullmatch(base)
    if not m:
        return {}
    d = {k: v.lower() for k, v in m.groupdict().items()}
    d["interval_min"] = interval_minutes(d["interval"])
    d["display_name"] = f"{d['project']} · {d['scope']} monitor"
    d["canonical"] = True
    return d


def read_monitor_meta(path):
    """Read a literal top-level MONITOR_META dict without importing/executing the script."""
    try:
        with open(path, encoding="utf-8-sig") as f:
            tree = ast.parse(f.read(), filename=str(path))
    except (OSError, SyntaxError):
        return {}
    for node in tree.body:
        if isinstance(node, (ast.Assign, ast.AnnAssign)):
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            if any(isinstance(t, ast.Name) and t.id == "MONITOR_META" for t in targets):
                try:
                    value = ast.literal_eval(node.value)
                except (ValueError, TypeError):
                    return {}
                return value if isinstance(value, dict) else {}
    return {}


def action_script_path(action):
    """Return the first .py token in a runner action, preserving Windows paths."""
    m = re.search(r"""(?:"([^"]+\.py)"|'([^']+\.py)'|([^\s]+\.py))""", str(action or ""), re.I)
    if not m:
        return ""
    return next((x for x in m.groups() if x), "").strip("()[]{};,")


def parse_monitor_identity(name, action=""):
    """Parse canonical identity and merge safe MONITOR_META when the script is locally readable."""
    script = action_script_path(action)
    d = parse_monitor_name(name)
    if not d and script:
        d = parse_monitor_name(script)
    if not d:
        d = {"canonical": False}
    if script:
        d["script"] = script
        meta = read_monitor_meta(script)
        if meta:
            d["meta"] = meta
            for key in ("project_id", "scope", "interval_min", "display_name"):
                if key in meta:
                    if key == "project_id":
                        d["project"] = str(meta[key])
                    else:
                        d[key] = meta[key]
    if name and str(name) != d.get("display_name"):
        d["runner_name"] = str(name)
    return d
