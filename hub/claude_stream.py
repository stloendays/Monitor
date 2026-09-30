"""Readable rendering of `claude -p --output-format stream-json --verbose` transcripts, 2026-09-28.

Shared by qoi_ext_monitor.py (takeover transcripts, --watch) and monitor_hub.py (the monitor window).
"""
from __future__ import annotations

import json


def _short(x, n=300):
    return str(x).replace("\r", " ").replace("\n", " ")[:n]


def render_event(d):
    """Readable lines for one stream-json event."""
    t = d.get("type")
    if t == "system" and d.get("subtype") == "init":
        return ["[start] session %s  model %s  cwd %s" % (d.get("session_id"), d.get("model"), d.get("cwd"))]
    if t == "rate_limit_event":
        info = d.get("rate_limit_info") or {}
        windows = info.get("unifiedWindows") or {}
        parts = []
        for key, label in (("five_hour", "5h"), ("seven_day", "7d")):
            w = windows.get(key) or {}
            util = w.get("utilization")
            reset = w.get("resetsAt")
            if isinstance(util, (int, float)):
                util = util * 100 if util <= 1 else util
                parts.append("%s %.0f%%" % (label, util))
            elif isinstance(reset, (int, float)):
                parts.append("%s reset %s" % (label, reset))
        if not parts:
            kind = info.get("rateLimitType")
            util = info.get("utilization")
            reset = info.get("resetsAt")
            text = str(kind or "rate limit")
            if isinstance(util, (int, float)):
                util = util * 100 if util <= 1 else util
                text += " %.0f%%" % util
            if isinstance(reset, (int, float)):
                text += " reset %s" % reset
            parts.append(text)
        return ["[Claude usage] " + " · ".join(parts)]
    if t == "assistant":
        out = []
        for c in d.get("message", {}).get("content", []) or []:
            if c.get("type") == "text" and c.get("text", "").strip():
                out += ["", "Claude: " + c["text"].strip()]
            elif c.get("type") == "tool_use":
                inp = c.get("input") or {}
                arg = next((inp[k] for k in ("command", "file_path", "pattern", "path", "url", "description", "prompt") if k in inp),
                           json.dumps(inp, ensure_ascii=False))
                out.append("  -> %s: %s" % (c.get("name"), _short(arg)))
        return out
    if t == "user":
        out = []
        content = d.get("message", {}).get("content", [])
        for c in content if isinstance(content, list) else []:
            if isinstance(c, dict) and c.get("type") == "tool_result":
                body = c.get("content")
                if isinstance(body, list):
                    body = " ".join(x.get("text", "") for x in body if isinstance(x, dict))
                out.append("     %s %s" % ("ERROR" if c.get("is_error") else "<-", _short(body)))
        return out
    if t == "result":
        # on success the result repeats the last assistant text, so only an error message is shown
        return ["", "[end] %s  is_error=%s  %.1f min  $%.2f" % (d.get("subtype"), d.get("is_error"), (d.get("duration_ms") or 0) / 60000,
                                                               d.get("total_cost_usd") or 0)] + (["ERROR " + str(d.get("result", ""))] if d.get("is_error") else [])
    return []


def render_stream(text):
    lines = []
    for ln in text.splitlines():
        s = ln.strip()
        if not s:
            continue
        try:
            d = json.loads(s) if s.startswith("{") else None
        except ValueError:
            d = None
        lines += render_event(d) if isinstance(d, dict) else [s]
    return lines


def stream_init(text):
    """The init event (session_id, cwd, model) of a transcript, or None."""
    for ln in text.splitlines():
        if '"subtype":"init"' in ln.replace(" ", ""):
            try:
                return json.loads(ln.strip())
            except ValueError:
                pass
    return None


def stream_session(text):
    d = stream_init(text)
    return d.get("session_id") if d else None


def parse_result(text):
    """The final `result` event of a transcript, or None."""
    for ln in reversed(text.splitlines()):
        ln = ln.strip()
        if ln.startswith("{") and '"type"' in ln:
            try:
                d = json.loads(ln)
            except ValueError:
                continue
            if d.get("type") == "result":
                return d
    return None
