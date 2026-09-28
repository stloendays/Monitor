# 开发中遇到的错误和难题（2026-09-28）

这份记录按"现象 → 根因 → 解决"整理了总台和它所管的监控在开发、试运行中出过的问题。每一条都已经变成代码里的一处处理或一条规则。改写成 C++ 时逐条对照，避免重新踩坑。

状态：✅ 已解决　⚠️ 已规避但仍需注意　⏳ 未解决

---

## A. Windows 环境

### A1. 从容器外启动 venv 报 `No Python at '"C:\...python.exe'`，exit 103 ✅
- **现象**：一个由 Claude Desktop 会话用 `uv` 建的 venv，在总台和定时任务里一启动就报错退出，从 Claude Desktop 里启动却正常。
- **根因**：Claude Desktop 是 MSIX 打包应用，它写到 `%APPDATA%` 的东西被重定向到 `%LOCALAPPDATA%\Packages\Claude_<id>\LocalCache\Roaming\`。venv 的 `pyvenv.cfg` 里 `home` 指向虚拟路径，容器外看不到。
- **解决**：不经过 venv 的启动器，直接运行物理路径下的基础解释器，并设置 `__PYVENV_LAUNCHER__=<venv>\Scripts\python.exe`。这正是启动器本身做的事，multiprocessing 的子进程也会跟着走（见 QoI 监控的 `interpreter()`）。
- **C++ 移植**：凡是要替别人启动 Python 的地方都要做同样的路径解析。

### A2. 定时任务每次运行都闪出命令行窗口 ✅
- **现象**：用户看到"时不时闪一个命令行窗口"。
- **根因**：Windows 11 的默认终端是 Windows Terminal。定时任务启动 `powershell.exe -WindowStyle Hidden` 时，控制台先被交给 Windows Terminal 打开，然后才隐藏。实测运行期间会多出 2 个可见窗口（`CASCADIA_HOSTING_WINDOW_CLASS` 和 `PseudoConsoleWindow`）。
- **解决**：定时任务的启动程序改为 `conhost.exe --headless powershell.exe …`，实测 0 个窗口，ssh 和 `Start-Process -WindowStyle Hidden` 的子进程也都正常。修改前的任务定义备份在 `D:\Research\monitor-hub\task_backups\`。
- **排查方法**：用 `EnumWindows` 统计可见的控制台类窗口，同时轮询新进程及其父进程（见 `tests/capture_pages.ps1`），就能直接看出是谁建的窗口，不用猜。

### A3. 总台刷新时启动子进程 ⚠️
- **现象**：旧版每 20 秒启动一次 pwsh 读进程列表和定时任务。虽然带了 `CREATE_NO_WINDOW` 没有闪窗，但每次要 3.7 秒，而且始终存在闪窗风险。
- **解决**：进程列表改用 WMI（`Win32_Process`），定时任务改用 Task Scheduler COM（`Schedule.Service`），都在进程内完成，0.55 秒。暂停、恢复、改间隔、立即运行也走 COM。剩下必须启动的子进程（ssh、claude、cdesktop-detach）一律 `CREATE_NO_WINDOW` + `STARTUPINFO.wShowWindow = SW_HIDE`。
- **注意**：pywin32 的 COM 对象必须在 `CoUninitialize` 之前释放，否则会打印 "Win32 exception occurred releasing IUnknown"。改完触发器后立刻查询，任务可能短暂查不到，下一轮刷新就正常了。

### A4. 中文和 ² 这类符号写进日志变成乱码 ✅
- **现象**：接管日志里 `R²` 变成 `R虏`，`−0.196` 变成 `鐚?.196`。
- **根因**：pwsh 把原生程序（claude）的 UTF-8 输出按控制台代码页（GBK）解码，再按 UTF-8 写入文件。部分字节在这一步永久丢失。
- **解决**：重定向前设置 `[Console]::OutputEncoding = [Text.Encoding]::UTF8; $OutputEncoding = [Text.Encoding]::UTF8`。已损坏的旧日志用 Claude Code 自己保存的会话副本（`<配置目录>\projects\<目录>\<session>.jsonl`，直接以 UTF-8 写入）重建。
- **C++ 移植**：管道读取一律当作 UTF-8 字节处理，只在调用 Win32 API 时转成 UTF-16。PowerShell 5.1 写的文件可能带 BOM，读取时要兼容 `utf-8-sig`。

### A5. cdesktop 会话结束时后台进程被一起杀掉 ⚠️
- **根因**：cdesktop 在每轮对话结束时杀掉整个作业对象，`Start-Process`、`&`、后台 shell 都在里面。
- **解决**：长时间运行的东西都用 `cdesktop-detach.ps1` 通过 WMI 的 `Win32_Process.Create` 启动，这样它不在作业对象里。附带好处：它的父进程不是监控本身，停止监控时 `taskkill /T` 也不会连带杀掉重启出来的计算。

---

## B. 监控逻辑

### B1. 失败计数恒为 0 ✅
失败记录写在每个检查点 JSON 的 `failures` 数组里，旧逻辑却按 `*.failed.json` 文件名去数。现在按作业类型分别处理：`payload` 型读 JSON 内容，`file` 型数 `<id>.failure.json`。

### B2. 只剩永久失败时空转 ✅
某个数据源对一个条目持续返回 HTTP 500，这个条目永远生成不了检查点，"完成数"就永远差 1。旧逻辑会白白用掉 3 次重启额度再报 DEAD。现在把失败记录也算作"已处理"，只重试一次（不占重启额度），之后进入汇总流程，失败条目写进 failures.csv，并计入分母。

### B3. 一个 worker 死了、另一个还活着时不重启 ✅
按 worker 分片（`ids[w::n]`）分别判断存活和剩余工作量，只重启确实有剩余工作的那个 worker。

### B4. 接管是同步阻塞的 ✅
旧版接管最长会阻塞 3 小时 × 2 次模型尝试，期间什么都不重启。现在接管作为独立的后台作业启动，监控照常巡检；正在接管的作业交给接管处理，监控不去动它。

### B5. "同一问题不重复接管"其实没生效 ✅
去重键里带着"已停滞 95 分钟"这类每轮都变的数字。现在键只由（问题类型, 作业）组成。同一问题最多接管 2 次，两次之间至少隔 60 分钟，之后转给用户处理。

### B6. 正常输出被误判为失败 ✅
旧逻辑在整段输出里搜 "rate limit"、"api error"。接管的正常报告里只要写了"没有 rate limit 问题"就被判失败，于是又跑一次接管。现在只看 JSON 结果里的 `is_error`、退出码，以及有没有写出报告。

### B7. `NEEDS_USER: none` 被当成"需要用户处理" ✅
旧逻辑只检查有没有出现 "NEEDS_USER" 这个词。现在读取冒号后面的内容，`none / no / 无 / 不需要` 都视为不需要。

### B8. 结果写好但没推送时不报警 ✅
新增 PUSH 检查：RESULTS.md 写出 30 分钟后分支仍不在 origin 上，就交给接管处理。

---

## C. 后台 Claude（接管、办理、提问）

### C1. 接管过程看不到 ✅
`--output-format json` 只在结束时输出一行结果。现在改用 `stream-json --verbose`，每个工具调用和结果都实时写进日志，总台实时渲染（`claude_stream.py`）。

### C2. 只读的提问会话其实能写文件、能推送 ✅（重要）
实测发现三个漏洞：
1. 用户级 `settings.json` 放行了 `git add/commit/checkout/push`，普通会话会继承这些规则。
2. Bash 的前缀放行规则拦得住 `>` 重定向，却拦不住 `git diff --output=文件`、`git show --output=`、`git log --output=`。实测真的写出了文件。
3. 带引号的 ssh 远程命令（`ssh host 'qstat; rm …'`）按前缀匹配看不清里面是什么。

解决：提问会话加 `--setting-sources project --strict-mcp-config --tools Read,Grep,Glob --disallowedTools Bash Edit Write NotebookEdit`，也就是完全不给 Bash、不连 MCP。它需要的实时状态（qstat 等）由总台自己查好，附在问题里。这个组合也已实测，写入全部被拒。

### C3. Claude 额度用完 ⏳
- 同一个 Team 账号的额度由所有后台任务共享：接管、新任务办理、提问、以及用户自己的对话。本机两个配置目录（`.claude` 和 `.claude-workspace`）登录的都是同一个账号。
- 2026-09-28 测试"新任务办理"时，后台 Claude 已经写完报告，却在收尾那一刻报 "You've hit your session limit · resets 10:10pm"，结果被判为失败。
- 现在的处理：总台显示"Claude 额度用完（几点重置）"，并注明"报告已写出，请核对"；接管失败时会先换默认模型重试一次，再通知用户。
- 仍未解决：额度用完期间，监控发现的问题只能等额度恢复后再处理。可以考虑的方向：
  - 给接管单独准备一个账号或配置目录。
  - 额度用完时自动推迟接管，并在总台显示"等额度恢复"。

### C4. 旧版接管判定"完成"时的竞态 ⚠️
接管智能体可能和另一个交互会话同时去动同一个作业。接管提示词要求它先看 git index.lock、最近 10 分钟内的文件改动、正在写的 RESULTS.md，发现有人在动就不操作。监控这一侧则不去动正在接管中的作业。

---

## D. 界面（tkinter）

| 问题 | 根因 | 解决 |
|---|---|---|
| 打开窗口时接管记录停在开头，不自动滚到末尾 | 窗口还没显示时，`Text.yview()` 返回 (0, 0) | 首次载入一律滚到末尾；之后按"加载前是否在末尾"决定是否跟随，每批只判断一次 |
| 进度显示 319 / 319（100%），实际是 318 成功 + 1 失败 | 把失败也算进了完成数 | 进度只数成功的，失败单独一列；进度百分比不四舍五入到 100% |
| 已暂停的旧监控显示"需要你处理" | 它暂停前最后记录的问题一直留在状态文件里 | 暂停时把这些问题显示为"暂停前最后记录的问题（仅供参考）"，状态显示为已暂停 |
| 截图脚本用 `FindWindow($null, title)` 找不到窗口 | PowerShell 把 `$null` 当空字符串传给 API | 改用 `[NullString]::Value` |
| 窗口被别的窗口挡住时截图不对 | `CopyFromScreen` 截的是屏幕 | 改用 `PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT)` |

---

## E. 数据和项目层面（总台如实显示，不在总台里改）

- 一个外部数据库条目持续返回 HTTP 500。按协议它计入失败数和分母（318 / 319），不重复尝试。
- 有一批 HPC 作业没有任何监控管，总台只能在别的项目的表格里看到它们（标为"不归这个监控管"）。
- 一张生成的图的坐标轴标签写的数和实际画出的点数不一致（65 对 64）。接管没有手改生成的图，这类问题要回到作图脚本里改。

## F. 开发过程中的小坑

- 在 Bash 里用 heredoc 塞 Python 代码时，反斜杠和 `\U` 会被当成转义，导致路径写坏。改写配置的一次性脚本应该先写成文件再运行。
- `git ls-remote` 对**空仓库**没有任何输出、退出码为 0；对不存在的仓库会输出 "Repository not found"（这个环境下退出码同样为 0）。所以要看输出内容，不能只看退出码。
