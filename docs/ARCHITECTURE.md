# 架构

总台本身**不做监控**，它是一个查看器和控制面板。真正干活的是各项目自己的监控脚本（Windows 定时任务或后台循环）。总台读取这些脚本写出的文件，在一个窗口里显示出来，并提供少量管理操作。这样做有两个好处：总台关掉了，监控照样运行；把总台改写成别的语言（比如 C++），也不用动任何监控。

```
┌──────────────────────────── 各项目自己的监控（和总台无关，独立运行） ───────────────────────────┐
│  Windows 定时任务 (conhost --headless powershell …)        本机后台循环 (cdesktop-detach 作业)      │
│      │ ssh → HPC 上的确定性脚本（重投、续算、验收）            │ 本机确定性脚本（重启、汇总）          │
│      ▼                                                     ▼                                     │
│  状态文件  hub_status.json / status_latest.md / status.json    操作日志   结果文件                 │
│      │ ATTENTION ──► 无头 Claude 接管 (claude -p … stream-json) ──► claude_takeover_*.jsonl / output.log │
└──────┼──────────────────────────────────────────────────────────────────────────────────────────┘
       │ 只读：文件、Task Scheduler COM、WMI
┌──────▼──────────────────────────── 监控总台 monitor_hub.py ────────────────────────────────────┐
│ 后台线程（每 60 s）: probe() ─► load_projects() ─► snapshot(p) × N ──► 锁保护的共享快照          │
│ 界面线程（每 3 s）:  渲染侧栏卡片 / 总览 / 项目页；追读接管记录（增量）                           │
│ 按需: 管理操作（COM / cdesktop-detach）· 实时查询（ssh）· 提问（只读 claude -p）· 新任务办理        │
└────────────────────────────────────────────────────────────────────────────────────────────────┘
```

## 1. 模块

| 模块 | 文件 / 函数 | 作用 |
|---|---|---|
| 配置 | `monitor_hub_projects.json`，`load_registry()`，`load_projects()` | 项目登记表；另外自动发现名字里带 monitor、但没登记的定时任务和后台作业 |
| 系统探测 | `probe()` | 通过 COM 读定时任务（状态、上次和下次运行、结果码、重复周期、启动程序）和进程列表（pid、名称、命令行） |
| 运行方式 | `runner_info()` | 把定时任务或后台作业的状态整理成统一结构：存在、运行中、已暂停、出错、间隔、说明文字 |
| 状态适配器 | `adapt_generic / adapt_markdown / adapt_qoi / adapt_setup / adapt_runner_only` | 把不同格式的状态文件转成同一种快照 |
| 接管记录 | `takeovers()`，`_detach_takeovers()`，`_glob_takeovers()` | 列出每次后台 Claude 运行：时间、结果（完成 / 失败 / 进行中）、摘要、记录文件 |
| 健康判定 | `snapshot()` | 按优先级算出项目状态（见第 3 节） |
| 渲染 | `claude_stream.py` | 把 stream-json 事件转成可读的文本行（同时供 QoI 监控使用） |
| 管理 | `schtask_do()`，`detach_do()`，`can_do()` | 立即运行、暂停、恢复、改间隔 |
| 提问 | `qa_args()`，`qa_context()`，`run_live_query()` | 只读的 claude 会话，启动参数见第 5 节 |
| 新任务 | `Hub._new_request()`，`_submit_request()`，`SETUP_PROMPT` | 按格式填写的请求 → 后台 Claude 按 README 第 9 节办理 |
| 界面 | `class Hub(tk.Tk)` | 侧栏卡片、总览（项目表 + 最近的后台处理 + 状态说明）、项目页（6 个标签页） |
| 参考输出 | `dump()`（`--dump`） | 把所有快照输出成 JSON，供改写后逐项对照 |

## 2. 数据约定

### 2.1 登记表 `monitor_hub_projects.json`
```
{ "_help": [...], "projects": [ Project, ... ] }
Project = {
  id, name, area,                       // 唯一 id、显示名、副标题
  adapter: "generic" | "markdown" | "qoi",
  runner: { kind: "schtask", name, interval_min }
        | { kind: "detach", name, interval_min, start_cmd /* 含 {interval} */, workdir }
        | { kind: "none", interval_min? },
  dir,                                  // 监控目录（「打开文件夹」）
  status_json | status_md,              // 状态文件
  attention?, done_file?,               // markdown 适配器：ATTENTION 文件、完成标记（内容以 DONE 开头）
  log?, report?,                        // 操作日志、最新接管报告
  takeovers?: { kind: "detach", prefix } | { kind: "glob", pattern, last?, lock? },
  results?: [{label, path}], results_glob?: [pattern],
  claude_config_dir: null | path,       // 这个项目的接管和提问用哪个 Claude 配置目录
  qa_cwd, qa_sources: [..],             // 提问会话的工作目录、告诉它去哪里找资料
  live_query?: { label, cmd: [argv] },  // 只读的实时查询
  monitor_script?                       // qoi 适配器：导入监控脚本，实时统计检查点
}
```

### 2.2 通用状态文件 `hub_status.json`
格式见 `hub/hub_status.schema.json` 和用户说明第 9.4 节。关键字段：`updated`（ISO 时间）、`headline`、`summary`、`done`、`table{cols,rows,tags}`、`attention[]`（只放需要用户决定的事）、`working`、`notes[]`、`next`、`results[]`、`error`（监控自身的故障）。

### 2.3 Markdown 状态文件（旧的 HPC 监控）
第一行 `# 标题`，第一段是一句话结论，然后是一张 Markdown 表格（必须有「状态」列，最好有「进度」列），`**备注：**` 下面是 `- ` 开头的列表，另有 `下次检查：…` 行。连不上服务器时正文里包含 `ssh/remote monitor FAILED`。

### 2.4 接管记录（stream-json，一行一个 JSON）
```
{"type":"system","subtype":"init","session_id":..,"cwd":..,"model":..,"tools":[..]}
{"type":"assistant","message":{"content":[{"type":"text","text":..} | {"type":"tool_use","name":..,"input":{..}}]}}
{"type":"user","message":{"content":[{"type":"tool_result","content":..,"is_error":bool}]}}
{"type":"result","subtype":"success"|..,"is_error":bool,"result":<最终文本>,"duration_ms":..,"total_cost_usd":..}
```
- `session_id` + `cwd` 用来「接着问」（`claude --resume <id> --fork-session`，用同一个配置目录、在同一个 `cwd` 里运行）。
- 文件名约定：`claude_takeover_<YYYYMMDD_HHMM>.jsonl`（定时任务型），或 cdesktop 作业 `<prefix><YYYYMMDD-HHMMSS>\output.log`。

### 2.5 cdesktop-detach 作业目录 `%LOCALAPPDATA%\cdesktop-jobs\<name>\`
`run.ps1`（包装脚本）、`output.log`、`pid`、`started`、`exitcode`（结束时写入；`stopped` 表示被手动停止）。判断运行中的条件：没有 `exitcode`，并且这个 pid 的进程命令行里包含作业目录路径（防止 pid 被别的进程复用）。

### 2.6 新任务请求 `D:\Research\monitor-hub\requests\`
`<stamp>_request.md`（用户填写的请求）、`<stamp>_prompt.txt`（发给后台 Claude 的完整提示词）、`<stamp>_report.md`（它写的办理报告，最后一行 `NEEDS_USER: …`）。对应的后台作业名是 `hub-setup-<stamp>`。

## 3. 健康判定（`snapshot()`，自上而下，命中即止）

1. `done` → ✔ 已完成
2. 适配器报的 `error`，或运行方式出错（找不到、结果码非 0、进程意外退出）→ ✖ 监控出错
3. 运行方式为已暂停 → ⏸ 已暂停（原来的 attention 转成"暂停前最后记录的问题"）
4. `attention` 不为空 → ⚠ 需要你处理
5. 状态文件超过「2 × 间隔 + 30 分钟」没更新 → ⚠ 很久没更新
6. `working` 不为空 → ⟳ 后台处理中
7. 其他 → ● 正常

markdown 适配器的 attention 判定：ATTENTION 文件的修改时间不早于状态文件 2 分钟以上，才算本轮的问题。
- 有正在运行的接管 → 后台处理中。
- 最新一次接管晚于 ATTENTION 文件且成功 → 需要你看结果。
- 最新一次接管失败 → 需要你处理。
- 还没接管 → 等待后台处理。

## 4. 线程和刷新

- **后台线程**：先 `CoInitialize`，然后每 `REFRESH_S = 60` 秒（或点「立即刷新」时）运行一次 probe → 读登记表 → 所有快照，结果在锁内替换，版本号加 1。
- **界面线程**：每 `TICK_MS = 3000` 毫秒检查一次版本号，变了就重绘；当前选中的接管记录按字节偏移增量读取，只渲染完整的行，不完整的行留到下次。
- **提问**：单独的工作线程先跑实时查询、拼好提示词，再启动 claude，把 stdout 的每一行放进队列；界面线程每 100 毫秒取一次队列。
- **管理操作**：各自一个短命线程，结束后唤醒后台线程立即刷新。

## 5. 安全边界

- **提问会话**：`--permission-mode default --setting-sources project --strict-mcp-config --tools Read,Grep,Glob --disallowedTools Bash Edit Write NotebookEdit --allowedTools Read Grep Glob`（原因见 LESSONS_LEARNED C2）。
- **接管和新任务办理**：`--dangerously-skip-permissions`，由提示词划定允许和禁止的操作。必须在有明确规程时使用。
- **所有管理操作**都先弹窗说明后果，确认后才执行。
- **子进程**一律不建窗口：`CREATE_NO_WINDOW` + `SW_HIDE`；定时任务用 `conhost --headless`。
