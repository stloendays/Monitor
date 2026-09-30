#include "monitor_hub/core.hpp"
#include "monitor_hub/event_store.hpp"
#include "monitor_hub/overview.hpp"
#include "monitor_hub/notification_outbox.hpp"
#include "monitor_hub/setup_request.hpp"
#include "monitor_hub/claude_cli.hpp"
#include "monitor_hub/windows_probe.hpp"

#include <boost/json.hpp>
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
namespace json = boost::json;

static void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << s;
}

int main() {
    using namespace monitor_hub;

    assert(classify_row("完成") == "done");
    assert(classify_row("R 02:14") == "run");
    assert(classify_row("Q") == "queue");
    assert(classify_row("停止") == "bad");
    assert(classify_row("NELM 停").empty());
    assert(classify_row("R", "非监控") == "other");
    assert(count_summary({"done", "run", "queue", "bad", "other"}) == "完成 1，运行 1，排队 1，异常 1；另有 1 个作业不归这个监控管");
    assert(iso_minutes("PT5H") == 300);
    assert(iso_minutes("PT1H30M") == 90);
    assert(short_time("2026-09-28T21:45:00") == "09-28 21:45");

    SystemInfo raw_probe_fixture;
    TaskInfo raw_task;
    raw_task.name = "demo__monitor__local__15m";
    raw_task.state = "Ready";
    raw_task.last = "2026-09-30T10:00:00";
    raw_task.result = 0;
    raw_task.next = "2026-09-30T10:15:00";
    raw_task.interval = "PT15M";
    raw_task.action = "monitor-demo.ps1";
    raw_probe_fixture.tasks[raw_task.name] = raw_task;
    raw_probe_fixture.procs.push_back(
        ProcessInfo{1234, "claude.exe", "claude --print"});
    const auto raw_probe_json = system_info_json(raw_probe_fixture);
    assert(raw_probe_json.at("tasks").as_object().contains(raw_task.name));
    assert(raw_probe_json.at("procs").as_array().size() == 1);
    assert(raw_probe_json.at("error").as_string().empty());
    const auto& raw_task_json =
        raw_probe_json.at("tasks").as_object().at(raw_task.name).as_object();
    assert(raw_task_json.at("state").as_string() == "Ready");
    assert(raw_task_json.at("interval").as_string() == "PT15M");

    const auto root = fs::temp_directory_path() / "monitor-hub-cpp-core-test";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto status = root / "hub_status.json";
    write_file(status,
        R"({"updated":"2026-09-28T21:45:00","headline":"正常","summary":"","done":false,"attention":[],"working":"","table":{"cols":["作业","状态"],"rows":[["a","完成"],["b","运行中"]],"tags":["done","run"],"row_meta":[{"task_id":"a","open_path":"C:/demo/a"}]},"notes":[],"next":"2026-09-28T22:00:00","error":""})");

    json::object p;
    p["id"] = "x";
    p["name"] = "x";
    p["adapter"] = "generic";
    p["status_json"] = status.string();
    json::object r;
    r["kind"] = "none";
    r["interval_min"] = 15;
    p["runner"] = r;
    p["runner_text"] = "test";

    RuntimePaths paths;
    paths.registry = root / "registry.json";
    paths.hub_data = root / "hubdata";
    paths.job_root = root / "jobs";
    paths.discovery = false;
    SystemInfo sys;
    const auto s = snapshot(p, sys, paths);
    assert(s.at("health").as_string() == "ok" || s.at("health").as_string() == "stale");
    assert(s.at("summary").as_string() == "完成 1，运行 1");
    assert(s.at("results_list").as_array().empty());
    const auto& meta = s.at("table").as_object().at("row_meta").as_array();
    assert(meta.size() == 2);
    assert(meta[0].as_object().at("task_id").as_string() == "a");

    write_file(status,
        R"({"updated":"2026-09-28T21:45:00","headline":"done","done":true,"attention":["x"],"working":"x","table":{"cols":[],"rows":[],"tags":[]},"error":"boom"})");
    const auto done = snapshot(p, sys, paths);
    assert(done.at("health").as_string() == "done");

    write_file(status,
        R"({"updated":"bad-time","headline":"","done":false,"table":{"cols":["任务","状态"],"rows":[["a"]],"tags":["run"],"row_meta":[{"task_id":"a"},{"task_id":"a"}]},"attention":[],"notes":[],"error":""})");
    const auto invalid = snapshot(p, sys, paths);
    assert(invalid.at("health").as_string() == "error");
    assert(std::string(invalid.at("problem").as_string()).find("状态文件格式错误") != std::string::npos);
    assert(!invalid.at("extras").as_array().empty());

    // Detached runners are resolved from pid/exitcode files plus the WMI-style process list.
    const auto job = paths.job_root / "demo__monitor__local__15m";
    write_file(job / "pid", "4242\n");
    json::object dp;
    dp["id"] = "detached";
    json::object dr;
    dr["kind"] = "detach";
    dr["name"] = "demo__monitor__local__15m";
    dr["interval_min"] = 15;
    dp["runner"] = dr;
    SystemInfo detached_sys;
    detached_sys.procs.push_back(ProcessInfo{4242, "python.exe", "python monitor.py --job-root \"" + job.string() + "\""});
    const auto live = runner_info(dp, detached_sys, paths);
    assert(live.exists && live.running && !live.paused && !live.error);
    write_file(job / "exitcode", "0\n");
    const auto stopped = runner_info(dp, detached_sys, paths);
    assert(stopped.exists && !stopped.running && stopped.paused && !stopped.error);

    // Detached takeover history is indexed from cdesktop job folders.
    const auto tk = paths.job_root / "demo-takeover-20260929-1315";
    write_file(tk / "exitcode", "0\n");
    write_file(tk / "output.log",
        "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"result\":\"fixed and resumed\",\"duration_ms\":1000,\"total_cost_usd\":0.01}\n");
    json::object tp;
    tp["id"] = "tk";
    json::object tr;
    tr["kind"] = "none";
    tp["runner"] = tr;
    json::object tcfg;
    tcfg["kind"] = "detach";
    tcfg["prefix"] = "demo-takeover-";
    tp["takeovers"] = tcfg;
    const auto indexed = takeovers(tp, detached_sys, paths);
    assert(indexed.size() == 1);
    assert(indexed[0].as_object().at("state").as_string() == "ok");
    assert(indexed[0].as_object().at("summary").as_string() == "fixed and resumed");

    // Discovery includes unregistered detached monitor folders.
    const auto unregistered = paths.job_root / "extra__monitor__local__15m";
    write_file(unregistered / "exitcode", "0\n");
    write_file(unregistered / "run.ps1", "python monitor__extra__local__15m.py\n");
    write_file(paths.registry, "{\"projects\":[]}");
    paths.discovery = true;
    const auto projects = load_projects(detached_sys, paths);
    bool found_detached = false;
    for(const auto& item : projects)
        if(item.if_contains("id") && item.at("id").is_string() && item.at("id").as_string() == "job:extra__monitor__local__15m")
            found_detached = true;
    assert(found_detached);

    // The cross-project overview is a pure normalized-state projection.
    std::vector<json::object> overview_projects;
    json::object alpha;
    alpha["id"] = "alpha";
    alpha["name"] = "Alpha";
    overview_projects.push_back(alpha);

    json::object beta;
    beta["id"] = "beta";
    beta["name"] = "Beta";
    overview_projects.push_back(beta);

    json::object builtin;
    builtin["id"] = "hub-setup";
    builtin["name"] = "Setup";
    builtin["builtin"] = true;
    overview_projects.push_back(builtin);

    std::map<std::string, json::object> overview_snapshots;
    json::object alpha_snapshot;
    alpha_snapshot["health"] = "attention";
    alpha_snapshot["summary"] = "完成 1，运行 1";
    alpha_snapshot["headline"] = "需要选择恢复方案";
    alpha_snapshot["attention"] = json::array{"请选择恢复方案"};
    json::object alpha_takeover;
    alpha_takeover["label"] = "09-29 14:00";
    alpha_takeover["state"] = "running";
    alpha_takeover["summary"] = "正在检查失败原因";
    alpha_takeover["path"] = "C:/demo/alpha/takeover.jsonl";
    alpha_takeover["time"] = 20.0;
    alpha_snapshot["takeovers"] = json::array{alpha_takeover};
    overview_snapshots["alpha"] = alpha_snapshot;

    json::object beta_snapshot;
    beta_snapshot["health"] = "done";
    beta_snapshot["summary"] = "完成 2";
    beta_snapshot["headline"] = "全部完成";
    beta_snapshot["attention"] = json::array{};
    json::object beta_takeover;
    beta_takeover["label"] = "09-29 13:00";
    beta_takeover["state"] = "ok";
    beta_takeover["summary"] = "恢复完成";
    beta_takeover["path"] = "C:/demo/beta/takeover.jsonl";
    beta_takeover["time"] = 10.0;
    beta_snapshot["takeovers"] = json::array{beta_takeover};
    overview_snapshots["beta"] = beta_snapshot;

    const auto overview = build_overview_model(
        overview_projects,
        overview_snapshots,
        8);
    assert(overview.total_projects == 2);
    assert(overview.attention_projects == 1);
    assert(overview.done_projects == 1);
    assert(overview.projects.size() == 2);
    assert(overview.projects[0].progress == "完成 1，运行 1");
    assert(overview.attention.size() == 1);
    assert(overview.attention[0].kind == "需要决策");
    assert(overview.activity.size() == 2);
    assert(overview.activity[0].project_id == "alpha");
    assert(overview.activity[1].project_id == "beta");

    // Claude CLI status is read from the zero-token statusLine bridge snapshot.
    const auto claude_status_file = paths.hub_data / "claude" / "cli_status.json";
    const auto claude_transcript = root / "claude-session.jsonl";
    write_file(
        claude_transcript,
        "{\"type\":\"assistant\",\"timestamp\":\"2026-09-30T10:14:58\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"name\":\"Read\",\"input\":{\"file_path\":\"secret.txt\"}}]}}\n"
        "{\"type\":\"assistant\",\"timestamp\":\"2026-09-30T10:14:59\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"name\":\"Agent\",\"input\":{\"subagent_type\":\"general-purpose\",\"prompt\":\"do not expose this prompt\"}}]}}\n");
    write_file(
        claude_status_file,
        std::string(R"({"schema_version":1,"source":"claude_statusline","captured_at":"2026-09-30T10:15:00","version":"2.1.259","session":{"id":"sess-123","name":"monitor-work","prompt_id":"prompt-456","transcript_path":")") +
        claude_transcript.generic_string() +
        R"("},"model":{"id":"claude-sonnet-5","display_name":"Claude Sonnet 5"},"workspace":{"current_dir":"C:/demo","project_dir":"C:/demo","git_worktree":"feature-monitor"},"agent":{"name":"monitor-agent","type":"general-purpose"},"context_window":{"used_percentage":31.5},"cost":{"total_cost_usd":1.23},"rate_limits_available":true,"rate_limits":{"five_hour":{"used_percentage":24.0,"resets_at":1788062400},"seven_day":{"used_percentage":13.0,"resets_at":1788580800}}})");
    SystemInfo claude_sys;
    claude_sys.procs.push_back(ProcessInfo{1234, "claude.exe", "claude"});
    const auto claude = load_claude_cli_status(claude_sys, paths);
    assert(claude.running_processes == 1);
    assert(claude.source == "claude_statusline");
    assert(claude.version == "2.1.259");
    assert(claude.model == "Claude Sonnet 5");
    assert(claude.session_id == "sess-123");
    assert(claude.session_name == "monitor-work");
    assert(claude.prompt_id == "prompt-456");
    assert(claude.project_dir == "C:/demo");
    assert(claude.git_worktree == "feature-monitor");
    assert(claude.agent_name == "monitor-agent");
    assert(claude.agent_type == "general-purpose");
    assert(claude.recent_tool == "Agent");
    assert(claude.recent_agent == "Agent · general-purpose");

    std::vector<json::object> claude_link_projects;
    json::object broad_project;
    broad_project["id"] = "broad";
    broad_project["dir"] = "C:/demo";
    claude_link_projects.push_back(broad_project);

    json::object specific_project;
    specific_project["id"] = "specific";
    json::object specific_runner;
    specific_runner["workdir"] = "C:\\DEMO\\project";
    specific_project["runner"] = specific_runner;
    claude_link_projects.push_back(specific_project);

    ClaudeCliStatus link_status;
    link_status.project_dir = "c:/demo/project/work";
    assert(match_claude_workspace_project(link_status, claude_link_projects) ==
           "specific");

    link_status.project_dir.clear();
    link_status.cwd = "C:/demo/other";
    assert(match_claude_workspace_project(link_status, claude_link_projects) ==
           "broad");

    link_status.cwd = "D:/unrelated";
    assert(match_claude_workspace_project(link_status, claude_link_projects).empty());

    assert(claude.rate_limits_available);
    assert(claude.five_hour.used_percentage &&
           *claude.five_hour.used_percentage == 24.0);
    assert(claude.seven_day.used_percentage &&
           *claude.seven_day.used_percentage == 13.0);
    assert(claude.context_used_percentage &&
           *claude.context_used_percentage == 31.5);

    // Native CLI statusLine bridge mirrors the Python compatibility bridge
    // without persisting secret or free-form prompt/tool fields.
    std::istringstream native_input(
        R"({"version":"2.2.0","session_id":"native-session","session_name":"native-work","prompt_id":"native-prompt","transcript_path":"C:/demo/native.jsonl","model":{"id":"claude-opus","display_name":"Claude Opus"},"cwd":"C:/demo/native","workspace":{"project_dir":"C:/demo","git_worktree":"native-worktree","secret":"must-not-leak-workspace"},"agent":{"name":"native-agent","type":"general-purpose","prompt":"must-not-leak-agent-prompt"},"context_window":{"used_percentage":28.0},"cost":{"total_cost_usd":0.75},"rate_limits":{"five_hour":{"used_percentage":30.0,"resets_at":1788062400},"seven_day":{"used_percentage":10.0,"resets_at":1788580800}},"oauth_token":"must-not-leak-token","api_key":"must-not-leak-key"})");
    std::ostringstream native_output;
    std::string native_diagnostic;
    assert(run_claude_statusline_bridge(
               native_input,
               native_output,
               paths,
               &native_diagnostic) == 0);
    assert(native_diagnostic.empty());
    assert(native_output.str().find("Claude Opus") != std::string::npos);
    assert(native_output.str().find("5h 30%") != std::string::npos);
    assert(native_output.str().find("7d 10%") != std::string::npos);

    const auto native_saved = read_json(claude_status_file);
    assert(native_saved && native_saved->is_object());
    const auto native_serialized = json::serialize(*native_saved);
    assert(native_serialized.find("native-session") != std::string::npos);
    assert(native_serialized.find("native-agent") != std::string::npos);
    assert(native_serialized.find("must-not-leak") == std::string::npos);
    assert(native_serialized.find("oauth_token") == std::string::npos);
    assert(native_serialized.find("api_key") == std::string::npos);

    // Fallback: background Claude stream-json can provide reset/utilization
    // without reading OAuth credentials when no statusLine cache exists.
    fs::remove(claude_status_file);
    const auto claude_job = paths.job_root / "claude-test";
    write_file(
        claude_job / "output.log",
        R"({"type":"rate_limit_event","rate_limit_info":{"status":"allowed","resetsAt":1788062400,"rateLimitType":"five_hour","unifiedWindows":{"five_hour":{"utilization":0.42,"resetsAt":1788062400},"seven_day":{"utilization":0.21,"resetsAt":1788580800}}}})");
    const auto stream_claude = load_claude_cli_status(claude_sys, paths);
    assert(stream_claude.source == "stream-json");
    assert(stream_claude.five_hour.used_percentage &&
           *stream_claude.five_hour.used_percentage == 42.0);
    assert(stream_claude.seven_day.used_percentage &&
           *stream_claude.seven_day.used_percentage == 21.0);

    // Protocol v1 events are append-only facts. Duplicate IDs are ignored,
    // malformed lines are isolated, and Agent action completion does not
    // resolve an issue without an explicit recovery/resolution event.
    const auto alpha_event_file = paths.hub_data / "events" / "alpha.jsonl";
    write_file(
        alpha_event_file,
        R"({"schema_version":1,"event_id":"evt-1","event_type":"issue.detected","occurred_at":"2026-09-30T10:00:00+08:00","project_id":"alpha","task_id":"a1","issue_id":"iss-1","correlation_id":"corr-1","source":{"kind":"monitor","id":"alpha-monitor"},"severity":"warning","payload":{"summary":"SCF non-convergence"}})"
        "\n"
        R"({"schema_version":1,"event_id":"evt-2","event_type":"agent.started","occurred_at":"2026-09-30T10:01:00+08:00","project_id":"alpha","task_id":"a1","issue_id":"iss-1","agent_run_id":"agent-1","correlation_id":"corr-1","source":{"kind":"child_agent","id":"troubleshooter"},"severity":"info","payload":{"summary":"正在检查 OUTCAR"}})"
        "\n"
        R"({"schema_version":1,"event_id":"evt-3","event_type":"agent.action_finished","occurred_at":"2026-09-30T10:02:00+08:00","project_id":"alpha","task_id":"a1","issue_id":"iss-1","agent_run_id":"agent-1","correlation_id":"corr-1","source":{"kind":"child_agent","id":"troubleshooter"},"severity":"info","payload":{"summary":"已完成受限恢复动作"}})"
        "\n"
        R"({"schema_version":1,"event_id":"evt-4","event_type":"issue.user_action_required","occurred_at":"2026-09-30T10:03:00+08:00","project_id":"alpha","task_id":"a1","issue_id":"iss-1","correlation_id":"corr-1","source":{"kind":"monitor","id":"alpha-monitor"},"severity":"warning","payload":{"summary":"需要选择科学上不同的恢复方案","authority":"L3"}})"
        "\n"
        R"({"schema_version":1,"event_id":"evt-4","event_type":"issue.user_action_required","occurred_at":"2026-09-30T10:03:00+08:00","project_id":"alpha","task_id":"a1","issue_id":"iss-1","correlation_id":"corr-1","source":{"kind":"monitor","id":"alpha-monitor"},"severity":"warning","payload":{"summary":"重复投递不应重复执行","authority":"L3"}})"
        "\n"
        "{bad json}\n"
        R"({"schema_version":1,"event_id":"evt-5","event_type":"vendor.custom_observation","occurred_at":"2026-09-30T10:04:00+08:00","project_id":"alpha","source":{"kind":"adapter","id":"custom"},"severity":"info","payload":{"summary":"未知事件仍保留"}})"
        "\n");

    const auto alpha_events =
        load_project_event_projection(paths, "alpha", 100);
    assert(alpha_events.events.size() == 5);
    assert(alpha_events.duplicate_events == 1);
    assert(alpha_events.malformed_lines == 1);
    assert(alpha_events.issues.size() == 1);
    assert(alpha_events.issues[0].issue_id == "iss-1");
    assert(alpha_events.issues[0].state == "user_action_required");
    assert(alpha_events.issues[0].authority == "L3");
    assert(alpha_events.issues[0].user_action_required);
    assert(!alpha_events.issues[0].resolved);
    assert(alpha_events.issues[0].current_action == "已完成受限恢复动作");
    assert(alpha_events.has_user_attention());
    assert(alpha_events.events.back().event_type == "vendor.custom_observation");

    std::map<std::string, ProjectEventProjection> event_projections;
    event_projections["alpha"] = alpha_events;
    const auto event_overview = build_overview_model(
        overview_projects,
        overview_snapshots,
        event_projections,
        8);
    const auto event_attention = std::find_if(
        event_overview.attention.begin(),
        event_overview.attention.end(),
        [](const OverviewAttentionItem& item) {
            return item.project_id == "alpha" &&
                   item.issue_id == "iss-1" &&
                   item.source == "event";
        });
    assert(event_attention != event_overview.attention.end());
    assert(event_attention->kind == "需要决策");
    assert(event_overview.activity.size() >= 2);
    assert(std::any_of(
        event_overview.activity.begin(),
        event_overview.activity.end(),
        [](const OverviewAgentActivity& item) {
            return item.project_id == "alpha" &&
                   item.source == "event" &&
                   item.summary == "已完成受限恢复动作";
        }));

    write_file(
        alpha_event_file,
        read_text(alpha_event_file) +
        R"({"schema_version":1,"event_id":"evt-6","event_type":"issue.recovery_verified","occurred_at":"2026-09-30T10:05:00+08:00","project_id":"alpha","task_id":"a1","issue_id":"iss-1","correlation_id":"corr-1","source":{"kind":"monitor","id":"alpha-monitor"},"severity":"info","payload":{"summary":"恢复证据通过"}})"
        "\n"
        R"({"schema_version":1,"event_id":"evt-7","event_type":"issue.resolved","occurred_at":"2026-09-30T10:06:00+08:00","project_id":"alpha","task_id":"a1","issue_id":"iss-1","correlation_id":"corr-1","source":{"kind":"monitor","id":"alpha-monitor"},"severity":"info","payload":{"summary":"问题已解决"}})"
        "\n");
    const auto resolved_events =
        load_project_event_projection(paths, "alpha", 100);
    assert(resolved_events.issues.size() == 1);
    assert(resolved_events.issues[0].resolved);
    assert(!resolved_events.issues[0].user_action_required);
    assert(!resolved_events.has_user_attention());

    // Qt monitor-request intake uses the same core request materializer
    // as legacy setup flow, so it can be tested without launching Claude.
    const auto setup_project_dir = root / "setup-project";
    fs::create_directories(setup_project_dir);
    const std::string setup_body =
        "【监控任务】\n"
        "项目名称：Qt intake test\n"
        "项目目录（本机路径）：" + setup_project_dir.string() + "\n"
        "完成标准：测试完成\n"
        "允许监控自动做的操作：只读检查\n"
        "禁止的操作：不删除输出\n";
    const auto setup_fields = parse_setup_request_fields(setup_body);
    assert(setup_fields);
    assert(setup_fields->project_name == "Qt intake test");
    assert(setup_fields->workdir == setup_project_dir.string());

    const auto setup_launch = prepare_setup_request(
        paths,
        setup_body,
        setup_project_dir,
        std::string("20300101-010203"));
    assert(setup_launch.job_name == "hub-setup-20300101-010203");
    assert(fs::exists(setup_launch.request_file));
    assert(fs::exists(setup_launch.prompt_file));
    assert(setup_launch.working_directory == setup_project_dir);
    assert(read_text(setup_launch.request_file).find("Qt intake test") !=
           std::string::npos);
    const auto setup_prompt_text = read_text(setup_launch.prompt_file);
    assert(setup_prompt_text.find(paths.registry.string()) !=
           std::string::npos);
    assert(setup_prompt_text.find(paths.hub_data.string()) !=
           std::string::npos);
    assert(setup_prompt_text.find("Agent/Event Protocol v1") !=
           std::string::npos);
    assert(setup_prompt_text.find("NEEDS_USER:") !=
           std::string::npos);
    assert(setup_launch.command.find("--dangerously-skip-permissions") !=
           std::string::npos);
    assert(std::find(
               setup_launch.arguments.begin(),
               setup_launch.arguments.end(),
               setup_launch.job_name) != setup_launch.arguments.end());

    // Durable main-Agent outbox: missing notification.requested is bridged
    // for L3/completion facts, explicit requests stay authoritative, repeated
    // sync is idempotent, and acknowledgement survives process restart.
    RuntimePaths outbox_paths = paths;
    outbox_paths.hub_data = root / "outbox-test-hub";
    const auto outbox_events = outbox_paths.hub_data / "events";

    write_file(
        outbox_events / "decision.jsonl",
        R"({"schema_version":1,"event_id":"evt-decision","event_type":"issue.user_action_required","occurred_at":"2026-09-30T11:00:00+08:00","project_id":"decision","task_id":"d1","issue_id":"iss-decision","correlation_id":"corr-decision","source":{"kind":"monitor","id":"decision-monitor"},"severity":"warning","payload":{"summary":"需要主 Agent 选择恢复方案","authority":"L3"}})"
        "\n");

    auto notification_outbox = sync_notification_outbox(outbox_paths);
    assert(notification_outbox.items.size() == 1);
    assert(notification_outbox.pending_count() == 1);
    assert(notification_outbox.items[0].notification_id ==
           "ntf:auto:evt-decision");
    assert(notification_outbox.items[0].reason == "decision_required");
    assert(notification_outbox.items[0].target == "main_agent");
    assert(notification_outbox.items[0].synthetic);

    const auto first_outbox_bytes =
        read_text(outbox_paths.hub_data / "outbox" / "notifications.jsonl");
    notification_outbox = sync_notification_outbox(outbox_paths);
    const auto second_outbox_bytes =
        read_text(outbox_paths.hub_data / "outbox" / "notifications.jsonl");
    assert(first_outbox_bytes == second_outbox_bytes);

    assert(acknowledge_notification(
        outbox_paths,
        "ntf:auto:evt-decision",
        "main-agent-test"));
    auto acknowledged_outbox = load_notification_outbox(outbox_paths);
    assert(acknowledged_outbox.pending_count() == 0);
    assert(acknowledged_outbox.items[0].state == "acknowledged");
    assert(acknowledged_outbox.items[0].actor == "main-agent-test");
    const auto acknowledged_bytes =
        read_text(outbox_paths.hub_data / "outbox" / "notifications.jsonl");
    assert(acknowledge_notification(
        outbox_paths,
        "ntf:auto:evt-decision",
        "main-agent-test"));
    assert(
        acknowledged_bytes ==
        read_text(outbox_paths.hub_data / "outbox" / "notifications.jsonl"));

    write_file(
        outbox_events / "complete.jsonl",
        R"({"schema_version":1,"event_id":"evt-complete","event_type":"project.completed","occurred_at":"2026-09-30T12:00:00+08:00","project_id":"complete","correlation_id":"corr-complete","source":{"kind":"monitor","id":"complete-monitor"},"severity":"info","payload":{"summary":"全部计算完成"}})"
        "\n"
        R"({"schema_version":1,"event_id":"evt-notify","event_type":"notification.requested","occurred_at":"2026-09-30T12:00:01+08:00","project_id":"complete","correlation_id":"corr-complete","source":{"kind":"monitor","id":"complete-monitor"},"severity":"info","payload":{"notification_id":"ntf-final","target":"main_agent","reason":"project_completed","summary":"结果已经准备好"}})"
        "\n"
        R"({"schema_version":1,"event_id":"evt-delivered","event_type":"notification.delivered","occurred_at":"2026-09-30T12:00:02+08:00","project_id":"complete","correlation_id":"corr-complete","source":{"kind":"core","id":"outbox-bridge"},"severity":"info","payload":{"notification_id":"ntf-final","summary":"已交付给主 Agent"}})"
        "\n");

    notification_outbox = sync_notification_outbox(outbox_paths);
    assert(notification_outbox.items.size() == 2);
    const auto completion_notification = std::find_if(
        notification_outbox.items.begin(),
        notification_outbox.items.end(),
        [](const NotificationRecord& item) {
            return item.notification_id == "ntf-final";
        });
    assert(completion_notification != notification_outbox.items.end());
    assert(!completion_notification->synthetic);
    assert(completion_notification->reason == "project_completed");
    assert(completion_notification->state == "delivered");
    assert(notification_outbox.pending_count() == 1);

    const auto complete_projection =
        load_project_event_projection(outbox_paths, "complete", 100);
    assert(complete_projection.issues.empty());
    const auto notify_event = std::find_if(
        complete_projection.events.begin(),
        complete_projection.events.end(),
        [](const EventRecord& event) {
            return event.event_type == "notification.requested";
        });
    assert(notify_event != complete_projection.events.end());
    assert(notify_event->notification_id == "ntf-final");
    assert(notify_event->notification_target == "main_agent");
    assert(notify_event->notification_reason == "project_completed");

    const auto pending_json =
        notification_outbox_to_json(notification_outbox, false);
    assert(pending_json.at("pending_count").as_uint64() == 1);
    assert(pending_json.at("notifications").as_array().size() == 1);

    fs::remove_all(root);
    std::cout << "core tests passed\n";
    return 0;
}
