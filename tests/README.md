# 测试

都在 Windows 上运行，解释器需要带 tkinter 和 pywin32（作者机器上用的是 `D:\Research\CatalystForge\.venv\Scripts\python.exe`）。

| 脚本 | 测什么 | 会不会动到真实环境 |
|---|---|---|
| `demo/make_demo.py` | 生成演示数据：3 个合成项目，含状态文件、接管记录、结果文件 | 只写到 `%TEMP%\monitor-hub-demo` |
| `demo_tour.py` + `capture_pages.ps1` | 在演示数据上依次打开各页面并截图，同时统计新出现的可见控制台窗口（必须为 0） | 不会 |
| `adapter_test.py` | 通用适配器的正常、需要处理、完成、很久没更新四种状态；然后列出本机真实项目的快照 | 只读 |
| `com_test.py` | 通过 Task Scheduler COM 对一个**临时**任务做：读取、改间隔、暂停、恢复、立即运行，最后删除 | 只动临时任务 `hub-com-test-monitor` |
| `detach_test.py` | 对一个**临时**后台作业做：启动、重启、带新间隔重启、停止 | 只动临时作业 `hub-test-monitor` |
| `setup_test.py` | 真实提交一个标注为"测试"的新任务请求，由后台 Claude 办理（它只写报告），然后清理掉 | 会用掉一点 Claude 额度 |

截图：
```
python tests/demo/make_demo.py
pwsh -File tests/capture_pages.ps1 -OutDir docs/images      # 在另一个窗口先启动
python tests/demo_tour.py
```

参考输出（改写成 C++ 时用来逐项对照，见 docs/PORTING_TO_CPP.md 第 3 节）：
```
set MONITOR_HUB_REGISTRY=%TEMP%\monitor-hub-demo\demo_projects.json
set MONITOR_HUB_DATA=%TEMP%\monitor-hub-demo\hubdata
set MONITOR_HUB_NO_DISCOVERY=1
python hub/monitor_hub.py --dump > demo_snapshots.json
```
