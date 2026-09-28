# 改写为 C++ 的指南

目标：用 C++ 重写 `hub/monitor_hub.py`（界面 + 读取 + 管理），行为和现在的 Python 版完全一致。监控脚本、状态文件、接管记录的格式都**不用改**，C++ 版读的是同一批文件。

## 1. 建议的技术栈

| 需要 | 建议 | 备注 |
|---|---|---|
| 语言标准 | C++20（MSVC 2022） | `std::filesystem`、`std::chrono`、`std::jthread` |
| 界面 | **Qt 6 Widgets**（首选），或 Win32 + WTL / Dear ImGui | Qt 有现成的表格（`QTableView`）、树（`QTreeView`）、`QTabWidget`、`QSplitter`、`QPlainTextEdit` 和中文字体回退；打包用 `windeployqt` |
| JSON | nlohmann/json | 登记表、状态文件、stream-json 都要读，登记表还要写 |
| 定时任务 | Task Scheduler 2.0 COM：`ITaskService`、`ITaskFolder`、`IRegisteredTask`、`ITaskDefinition`、`ITriggerCollection`、`IRepetitionPattern`（`taskschd.h`，链接 `taskschd.lib`） | 对应 Python 里的 `win32com.client.Dispatch("Schedule.Service")` |
| 进程列表 | WMI：`IWbemLocator` → `IWbemServices::ExecQuery("SELECT ProcessId, Name, CommandLine FROM Win32_Process WHERE …")` | 要拿到别的进程的命令行，WMI 最省事；也可以用 `NtQueryInformationProcess`，但更麻烦 |
| 启动子进程 | `CreateProcessW` + `CREATE_NO_WINDOW` + `STARTUPINFOW{dwFlags=STARTF_USESHOWWINDOW, wShowWindow=SW_HIDE}`，stdin / stdout 用匿名管道 | **绝不能**让子进程弹出窗口，见 LESSONS A2、A3 |
| 线程 | `std::jthread` + `std::mutex`；界面更新走 Qt 的 `QMetaObject::invokeMethod` 或信号槽 | 每个用 COM 的线程都要调用 `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` |
| 编码 | 内部统一用 UTF-8 的 `std::string`；只在调用 Win32 API 时用 `MultiByteToWideChar` / `WideCharToMultiByte` 转成 `std::wstring` | 读文件时要兼容 UTF-8 BOM |

## 2. Python → C++ 对照

| Python（monitor_hub.py） | C++ 建议 | 说明 |
|---|---|---|
| `load_registry()` / `save_registry_value()` | `class ProjectRegistry { load(); setValue(id, path, value); }` | 写入时先写临时文件，再 `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`，保持原子性 |
| `load_projects()` | `ProjectRegistry::projectsWithDiscovered(const SystemInfo&)` | 登记的项目 + 「新任务办理」+ 自动发现的监控（名字或启动命令里含 monitor） |
| `probe()` | `class SystemProbe { SystemInfo run(); }` | 遍历任务文件夹时跳过 `\Microsoft`；`LastTaskResult` 按无符号数处理：0 = 成功，0x41301 = 正在运行，0x41303 = 还没运行过 |
| `detach_state()` | `DetachJob::state(name, procs)` | 读 `exitcode` / `pid`，并核对该进程的命令行包含作业目录 |
| `runner_info()` | `RunnerInfo Runner::info(const Project&, const SystemInfo&)` | 返回 exists / running / paused / error / interval / text |
| `takeovers()` | `std::vector<Takeover> TakeoverIndex::list(const Project&, const SystemInfo&)` | 两种来源：detach 前缀，或 `claude_takeover_<stamp>.(jsonl|md)` |
| `adapt_*()` | `struct Adapter { virtual Snapshot read(...) = 0; }` 加 5 个实现 | Generic / Markdown / Qoi / Setup / RunnerOnly |
| `parse_status_md()` / `classify_row()` | `MarkdownStatus::parse(text)` | 一定要照搬规则：状态列、进度列里的「非监控」、以 R / Q / H 开头的状态等 |
| `snapshot()` | `Snapshot HealthEvaluator::evaluate(...)` | 优先级见 ARCHITECTURE 第 3 节 |
| `claude_stream.render_event/render_stream/parse_result/stream_init` | `class StreamJson { static std::vector<Line> render(const json&); }` | 增量读取：按字节偏移读，只处理完整的行 |
| `schtask_do()` | `TaskScheduler::run/enable/disable/setInterval(name, minutes)` | 改间隔 = 改第一个带 Repetition 的触发器的 `Interval`（`PT5H` / `PT90M`），再调用 `RegisterTaskDefinition(name, def, TASK_UPDATE, VARIANT(), VARIANT(), logonType, VARIANT())` |
| `detach_do()` | `DetachJob::stop/start(...)` | 目前调用 `cdesktop-detach.ps1`；也可以在 C++ 里直接用 WMI 的 `Win32_Process.Create` 实现同样的效果 |
| `qa_args()` / `qa_context()` | `QaSession::start(project, question, resume?)` | 参数必须和 ARCHITECTURE 第 5 节一字不差，否则只读就不可靠了 |
| `run_live_query()` | `LiveQuery::run(argv, timeout)` | 过滤掉 ssh 的 post-quantum 等警告横幅 |
| `_submit_request()` | `SetupRequest::submit(body, workdir)` | 写 `_request.md` / `_prompt.txt`，启动 detach 作业 `hub-setup-<stamp>` |
| `Hub` 类 | `MainWindow`（侧栏 `QListView` 用自定义委托画卡片 + `QStackedWidget`：总览页 / 项目页） | 颜色、字号见第 4 节 |

## 3. 建议的实施顺序

1. **只读核心，没有界面**：`ProjectRegistry`、`SystemProbe`、适配器、`HealthEvaluator`、`StreamJson`。做一个命令行程序 `monitor_hub_cli --dump`，输出和 Python 版 `python hub/monitor_hub.py --dump` **同样结构的 JSON**。
2. **对照测试**：
   - 用 `tests/demo/make_demo.py` 生成演示数据。
   - 两个版本都设 `MONITOR_HUB_REGISTRY`、`MONITOR_HUB_DATA`、`MONITOR_HUB_NO_DISCOVERY=1`，各自 `--dump`，逐字段比较：health、summary、table、takeovers、results_list。
   - 除时间字段外，其余必须全部一致。
3. **界面**：先做总览页和项目页里的「进度」「后台处理记录」（增量追读）「最终结果」。
4. **管理操作**：`TaskScheduler` 的四个操作都先在临时任务上测（参照 `tests/com_test.py`）；detach 操作参照 `tests/detach_test.py`。
5. **提问和新任务办理**：启动 claude 子进程，从管道逐行读取；参数照抄。
6. **闪窗验收**：运行 `tests/capture_pages.ps1`，它会统计运行期间新出现的可见控制台窗口，必须是 0。

## 4. 必须保留的行为

- **刷新**：后台每 60 秒一次，「立即刷新」可以马上唤醒；界面刷新不能卡住（所有 IO 都在后台线程里做）。
- **刷新期间不启动子进程**，只用 COM 和文件读取。
- **只读提问会话**：只提供 Read、Grep、Glob 三个工具，不加载用户设置，不连 MCP。
- **管理操作一律先确认**，确认框里用中文说明后果。
- **界面风格**：
  - 白底、深色字（`#1a1a1a`），不用浅灰字；表格要有可见的网格线，不用灰色斑马纹。
  - 正文 11 pt，表头 11 pt 加粗，标题 18 pt，项目状态 14 pt 加粗。
  - 状态颜色：需要处理和出错用红 `#b00020`，处理中用蓝 `#0d47a1`，正常和完成用绿 `#1b5e20`，暂停和排队用棕 `#7a4a00`；只给要强调的那一项上色，不给整列上色。
- **接管记录追读**：第一次载入滚到末尾；之后只有用户停在末尾时才自动跟随。
- **状态文字**：沿用 `HEALTH` 表里的中文名称和说明，用户已经熟悉这些用语。
- **所有文件按 UTF-8 读写。**

## 5. 可以借改写的机会改进的地方

- 用 `ReadDirectoryChangesW` 或 `QFileSystemWatcher` 监视状态文件，变了就刷新，不必等 60 秒。
- 系统托盘图标：有「需要你处理」时弹出 Windows 通知。
- 把本机路径（claude.exe、pwsh.exe、数据目录）集中到一个配置文件里，现在它们是 `monitor_hub.py` 顶部的常量。
- 额度用完（LESSONS C3）时自动推迟接管，并显示预计恢复的时间。
