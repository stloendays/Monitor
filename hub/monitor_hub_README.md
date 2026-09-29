# 监控总台 使用说明

最后更新：2026-09-28

监控总台是一个桌面小程序。它把本机上所有项目的计算监控放在一个窗口里：看进度、看后台 Claude 处理问题的每一步、看最终结果、提问，也能管理监控本身（立即检查、暂停、恢复、改检查间隔）。

**以后凡是需要监控的计算任务，都按第 5 节的格式交给 Claude。** Claude 负责写监控、登记到总台、按你给的间隔启动，出问题时按规程处理，一直做到全部完成、把完整结果写好。你只需要打开总台看。

---

## 1. 打开

双击桌面上的 **「监控总台」**。

- 程序：`D:\Research\Monitor\hub\monitor_hub.py`（用 `D:\Research\CatalystForge\.venv\Scripts\pythonw.exe` 运行，没有命令行窗口）
- 窗口每 60 秒自动刷新一次；想马上看最新情况，点左下角「立即刷新」。
- 关掉窗口不影响任何监控和计算，监控都在后台独立运行。

## 2. 界面

**左侧**：每个项目一张卡片，右上角是状态，下面一句话说明现在的情况。点卡片进入这个项目。

**总览**（左侧最上面）：所有项目一张表，下面是「最近的后台处理」（所有项目的后台 Claude 记录，按时间排列，双击直接看那一次的全过程）。

### 状态的意思

| 状态 | 意思 | 你要做什么 |
|---|---|---|
| ⚠ 需要你处理 | 监控发现了它自己不能决定的事 | 进项目看红框里的说明 |
| ⟳ 后台处理中 | 监控发现了问题，后台 Claude 正在按规程处理 | 不用做什么，可以在「后台处理记录」实时看 |
| ● 正常 | 监控按时运行，没有需要你做的事 | 不用做什么 |
| ✔ 已完成 | 计算都做完了 | 到「最终结果」页看结果 |
| ⏸ 已暂停 | 监控被暂停了 | 需要时点「恢复监控」 |
| ⚠ 很久没更新 | 状态文件超过两个检查周期没更新 | 监控可能停了，点「立即检查一次」或问 Claude |
| ✖ 监控出错 | 监控本身有问题（找不到、上次运行失败、连不上服务器） | 看状态下面的说明；连不上服务器通常是集群维护，下一轮会自己恢复 |

### 项目里的各页

- **进度**：作业表。绿色 = 已完成，蓝色 = 运行中，棕色 = 排队中，红色 = 异常。下面是备注（配额、排队原因等）。
- **后台处理记录**：每次后台 Claude 处理问题的记录。左边选一次，右边显示它读了什么、运行了什么命令、得到什么结果、最后的结论。正在处理的会实时往下滚。
- **最终结果**：这个项目的交付文件（结果表、RESULTS.md 等）。还没生成的会标「尚未生成」。
- **提问**：见第 6 节。
- **操作日志**：监控自动做过的操作（重投、续算、验收等）。
- **实时查询**：直接向服务器查当前状态（例如集群的 qstat），不经过监控，只读。

## 3. 管理监控

项目页右上角的按钮（每个都会先弹窗说明后果，确认后才执行）：

| 按钮 | 作用 |
|---|---|
| 立即检查一次 | 马上运行一轮监控，和定时运行完全一样（会按规程自动处理问题） |
| 暂停监控 | 监控不再定时检查、不再自动处理；正在跑的计算不受影响 |
| 恢复监控 | 按原来的周期继续 |
| 检查间隔… | 修改检查间隔（分钟，5–10080）。定时任务直接改触发周期；后台作业会以新间隔重启一次 |
| 打开文件夹 | 打开这个监控的目录 |

## 4. 登记了哪些项目

窗口左侧「项目」下面就是登记表 `monitor_hub_projects.json` 里的全部项目。总台支持三种监控：

| 监控方式 | 例子 | 间隔怎么定 |
|---|---|---|
| Windows 定时任务（通常 ssh 到 HPC 查 PBS 作业） | 每 5 小时查一次集群作业 | 定时任务触发器的重复周期 |
| 本机后台作业（cdesktop-detach，循环运行的脚本） | 每 15 分钟查一次本机计算的检查点 | 脚本的 `--interval` 参数 |
| 名字里带 monitor、但还没登记的定时任务 / 后台作业 | — | 只显示运行情况，登记后才有进度和记录 |

另外总台自带一项 **「新任务办理」**：你从总台提交的新监控任务都在这里。

## 5. 提交新的监控任务

### 格式

点左下角 **「＋ 新建监控任务」**，窗口里已经放好了下面的格式，在冒号后面填写。写不全也可以，Claude 会先读项目目录里的文件（AGENTS.md、交接文档、提交记录）再办理，但**项目名称和项目目录必须填**。

```
【监控任务】
项目名称：
项目目录（本机路径）：
计算在哪里跑：本机 / HPC 集群 PBS / 其他服务器（写清楚）
要监控的作业：作业名、PBS 作业号或启动命令、检查点 / 输出文件在哪里
完成标准：什么情况算全部完成
检查间隔：例如 15 分钟、5 小时
允许监控自动做的操作：例如 进程停了原地续算、SCF 不收敛按规程重投、跑完汇总结果并提交推送
禁止的操作：例如 不改计算参数、不删数据、不提交新体系
需要通知我的情况：
最终交付：例如 结果表、RESULTS.md、推送到 GitHub 分支 xxx
备注：
```

填写示例（窗口里点「填入示例」可以直接放进去）：

```
【监控任务】
项目名称：示例 · 表面吸附能计算（第 2 批）
项目目录（本机路径）：D:\Research\Example\adsorption_batch2
计算在哪里跑：HPC 集群 PBS（ssh <主机别名>，分配项目 <项目代码>，队列 batch_cpu）
要监控的作业：slab_clean、slab_CO_top、slab_CO_bridge、slab_O_fcc 等 8 个体系，作业号见 SUBMISSION.txt；输出在 /scratch/<用户名>/…/batch2
完成标准：8 个体系的最终单点都完成，能量和磁矩写进结果表
检查间隔：5 小时
允许监控自动做的操作：墙时或 NELM 停机时从 WAVECAR/CONTCAR 原地续算；弛豫验收通过后建最终单点
禁止的操作：不改 INCAR 的泛函、U、ENCUT；不提交新体系；不删除任何输出
需要通知我的情况：同一体系第 2 次 NELM 用满；自旋态和初始 MAGMOM 不一致
最终交付：results/batch2_table.md（E0、磁矩、吸附能），推送到项目仓库
备注：和其他批次共用集群配额，注意 scratch 使用量
```

### 两种提交方式

1. **复制，粘贴给 Claude 对话**：点「复制」，粘贴到和 Claude 的对话里发送。适合你想边聊边定细节的时候。
2. **交给后台 Claude 办理**：点「交给后台 Claude 办理」。总台会启动一个后台 Claude，办理过程实时显示在左侧「新任务办理」里；办好后新项目出现在左侧「项目」里。它写的办理报告在 `D:\Research\monitor-hub\requests\` 下。

### Claude 会负责到哪一步

1. 读项目资料，确认这些作业现在的真实状态（如果已经有监控在管，就把那个监控登记进总台，不重复写一个）。
2. 按你给的规则写监控：能自动处理的（续算、重投、汇总）由脚本直接做；判断不了的交给后台 Claude 接管。
3. 登记到总台，按你给的间隔启动，第一轮检查通过后才算办好。
4. 之后每一轮：出问题自动处理，处理过程在「后台处理记录」里；它决定不了的，项目会变成「⚠ 需要你处理」。
5. 全部完成时：写好完整的最终结果（你在「最终交付」里要求的文件），显示在「最终结果」页，项目变成「✔ 已完成」，监控自己停下。

## 6. 提问

项目页的「提问」页可以用中文问任何关于这个项目的问题，例如「现在进度怎样」「上次后台处理改了什么参数」「哪个作业最慢」。

- **助手（新对话）**：总台会把此刻的状态、进度表和实时查询结果（HPC 项目会先查一次 qstat）一起发过去。
- **接着问某次处理**：带着那次后台处理的完整记录继续问，适合追问「你为什么这样做」。上下文长，费用约为新对话的 10 倍以上。
- 提问会话是**只读**的：只能读文件，不能运行命令、不能改文件、不能提交或推送。需要操作时，它会告诉你该做什么，由你决定。
- 所有问答记录在 `D:\Research\monitor-hub\qa\<项目>.md`。

## 7. 费用

后台处理、新任务办理和提问都使用 Claude 订阅（Team 计划）的额度，不按次扣费；额度用完时它们会中断，总台会显示「Claude 额度用完（几点重置）」。每次结束时显示的「折合 $x」是按 API 价格估算的用量，用来看用量大小。

## 8. 文件位置

| 内容 | 位置 |
|---|---|
| 程序 | `D:\Research\Monitor\hub\monitor_hub.py`（源码仓库 `D:\Research\Monitor`） |
| 项目登记表 | `D:\Research\Monitor\hub\monitor_hub_projects.json`（左下角「编辑项目表」） |
| 本说明 | `D:\Research\Monitor\hub\monitor_hub_README.md` |
| 新任务请求与办理报告 | `D:\Research\monitor-hub\requests\` |
| 问答记录 | `D:\Research\monitor-hub\qa\` |
| 定时任务修改前的备份 | `D:\Research\monitor-hub\task_backups\` |
| 后台作业（监控、接管、办理）日志 | `C:\Users\ASUS\AppData\Local\cdesktop-jobs\<作业名>\output.log` |

---

## 9. 给 Claude 的执行规范

接到监控任务（用户在对话里给出，或从总台提交到后台）时，Claude 按下面做。同时遵守 `monitor-script-rules` skill（三层结构、接管合同、从历史失败推出 ATTENTION 条件并回放验证）。

### 9.1 先查再做
- 读项目的 AGENTS.md / CLAUDE.md / 交接文档 / 提交记录 / 状态文件；在服务器上核对作业的真实状态。
- 已有监控在管这些作业：把它登记进总台（9.3），不要再写第二个。
- 只做请求里允许的操作；禁止的和没提到的，只能作为建议写进报告。

### 9.2 监控必须满足
- **确定性脚本做机械操作**，判断不了的交给无头 Claude 接管（`claude.exe -p --dangerously-skip-permissions --output-format stream-json --verbose`，提示词走 stdin）。接管记录用 stream-json 保存，文件名 `claude_takeover_<YYYYMMDD_HHMM>.jsonl`，或作为 cdesktop-detach 作业 `<前缀>-takeover-<时间>` 的 output.log。
- **不弹窗**：
  - Windows 定时任务的启动程序写成 `conhost.exe --headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File <脚本>`。直接启动 `powershell.exe -WindowStyle Hidden` 会在默认终端（Windows Terminal）里闪出窗口。
  - 本机长时间运行的循环用 cdesktop-detach 后台作业（`C:\Users\ASUS\.claude\tools\cdesktop-detach.ps1`）。
  - 脚本里再启动子进程时也要无窗口（`Start-Process -WindowStyle Hidden` 或 Python 的 `CREATE_NO_WINDOW`）。
- **编码**：用 pwsh 把原生程序（claude、ssh）的输出重定向到文件前，先设 `[Console]::OutputEncoding = [Text.Encoding]::UTF8; $OutputEncoding = [Text.Encoding]::UTF8`，否则中文和 ² 这类符号会写成乱码。
- **间隔可调**：定时任务的间隔就是触发器的重复周期（总台直接改）；后台作业的脚本必须接受 `--interval <分钟>`，登记表 `start_cmd` 里写 `{interval}` 占位。
- **完成时**：写出请求里「最终交付」要求的全部文件（结果表、RESULTS.md 等），把 `done` 置为 true，在登记表的 `results` 里列出这些文件，然后让监控自己停下（定时任务停用自己；后台循环退出）。结果要带分母和失败数。

### 9.3 登记到总台
在 `D:\Research\Monitor\hub\monitor_hub_projects.json` 的 `projects` 里加一项（字段说明见文件里的 `_help`）。新监控统一用 `adapter: "generic"`：

```json
{
  "id": "example-hpc-batch2",
  "name": "示例 · 表面吸附能计算（第 2 批）",
  "area": "计算化学 · HPC PBS",
  "adapter": "generic",
  "runner": {"kind": "schtask", "name": "example-batch2-monitor-5h", "interval_min": 300},
  "dir": "<监控目录>",
  "status_json": "<监控目录>\\hub_status.json",
  "log": "<操作日志路径>",
  "report": "<最新接管报告路径>",
  "takeovers": {"kind": "glob", "pattern": "<监控目录>\\claude_takeover_*.jsonl", "last": "<监控目录>\\claude_takeover_last.json", "lock": "<锁文件>"},
  "results": [{"label": "结果表", "path": "<项目目录>\\results\\batch2_table.md"}],
  "claude_config_dir": null,
  "qa_cwd": "<项目目录>",
  "qa_sources": ["<监控目录>\\", "<提交记录>"],
  "live_query": {"label": "集群队列（qstat）", "cmd": ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=20", "<主机别名>", "qstat -u <用户名> -w"]}
}
```

后台作业型的 runner 写成：`{"kind": "detach", "name": "<作业名>", "interval_min": 15, "start_cmd": "& '<python>' '<脚本>' --loop --interval {interval}", "workdir": "<目录>"}`，接管记录用 `{"kind": "detach", "prefix": "<前缀>-takeover-"}`。

### 9.4 状态文件 hub_status.json（每轮覆盖写，UTF-8）

```json
{
  "updated": "2026-09-28T21:45:00",
  "headline": "一句话结论，例如：所有作业正常运行，无需操作。",
  "summary": "完成 3，运行 5，排队 2",
  "done": false,
  "table": {
    "cols": ["体系", "作业号", "状态", "进度", "E (eV)", "本次操作"],
    "rows": [["L0_Rh111_3x3", "1407166", "完成", "最终单点", "-247.32324", ""]],
    "tags": ["done"]
  },
  "attention": [],
  "working": "",
  "notes": ["scratch 配额：78.5 / 500 G"],
  "next": "2026-09-29T02:45:00",
  "results": [{"label": "结果表", "path": "D:\\...\\results\\batch2_table.md"}],
  "error": ""
}
```

- 新格式状态文件建议写 `"schema_version": 1`；旧文件不写仍兼容。完整可运行示例见 `hub_status.example.json`。总台会校验核心结构：严重格式错误显示为“监控出错”，长度不一致、缺少稳定 `task_id` 等非致命问题只显示协议提示。
- 调试新 monitor 时先跑 `python validate_status.py <hub_status.json>`；无 error 时退出码为 0，有结构错误时为 1。这样可以在正式登记前发现列数、tag、row_meta 和 task_id 问题。
- `tags` 每行一个：`done` 已完成、`run` 运行中、`queue` 排队、`bad` 异常、`other` 不归本监控管、空字符串 = 其他。
- `row_meta` 可选，和 `rows` 一一对应；用于任务下钻。推荐写 `task_id`、`job_id`、`host`、`open_path`、`log`、`result`、`script`、`command`、`params`。总台双击进度行时优先打开 `open_path`，然后依次尝试 `path / workdir / log / result`。
- 新脚本统一按 `monitor__<project>__<scope>__<interval>.py` 命名，定时任务 / 后台作业按 `<project>__monitor__<scope>__<interval>` 命名；完整规则见 `docs/MONITOR_NAMING.md`。
- `attention` 只放**需要用户决定**的事（写成用户能直接看懂的中文）；后台 Claude 正在处理的写在 `working`。
- `error` 只放监控自身的故障（如 ssh 连不上）；它不是计算失败。
- `done` 为 true 表示全部完成，总台显示「✔ 已完成」。

### 9.5 办好的标准
登记后打开总台（或读 `monitor_hub.py` 的 `snapshot()`）确认：项目出现在左侧、状态不是「✖ 监控出错」、第一轮检查的表格正确、间隔与请求一致、没有弹窗。然后给用户（或写进办理报告）：设置了什么（文件、定时任务、登记 id）、第一轮结果，以及一行 `NEEDS_USER: <原因>` 或 `NEEDS_USER: none`。

## 10. 常见问题

- **窗口会闪出命令行吗？** 总台刷新时不启动任何子进程；本机的 HPC 定时任务已改为 `conhost --headless` 启动（2026-09-28，修改前的任务定义备份在 `task_backups`）。如果又看到闪窗，多半是新加的定时任务没有按 9.2 写。
- **「很久没更新」但计算在跑**：监控本身停了。点「立即检查一次」；还不行就在「提问」里问原因。
- **「需要你处理」处理完了，怎么消掉？** QoI 监控：在 `D:\Research\QoI-ext-cache\monitor\state.json` 删掉 `notify` 里的那条和 `handled` 里对应的键。定时任务型监控：下一轮检查不再报这个问题时自动消失。
- **想新增一个已有的监控**：点「编辑项目表」按 9.3 加一项，保存后点「立即刷新」。
