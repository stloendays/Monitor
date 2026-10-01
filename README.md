# Monitor · 监控总台

一个 Windows 桌面小程序，把本机上所有长时间计算任务的监控集中到一个窗口里。任务可以在本机跑（机器学习训练、数据处理），也可以在 HPC 集群的 PBS 上跑（VASP 等）。

## 下载 Windows 版

**普通用户推荐直接下载最新 Release 的安装版：**
[GitHub Releases · Latest](https://github.com/stloendays/Monitor/releases/latest)

- **推荐：** `Monitor-Hub-<version>-win64-setup.exe` — 当前用户安装，不需要管理员权限，带开始菜单与卸载器。
- **免安装：** `Monitor-Hub-<version>-win64-portable.zip` — 解压后运行 `MonitorHub\bin\monitor_hub_qt.exe`。
- **自动更新专用：** `monitor-hub-windows-x64.zip` — 给 Monitor Hub updater 使用，不建议普通用户手工解压。
- **完整性校验：** 使用 Release 中的 `SHA256SUMS.txt`。

正式 Qt 安装版已经包含运行所需的 Qt 与 MSVC runtime，**普通用户不需要安装 Python**。只有使用 Claude Code 集成功能时才需要本机已有 Claude CLI。

> 当前 Windows 安装包尚未使用商业代码签名证书；SmartScreen 可能提示“未知发布者”。请只从本仓库官方 Release 下载，并核验 SHA-256。

在这个窗口里你可以：

- **看完成情况**：每个项目一张卡片，状态一眼看清（⚠ 需要你处理 / ⟳ 后台处理中 / ● 正常 / ✔ 已完成 / ⏸ 已暂停）。进度表支持任务下钻：新格式监控提供 `row_meta` 后，双击一行可直接打开该任务目录、日志或结果文件。
- **看后台 Claude 的处理过程**：监控发现自己处理不了的问题时，会启动一个无头 Claude（`claude -p`）按规程处理。它读了什么、运行了什么命令、得到什么结果，都能实时看到。
- **看最终结果**：计算全部完成后，结果表和 RESULTS.md 直接在窗口里打开。
- **提问**：用中文问这个项目的情况。提问会话是真正只读的（只能读文件，不能运行命令、不能改文件）。
- **管理监控**：立即检查一次、暂停、恢复、修改检查间隔。
- **提交新的监控任务**：按固定格式填写，交给后台 Claude 办理（写监控脚本 → 登记到总台 → 启动 → 出问题自动处理 → 全部完成后交付最终结果）。

![总览](docs/images/overview.png)

| 项目页：需要你处理 + 后台处理记录 | 最终结果 | 新建监控任务 |
|---|---|---|
| ![](docs/images/project_takeover.png) | ![](docs/images/project_results.png) | ![](docs/images/new_request.png) |

（截图用的是 `tests/demo/make_demo.py` 生成的演示数据。）

## 工作方式

总台本身**不做监控**，它只是查看器和控制面板：

1. **各项目的监控**独立运行，用 Windows 定时任务或本机后台循环。确定性脚本负责能按规程做的机械操作（续算、重投、汇总），并把状态写成文件。
2. 遇到它决定不了的事（ATTENTION），启动**无头 Claude 接管**，处理过程用 stream-json 记录下来。
3. **总台**读取这些文件和系统状态（Task Scheduler COM、WMI），在界面上显示出来。刷新时不启动任何子进程，所以不会闪出命令行窗口。

所以关掉总台不影响任何监控；把总台改写成别的语言，也不用改任何监控。详见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 运行

### 正式 Windows 安装版

安装后从开始菜单打开 **Monitor Hub**，或直接运行：

```powershell
"$env:LOCALAPPDATA\Programs\Monitor Hub\bin\monitor_hub_qt.exe"
```

常用诊断命令：

```powershell
monitor_hub_cli.exe --version
monitor_hub_cli.exe --probe-system
monitor_hub_cli.exe --dump
monitor_hub_orchestrator.exe --tick
```

程序支持系统托盘、后台运行、单实例、通知和可选开机启动。关闭主窗口时，如果设置了“关闭到托盘”，程序仍会在后台运行；需要完全退出时请使用托盘菜单中的 **退出**。

### 源码 / Legacy Python

仓库仍保留原 Python UI 与兼容脚本用于迁移、对照和部分开发工具。需要运行旧版 Python UI 时：

```bat
copy hub\monitor_hub_projects.example.json hub\monitor_hub_projects.json
pythonw hub\monitor_hub.py
```

不想碰真实项目、只想生成演示数据：

```bat
python tests\demo\make_demo.py
set MONITOR_HUB_REGISTRY=%TEMP%\monitor-hub-demo\demo_projects.json
set MONITOR_HUB_DATA=%TEMP%\monitor-hub-demo\hubdata
set MONITOR_HUB_NO_DISCOVERY=1
```

Windows 安装版还支持在 **设置** 中保存登记表和 Hub 数据目录。路径优先级是：

1. 命令行 `--registry` / `--hub-data`；
2. 环境变量 `MONITOR_HUB_REGISTRY` / `MONITOR_HUB_DATA`；
3. 设置中保存的路径；
4. 默认路径。

如果安装版首次启动没有找到登记表，会提示选择已有的 `monitor_hub_projects.json`。未指定 `MONITOR_HUB_DATA` 时，Qt/C++ 版默认使用当前用户的 `%LOCALAPPDATA%\Monitor Hub\`，不再依赖开发机绝对路径。

新监控状态格式、命名规则与 UI 约定仍分别见：

- [hub/hub_status.schema.json](hub/hub_status.schema.json)
- [hub/hub_status.example.json](hub/hub_status.example.json)
- [docs/MONITOR_NAMING.md](docs/MONITOR_NAMING.md)
- [docs/UI_GUIDELINES.md](docs/UI_GUIDELINES.md)

## 目录

```
hub/
  monitor_hub.py                    主程序（界面 + 读取 + 管理 + 提问 + 新任务办理）
  claude_stream.py                  stream-json 接管记录的解析和渲染（QoI 监控也在用）
  monitor_hub_README.md             用户使用说明（窗口里的「使用说明」）；第 9 节是给 Claude 的执行规范
  hub_status.schema.json            通用状态文件格式
  monitor_hub_projects.example.json 登记表示例（真实登记表 monitor_hub_projects.json 不进仓库）
deps/cdesktop-detach.ps1            启动不受 cdesktop 会话影响的后台作业（副本；程序使用 ~/.claude/tools 下的那一份）
docs/
  ARCHITECTURE.md                   模块、数据约定、健康判定、线程、安全边界
  PORTING_TO_CPP.md                 改写为 C++ 的指南：技术栈、逐函数对照、实施顺序、对照测试
  LESSONS_LEARNED.md                开发中遇到的错误和难题，以及怎么解决的
  images/                           截图（演示数据）
tests/                              演示数据、页面巡览截图、COM / 后台作业 / 适配器 / 新任务办理测试（见 tests/README.md）
```

## C++ / Qt 主线

当前正式 Release 已经以 **C++20 + Qt 6** 为主线，Python UI 保留用于兼容、迁移和历史对照。

C++ 主线目前包含：

- Windows Task Scheduler COM + WMI 只读探测；
- Monitor 项目/任务/Issue/Agent Event 统一投影；
- Qt 6 桌面 UI、系统托盘、单实例、设置与通知；
- Claude CLI 用量/session/workspace/statusLine 集成；
- policy-bounded L1/L2/L3 command control；
- deterministic local/PBS recovery handlers；
- durable notification outbox；
- Windows installer / portable / updater package；
- stable Release workflow 与 SHA-256 校验。

迁移设计与历史对照仍保留在 [docs/PORTING_TO_CPP.md](docs/PORTING_TO_CPP.md)。

## 关于这个项目

这个项目由 **Claude**（Anthropic 的 AI 助手；模型 Claude Opus 5.5，在 Claude Code 中运行）在 2026-09-28 按 Tony 的要求编写。起点是修复一个 QoI 计算监控脚本里的问题，后来一步步扩展成覆盖本机和 HPC 计算的通用监控总台。开发过程中踩过的坑都记录在 [docs/LESSONS_LEARNED.md](docs/LESSONS_LEARNED.md)，其中包括：
- MSIX 虚拟化路径导致 venv 启动失败；
- Windows Terminal 下定时任务闪窗；
- 管道编码导致的乱码；
- 所谓"只读"会话其实能写文件、能推送；
- Claude 额度共享导致后台任务中断。

所有功能都在作者的机器上实测过（截图、临时定时任务、临时后台作业、真实的后台 Claude 办理），测试脚本在 `tests/` 里。

目前未指定开源许可证。
