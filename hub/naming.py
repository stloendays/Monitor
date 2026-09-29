"""Machine-readable naming convention for monitor scripts and runners.

Canonical forms:
  monitor__<project>__<scope>__<interval>.py
  <project>__monitor__<scope>__<interval>

Examples:
  monitor__ceox-rh__hpc__300m.py
  ceox-rh__monitor__hpc__300m
"""
from __future__ import annotations

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
    base = os.path.basename(str(name or ""))
    m = SCRIPT_RE.fullmatch(base) or RUNNER_RE.fullmatch(base)
    if not m:
        return {}
    d = {k: v.lower() for k, v in m.groupdict().items()}
    d["interval_min"] = interval_minutes(d["interval"])
    d["display_name"] = f"{d['project']} · {d['scope']} monitor"
    d["canonical"] = True
    return d


def parse_monitor_identity(name, action=""):
    """Parse runner name first, then any canonical script basename in the action."""
    d = parse_monitor_name(name)
    if d:
        return d
    for token in re.findall(r"[^\s\"']+\.py", str(action or ""), re.I):
        d = parse_monitor_name(token.strip("()[]{};,"))
        if d:
            d["runner_name"] = str(name or "")
            return d
    return {"canonical": False}
