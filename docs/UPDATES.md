# 自动更新与 Release 规则

Monitor Hub 的自动更新以 GitHub **stable Release** 为发布源，但开发分支、PR 和本地 Git 工作树不会被自动更新器覆盖。

## 单一版本源

仓库根目录的 `VERSION` 是唯一版本号来源，格式固定为 `MAJOR.MINOR.PATCH`。

- CMake 从 `VERSION` 读取项目版本；
- `monitor_hub_cli --version` 必须返回同一个版本；
- Release tag 必须严格等于 `v<VERSION>`；
- 桌面端更新器用本地 `VERSION` 与 GitHub Release tag 比较。

## 发布链路

现有 `C++ core` 工作流继续负责 PR/main 的编译与测试。新增的 `Release` 工作流只在推送 `v*.*.*` tag 时运行，不参与普通 PR 发布。

Release 前必须同时满足：

1. tag 与根目录 `VERSION` 完全一致；
2. tag 指向的 commit 已经包含在远端 `main` 中。

任何 feature branch 上的 tag、版本号不一致的 tag 都会失败，不生成 Release。

推荐顺序：完成 feature PR 和 CI → 合并到 `main` → 确认 `VERSION` → 在该 main commit 上创建并 push `v<VERSION>` tag。

## 客户端更新策略

GUI 启动后后台检查 `/releases/latest`，成功结果缓存 6 小时。普通自动更新只接受正式 Release，不接受 draft/prerelease。

每个 Release 固定上传：

- `monitor-hub-windows-x64.zip`
- `monitor-hub-windows-x64.zip.sha256`

更新器先校验 ZIP SHA-256，再校验包内 manifest、逐文件 SHA-256 和路径安全性。

### Release 安装目录

Release ZIP 的 `payload` 目录包含 `.monitor-hub-release.json`。只有这种目录支持一键更新：先下载到 `HUB_DATA/updates` 暂存，退出 GUI 后由独立 helper 备份旧文件、替换受管文件并重启。

本地 `hub/monitor_hub_projects.json` 不进入 Release 包，不会被覆盖。

### Git 开发目录

只要安装根目录存在 `.git`，就视为开发工作树。检测到新 Release 时可以提示和打开 Release 页面，但不会自动覆盖文件、checkout tag、pull main 或改变当前分支。

这条规则用于避免自动更新与正在进行的 PR、其他开发代理以及本地未提交修改发生冲突。

## CI 验证

PR CI 会运行更新器单元测试、`monitor_hub_cli --version` 一致性检查以及 Release 打包 smoke test，在真正打 tag 前验证版本与包格式。
