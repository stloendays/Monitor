# Qt New Monitor Request Intake

The Qt desktop now carries the legacy **新建监控任务** workflow forward as a first-class Monitor Hub function.

## User flow

1. Click **＋ 新建监控任务** in the sidebar.
2. Fill the request template. At minimum:
   - `项目名称`
   - `项目目录（本机路径）`
3. Describe completion criteria, monitor cadence, allowed automatic actions, forbidden actions, notification conditions, and final deliverables.
4. Confirm the authority boundary.
5. Monitor Hub writes:
   - `MONITOR_HUB_DATA/requests/<stamp>_request.md`
   - `MONITOR_HUB_DATA/requests/<stamp>_prompt.txt`
   - expected report path `<stamp>_report.md`
6. The existing `cdesktop-detach.ps1` helper starts `hub-setup-<stamp>`.
7. The UI refreshes and selects the built-in **新任务办理** project so the user can watch setup progress.

## Safety and authority

The setup prompt requires the Agent to:

- inspect project governance before changing files;
- verify live state and reuse an existing monitor when one already covers the work;
- treat **允许监控自动做的操作** and **禁止的操作** as hard authority boundaries;
- escalate anything outside the delegated boundary as L3;
- avoid deleting outputs or changing scientific/business method without explicit authorization;
- register the resulting monitor in the existing registry;
- keep the current `hub_status.json` contract;
- emit Protocol v1 events when practical;
- end the setup report with one `NEEDS_USER:` line.

The Qt UI validates the local PowerShell, detach-helper and Claude CLI runtime before creating a request. The final launch still requires an explicit confirmation.

## Compatibility

This is a migration of an existing Tkinter capability, not a new request format. It preserves:

- `hub-setup-<stamp>` detached-job names;
- `*_request.md`, `*_prompt.txt`, and `*_report.md` file naming;
- the existing built-in `hub-setup` adapter/project;
- the current registry and Hub Data locations.

If process launch fails after materialization, the request files are intentionally kept so the request is auditable and can be retried after the local runtime is repaired.
