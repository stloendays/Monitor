"""监控总台：在一个窗口里查看和管理本机的所有项目监控，2026-09-28。

说明书：同目录 monitor_hub_README.md（窗口左下角“使用说明”）。项目登记在 monitor_hub_projects.json。
支持的状态来源（adapter）：
  * generic   新监控统一写的 hub_status.json（格式见 README 第 9 节）；
  * qoi       QoI 扩展监控（qoi_ext_monitor.py）的 status.json；
  * markdown  Vanda 计算化学监控的 status_latest.md 表格。
名字里带 monitor 的定时任务 / 后台作业即使没登记，也会列在“其他监控”里。

刷新时不启动任何子进程（进程列表和定时任务都经 COM 读取），不会闪出命令行窗口；必须启动子进程时
（qstat、提问、管理操作）一律 CREATE_NO_WINDOW + SW_HIDE。

    pythonw monitor_hub.py
"""
from __future__ import annotations

import collections
import datetime as dt
import glob
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import messagebox, simpledialog, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import claude_stream as cs  # noqa: E402
import naming as nm  # noqa: E402

# MONITOR_HUB_REGISTRY / MONITOR_HUB_DATA point the hub at other data (demo mode: tests/demo/make_demo.py);
# MONITOR_HUB_NO_DISCOVERY=1 hides the machine's unregistered monitors.
REGISTRY = os.environ.get("MONITOR_HUB_REGISTRY") or os.path.join(HERE, "monitor_hub_projects.json")
README = os.path.join(HERE, "monitor_hub_README.md")
DISCOVERY = not os.environ.get("MONITOR_HUB_NO_DISCOVERY")
DETACH = r"C:\Users\ASUS\.claude\tools\cdesktop-detach.ps1"       # shared detached-job helper (copy in ../deps)
JOBROOT = os.path.join(os.environ.get("LOCALAPPDATA", r"C:\Users\ASUS\AppData\Local"), "cdesktop-jobs")
CLAUDE = r"D:\Download\npm-global\node_modules\@anthropic-ai\claude-code\bin\claude.exe"
PWSH = r"D:\Tools\PowerShell\7.6.3\pwsh.exe" if os.path.exists(r"D:\Tools\PowerShell\7.6.3\pwsh.exe") else "pwsh"
HUB_DATA = os.environ.get("MONITOR_HUB_DATA") or r"D:\Research\monitor-hub"
REQ_DIR = os.path.join(HUB_DATA, "requests")
REFRESH_S, TICK_MS = 60, 3000

UI, MONO = "Microsoft YaHei UI", "Consolas"
INK, RED, GREEN, BLUE, BROWN, LINE = "#1a1a1a", "#b00020", "#1b5e20", "#0d47a1", "#7a4a00", "#9e9e9e"
SEL_BG = "#e3ecf7"

HEALTH = {
    "attention": ("⚠ 需要你处理", RED, "监控发现了它自己处理不了、需要你决定的事"),
    "error": ("✖ 监控出错", RED, "监控本身出了问题：找不到、上次运行失败、连不上服务器"),
    "stale": ("⚠ 很久没更新", RED, "状态文件超过两个检查周期没更新，监控可能没在运行"),
    "working": ("⟳ 后台处理中", BLUE, "监控发现了问题，后台 Claude 正在按规程处理"),
    "ok": ("● 正常", GREEN, "监控按时运行，没有需要你做的事"),
    "done": ("✔ 已完成", GREEN, "这个项目的计算都做完了，结果在“最终结果”页"),
    "paused": ("⏸ 已暂停", BROWN, "监控被暂停（定时任务停用或后台作业没在运行）"),
    "unknown": ("… 读取中", INK, ""),
}
ORDER = ["attention", "error", "stale", "working", "ok", "paused", "done", "unknown"]
TAKEOVER_STATE = {"ok": "✔ 完成", "failed": "✖ 失败", "running": "⟳ 进行中"}

# Questions get only the pure read tools. No Bash at all: prefix rules let `git diff/show/log --output=<file>` write
# files and cannot see into a quoted ssh remote command (tested 2026-09-28); live state (runner, qstat) is fetched by the
# hub itself and attached to each question instead.
QA_TOOLS = ["Read", "Grep", "Glob"]
QA_SYSTEM = ("You answer the user's questions about the monitored project \"%s\" and its monitor's takeovers. Answer in "
             "Chinese, lead with the answer, keep it short and concrete (numbers, job names, times). You can only read "
             "files (Read, Grep, Glob); each question comes with the monitor hub's current view and, where available, a "
             "live server query taken seconds ago. Do not offer to modify anything, start or stop jobs, submit, commit or "
             "push; if an action is needed, say exactly what should be done and let the user decide. Sources: %s")

REQUEST_TEMPLATE = """【监控任务】
项目名称：
项目目录（本机路径）：
计算在哪里跑：本机 / Vanda PBS / 其他服务器（写清楚）
要监控的作业：作业名、PBS 作业号或启动命令、检查点 / 输出文件在哪里
完成标准：什么情况算全部完成
检查间隔：例如 15 分钟、5 小时
允许监控自动做的操作：例如 进程停了原地续算、SCF 不收敛按规程重投、跑完汇总结果并提交推送
禁止的操作：例如 不改计算参数、不删数据、不提交新体系
需要通知我的情况：
最终交付：例如 结果表、RESULTS.md、推送到 GitHub 分支 xxx
备注：
"""

REQUEST_EXAMPLE = """【监控任务】
项目名称：示例 · 表面吸附能计算（第 2 批）
项目目录（本机路径）：D:\\Research\\Example\\adsorption_batch2
计算在哪里跑：HPC 集群 PBS（ssh <主机别名>，分配项目 <项目代码>，队列 batch_cpu）
要监控的作业：slab_clean、slab_CO_top、slab_CO_bridge、slab_O_fcc 等 8 个体系，作业号见 SUBMISSION.txt；输出在 /scratch/<用户名>/…/batch2
完成标准：8 个体系的最终单点都完成，能量和磁矩写进结果表
检查间隔：5 小时
允许监控自动做的操作：墙时或 NELM 停机时从 WAVECAR/CONTCAR 原地续算；弛豫验收通过后建最终单点
禁止的操作：不改 INCAR 的泛函、U、ENCUT；不提交新体系；不删除任何输出
需要通知我的情况：同一体系第 2 次 NELM 用满；自旋态和初始 MAGMOM 不一致
最终交付：results/batch2_table.md（E0、磁矩、吸附能），推送到项目仓库
备注：和其他批次共用集群配额，注意 scratch 使用量
"""

SETUP_PROMPT = """You are the setup agent of the user's monitor hub (监控总台). The user submitted the monitoring task below from the
hub window. Set it up end to end, following exactly:
  1. D:\\Research\\Monitor\\hub\\monitor_hub_README.md, section "9. 给 Claude 的执行规范" (hub registration, status file
     format hub_status.json, headless scheduling, takeovers, interval, final report);
  2. the monitor-script-rules skill (C:\\Users\\ASUS\\.claude\\skills\\monitor-script-rules\\SKILL.md);
  3. the project's own AGENTS.md / CLAUDE.md / handoff and state files (read them first).
Verify live state before acting (another agent or monitor may already cover these jobs; if so, register that monitor in the
hub instead of writing a second one). Do only what the request allows; anything it forbids or does not cover is a proposal.
When finished, write a short Chinese report to %(report)s: what was set up (files, scheduler entry, registry id), the first
monitoring round's result, and one line `NEEDS_USER: <reason>` or `NEEDS_USER: none`.

Request (submitted %(time)s):
%(request)s
"""


# ---------------------------------------------------------------- small helpers
def now():
    return dt.datetime.now()


def hidden():
    """Subprocess kwargs that never show a console window."""
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 0
    return dict(creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), startupinfo=si)


def read_text(p, limit=None):
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read() if limit is None else f.read(limit)
    except (OSError, TypeError):
        return ""


def tail_text(p, nbytes=60000):
    try:
        with open(p, "rb") as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(max(0, size - nbytes))
            return f.read().decode("utf-8", errors="replace")
    except (OSError, TypeError):
        return ""


def read_json(p):
    try:
        return json.loads(read_text(p))
    except ValueError:
        return None


def mtime(p):
    try:
        return os.path.getmtime(p)
    except (OSError, TypeError):
        return None


def ago(ts):
    if not ts:
        return "—"
    m = (time.time() - ts) / 60
    if m < 1:
        return "刚刚"
    if m < 60:
        return "%d 分钟前" % m
    if m < 48 * 60:
        return "%.1f 小时前" % (m / 60)
    return "%d 天前" % (m / 1440)


def hm(ts):
    return dt.datetime.fromtimestamp(ts).strftime("%m-%d %H:%M") if ts else "—"


def iso_minutes(s):
    """'PT5H' / 'PT15M' / 'PT1H30M' / 'P1D' -> minutes."""
    mo = re.fullmatch(r"P(?:(\d+)D)?T?(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?", s or "")
    if not mo or not any(mo.groups()):
        return None
    d, h, mi, _ = (int(x) if x else 0 for x in mo.groups())
    return d * 1440 + h * 60 + mi


def iso_duration(minutes):
    return "PT%dH" % (minutes // 60) if minutes % 60 == 0 else "PT%dM" % minutes


def every(minutes):
    if not minutes:
        return ""
    return "每 %d 分钟" % minutes if minutes < 60 else "每 %g 小时" % (minutes / 60)


def q(s):
    return "'" + str(s).replace("'", "''") + "'"


def strip_md(s):
    return re.sub(r"\*\*(.*?)\*\*", r"\1", s).replace("`", "")


def short_time(s):
    return (s or "—").replace("T", " ")[5:16] if s else "—"


# ---------------------------------------------------------------- system probe through COM (no child processes)
def _com_dt(v):
    try:
        return "" if v.year < 2000 else v.strftime("%Y-%m-%dT%H:%M:%S")
    except (AttributeError, ValueError):
        return ""


def _task_service():
    import win32com.client
    svc = win32com.client.Dispatch("Schedule.Service")
    svc.Connect()
    return svc


def _walk_tasks(svc, skip_microsoft=True):
    stack = [svc.GetFolder("\\")]
    while stack:
        f = stack.pop()
        for t in f.GetTasks(1):
            yield f, t
        for sub in f.GetFolders(0):
            if skip_microsoft and sub.Path.lower().startswith("\\microsoft"):
                continue
            stack.append(sub)


def probe():
    """{'tasks': {name: info}, 'procs': [dict(pid, name, cmd)]} read through COM (Task Scheduler + WMI)."""
    import win32com.client
    tasks, procs, err = {}, [], None
    try:
        for _, t in _walk_tasks(_task_service()):
            try:
                d = t.Definition
                action = " | ".join("%s %s" % (getattr(a, "Path", ""), getattr(a, "Arguments", "") or "") for a in d.Actions)
                if "monitor" not in (t.Name + " " + action).lower():
                    continue
                interval = next((trg.Repetition.Interval for trg in d.Triggers if trg.Repetition.Interval), "")
                tasks[t.Name] = dict(name=t.Name, state={1: "Disabled", 2: "Queued", 3: "Ready", 4: "Running"}.get(t.State, "Unknown"),
                                     last=_com_dt(t.LastRunTime), result=int(t.LastTaskResult) & 0xFFFFFFFF,
                                     next=_com_dt(t.NextRunTime) if t.Enabled else "", interval=interval, action=action)
            except Exception:  # noqa: BLE001
                continue
    except Exception as e:  # noqa: BLE001
        err = "读取定时任务失败：%r" % e
    try:
        wmi = win32com.client.GetObject(r"winmgmts:\\.\root\cimv2")
        for p in wmi.ExecQuery("SELECT ProcessId, Name, CommandLine FROM Win32_Process WHERE Name='pwsh.exe' OR "
                               "Name='powershell.exe' OR Name='python.exe' OR Name='claude.exe'"):
            procs.append(dict(pid=int(p.ProcessId), name=p.Name, cmd=p.CommandLine or ""))
    except Exception as e:  # noqa: BLE001
        err = (err or "") + " 读取进程失败：%r" % e
    return {"tasks": tasks, "procs": procs, "error": err}


def pid_alive(pid, procs):
    return any(p.get("pid") == pid for p in procs)


def detach_state(name, procs):
    """None (no such job), 'running', 'gone' or 'exit:<code>'."""
    d = os.path.join(JOBROOT, name)
    if not os.path.isdir(d):
        return None
    ex = os.path.join(d, "exitcode")
    if os.path.exists(ex):
        return "exit:" + read_text(ex).strip().lstrip("\ufeff")
    try:
        pid = int(read_text(os.path.join(d, "pid")).strip().lstrip("\ufeff"))
    except ValueError:
        return "gone"
    return "running" if any(p.get("pid") == pid and d.lower() in (p.get("cmd") or "").lower() for p in procs) else "gone"


# ---------------------------------------------------------------- runner (how the monitor itself runs)
def runner_info(p, sysinfo):
    r = p.get("runner") or {}
    kind, name = r.get("kind"), r.get("name")
    info = dict(kind=kind, name=name, interval=r.get("interval_min"), exists=False, running=False, paused=False,
                error=None, text="", last=None, next=None)
    if kind == "schtask":
        t = sysinfo["tasks"].get(name)
        if not t:
            info.update(error="找不到定时任务 %s" % name, text="Windows 定时任务 %s（不存在）" % name)
            return info
        info["exists"] = True
        info["interval"] = iso_minutes(t.get("interval")) or info["interval"]
        info["paused"] = t.get("state") == "Disabled"
        info["running"] = t.get("state") == "Running"
        res = int(t.get("result") or 0)
        restext = {0: "成功", 267009: "正在运行", 267011: "还没运行过"}.get(res, "出错（代码 0x%X）" % res)
        if res not in (0, 267009, 267011):
            info["error"] = "定时任务上次运行出错（代码 0x%X）" % res
        info["last"] = t.get("last") or None
        info["next"] = None if info["paused"] else (t.get("next") or None)
        info["text"] = "Windows 定时任务 %s，%s · 上次运行 %s（%s）· %s" % (
            name, every(info["interval"]) or "按计划", short_time(info["last"]), restext,
            "已停用" if info["paused"] else "下次 %s" % short_time(info["next"]))
    elif kind == "detach":
        st = detach_state(name, sysinfo["procs"])
        info["exists"] = st is not None
        info["running"] = st == "running"
        info["paused"] = st in ("exit:stopped", "exit:0")          # stopped on purpose, or finished its work
        if st is None:
            info["error"] = "找不到后台作业 %s" % name
        elif st not in ("running", "exit:stopped", "exit:0"):
            info["error"] = "监控进程意外退出（%s）" % st
        info["text"] = "后台作业 %s，%s · %s" % (name, every(info["interval"]) or "", {"running": "运行中", None: "不存在"}.get(
            st, "没在运行（%s）" % st))
    elif kind == "none":
        info.update(exists=True, text=p.get("runner_text", ""))
    return info


# ---------------------------------------------------------------- takeovers (background Claude runs)
def friendly_error(text):
    """Plain-language reason for a failed Claude run."""
    if re.search(r"session limit|usage limit|hit your limit", text, re.I):
        return "Claude 额度用完（%s）" % text.split("·")[-1].strip() if "·" in text else "Claude 额度用完"
    if re.search(r"not logged in|please run /login|invalid api key", text, re.I):
        return "Claude 没有登录"
    if re.search(r"api error|internal server error|overloaded", text, re.I):
        return "Claude 服务出错（%s）" % text[:80]
    return text


def _detach_takeovers(prefix, sysinfo):
    out = []
    for d in glob.glob(os.path.join(JOBROOT, prefix + "*")):
        name = os.path.basename(d)
        log = os.path.join(d, "output.log")
        st = detach_state(name, sysinfo["procs"])
        res = cs.parse_result(tail_text(log, 200000))
        state = "running" if st == "running" else "ok" if (res and not res.get("is_error") and st == "exit:0") else "failed"
        summary = (res.get("result") or "").strip().splitlines()[0] if res and res.get("result") else ""
        error = ""
        if state == "failed":
            error = friendly_error((res.get("result") or "").strip() if res and res.get("is_error") else "进程退出（%s），没有结果" % st)
        m = re.search(r"(\d{8})-(\d{4})", name)
        out.append(dict(key=name, time=mtime(os.path.join(d, "started")) or mtime(d), state=state, error=error[:160],
                        summary=strip_md(error or summary)[:200], path=log, kind="jsonl",
                        label="%s-%s %s:%s" % (m.group(1)[4:6], m.group(1)[6:], m.group(2)[:2], m.group(2)[2:]) if m else name))
    return out


def _glob_takeovers(t, sysinfo):
    folder = os.path.dirname(t["pattern"])
    stamps = {}
    for f in glob.glob(os.path.join(folder, "claude_takeover_*")):
        m = re.fullmatch(r"claude_takeover_(\d{8}_\d{4})\.(jsonl|md)", os.path.basename(f))
        if m:
            stamps.setdefault(m.group(1), {})[m.group(2)] = f
    last = read_json(t.get("last") or "") or {}
    lock_pid = None
    if t.get("lock") and os.path.exists(t["lock"]):
        try:
            lock_pid = int(read_text(t["lock"]).split()[0])
        except (ValueError, IndexError):
            lock_pid = None
    newest = max(stamps) if stamps else None
    out = []
    for s, files in stamps.items():
        md = read_text(files.get("md", "")).strip()
        res = cs.parse_result(tail_text(files["jsonl"], 200000)) if "jsonl" in files else None
        if s == newest and lock_pid and pid_alive(lock_pid, sysinfo["procs"]):
            state = "running"
        elif res is not None:
            state = "failed" if res.get("is_error") else "ok"
        elif last.get("report") == "claude_takeover_%s.md" % s:
            state = "ok" if last.get("ok") else "failed"
        else:
            state = "ok" if md else "failed"
        summary = md.splitlines()[0] if md else ((res.get("result", "").strip().splitlines() or [""])[0] if res else "")
        error = friendly_error((res.get("result") or "").strip()) if state == "failed" and res else ""
        ts = time.mktime(time.strptime(s, "%Y%m%d_%H%M"))
        out.append(dict(key=s, time=ts, state=state, error=error, summary=strip_md(error or summary)[:200],
                        path=files.get("jsonl") or files.get("md"), kind="jsonl" if "jsonl" in files else "md",
                        label=dt.datetime.fromtimestamp(ts).strftime("%m-%d %H:%M")))
    return out


def takeovers(p, sysinfo):
    """Newest first: dict(key, time, state, summary, path, kind, label)."""
    t = p.get("takeovers") or {}
    out = _detach_takeovers(t["prefix"], sysinfo) if t.get("kind") == "detach" else _glob_takeovers(t, sysinfo) if t.get("kind") == "glob" else []
    out.sort(key=lambda x: x["time"] or 0, reverse=True)
    return out


# ---------------------------------------------------------------- adapters
def parse_status_md(text):
    title, headline, tables, extras, notes = "", "", [], [], []
    cur, in_notes = None, False
    for ln in text.splitlines():
        s = ln.strip()
        if s.startswith("|"):
            cells = [c.strip() for c in s.strip("|").split("|")]
            if cur is None:
                cur = {"cols": cells, "rows": []}
                tables.append(cur)
            elif not all(re.fullmatch(r":?-{2,}:?", c) for c in cells if c):
                cur["rows"].append((cells + [""] * len(cur["cols"]))[:len(cur["cols"])])
            continue
        cur = None
        if not s:
            continue
        if s.startswith("# ") and not title:
            title = s[2:].strip()
        elif s.startswith("**") and "备注" in s:
            in_notes = True
        elif in_notes and s.startswith("- "):
            notes.append(strip_md(s[2:]))
        elif not headline:
            headline = strip_md(s)
        else:
            extras.append(strip_md(s))
    return dict(title=title, headline=headline, tables=tables, extras=extras, notes=notes)


def classify_row(status, progress=""):
    s = status.strip()
    if "非监控" in progress:
        return "other"
    if "完成" in s or s.upper().startswith("COMPLETE"):
        return "done"
    if re.match(r"(失败|FAIL|DEAD|停)", s, re.I):
        return "bad"
    if s.startswith("R") or "运行" in s:
        return "run"
    if s.startswith(("Q", "H")) or "排队" in s:
        return "queue"
    return ""


def count_summary(tags):
    mine = [t for t in tags if t != "other"]
    parts = [("完成", mine.count("done")), ("运行", mine.count("run")), ("排队", mine.count("queue")), ("异常", mine.count("bad"))]
    s = "，".join("%s %d" % (k, n) for k, n in parts if n)
    if tags.count("other"):
        s += "；另有 %d 个作业不归这个监控管" % tags.count("other")
    return s


def adapt_markdown(p, sysinfo, runner):
    path = p["status_md"]
    text = read_text(path)
    ts = mtime(path)
    md = parse_status_md(text)
    snap = dict(updated=ts, headline=md["headline"], notes=md["notes"], attention=[], table=dict(cols=[], rows=[], tags=[]),
                summary="", next=None)
    if md["tables"]:
        tb = md["tables"][0]
        cols = tb["cols"]
        si = next((i for i, c in enumerate(cols) if c == "状态"), None)
        pi = next((i for i, c in enumerate(cols) if c == "进度"), None)
        tags = [classify_row(r[si] if si is not None else "", r[pi] if pi is not None else "") for r in tb["rows"]]
        snap["table"] = dict(cols=cols, rows=[[strip_md(c) for c in r] for r in tb["rows"]], tags=tags)
        snap["summary"] = count_summary(tags)
    md_next = next((re.search(r"下次检查[:：]\s*(.+)", e).group(1).strip() for e in md["extras"] if re.search(r"下次检查[:：]", e)), None)
    snap["next"] = short_time(runner["next"]) if runner.get("next") else md_next    # the scheduler knows the real cadence
    snap["extras"] = [e for e in md["extras"] if not e.startswith("下次检查")]
    ssh_fail = "ssh/remote monitor FAILED" in text
    att_p = p.get("attention")
    att_t = mtime(att_p) if att_p else None
    fresh_att = bool(att_t and ts and att_t >= ts - 120)
    tks = takeovers(p, sysinfo)
    if fresh_att:
        items = [ln.strip() for ln in read_text(att_p).splitlines() if ln.strip()]
        last_tk = tks[0] if tks else None
        handled = last_tk and last_tk["time"] and last_tk["time"] >= att_t - 60
        if any(t["state"] == "running" for t in tks):
            snap["working"] = "后台 Claude 正在处理：" + "；".join(items)
        elif handled and last_tk["state"] == "ok":
            snap["attention"] = ["后台 Claude 已处理过这些问题，结果需要你看一下（见“后台处理记录”）："] + items
        elif handled:
            snap["attention"] = ["后台 Claude 处理失败，需要你处理："] + items
        else:
            snap["working"] = "监控发现问题，等待后台处理：" + "；".join(items)
    if ssh_fail:
        snap["error"] = "上次检查连不上服务器（ssh 失败）"
        snap["headline"] = "上次检查时连不上服务器，监控没有取到作业状态。"
    snap["done"] = read_text(p.get("done_file", "")).strip().upper().startswith("DONE")
    snap["takeovers"] = tks
    return snap


def adapt_qoi(p, sysinfo, runner):
    st = read_json(p["status_json"]) or {}
    try:
        ts = dt.datetime.fromisoformat(st["updated"]).timestamp()
    except (KeyError, ValueError, TypeError):
        ts = mtime(p["status_json"])
    rows = st.get("rows", [])
    live = {}
    try:
        mdir = os.path.dirname(p.get("monitor_script") or "")
        if mdir and mdir not in sys.path:
            sys.path.append(mdir)
        import qoi_ext_monitor as qm  # project adapter: live checkpoint counts between monitor rounds
        for name, job in qm.JOBS.items():
            ck, failed, _ = qm.scan(job)
            live[name] = (len(ck), len(failed), max((os.path.getmtime(x) for x in ck.values()), default=0))
    except Exception:  # noqa: BLE001
        pass
    cn = [("COMPLETE", "已完成并推送"), ("RESULTS_WRITTEN_NOT_PUSHED", "结果已写，待推送"), ("TAKEOVER_ACTIVE", "后台处理中"),
          ("CHECKPOINTS_DONE_NO_RESULTS", "计算完成，待汇总"), ("RETRY_FAILED_ITEMS", "重试失败项"), ("DEAD", "进程停止"), ("RUNNING", "运行中")]
    out_rows, tags = [], []
    for r in rows:
        s = r.get("status", "")
        n, nf, newest = live.get(r["job"], (r.get("checkpoints", 0), r.get("failures", 0), None))
        tot = r.get("total") or 0
        pct = ("%.1f" % (100.0 * n / tot)).rstrip("0").rstrip(".") if tot and n < tot else "100" if tot else "0"
        last = ago(newest) if newest else ("%s 分钟前" % r["last_checkpoint_min_ago"] if r.get("last_checkpoint_min_ago") is not None else "—")
        out_rows.append([r["job"], next((v for k, v in cn if s.startswith(k)), s), "%d / %d（%s%%）" % (n, tot, pct), str(nf),
                         str(r.get("rate_per_h") or "—"), "%.1f 小时" % r["eta_h"] if r.get("eta_h") else "—", last,
                         "是" if r.get("branch_pushed") else "否"])
        tags.append("done" if s == "COMPLETE" else "bad" if s.startswith("DEAD") else "run" if s.startswith("RUNNING") else "")
    done_n = sum(1 for r in rows if r.get("status") == "COMPLETE")
    runs = ["%s %s" % (o[0], o[2].split("（")[1].rstrip("）")) + ("，约 %s" % o[5] if o[5] != "—" else "")
            for o, r in zip(out_rows, rows) if r.get("status", "").startswith("RUNNING")]
    snap = dict(updated=ts, notes=[], extras=[], table=dict(cols=["作业", "状态", "进度（实时）", "失败", "速率（个/小时）", "预计剩余",
                                                                "最近检查点", "已推送"], rows=out_rows, tags=tags))
    snap["summary"] = "%d/%d 个作业已完成" % (done_n, len(rows)) + ("；" + "；".join(runs) + " 进行中" if runs else "")
    snap["headline"] = ("所有作业都已完成并推送。" if rows and done_n == len(rows) else
                        "计算在跑，监控%s检查一次，出问题会自动重启或请后台 Claude 处理。" % every(runner.get("interval") or 15))
    snap["attention"] = ["[需要你处理] " + n for n in st.get("notify", [])]
    items = ["%s %s：%s" % tuple(a) for a in st.get("attention", [])]
    tk_ = st.get("takeover")
    if tk_:
        snap["working"] = "后台 Claude 正在处理 %s" % tk_.get("key", "")
    elif items:
        snap["working"] = "监控发现问题，将请后台 Claude 处理：" + "；".join(items)
    snap["next"] = hm(ts + (runner.get("interval") or 15) * 60) if ts and runner.get("running") else None
    snap["done"] = bool(rows) and done_n == len(rows)
    snap["takeovers"] = takeovers(p, sysinfo)
    return snap


def adapt_generic(p, sysinfo, runner):
    """hub_status.json written by monitors set up under README section 9."""
    st = read_json(p["status_json"]) or {}
    try:
        ts = dt.datetime.fromisoformat(st["updated"]).timestamp()
    except (KeyError, ValueError, TypeError):
        ts = mtime(p["status_json"])
    tb = st.get("table") or {}
    cols, rows = list(tb.get("cols") or []), [[str(c) for c in r] for r in (tb.get("rows") or [])]
    tags = list(tb.get("tags") or [])
    tags = (tags + [""] * len(rows))[:len(rows)]
    row_meta = [m if isinstance(m, dict) else {} for m in (tb.get("row_meta") or [])]
    row_meta = (row_meta + [{} for _ in rows])[:len(rows)]
    nxt = st.get("next")
    snap = dict(updated=ts, headline=st.get("headline", ""), summary=st.get("summary") or count_summary(tags), notes=list(st.get("notes") or []),
                extras=[], table=dict(cols=cols, rows=rows, tags=tags, row_meta=row_meta), attention=list(st.get("attention") or []),
                working=st.get("working") or None, done=bool(st.get("done")), results=list(st.get("results") or []),
                next=short_time(runner["next"]) if runner.get("next") else (short_time(nxt) if nxt and "T" in str(nxt) else nxt),
                error=st.get("error") or None)
    snap["takeovers"] = takeovers(p, sysinfo)
    return snap


def adapt_setup(p, sysinfo, runner):
    """Requests submitted from the hub's 新建监控任务 dialog and their setup agents."""
    tks = {t["key"]: t for t in _detach_takeovers("hub-setup-", sysinfo)}
    rows, tags, attention = [], [], []
    for f in sorted(glob.glob(os.path.join(REQ_DIR, "*_request.md")), reverse=True):
        stamp = os.path.basename(f)[:-len("_request.md")]
        text = read_text(f)
        name = next((ln.split("：", 1)[1].strip() for ln in text.splitlines() if ln.startswith("项目名称：")), "") or "（未写项目名称）"
        t = tks.get("hub-setup-" + stamp)
        rep = read_text(os.path.join(REQ_DIR, stamp + "_report.md"))
        asks = [v.strip() for v in re.findall(r"^\s*NEEDS_USER:\s*(.*)$", rep, re.M)]
        need = [v for v in asks if not re.match(r"(none|no\b|n/?a|nothing|无|不需要)", v, re.I)]
        state = t["state"] if t else "failed"
        label = {"running": "办理中", "ok": "已办好" if not need else "办好了，有事要你定",
                 "failed": "中断：%s" % (t["error"] if t and t.get("error") else "后台作业不存在")}[state]
        summary = next((ln.strip() for ln in rep.splitlines() if ln.strip() and not ln.startswith("#")), "")
        if state == "failed":
            summary = ("报告已写出：" + summary + "；但任务没有正常结束，请核对报告") if summary else "没有写出办理报告"
        elif not summary and t:
            summary = t["summary"]
        rows.append([dt.datetime.strptime(stamp, "%Y%m%d-%H%M%S").strftime("%m-%d %H:%M"), name, label, strip_md(summary)[:120]])
        tags.append({"running": "run", "ok": "done" if not need else "bad", "failed": "bad"}[state])
        if need:
            attention.append("%s：%s" % (name, "；".join(need)))
        if state == "failed":
            attention.append("%s：后台办理中断（%s），见“后台处理记录”；额度恢复后可以重新提交" % (name, t["error"] if t and t.get("error") else "后台作业不存在"))
    snap = dict(updated=max([mtime(x) or 0 for x in glob.glob(os.path.join(REQ_DIR, "*"))] or [0]) or None,
                headline=("交给后台 Claude 设置的监控任务。办好后，新项目会出现在左侧“项目”里。" if rows else
                          "还没有提交过新任务。点左下角“新建监控任务”，按格式填写后交给后台 Claude 办理。"),
                summary="%d 个请求" % len(rows) if rows else "", notes=[], extras=[],
                table=dict(cols=["提交时间", "项目", "状态", "说明"], rows=rows, tags=tags), attention=attention,
                working="后台 Claude 正在设置新监控" if any(t == "run" for t in tags) else None, done=False)
    snap["takeovers"] = sorted(tks.values(), key=lambda x: x["time"] or 0, reverse=True)
    return snap


def adapt_runner_only(p, sysinfo, runner):
    return dict(updated=None, headline="这个监控还没登记到监控总台，这里只能看到它的运行情况。登记后可以看到进度和后台处理记录。",
                notes=[], extras=[p.get("action", "")], table=dict(cols=[], rows=[], tags=[]), summary=runner.get("text", ""),
                attention=[], takeovers=[], next=None, done=False)


ADAPTERS = {"generic": adapt_generic, "qoi": adapt_qoi, "markdown": adapt_markdown, "setup": adapt_setup, "runner_only": adapt_runner_only}


def results_of(p, snap):
    """[(label, path)] of the project's final deliverables."""
    items = [(r.get("label") or os.path.basename(r["path"]), r["path"]) for r in (p.get("results") or []) + (snap.get("results") or [])
             if isinstance(r, dict) and r.get("path")]
    for pat in p.get("results_glob") or []:
        items += [(os.path.basename(f), f) for f in sorted(glob.glob(pat))]
    return items


def snapshot(p, sysinfo):
    runner = runner_info(p, sysinfo)
    try:
        snap = ADAPTERS[p.get("adapter", "runner_only")](p, sysinfo, runner)
    except Exception as e:  # noqa: BLE001
        snap = dict(updated=None, headline="读取状态出错：%r" % e, notes=[], extras=[], table=dict(cols=[], rows=[], tags=[]),
                    summary="", attention=[], takeovers=[], next=None, done=False)
    snap.setdefault("extras", [])
    snap["runner"] = runner
    snap["results_list"] = results_of(p, snap)
    interval = runner.get("interval")
    stale = bool(snap.get("updated") and interval and not runner["paused"] and p.get("adapter") != "setup"
                 and time.time() - snap["updated"] > (2 * interval + 30) * 60)
    if snap.get("done"):
        h = "done"
    elif snap.get("error") or runner.get("error"):
        h = "error"
    elif runner["paused"]:
        h = "paused"          # a paused monitor's last attention items are history, not a request to the user
        snap["history"], snap["attention"] = snap.get("attention") or [], []
    elif snap.get("attention"):
        h = "attention"
    elif stale:
        h = "stale"
    elif snap.get("working"):
        h = "working"
    elif p.get("adapter") == "runner_only":
        h = "ok" if runner["exists"] else "error"
    else:
        h = "ok"
    snap["health"] = h
    snap["problem"] = snap.get("error") or runner.get("error") or (
        "状态文件 %s 没更新（应%s更新一次）" % (ago(snap["updated"]), every(interval)) if stale else "")
    return snap


SETUP_PROJECT = dict(id="hub-setup", name="新任务办理", area="交给后台 Claude 设置的新监控", adapter="setup",
                     runner=dict(kind="none"), runner_text="点左下角“新建监控任务”提交；每个请求由一个后台 Claude 办理",
                     dir=REQ_DIR, qa_cwd=HUB_DATA, claude_config_dir=None, builtin=True,
                     qa_sources=[REQ_DIR + " (*_request.md, *_report.md)", README, REGISTRY])


def load_registry():
    return read_json(REGISTRY) or {"projects": []}


def load_projects(sysinfo):
    reg = load_registry()
    projects = [p for p in reg.get("projects", []) if isinstance(p, dict) and p.get("id")]
    known_tasks = {p["runner"]["name"] for p in projects if p.get("runner", {}).get("kind") == "schtask"}
    known_jobs = {p["runner"]["name"] for p in projects if p.get("runner", {}).get("kind") == "detach"}
    extra = []
    if not DISCOVERY:
        return projects + [SETUP_PROJECT]
    for name, t in sorted(sysinfo["tasks"].items()):
        if name not in known_tasks:
            meta = nm.parse_monitor_identity(name, t.get("action", ""))
            display = meta.get("display_name") or name
            area = "其他监控 · 定时任务" + ((" · " + meta["scope"]) if meta.get("scope") else "")
            extra.append(dict(id="task:" + name, name=display, area=area, adapter="runner_only",
                              runner=dict(kind="schtask", name=name, interval_min=meta.get("interval_min")), action=t.get("action", ""),
                              naming=meta, unregistered=True))
    for d in glob.glob(os.path.join(JOBROOT, "*monitor*")):
        name = os.path.basename(d)
        if name not in known_jobs:
            extra.append(dict(id="job:" + name, name=name, area="其他监控 · 后台作业", adapter="runner_only",
                              runner=dict(kind="detach", name=name), action=read_text(os.path.join(d, "run.ps1"))[-300:], unregistered=True))
    return projects + [SETUP_PROJECT] + extra


def save_registry_value(pid, path, value):
    """Set projects[id=pid][path...] = value in the registry file."""
    reg = json.loads(read_text(REGISTRY), object_pairs_hook=collections.OrderedDict)
    p = next(x for x in reg["projects"] if x.get("id") == pid)
    for k in path[:-1]:
        p = p.setdefault(k, collections.OrderedDict())
    p[path[-1]] = value
    tmp = REGISTRY + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(reg, f, ensure_ascii=False, indent=2)
    os.replace(tmp, REGISTRY)


# ---------------------------------------------------------------- read-only questions
def qa_args(p, resume=None, fork=False):
    sysprompt = QA_SYSTEM % (p.get("name"), "; ".join(p.get("qa_sources") or [p.get("dir", "")]))
    # --setting-sources project: skip user/local settings, whose allow rules include git commit/push;
    # --strict-mcp-config: no MCP servers (some can move or upload files); --tools: only the read tools exist
    a = [CLAUDE, "-p", "--output-format", "stream-json", "--verbose", "--model", "opus",
         "--permission-mode", "default", "--setting-sources", "project", "--strict-mcp-config",
         "--tools", ",".join(QA_TOOLS), "--disallowedTools", "Bash", "Edit", "Write", "NotebookEdit",
         "--append-system-prompt", sysprompt, "--allowedTools"] + QA_TOOLS
    if resume:
        a += ["--resume", resume] + (["--fork-session"] if fork else [])
    return a


def run_live_query(p):
    """Output of the project's read-only live query (ssh banners removed), or ''."""
    lq = p.get("live_query")
    if not lq:
        return ""
    try:
        r = subprocess.run(lq["cmd"], capture_output=True, timeout=90, **hidden())
        out = (r.stdout + r.stderr).decode("utf-8", errors="replace")
    except (OSError, subprocess.TimeoutExpired) as e:
        return "查询失败：%r" % e
    return "\n".join(ln for ln in out.splitlines() if not re.search(r"post-quantum|store now|openssh.com/pq|may need to be upgraded|Authorized users", ln))


def qa_context(p, s, live):
    """What the hub sees right now, attached to every question."""
    r = s.get("runner", {})
    lines = ["[监控总台此刻看到的情况，%s]" % time.strftime("%Y-%m-%d %H:%M"),
             "状态：%s。%s" % (HEALTH[s.get("health", "unknown")][0], s.get("problem") or s.get("headline", "")),
             "进度摘要：%s" % (s.get("summary") or "—"),
             "监控方式：%s" % r.get("text", ""),
             "状态文件更新：%s" % (ago(s.get("updated")) if s.get("updated") else "—")]
    if s.get("attention"):
        lines += ["需要用户处理："] + ["  " + a for a in s["attention"]]
    if s.get("working"):
        lines.append("后台：" + s["working"])
    tks = s.get("takeovers") or []
    if tks:
        lines.append("最近的后台处理：" + "；".join("%s %s %s" % (t["label"], t["state"], t["summary"][:60]) for t in tks[:3]))
    if live:
        lines += ["", "[刚刚查询的实时状态：%s]" % p["live_query"].get("label", ""), live]
    return "\n".join(lines)


def qa_env(p):
    env = dict(os.environ)
    env.pop("CLAUDE_CONFIG_DIR", None)
    if p.get("claude_config_dir"):
        env["CLAUDE_CONFIG_DIR"] = p["claude_config_dir"]
    return env


# ---------------------------------------------------------------- management actions (no console windows)
def schtask_do(name, what, minutes=None):
    import pythoncom
    pythoncom.CoInitialize()
    try:
        _schtask_do(name, what, minutes)       # COM objects are released when it returns, before CoUninitialize
    finally:
        pythoncom.CoUninitialize()


def _schtask_do(name, what, minutes):
    svc = _task_service()
    folder, t = next(((f, t) for f, t in _walk_tasks(svc, skip_microsoft=False) if t.Name == name), (None, None))
    if t is None:
        raise RuntimeError("找不到定时任务 %s" % name)
    if what == "run":
        t.Run(None)
    elif what == "pause":
        t.Enabled = False
    elif what == "resume":
        t.Enabled = True
    elif what == "interval":
        d = t.Definition
        trgs = list(d.Triggers)
        if not trgs:
            raise RuntimeError("这个定时任务没有触发器")
        target = next((x for x in trgs if x.Repetition.Interval), trgs[0])
        target.Repetition.Interval = iso_duration(minutes)
        folder.RegisterTaskDefinition(t.Name, d, 4, None, None, d.Principal.LogonType)     # 4 = TASK_UPDATE


def detach_do(r, what, interval=None):
    def call(args):
        res = subprocess.run([PWSH, "-NoProfile", "-NonInteractive", "-File", DETACH] + args, capture_output=True, timeout=120, **hidden())
        if res.returncode != 0:
            raise RuntimeError((res.stdout + res.stderr).decode("utf-8", errors="replace")[-400:])
    if what in ("pause", "run", "interval"):
        try:
            call(["-Stop", r["name"]])
        except RuntimeError as e:
            if "not running" not in str(e):
                raise
    if what in ("resume", "run", "interval"):
        cmd = r["start_cmd"].replace("{interval}", str(interval or r.get("interval_min") or 15))
        call(["-Name", r["name"], "-Command", cmd, "-WorkDir", r["workdir"]])


def can_do(p, what, runner):
    r = p.get("runner") or {}
    if r.get("kind") == "schtask":
        return runner.get("exists") and {"run": not runner["paused"], "pause": not runner["paused"], "resume": runner["paused"],
                                         "interval": True}[what]
    if r.get("kind") == "detach" and not p.get("unregistered"):
        startable = bool(r.get("start_cmd") and r.get("workdir"))
        return {"run": startable and runner["running"], "pause": runner["running"], "resume": startable and not runner["running"],
                "interval": startable and "{interval}" in r.get("start_cmd", "")}[what]
    return False


ACTION_TEXT = {
    "run": ("立即检查一次", "马上运行一次监控（和定时运行完全一样：会按规程自动处理问题，比如重投作业）。\n\n确定现在运行吗？"),
    "pause": ("暂停监控", "暂停后，监控不再定时检查，也不会自动处理问题；正在跑的计算不受影响。\n\n确定暂停吗？"),
    "resume": ("恢复监控", "恢复后，监控按原来的周期继续检查和自动处理。\n\n确定恢复吗？"),
}


# ---------------------------------------------------------------- the window
class Hub(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("监控总台")
        self.geometry("1640x1000")
        self.minsize(1200, 760)
        self.configure(bg="white")
        self.lock = threading.Lock()
        self.projects, self.snaps, self.version, self.shown_version = [], {}, 0, -1
        self.sel = None
        self.cards = {}
        self.tr_path, self.tr_pos, self.tr_buf, self.tr_kind, self.tr_manual = None, 0, b"", None, False
        self.qa = {}
        self.qa_proc, self.qa_q, self.qa_cur = None, queue.Queue(), None
        self.wake = threading.Event()
        self.pg_cols = None
        self.res_path = None
        self._tip_win = None
        self._tip_after = None
        os.makedirs(os.path.join(HUB_DATA, "qa"), exist_ok=True)
        os.makedirs(REQ_DIR, exist_ok=True)
        self._style()
        self._build()
        threading.Thread(target=self._bg, daemon=True).start()
        self.protocol("WM_DELETE_WINDOW", self._close)
        self.after(300, self._tick)
        self.after(100, self._qa_pump)
        self.after(50, lambda: (self.deiconify(), self.lift(), self.focus_force()))

    # ---------- style / layout ----------
    def _style(self):
        s = ttk.Style(self)
        s.theme_use("clam")
        s.configure(".", font=(UI, 11), foreground=INK, background="white")
        s.configure("Treeview", font=(UI, 11), rowheight=30, foreground=INK, background="white", fieldbackground="white",
                    bordercolor=LINE, borderwidth=1)
        s.configure("Treeview.Heading", font=(UI, 11, "bold"), foreground=INK, background="white", relief="solid", borderwidth=1)
        s.map("Treeview", background=[("selected", SEL_BG)], foreground=[("selected", INK)])
        s.configure("TNotebook", background="white", borderwidth=0)
        s.configure("TNotebook.Tab", font=(UI, 12), padding=(16, 6), background="white", foreground=INK)
        s.map("TNotebook.Tab", background=[("selected", SEL_BG)], font=[("selected", (UI, 12, "bold"))])
        s.configure("TButton", font=(UI, 11), padding=(10, 4))
        s.configure("Accent.TButton", font=(UI, 11, "bold"), padding=(10, 4))
        s.configure("TLabelframe", background="white", bordercolor=LINE)
        s.configure("TLabelframe.Label", font=(UI, 12, "bold"), foreground=INK, background="white")
        self.option_add("*TCombobox*Listbox.font", (UI, 11))

    def _textbox(self, parent, height=10, font=(UI, 11), mono=False, editable=False):
        f = tk.Frame(parent, bg="white")
        t = tk.Text(f, wrap="word", height=height, font=(MONO, 11) if mono else font, fg=INK, bg="white", relief="solid",
                    borderwidth=1, padx=8, pady=6, state="normal" if editable else "disabled", highlightthickness=0, undo=editable)
        sb = ttk.Scrollbar(f, orient="vertical", command=t.yview)
        t.configure(yscrollcommand=sb.set)
        t.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        for tag, kw in {"claude": dict(foreground=INK, font=(UI, 11)), "tool": dict(foreground=BLUE),
                        "result": dict(foreground=GREEN), "error": dict(foreground=RED),
                        "head": dict(foreground=BROWN, font=(MONO, 11, "bold")), "you": dict(foreground=BLUE, font=(UI, 11, "bold")),
                        "red": dict(foreground=RED, font=(UI, 12, "bold")), "redtext": dict(foreground=RED, font=(UI, 11)),
                        "green": dict(foreground=GREEN, font=(UI, 11, "bold")), "bold": dict(font=(UI, 11, "bold")),
                        "hint": dict(foreground=BLUE, font=(UI, 11)), "paused": dict(foreground=BROWN, font=(UI, 12, "bold"))}.items():
            t.tag_configure(tag, **kw)
        return f, t

    def _tooltip(self, widget, text):
        """Attach a hover-only helper. Functional state stays in the main UI; explanatory text lives here."""
        widget._hub_tip = text
        widget.bind("<Enter>", lambda e, w=widget: self._tip_schedule(w), add="+")
        widget.bind("<Leave>", lambda e: self._tip_hide(), add="+")
        widget.bind("<ButtonPress>", lambda e: self._tip_hide(), add="+")
        return widget

    @staticmethod
    def _set_tooltip(widget, text):
        widget._hub_tip = text

    def _tip_schedule(self, widget):
        self._tip_hide()
        self._tip_after = self.after(350, lambda: self._tip_show(widget))

    def _tip_show(self, widget):
        self._tip_after = None
        text = getattr(widget, "_hub_tip", "")
        if not text or not widget.winfo_exists():
            return
        win = tk.Toplevel(self)
        win.wm_overrideredirect(True)
        try:
            win.wm_attributes("-topmost", True)
        except tk.TclError:
            pass
        label = tk.Label(win, text=text, font=(UI, 10), fg=INK, bg="#fffbe6", relief="solid", borderwidth=1,
                         justify="left", anchor="w", wraplength=460, padx=9, pady=7)
        label.pack()
        win.update_idletasks()
        x = widget.winfo_rootx() + 8
        y = widget.winfo_rooty() + widget.winfo_height() + 7
        x = min(x, max(0, win.winfo_screenwidth() - win.winfo_reqwidth() - 8))
        y = min(y, max(0, win.winfo_screenheight() - win.winfo_reqheight() - 8))
        win.wm_geometry("+%d+%d" % (x, y))
        self._tip_win = win

    def _tip_hide(self):
        if self._tip_after is not None:
            try:
                self.after_cancel(self._tip_after)
            except tk.TclError:
                pass
            self._tip_after = None
        if self._tip_win is not None:
            try:
                self._tip_win.destroy()
            except tk.TclError:
                pass
            self._tip_win = None

    def _info_button(self, parent, text):
        b = ttk.Button(parent, text="ⓘ 说明", takefocus=False)
        self._tooltip(b, text)
        return b

    def _build(self):
        side = tk.Frame(self, bg="white", width=380, highlightthickness=1, highlightbackground=LINE)
        side.pack(side="left", fill="y")
        side.pack_propagate(False)
        tk.Label(side, text="监控总台", font=(UI, 20, "bold"), fg=INK, bg="white").pack(anchor="w", padx=16, pady=(14, 2))
        tk.Label(side, text="本机所有项目的计算监控", font=(UI, 11), fg=INK, bg="white").pack(anchor="w", padx=16, pady=(0, 10))
        self.side_list = tk.Frame(side, bg="white")
        self.side_list.pack(fill="both", expand=True, padx=8)
        bot = tk.Frame(side, bg="white")
        bot.pack(fill="x", padx=12, pady=(8, 2))
        b = ttk.Button(bot, text="＋ 新建监控任务", style="Accent.TButton", command=self._new_request)
        b.pack(side="left")
        self._tooltip(b, "填写并提交新的监控请求；后台会按你的权限范围设置监控并登记到总台。")
        b = ttk.Button(bot, text="使用说明", command=lambda: os.path.exists(README) and os.startfile(README))
        b.pack(side="left", padx=6)
        self._tooltip(b, "打开监控总台的详细使用说明和状态文件规范。")
        bot2 = tk.Frame(side, bg="white")
        bot2.pack(fill="x", padx=12, pady=(4, 4))
        b = ttk.Button(bot2, text="立即刷新", command=self.wake.set)
        b.pack(side="left")
        self._tooltip(b, "立即唤醒总台刷新监控状态；不会启动、停止或修改计算任务。")
        b = ttk.Button(bot2, text="编辑项目表", command=lambda: os.startfile(REGISTRY))
        b.pack(side="left", padx=6)
        self._tooltip(b, "打开项目注册表 monitor_hub_projects.json；这里决定哪些正式项目出现在总台。")
        self.side_time = tk.Label(side, text="", font=(UI, 10), fg=INK, bg="white")
        self.side_time.pack(anchor="w", padx=16, pady=(2, 8))

        self.main = tk.Frame(self, bg="white")
        self.main.pack(side="left", fill="both", expand=True)
        self._build_overview()
        self._build_project()
        self._show_overview()

    def _build_overview(self):
        f = self.ov = tk.Frame(self.main, bg="white")
        top = tk.Frame(f, bg="white")
        top.pack(fill="x", padx=20, pady=(16, 2))
        tk.Label(top, text="总览", font=(UI, 18, "bold"), fg=INK, bg="white").pack(side="left")
        info = self._info_button(top, "每行代表一个项目监控。双击项目或按 Enter 可进入详情、后台处理记录、最终结果和提问。\n\n状态颜色只用于快速识别；具体结论以“当前情况”和项目详情为准。")
        info.pack(side="right")
        box = tk.Frame(f, bg="white")
        box.pack(fill="x", padx=20, pady=10)
        cols = [("name", "项目", 290), ("health", "状态", 140), ("summary", "当前情况", 380), ("last", "上次检查", 120), ("next", "下次检查", 150)]
        self.ov_tree = ttk.Treeview(box, columns=[c for c, _, _ in cols], show="headings", height=6)
        for c, h, w in cols:
            self.ov_tree.heading(c, text=h)
            self.ov_tree.column(c, width=w, minwidth=w if c != "summary" else 200, anchor="w", stretch=(c == "summary"))
        for h, (lab, col, _) in HEALTH.items():
            self.ov_tree.tag_configure(h, foreground=col)
        self.ov_tree.pack(fill="x")
        self.ov_tree.bind("<Double-1>", lambda e: self.ov_tree.focus() and self._select(self.ov_tree.focus()))
        self.ov_tree.bind("<Return>", lambda e: self.ov_tree.focus() and self._select(self.ov_tree.focus()))
        recent_head = tk.Frame(f, bg="white")
        recent_head.pack(fill="x", padx=20, pady=(10, 2))
        tk.Label(recent_head, text="最近的后台处理（所有项目）", font=(UI, 13, "bold"), fg=INK, bg="white").pack(side="left")
        info = self._info_button(recent_head, "这里记录监控自动请后台 Claude 处理问题，或按你的请求设置新监控的过程。最新记录在最上面；双击一行可查看完整处理过程。")
        info.pack(side="right")
        box2 = tk.Frame(f, bg="white")
        box2.pack(fill="both", expand=True, padx=20, pady=(6, 10))
        cols = [("time", "时间", 130), ("proj", "项目", 260), ("state", "结果", 100), ("summary", "摘要", 600)]
        self.rc_tree = ttk.Treeview(box2, columns=[c for c, _, _ in cols], show="headings", height=8)
        for c, h, w in cols:
            self.rc_tree.heading(c, text=h)
            self.rc_tree.column(c, width=w, anchor="w", stretch=(c == "summary"))
        for tag, col in (("ok", GREEN), ("failed", RED), ("running", BLUE)):
            self.rc_tree.tag_configure(tag, foreground=col)
        self.rc_tree.pack(fill="both", expand=True)
        self.rc_tree.bind("<Double-1>", lambda e: self._open_recent())

    def _build_project(self):
        f = self.pv = tk.Frame(self.main, bg="white")
        head = tk.Frame(f, bg="white")
        head.pack(fill="x", padx=20, pady=(14, 0))
        self.pv_name = tk.Label(head, text="", font=(UI, 18, "bold"), fg=INK, bg="white")
        self.pv_name.pack(side="left")
        self.pv_area = tk.Label(head, text="", font=(UI, 11), fg=INK, bg="white")
        self.pv_area.pack(side="left", padx=12, pady=(6, 0))
        self.btns = {}
        action_tips = {
            "folder": "打开这个项目在本机登记的工作目录。",
            "interval": "修改监控检查周期。只改变监控频率，不改变科学计算参数。",
            "resume": "恢复已暂停的监控；计算任务本身不会因此重新开始。",
            "pause": "暂停定时监控和自动处理；已经在跑的计算不受影响。",
            "run": "立即执行一轮和定时检查相同的监控逻辑；若规程允许，可能触发自动恢复或重投。"
        }
        for key, label in (("folder", "打开文件夹"), ("interval", "检查间隔…"), ("resume", "恢复监控"), ("pause", "暂停监控"), ("run", "立即检查一次")):
            b = ttk.Button(head, text=label, command=lambda k=key: self._action(k))
            b.pack(side="right", padx=4)
            self._tooltip(b, action_tips[key])
            self.btns[key] = b
        stat = tk.Frame(f, bg="white")
        stat.pack(fill="x", padx=20, pady=(6, 0))
        self.pv_health = tk.Label(stat, text="", font=(UI, 14, "bold"), fg=INK, bg="white")
        self.pv_health.pack(side="left")
        self.pv_headline = tk.Label(stat, text="", font=(UI, 12), fg=INK, bg="white", justify="left", anchor="w", wraplength=1000)
        self.pv_headline.pack(side="left", padx=14, fill="x", expand=True)
        self.pv_runner = tk.Label(f, text="", font=(UI, 11), fg=INK, bg="white", anchor="w", justify="left")
        self.pv_runner.pack(fill="x", padx=20, pady=(4, 0))
        self.pv_alert_f, self.pv_alert = self._textbox(f, height=3)
        self.pv_alert.configure(borderwidth=2)

        self.nb = ttk.Notebook(f)
        self.nb.pack(fill="both", expand=True, padx=20, pady=(10, 14))
        # progress
        t1 = tk.Frame(self.nb, bg="white")
        self.nb.add(t1, text="进度")
        pgh = tk.Frame(t1, bg="white")
        pgh.pack(fill="x", pady=(8, 0))
        info = self._info_button(pgh, "颜色：绿色 = 已完成，蓝色 = 运行中，棕色 = 排队中，红色 = 异常，黑色 = 其他。\n\n选中任务行会在下方显示 task_id、job_id、主机、脚本、命令和参数；双击或按 Enter 可打开任务目录、日志或结果文件。")
        info.pack(side="right")
        box = tk.Frame(t1, bg="white")
        box.pack(fill="both", expand=True, pady=(4, 0))
        self.pg_tree = ttk.Treeview(box, show="headings")
        vs = ttk.Scrollbar(box, orient="vertical", command=self.pg_tree.yview)
        hs = ttk.Scrollbar(box, orient="horizontal", command=self.pg_tree.xview)
        self.pg_tree.configure(yscrollcommand=vs.set, xscrollcommand=hs.set)
        self.pg_tree.grid(row=0, column=0, sticky="nsew")
        self.pg_tree.bind("<Double-1>", self._open_progress_task)
        self.pg_tree.bind("<Return>", self._open_progress_task)
        self.pg_tree.bind("<<TreeviewSelect>>", lambda e: self._show_progress_task_meta())
        vs.grid(row=0, column=1, sticky="ns")
        hs.grid(row=1, column=0, sticky="ew")
        box.rowconfigure(0, weight=1)
        box.columnconfigure(0, weight=1)
        for tag, col in (("done", GREEN), ("bad", RED), ("run", BLUE), ("queue", BROWN), ("other", INK)):
            self.pg_tree.tag_configure(tag, foreground=col)
        nf, self.pg_notes = self._textbox(t1, height=5)
        nf.pack(fill="x", pady=(6, 0))
        # takeovers
        t2 = tk.Frame(self.nb, bg="white")
        self.nb.add(t2, text="后台处理记录")
        tkh = tk.Frame(t2, bg="white")
        tkh.pack(fill="x", pady=(8, 4))
        self.tk_info = self._info_button(tkh, "监控发现自己处理不了的问题时，会自动请一个后台 Claude 按规程处理。左边选一次处理，右边显示它每一步做了什么；正在处理的记录会实时更新。")
        self.tk_info.pack(side="right")
        pan = ttk.PanedWindow(t2, orient="horizontal")
        pan.pack(fill="both", expand=True)
        lf = tk.Frame(pan, bg="white")
        self.tk_tree = ttk.Treeview(lf, columns=("time", "state", "summary"), show="headings", height=12)
        for c, h, w in (("time", "时间", 110), ("state", "结果", 90), ("summary", "摘要", 260)):
            self.tk_tree.heading(c, text=h)
            self.tk_tree.column(c, width=w, anchor="w", stretch=(c == "summary"))
        for tag, col in (("ok", GREEN), ("failed", RED), ("running", BLUE)):
            self.tk_tree.tag_configure(tag, foreground=col)
        self.tk_tree.pack(fill="both", expand=True)
        self.tk_tree.bind("<<TreeviewSelect>>", lambda e: self._pick_takeover(manual=True))
        rf = tk.Frame(pan, bg="white")
        bar = tk.Frame(rf, bg="white")
        bar.pack(fill="x", pady=(0, 4))
        self.tk_state = tk.Label(bar, text="", font=(UI, 11, "bold"), fg=INK, bg="white")
        self.tk_state.pack(side="left")
        b = ttk.Button(bar, text="用文本编辑器打开", command=lambda: self.tr_path and os.startfile(self.tr_path))
        b.pack(side="right")
        self._tooltip(b, "在系统默认文本编辑器中打开当前这次后台处理的原始记录。")
        trf, self.tr_text = self._textbox(rf, height=20, mono=True)
        trf.pack(fill="both", expand=True)
        pan.add(lf, weight=1)
        pan.add(rf, weight=3)
        # results
        self.t_res = tk.Frame(self.nb, bg="white")
        self.nb.add(self.t_res, text="最终结果")
        resh = tk.Frame(self.t_res, bg="white")
        resh.pack(fill="x", pady=(8, 4))
        info = self._info_button(resh, "这里列出项目登记的最终交付文件，例如结果表、RESULTS.md 等。文件尚未生成时会显示“尚未生成”；完成后可在右侧预览并打开原文件。")
        info.pack(side="right")
        pan2 = ttk.PanedWindow(self.t_res, orient="horizontal")
        pan2.pack(fill="both", expand=True)
        lf2 = tk.Frame(pan2, bg="white")
        self.res_tree = ttk.Treeview(lf2, columns=("label", "state"), show="headings", height=12)
        for c, h, w in (("label", "文件", 220), ("state", "状态", 190)):
            self.res_tree.heading(c, text=h)
            self.res_tree.column(c, width=w, anchor="w", stretch=(c == "label"))
        self.res_tree.tag_configure("missing", foreground=BROWN)
        self.res_tree.pack(fill="both", expand=True)
        self.res_tree.bind("<<TreeviewSelect>>", lambda e: self._pick_result())
        rf2 = tk.Frame(pan2, bg="white")
        bar = tk.Frame(rf2, bg="white")
        bar.pack(fill="x", pady=(0, 4))
        self.res_label = tk.Label(bar, text="", font=(UI, 11, "bold"), fg=INK, bg="white")
        self.res_label.pack(side="left")
        b = ttk.Button(bar, text="用文本编辑器打开", command=lambda: self.res_path and os.path.exists(self.res_path) and os.startfile(self.res_path))
        b.pack(side="right")
        self._tooltip(b, "在系统默认文本编辑器中打开当前选中的最终结果文件。")
        rtf, self.res_text = self._textbox(rf2, height=20)
        rtf.pack(fill="both", expand=True)
        pan2.add(lf2, weight=1)
        pan2.add(rf2, weight=3)
        # questions
        t3 = tk.Frame(self.nb, bg="white")
        self.nb.add(t3, text="提问")
        bar = tk.Frame(t3, bg="white")
        bar.pack(fill="x", pady=(0, 6))
        tk.Label(bar, text="问谁：", font=(UI, 11), fg=INK, bg="white").pack(side="left")
        self.qa_box = ttk.Combobox(bar, state="readonly", width=46, font=(UI, 11))
        self.qa_box.pack(side="left")
        self.qa_box.bind("<<ComboboxSelected>>", lambda e: self._qa_new_thread())
        self.qa_info = self._info_button(bar, "这里可以向 Claude 查询当前项目。提问会话只能读取文件，不能运行命令，也不会修改作业。总台会附带当前监控状态和实时查询结果。\n\n选择“接着问某次处理”会携带那次后台处理的上下文，通常比新对话占用更多上下文。\n\n示例：现在进度怎样？上一次后台处理做了什么？哪个作业最慢，为什么？")
        self.qa_info.pack(side="right", padx=(6, 0))
        b = ttk.Button(bar, text="新对话", command=self._qa_new_thread)
        b.pack(side="right")
        self._tooltip(b, "清空这个项目当前提问会话的 session，从新的上下文开始问。")
        qf, self.qa_text = self._textbox(t3, height=14)
        qf.pack(fill="both", expand=True)
        inp = tk.Frame(t3, bg="white")
        inp.pack(fill="x", pady=(6, 0))
        self.qa_in = tk.Text(inp, height=3, font=(UI, 12), fg=INK, bg="white", relief="solid", borderwidth=1, wrap="word", padx=6, pady=4)
        self.qa_in.pack(side="left", fill="x", expand=True)
        self.qa_in.bind("<Return>", self._qa_enter)
        self.qa_btn = ttk.Button(inp, text="发送", command=self._qa_ask)
        self.qa_btn.pack(side="left", padx=(6, 0), fill="y")
        self._tooltip(self.qa_btn, "发送当前问题。Enter 发送，Shift+Enter 换行。")
        self.qa_state = tk.Label(t3, text="", font=(UI, 10), fg=INK, bg="white", anchor="w")
        self.qa_state.pack(fill="x", pady=(4, 0))
        # log
        t4 = tk.Frame(self.nb, bg="white")
        self.nb.add(t4, text="操作日志")
        bar = tk.Frame(t4, bg="white")
        bar.pack(fill="x", pady=(8, 4))
        self.log_label = tk.Label(bar, text="", font=(UI, 11), fg=INK, bg="white", anchor="w")
        self.log_label.pack(side="left")
        b = ttk.Button(bar, text="打开完整日志", command=lambda: self._open_key("log"))
        b.pack(side="right")
        self._tooltip(b, "打开当前项目登记的完整监控日志。")
        b = ttk.Button(bar, text="打开最新接管报告", command=lambda: self._open_key("report"))
        b.pack(side="right", padx=6)
        self._tooltip(b, "打开当前项目最近一次后台接管生成的报告文件。")
        lgf, self.log_text = self._textbox(t4, height=20, mono=True)
        lgf.pack(fill="both", expand=True)
        # live query
        self.t5 = tk.Frame(self.nb, bg="white")
        self.nb.add(self.t5, text="实时查询")
        bar = tk.Frame(self.t5, bg="white")
        bar.pack(fill="x", pady=(8, 4))
        self.lq_btn = ttk.Button(bar, text="查询", command=self._live_query)
        self.lq_btn.pack(side="left")
        self._tooltip(self.lq_btn, "直接向服务器查询当前状态，只读，不经过监控逻辑，也不会修改或重启作业。")
        lqf, self.lq_text = self._textbox(self.t5, height=20, mono=True)
        lqf.pack(fill="both", expand=True)

    # ---------- text helpers ----------
    @staticmethod
    def _set(t, lines):
        t.configure(state="normal")
        t.delete("1.0", "end")
        for ln, tag in lines:
            t.insert("end", ln + "\n", tag)
        t.configure(state="disabled")

    @staticmethod
    def _append(t, line, tag=None, follow=None):
        at_end = t.yview()[1] > 0.98 if follow is None else follow
        if tag is None:
            s = line.lstrip()
            tag = ("head" if s.startswith(("[start]", "[end]", "[job", "——")) else "error" if s.startswith("ERROR") else
                   "tool" if s.startswith("->") else "result" if s.startswith("<-") else "claude")
        t.configure(state="normal")
        t.insert("end", line + "\n", tag)
        t.configure(state="disabled")
        if at_end:
            t.see("end")

    def _project(self, pid=None):
        with self.lock:
            return next((x for x in self.projects if x["id"] == (pid or self.sel)), None), self.snaps.get(pid or self.sel)

    # ---------- background refresh ----------
    def _bg(self):
        import pythoncom
        pythoncom.CoInitialize()
        while True:
            try:
                sysinfo = probe()
                projects = load_projects(sysinfo)
                snaps = {p["id"]: snapshot(p, sysinfo) for p in projects}
                with self.lock:
                    self.projects, self.snaps = projects, snaps
                    self.version += 1
                    self.bg_error = sysinfo.get("error")
            except Exception as e:  # noqa: BLE001
                with self.lock:
                    self.bg_error = repr(e)
            self.wake.wait(REFRESH_S)
            self.wake.clear()

    def _tick(self):
        try:
            with self.lock:
                v = self.version
            if v != self.shown_version:
                self.shown_version = v
                self._render_all()
            if self.sel:
                self._follow_transcript()
        finally:
            self.after(TICK_MS, self._tick)

    def _render_all(self):
        with self.lock:
            projects, snaps, err = list(self.projects), dict(self.snaps), getattr(self, "bg_error", None)
        self.side_time.configure(text="上次刷新 %s（每 %d 秒自动刷新）%s" % (time.strftime("%H:%M:%S"), REFRESH_S, "  " + err if err else ""),
                                 fg=RED if err else INK)
        ids = [p["id"] for p in projects]
        if list(self.cards) != ["__overview__"] + ids:
            self._build_cards(projects)
        for p in projects:
            self._update_card(p, snaps[p["id"]])
        self._update_card_overview(projects, snaps)
        self._render_overview(projects, snaps)
        if self.sel and self.sel in snaps:
            self._render_project(next(p for p in projects if p["id"] == self.sel), snaps[self.sel])

    # ---------- sidebar ----------
    def _build_cards(self, projects):
        for w in self.side_list.winfo_children():
            w.destroy()
        self.cards = {}
        self._make_card("__overview__", "总览", "所有项目一览")
        section = None
        for p in projects:
            sec = "其他监控（未登记）" if p.get("unregistered") else "新任务" if p.get("builtin") else "项目"
            if sec != section:
                section = sec
                tk.Label(self.side_list, text=sec, font=(UI, 11, "bold"), fg=INK, bg="white").pack(anchor="w", padx=8, pady=(12, 2))
            self._make_card(p["id"], p["name"], p.get("area", ""))

    def _make_card(self, pid, title, sub):
        c = tk.Frame(self.side_list, bg="white", highlightthickness=1, highlightbackground=LINE, cursor="hand2")
        c.pack(fill="x", pady=3)
        top = tk.Frame(c, bg="white")
        top.pack(fill="x", padx=10, pady=(6, 0))
        badge = tk.Label(top, text="", font=(UI, 11, "bold"), fg=INK, bg="white")
        badge.pack(side="right")
        name = tk.Label(top, text=title, font=(UI, 12, "bold"), fg=INK, bg="white", anchor="w")
        name.pack(side="left", fill="x", expand=True)
        area = tk.Label(c, text=sub, font=(UI, 10), fg=INK, bg="white", anchor="w")
        area.pack(fill="x", padx=10)
        line = tk.Label(c, text="", font=(UI, 11), fg=INK, bg="white", anchor="w", justify="left", wraplength=340)
        line.pack(fill="x", padx=10, pady=(0, 6))
        for w in (c, top, badge, name, area, line):
            w.bind("<Button-1>", lambda e, i=pid: self._select(None if i == "__overview__" else i))
        self.cards[pid] = dict(frame=c, widgets=(c, top, badge, name, area, line), badge=badge, line=line)

    def _paint_card(self, pid):
        for k, c in self.cards.items():
            on = (k == pid) or (pid is None and k == "__overview__")
            for w in c["widgets"]:
                w.configure(bg=SEL_BG if on else "white")
            c["frame"].configure(highlightbackground=BLUE if on else LINE, highlightthickness=2 if on else 1)

    def _update_card(self, p, s):
        c = self.cards.get(p["id"])
        if c:
            lab, col, _ = HEALTH[s["health"]]
            c["badge"].configure(text=lab, fg=col)
            c["line"].configure(text=s.get("problem") or s.get("summary") or s.get("headline", ""))

    def _update_card_overview(self, projects, snaps):
        c = self.cards.get("__overview__")
        if c:
            need = [p["name"] for p in projects if snaps[p["id"]]["health"] in ("attention", "error", "stale")]
            c["badge"].configure(text=("⚠ %d 个要看" % len(need)) if need else "● 都正常", fg=RED if need else GREEN)
            c["line"].configure(text=("需要看：" + "、".join(need)) if need else "没有需要你处理的事")

    # ---------- overview ----------
    def _render_overview(self, projects, snaps):
        order = sorted(projects, key=lambda p: (ORDER.index(snaps[p["id"]]["health"]), bool(p.get("builtin")), bool(p.get("unregistered"))))
        cur = self.ov_tree.focus()
        self.ov_tree.delete(*self.ov_tree.get_children())
        for p in order:
            s = snaps[p["id"]]
            r = s["runner"]
            nxt = "已暂停" if r.get("paused") else s.get("next") or short_time(r.get("next"))
            self.ov_tree.insert("", "end", iid=p["id"], tags=(s["health"],),
                                values=(p["name"], HEALTH[s["health"]][0], s.get("problem") or s.get("summary") or s.get("headline", ""),
                                        ago(s.get("updated")) if s.get("updated") else short_time(r.get("last")), nxt))
        if cur and self.ov_tree.exists(cur):
            self.ov_tree.focus(cur)
        self.ov_tree.configure(height=max(3, len(order)))
        recent = sorted(((t, p) for p in projects for t in snaps[p["id"]].get("takeovers", [])), key=lambda x: x[0]["time"] or 0, reverse=True)[:15]
        self.rc_tree.delete(*self.rc_tree.get_children())
        for t, p in recent:
            self.rc_tree.insert("", "end", iid="%s|%s" % (p["id"], t["key"]), tags=(t["state"],),
                                values=(t["label"], p["name"], TAKEOVER_STATE[t["state"]], t["summary"]))

    def _open_recent(self):
        iid = self.rc_tree.focus()
        if not iid:
            return
        pid, key = iid.split("|", 1)
        self._select(pid)
        self.nb.select(1)
        if self.tk_tree.exists(key):
            self.tr_manual = True
            self.tk_tree.selection_set(key)
            self.tk_tree.see(key)

    def _show_overview(self):
        self.pv.pack_forget()
        self.ov.pack(fill="both", expand=True)
        self._paint_card(None)

    # ---------- project page ----------
    def _select(self, pid):
        self.sel = pid
        if pid is None:
            self._show_overview()
            return
        self.ov.pack_forget()
        self.pv.pack(fill="both", expand=True)
        self._paint_card(pid)
        self.tr_path, self.tr_manual, self.res_path, self.pg_cols = None, False, None, None
        self._set(self.tr_text, [])
        self._set(self.res_text, [])
        self._qa_restore(pid)
        p, s = self._project(pid)
        if p and s:
            self._render_project(p, s)
            self.nb.select(0)

    def _render_project(self, p, s):
        r = s["runner"]
        lab, col, _ = HEALTH[s["health"]]
        self.pv_name.configure(text=p["name"])
        self.pv_area.configure(text=p.get("area", ""))
        self.pv_health.configure(text=lab, fg=col)
        head = s.get("problem") or s.get("headline", "")
        if s.get("summary") and s.get("headline") and not s.get("problem"):
            head = "%s（%s）" % (s["headline"].rstrip("。"), s["summary"])
        self.pv_headline.configure(text=head)
        upd = "状态更新于 %s（%s）" % (hm(s["updated"]), ago(s["updated"])) if s.get("updated") else ""
        nxt = "下次检查 %s" % s["next"] if s.get("next") and not r.get("paused") else ""
        self.pv_runner.configure(text="监控方式：%s%s" % (r.get("text", ""), ("\n" + "  ·  ".join(x for x in (upd, nxt) if x)) if (upd or nxt) else ""))
        alert = []
        if s.get("attention"):
            alert.append(("需要你处理：", "red"))
            alert += [("  " + a, "redtext") for a in s["attention"]]
        elif s.get("working") and s["health"] == "working":
            alert.append((s["working"], "hint"))
        elif s.get("history"):
            alert.append(("监控暂停前最后记录的问题（仅供参考）：", "paused"))
            alert += [("  " + a, None) for a in s["history"]]
        if alert:
            self._set(self.pv_alert, alert)
            self.pv_alert.configure(height=min(6, len(alert) + 1))
            self.pv_alert_f.pack(fill="x", padx=20, pady=(8, 0), before=self.nb)
        else:
            self.pv_alert_f.pack_forget()
        for k in ("run", "pause", "resume", "interval"):
            self.btns[k].configure(state="normal" if can_do(p, k, r) else "disabled")
        self.btns["folder"].configure(state="normal" if p.get("dir") else "disabled")
        # progress
        tb = s["table"]
        cols = tb["cols"]
        if cols != self.pg_cols:
            self.pg_cols = cols
            ids = ["c%d" % i for i in range(len(cols))]
            self.pg_tree.configure(columns=ids)
            for i, c in zip(ids, cols):
                longest = max([len(c)] + [len(r_[cols.index(c)]) for r_ in tb["rows"]]) if tb["rows"] else len(c)
                self.pg_tree.heading(i, text=c)
                self.pg_tree.column(i, width=min(420, max(80, 14 * longest)), anchor="w", stretch=True)
        self.pg_tree.delete(*self.pg_tree.get_children())
        self._pg_meta = list(tb.get("row_meta") or [])
        if len(self._pg_meta) < len(tb["rows"]):
            self._pg_meta += [{} for _ in range(len(tb["rows"]) - len(self._pg_meta))]
        for i, (row, tag) in enumerate(zip(tb["rows"], tb["tags"])):
            self.pg_tree.insert("", "end", iid=str(i), values=row, tags=(tag,) if tag else ())
        self.pg_tree.configure(height=max(3, min(18, len(tb["rows"]))))
        notes = [(e, None) for e in s.get("extras", []) if e]
        if s.get("notes"):
            notes += [("备注：", "bold")] + [("  • " + n, None) for n in s["notes"]]
        if not tb["rows"]:
            notes = [(s.get("headline", ""), None)] + notes
        self._pg_base_notes = notes or [("（没有备注）", None)]
        self._set(self.pg_notes, self._pg_base_notes)
        # takeovers
        self._set_tooltip(self.tk_info, ("每个新任务请求由一个后台 Claude 办理：写监控脚本、登记到总台、启动监控并做第一轮检查。左边选一个请求，右边显示它每一步做了什么。"
                                         if p.get("builtin") else
                                         "监控发现自己处理不了的问题时，会自动请一个后台 Claude 按规程处理。左边选一次处理，右边显示它每一步做了什么；正在处理的会实时更新。"))
        tks = s.get("takeovers", [])
        keys = [t["key"] for t in tks]
        sel = self.tk_tree.selection()
        cur = sel[0] if sel else None
        self._tk_list = tks
        self.tk_tree.delete(*self.tk_tree.get_children())
        for t in tks:
            self.tk_tree.insert("", "end", iid=t["key"], tags=(t["state"],), values=(t["label"], TAKEOVER_STATE[t["state"]], t["summary"]))
        if cur and self.tk_tree.exists(cur) and self.tr_manual:
            self.tk_tree.selection_set(cur)
        elif keys:
            self.tk_tree.selection_set(keys[0])
        else:
            self._set(self.tr_text, [("这里还没有记录。监控发现需要处理的问题时会自动启动后台 Claude，过程会显示在这里。", "claude")])
            self.tk_state.configure(text="")
        # results
        res = s.get("results_list", [])
        self.nb.tab(self.t_res, state="normal" if res else "hidden")
        rsel = self.res_tree.selection()
        rcur = rsel[0] if rsel else None
        self.res_tree.delete(*self.res_tree.get_children())
        for i, (label, path) in enumerate(res):
            ok = os.path.exists(path)
            self.res_tree.insert("", "end", iid=str(i), tags=() if ok else ("missing",),
                                 values=(label, ("更新于 " + hm(mtime(path))) if ok else "尚未生成"))
        self._res_list = res
        if res:
            self.res_tree.selection_set(rcur if rcur and self.res_tree.exists(rcur) else "0")
        # questions
        targets = ["助手（新对话）"] + ["接着问 %s 那次处理" % t["label"] for t in tks if t["kind"] == "jsonl" and t["state"] != "running"]
        if list(self.qa_box.cget("values")) != targets:
            keep = self.qa_box.get()
            self.qa_box.configure(values=targets)
            self.qa_box.set(keep if keep in targets else targets[0])
        # log
        lp = p.get("log")
        self.log_label.configure(text=("%s（最后更新 %s）" % (lp, ago(mtime(lp)))) if lp else "这个监控没有操作日志")
        if lp:
            first = self.log_text.yview()[1] > 0.98 or self.log_text.index("end-1c") == "1.0"
            self._set(self.log_text, [(ln, None) for ln in tail_text(lp, 40000).splitlines()[-120:]])
            if first:
                self.log_text.see("end")
        else:
            self._set(self.log_text, [])
        lq = p.get("live_query")
        self.nb.tab(self.t5, state="normal" if lq else "hidden")
        if lq:
            self.lq_btn.configure(text=lq.get("label", "查询"))

    def _progress_meta(self, iid=None):
        if iid is None:
            sel = self.pg_tree.selection()
            iid = sel[0] if sel else self.pg_tree.focus()
        try:
            meta = self._pg_meta[int(iid)] if iid and getattr(self, "_pg_meta", None) else {}
        except (ValueError, IndexError):
            meta = {}
        return meta if isinstance(meta, dict) else {}

    def _show_progress_task_meta(self):
        meta = self._progress_meta()
        lines = list(getattr(self, "_pg_base_notes", []))
        if not meta:
            self._set(self.pg_notes, lines)
            return
        detail = []
        for key, label in (("task_id", "任务"), ("job_id", "作业号"), ("host", "主机"), ("script", "脚本"),
                           ("command", "命令"), ("open_path", "路径"), ("log", "日志"), ("result", "结果")):
            if meta.get(key):
                detail.append("%s：%s" % (label, meta[key]))
        params = meta.get("params")
        if isinstance(params, dict) and params:
            detail.append("参数：" + "；".join("%s=%s" % (k, v) for k, v in params.items()))
        if detail:
            lines += [("", None), ("任务详情：", "bold")] + [("  " + x, None) for x in detail]
        self._set(self.pg_notes, lines)

    def _open_progress_task(self, event=None):
        iid = self.pg_tree.identify_row(event.y) if event is not None and hasattr(event, "y") else None
        if iid:
            self.pg_tree.selection_set(iid)
            self.pg_tree.focus(iid)
        else:
            sel = self.pg_tree.selection()
            iid = sel[0] if sel else self.pg_tree.focus()
        if not iid:
            return
        meta = self._progress_meta(iid)
        target = next((meta.get(k) for k in ("open_path", "path", "workdir", "log", "result") if meta.get(k)), None)
        if target and os.path.exists(target):
            os.startfile(target)
            return
        detail = []
        for key, label in (("task_id", "任务"), ("job_id", "作业号"), ("host", "主机"), ("script", "脚本"),
                           ("command", "命令"), ("log", "日志"), ("result", "结果")):
            if meta.get(key):
                detail.append("%s：%s" % (label, meta[key]))
        params = meta.get("params")
        if isinstance(params, dict) and params:
            detail.append("参数：" + "；".join("%s=%s" % (k, v) for k, v in params.items()))
        messagebox.showinfo("任务详情", "\n".join(detail) if detail else
                            "这个任务还没有提供可打开路径。请让监控在 hub_status.json 的 table.row_meta 中写入 open_path、log 或 result。",
                            parent=self)

    def _open_key(self, key):
        p, _ = self._project()
        if p and p.get(key) and os.path.exists(p[key]):
            os.startfile(p[key])

    def _pick_result(self):
        sel = self.res_tree.selection()
        if not sel or not getattr(self, "_res_list", None):
            return
        label, path = self._res_list[int(sel[0])]
        if path == self.res_path and mtime(path) == getattr(self, "res_mtime", None):
            return
        self.res_path, self.res_mtime = path, mtime(path)
        self.res_label.configure(text=label)
        if os.path.exists(path):
            self._set(self.res_text, [(ln, None) for ln in read_text(path, 300000).splitlines()])
        else:
            self._set(self.res_text, [("这个文件还没生成。计算全部完成后，监控会把最终结果写到：", "bold"), (path, None)])

    # ---------- takeover transcript ----------
    def _pick_takeover(self, manual=False):
        sel = self.tk_tree.selection()
        if not sel:
            return
        t = next((x for x in getattr(self, "_tk_list", []) if x["key"] == sel[0]), None)
        if not t:
            return
        if manual and self.focus_get() is self.tk_tree:
            self.tr_manual = True
        state = {"ok": "已完成", "failed": "失败", "running": "正在处理（实时更新）"}[t["state"]]
        self.tk_state.configure(text="%s · %s" % (t["label"], state), fg={"ok": GREEN, "failed": RED, "running": BLUE}[t["state"]])
        if t["path"] != self.tr_path:
            self.tr_path, self.tr_pos, self.tr_buf, self.tr_kind = t["path"], 0, b"", t["kind"]
            self._set(self.tr_text, [])
            self._follow_transcript()

    def _follow_transcript(self):
        p = self.tr_path
        if not p or not os.path.exists(p):
            return
        try:
            with open(p, "rb") as f:
                f.seek(self.tr_pos)
                raw = f.read()
        except OSError:
            return
        if not raw:
            return
        # first load always goes to the end (an unmapped Text reports yview (0, 0)); later batches follow only if the
        # user has not scrolled up; decided once per batch because yview is stale until idle
        follow = self.tr_pos == 0 or self.tr_text.yview()[1] > 0.98
        self.tr_pos += len(raw)
        if self.tr_kind == "md":
            for ln in raw.decode("utf-8", errors="replace").splitlines():
                self._append(self.tr_text, ln, "claude", follow=follow)
        else:
            self.tr_buf += raw
            *complete, self.tr_buf = self.tr_buf.split(b"\n")
            for ln in cs.render_stream(b"\n".join(complete).decode("utf-8", errors="replace")):
                if not ln.startswith("[mcp-sdk]"):
                    self._append(self.tr_text, ln, follow=follow)
        if follow:
            self.tr_text.see("end")
            self.after(400, lambda: self.tr_text.see("end"))

    # ---------- management ----------
    def _action(self, what):
        p, s = self._project()
        if not p:
            return
        if what == "folder":
            if p.get("dir") and os.path.isdir(p["dir"]):
                os.startfile(p["dir"])
            return
        r = p.get("runner") or {}
        runner = s["runner"]
        if what == "interval":
            cur = runner.get("interval") or r.get("interval_min") or 15
            m = simpledialog.askinteger("检查间隔", "“%s”现在%s检查一次。\n\n新的检查间隔（分钟，5–10080）：\n例如 15 = 15 分钟，300 = 5 小时。"
                                        % (p["name"], every(cur)), parent=self, initialvalue=cur, minvalue=5, maxvalue=10080)
            if not m or m == cur:
                return
            text = "把检查间隔从%s改为%s。%s\n\n确定吗？" % (every(cur), every(m), "（后台作业会以新间隔重启一次，重启时立即检查一轮。）" if r.get("kind") == "detach" else "")
            if not messagebox.askyesno("检查间隔", text, parent=self):
                return
        else:
            title, text = ACTION_TEXT[what]
            if not messagebox.askyesno(title, "%s\n\n项目：%s" % (text, p["name"]), parent=self):
                return
            m = None
        self.btns[what].configure(state="disabled")

        def run():
            err = None
            try:
                if r.get("kind") == "schtask":
                    schtask_do(r["name"], what, m)
                elif r.get("kind") == "detach":
                    detach_do(r, what, m)
                if what == "interval" and not p.get("unregistered"):
                    save_registry_value(p["id"], ["runner", "interval_min"], m)
            except Exception as e:  # noqa: BLE001
                err = str(e) or repr(e)
            title = "检查间隔" if what == "interval" else ACTION_TEXT[what][0]
            self.after(0, lambda: (messagebox.showinfo(title, "已完成。" if not err else "没有成功：\n%s" % err[-500:], parent=self), self.wake.set()))
        threading.Thread(target=run, daemon=True).start()

    def _live_query(self):
        p, _ = self._project()
        lq = p and p.get("live_query")
        if not lq:
            return
        self.lq_btn.configure(state="disabled")
        self._set(self.lq_text, [("查询中…（%s）" % " ".join(lq["cmd"]), "hint")])

        def run():
            out = run_live_query(p)
            self.after(0, lambda: (self._set(self.lq_text, [("%s 查询结果：" % time.strftime("%H:%M:%S"), "bold")] + [(ln, None) for ln in out.splitlines()]),
                                   self.lq_btn.configure(state="normal")))
        threading.Thread(target=run, daemon=True).start()

    # ---------- new monitoring request ----------
    def _new_request(self):
        w = tk.Toplevel(self)
        w.title("新建监控任务")
        w.geometry("1000x820")
        w.configure(bg="white")
        tk.Label(w, text="新建监控任务", font=(UI, 16, "bold"), fg=INK, bg="white").pack(anchor="w", padx=16, pady=(12, 2))
        tk.Label(w, text="按下面的格式在冒号后面填写（写不全也可以，Claude 会先读项目目录里的文件再办理）。填好后有两种提交方式：\n"
                         "① 复制下来，粘贴给和 Claude 的对话；② 直接交给后台 Claude 办理，过程显示在“新任务办理”里。\n"
                         "办理内容：写监控脚本、登记到总台、按你给的间隔启动监控；之后出问题自动处理，全部完成后把最终结果写好并显示在“最终结果”页。",
                 font=(UI, 11), fg=INK, bg="white", justify="left", anchor="w").pack(fill="x", padx=16)
        f, t = self._textbox(w, height=22, font=(UI, 12), editable=True)
        f.pack(fill="both", expand=True, padx=16, pady=8)
        t.insert("1.0", REQUEST_TEMPLATE)
        bar = tk.Frame(w, bg="white")
        bar.pack(fill="x", padx=16, pady=(0, 12))

        def text():
            return t.get("1.0", "end").strip()

        def example():
            if text() in (REQUEST_TEMPLATE.strip(), "") or messagebox.askyesno("填入示例", "用示例替换当前内容？", parent=w):
                t.delete("1.0", "end")
                t.insert("1.0", REQUEST_EXAMPLE)

        def copy():
            self.clipboard_clear()
            self.clipboard_append(text())
            messagebox.showinfo("已复制", "已复制到剪贴板。粘贴到和 Claude 的对话里发送即可。", parent=w)

        def submit():
            body = text()
            fields = dict(re.findall(r"^(项目名称|项目目录（本机路径）)：\s*(.*)$", body, re.M))
            if not fields.get("项目名称") or not fields.get("项目目录（本机路径）"):
                messagebox.showwarning("还差一点", "至少要填“项目名称”和“项目目录”。", parent=w)
                return
            if not messagebox.askyesno("交给后台 Claude", "后台 Claude 会按这份说明写监控脚本、登记到总台并启动监控。\n"
                                                           "它有完整权限（和监控接管一样），只做说明里允许的操作。\n\n确定提交吗？", parent=w):
                return
            try:
                name = self._submit_request(body, fields["项目目录（本机路径）"].strip())
            except Exception as e:  # noqa: BLE001
                messagebox.showerror("提交失败", str(e), parent=w)
                return
            w.destroy()
            messagebox.showinfo("已提交", "已交给后台 Claude（%s）。在左侧“新任务办理”里可以实时看到办理过程。" % name, parent=self)
            self.wake.set()
            self.after(1500, lambda: self._select("hub-setup"))

        ttk.Button(bar, text="交给后台 Claude 办理", style="Accent.TButton", command=submit).pack(side="right")
        ttk.Button(bar, text="复制（粘贴给 Claude 对话）", command=copy).pack(side="right", padx=8)
        ttk.Button(bar, text="填入示例", command=example).pack(side="left")
        ttk.Button(bar, text="填写说明", command=lambda: os.path.exists(README) and os.startfile(README)).pack(side="left", padx=8)

    def _submit_request(self, body, workdir):
        stamp = time.strftime("%Y%m%d-%H%M%S")
        req = os.path.join(REQ_DIR, stamp + "_request.md")
        rep = os.path.join(REQ_DIR, stamp + "_report.md")
        prompt_f = os.path.join(REQ_DIR, stamp + "_prompt.txt")
        with open(req, "w", encoding="utf-8") as f:
            f.write(body + "\n")
        with open(prompt_f, "w", encoding="utf-8") as f:
            f.write(SETUP_PROMPT % dict(report=rep, time=time.strftime("%Y-%m-%d %H:%M"), request=body))
        cwd = workdir if os.path.isdir(workdir) else HUB_DATA
        name = "hub-setup-" + stamp
        cmd = ("[Console]::OutputEncoding = [Text.Encoding]::UTF8; $OutputEncoding = [Text.Encoding]::UTF8; "
               "Remove-Item Env:CLAUDE_CONFIG_DIR -ErrorAction SilentlyContinue; "
               "Get-Content -Raw -Encoding utf8 -LiteralPath %s | & %s -p --dangerously-skip-permissions --output-format stream-json --verbose --model opus"
               % (q(prompt_f), q(CLAUDE)))
        res = subprocess.run([PWSH, "-NoProfile", "-NonInteractive", "-File", DETACH, "-Name", name, "-Command", cmd, "-WorkDir", cwd],
                             capture_output=True, timeout=120, **hidden())
        if res.returncode != 0:
            raise RuntimeError((res.stdout + res.stderr).decode("utf-8", errors="replace")[-400:])
        return name

    # ---------- questions ----------
    def _qa_state_for(self, pid):
        return self.qa.setdefault(pid, dict(session=None, lines=[], target=None))

    def _qa_restore(self, pid):
        st = self._qa_state_for(pid)
        self._set(self.qa_text, st["lines"])
        self.qa_text.see("end")

    def _qa_log(self, line, tag=None):
        st = self._qa_state_for(self.qa_cur or self.sel)
        st["lines"].append((line, tag))
        if (self.qa_cur or self.sel) == self.sel:
            if len(st["lines"]) == 1:
                self._set(self.qa_text, [])
            self._append(self.qa_text, line, tag)

    def _qa_new_thread(self):
        if self.qa_proc or not self.sel:
            return
        self._qa_state_for(self.sel)["session"] = None
        self._qa_log("—— 新对话：%s ——" % self.qa_box.get(), "head")

    def _qa_enter(self, event):
        if event.state & 0x0001:
            return None
        self._qa_ask()
        return "break"

    def _qa_ask(self):
        question = self.qa_in.get("1.0", "end").strip()
        if not question or self.qa_proc or not self.sel:
            return
        p, s = self._project()
        if not p or p.get("unregistered"):
            messagebox.showinfo("提问", "这个监控还没登记，不能提问。先在“编辑项目表”里登记它。", parent=self)
            return
        st = self._qa_state_for(p["id"])
        target = self.qa_box.get()
        cwd = p.get("qa_cwd") or p.get("dir") or HERE
        if st["session"]:
            args, first = qa_args(p, resume=st["session"]), False
            cwd = st.get("cwd") or cwd
        elif target.startswith("接着问 "):
            label = target[len("接着问 "):-len(" 那次处理")]
            t = next((x for x in s.get("takeovers", []) if x["label"] == label), None)
            init = cs.stream_init(read_text(t["path"], 400000)) if t else None
            if not init:
                messagebox.showinfo("提问", "找不到那次处理的会话记录。", parent=self)
                return
            cwd = init.get("cwd") or cwd
            args, first = qa_args(p, resume=init["session_id"], fork=True), False
        else:
            args, first = qa_args(p), True
        st["cwd"] = cwd
        self.qa_in.delete("1.0", "end")
        self.qa_cur = p["id"]
        self._qa_log("", "claude")
        self._qa_log("你：" + question, "you")
        st["question"], st["answer"] = question, []
        self.qa_btn.configure(state="disabled")
        self.qa_state.configure(text=("先查询%s，然后提问…" % p["live_query"].get("label", "")) if p.get("live_query") else "回答中…", fg=BLUE)
        self.qa_proc = True                              # busy marker until the worker has started the process
        threading.Thread(target=self._qa_worker, args=(p, s, args, cwd, question, first), daemon=True).start()

    def _qa_worker(self, p, s, args, cwd, question, first):
        ctx = qa_context(p, s, run_live_query(p))
        if first:
            status_file = p.get("status_md") or p.get("status_json") or ""
            prompt = "项目：%s\n\n%s\n\n[状态文件 %s]\n%s\n\n我的问题：%s" % (p["name"], ctx, status_file, read_text(status_file)[:12000], question)
        else:
            prompt = "%s\n\n我的问题：%s" % (ctx, question)
        try:
            proc = subprocess.Popen(args, cwd=cwd, env=qa_env(p), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, **hidden())
        except OSError as e:
            self.qa_q.put(("line", "无法启动 Claude：%r" % e))
            self.qa_q.put(("exit", -1))
            return
        self.qa_proc = proc
        self.qa_q.put(("started", None))
        try:
            proc.stdin.write(prompt.encode("utf-8"))
            proc.stdin.close()
        except OSError:
            pass
        for raw in proc.stdout:
            self.qa_q.put(("line", raw.decode("utf-8", errors="replace")))
        self.qa_q.put(("exit", proc.wait()))

    def _qa_pump(self):
        try:
            while True:
                kind, val = self.qa_q.get_nowait()
                st = self._qa_state_for(self.qa_cur)
                if kind == "started":
                    self.qa_state.configure(text="回答中…（Claude 可能会先查几个文件）", fg=BLUE)
                elif kind == "line":
                    s = val.strip()
                    try:
                        d = json.loads(s) if s.startswith("{") else None
                    except ValueError:
                        d = None
                    if isinstance(d, dict) and d.get("type") == "system" and d.get("subtype") == "init":
                        st["session"] = d.get("session_id")
                        continue
                    for ln in (cs.render_event(d) if isinstance(d, dict) else [s] if s and not s.startswith("[mcp-sdk]") else []):
                        st["answer"].append(ln)
                        self._qa_log(ln)
                    if isinstance(d, dict) and d.get("type") == "result":
                        self.qa_state.configure(text="本次折合 $%.2f（订阅额度，不单独计费）" % (d.get("total_cost_usd") or 0), fg=INK)
                else:
                    if val:
                        self._qa_log("[Claude 进程退出码 %s]" % val, "error")
                    self._qa_done()
        except queue.Empty:
            pass
        self.after(100, self._qa_pump)

    def _qa_done(self):
        pid = self.qa_cur
        st = self._qa_state_for(pid) if pid else {}
        if st.get("question"):
            with open(os.path.join(HUB_DATA, "qa", "%s.md" % re.sub(r"[^\w.-]", "_", pid)), "a", encoding="utf-8") as f:
                f.write("\n## %s  %s  (session %s)\n\n问：%s\n\n```text\n%s\n```\n" % (
                    time.strftime("%Y-%m-%d %H:%M"), self.qa_box.get(), st.get("session"), st["question"], "\n".join(st.get("answer", []))))
            st["question"] = ""
        self.qa_proc, self.qa_cur = None, None
        self.qa_btn.configure(state="normal")

    def _close(self):
        self._tip_hide()
        if isinstance(self.qa_proc, subprocess.Popen) and self.qa_proc.poll() is None:
            self.qa_proc.kill()
        self.destroy()


def dump():
    """Every project's snapshot as JSON on stdout: the reference output for a port (see docs/PORTING_TO_CPP.md)."""
    import pythoncom
    pythoncom.CoInitialize()
    sysinfo = probe()
    out = {p["id"]: snapshot(p, sysinfo) for p in load_projects(sysinfo)}
    sys.stdout.reconfigure(encoding="utf-8")
    json.dump(dict(generated=now().isoformat(timespec="seconds"), registry=REGISTRY, projects=out), sys.stdout,
              ensure_ascii=False, indent=1, default=str)
    print()


if __name__ == "__main__":
    if "--dump" in sys.argv:
        dump()
    else:
        Hub().mainloop()
