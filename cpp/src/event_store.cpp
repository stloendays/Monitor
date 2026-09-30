#include "monitor_hub/event_store.hpp"

#include <boost/system/error_code.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace monitor_hub {
namespace {

std::string str(const json::value* value, std::string fallback = {}) {
    if (!value) return fallback;
    if (value->is_string()) return std::string(value->as_string());
    if (value->is_int64()) return std::to_string(value->as_int64());
    if (value->is_uint64()) return std::to_string(value->as_uint64());
    if (value->is_bool()) return value->as_bool() ? "true" : "false";
    return fallback;
}

int integer(const json::value* value, int fallback = 0) {
    if (!value) return fallback;
    if (value->is_int64()) return static_cast<int>(value->as_int64());
    if (value->is_uint64()) return static_cast<int>(value->as_uint64());
    return fallback;
}

const json::object* object(const json::value* value) {
    return value && value->is_object() ? &value->as_object() : nullptr;
}

const json::array* array(const json::value* value) {
    return value && value->is_array() ? &value->as_array() : nullptr;
}

std::string payload_summary(const json::object& payload) {
    for (const auto* key : {
             "summary", "message", "reason", "classification",
             "outcome", "action", "action_type"}) {
        const auto value = str(payload.if_contains(key));
        if (!value.empty()) return value;
    }
    return {};
}

std::string payload_authority(const json::object& payload) {
    auto value = str(payload.if_contains("authority"));
    if (value.empty()) value = str(payload.if_contains("authority_level"));
    return value;
}

std::vector<std::string> payload_evidence_refs(const json::object& payload) {
    std::vector<std::string> refs;
    const auto* items = array(payload.if_contains("evidence_refs"));
    if (!items) return refs;
    for (const auto& item : *items) {
        const auto value = str(&item);
        if (!value.empty()) refs.push_back(value);
    }
    return refs;
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string suffix_after_dot(const std::string& value) {
    const auto pos = value.find('.');
    return pos == std::string::npos ? value : value.substr(pos + 1);
}

std::string fallback_summary(const EventRecord& event) {
    if (!event.summary.empty()) return event.summary;
    return event_display_name(event.event_type);
}

void append_diagnostic(ProjectEventProjection& out, std::string message) {
    constexpr std::size_t kMaxDiagnostics = 32;
    if (out.diagnostics.size() < kMaxDiagnostics)
        out.diagnostics.push_back(std::move(message));
}

bool parse_event(
    const json::object& root,
    const std::string& expected_project,
    EventRecord& event,
    std::string& error) {

    event.schema_version = integer(root.if_contains("schema_version"));
    event.event_id = str(root.if_contains("event_id"));
    event.event_type = str(root.if_contains("event_type"));
    event.occurred_at = str(root.if_contains("occurred_at"));
    event.project_id = str(root.if_contains("project_id"));
    event.task_id = str(root.if_contains("task_id"));
    event.issue_id = str(root.if_contains("issue_id"));
    event.agent_run_id = str(root.if_contains("agent_run_id"));
    event.correlation_id = str(root.if_contains("correlation_id"));
    event.severity = str(root.if_contains("severity"), "info");

    const auto* source = object(root.if_contains("source"));
    const auto* payload = object(root.if_contains("payload"));
    if (source) {
        event.source_kind = str(source->if_contains("kind"));
        event.source_id = str(source->if_contains("id"));
    }
    if (payload) {
        event.summary = payload_summary(*payload);
        event.authority = payload_authority(*payload);
        event.notification_id = str(payload->if_contains("notification_id"));
        event.notification_target = str(payload->if_contains("target"));
        event.notification_reason = str(payload->if_contains("reason"));
        if (event.task_id.empty())
            event.task_id = str(payload->if_contains("task_id"));
        if (event.issue_id.empty())
            event.issue_id = str(payload->if_contains("issue_id"));
        event.evidence_refs = payload_evidence_refs(*payload);
    }

    if (event.schema_version != 1) error = "unsupported schema_version";
    else if (event.event_id.empty()) error = "missing event_id";
    else if (event.event_type.empty()) error = "missing event_type";
    else if (event.occurred_at.empty()) error = "missing occurred_at";
    else if (event.project_id.empty()) error = "missing project_id";
    else if (event.project_id != expected_project) error = "project_id mismatch";
    else if (!source || event.source_kind.empty()) error = "missing source.kind";
    else if (!payload) error = "missing payload object";
    else return true;
    return false;
}

void update_issue_projection(
    ProjectEventProjection& out,
    std::map<std::string, std::size_t>& issue_index,
    const EventRecord& event) {

    const bool issue_related =
        starts_with(event.event_type, "issue.") ||
        starts_with(event.event_type, "agent.") ||
        event.event_type == "task.restarted";
    if (!issue_related) return;

    if (event.issue_id.empty()) {
        if (starts_with(event.event_type, "issue."))
            append_diagnostic(
                out,
                event.event_id + ": issue event missing issue_id");
        return;
    }

    auto found = issue_index.find(event.issue_id);
    if (found == issue_index.end()) {
        issue_index[event.issue_id] = out.issues.size();
        IssueProjection issue;
        issue.issue_id = event.issue_id;
        out.issues.push_back(std::move(issue));
        found = issue_index.find(event.issue_id);
    }
    auto& issue = out.issues[found->second];

    if (!event.task_id.empty()) issue.task_id = event.task_id;
    if (!event.agent_run_id.empty())
        issue.agent_run_id = event.agent_run_id;
    if (!event.authority.empty())
        issue.authority = event.authority;
    issue.last_event_at = event.occurred_at;

    if (starts_with(event.event_type, "issue.")) {
        issue.state = suffix_after_dot(event.event_type);
        if (!event.summary.empty()) issue.summary = event.summary;

        if (event.event_type == "issue.detected") {
            issue.agent_active = false;
            issue.agent_completed = false;
            issue.agent_failed = false;
            issue.action_applied = false;
            issue.task_restarted = false;
            issue.recovery_started = false;
            issue.recovery_verified = false;
            issue.user_action_required = false;
            issue.resolved = false;
        } else if (event.event_type ==
                   "issue.user_action_required") {
            issue.user_action_required = true;
            issue.resolved = false;
            if (issue.authority.empty()) issue.authority = "L3";
        } else if (event.event_type == "issue.escalated" &&
                   issue.authority == "L3") {
            issue.user_action_required = true;
            issue.resolved = false;
        } else if (event.event_type == "issue.action_applied") {
            issue.action_applied = true;
            issue.resolved = false;
        } else if (event.event_type == "issue.recovery_started") {
            issue.recovery_started = true;
            issue.resolved = false;
        } else if (event.event_type == "issue.recovery_verified") {
            issue.recovery_started = true;
            issue.recovery_verified = true;
            issue.user_action_required = false;
            issue.resolved = false;
        } else if (event.event_type == "issue.resolved") {
            issue.resolved = true;
            issue.user_action_required = false;
            issue.agent_active = false;
        } else {
            issue.resolved = false;
        }

        if (event.event_type == "issue.action_selected" ||
            event.event_type == "issue.action_applied") {
            issue.current_action = fallback_summary(event);
        }
        return;
    }

    if (starts_with(event.event_type, "agent.")) {
        if (issue.authority.empty()) issue.authority = "L2";
        if (!event.summary.empty())
            issue.current_action = event.summary;

        if (event.event_type == "agent.started" ||
            event.event_type == "agent.action_started") {
            issue.agent_active = true;
            issue.agent_completed = false;
            issue.agent_failed = false;
        } else if (event.event_type == "agent.action_finished") {
            issue.agent_active = false;
            issue.action_applied = true;
        } else if (event.event_type == "agent.completed") {
            issue.agent_active = false;
            issue.agent_completed = true;
            issue.agent_failed = false;
        } else if (event.event_type == "agent.failed") {
            issue.agent_active = false;
            issue.agent_completed = false;
            issue.agent_failed = true;
        }
        return;
    }

    if (event.event_type == "task.restarted") {
        issue.task_restarted = true;
        issue.action_applied = true;
        if (issue.authority.empty() &&
            event.source_kind == "core" &&
            event.source_id == "monitor-hub-dispatch-worker")
            issue.authority = "L1";
        if (!event.summary.empty())
            issue.current_action = event.summary;
    }
}


}  // namespace

bool ProjectEventProjection::has_user_attention() const {
    return std::any_of(
        issues.begin(),
        issues.end(),
        [](const IssueProjection& issue) {
            return !issue.resolved && issue.user_action_required;
        });
}

ProjectEventProjection load_project_event_projection(
    const RuntimePaths& paths,
    const std::string& project_id,
    std::size_t max_events) {

    ProjectEventProjection out;
    out.project_id = project_id;
    out.source_path = paths.hub_data / "events" / (project_id + ".jsonl");

    std::ifstream in(out.source_path, std::ios::binary);
    if (!in) return out;

    std::set<std::string> seen_event_ids;
    std::map<std::string, std::size_t> issue_index;
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(in, line)) {
        ++line_number;
        if (line.empty()) continue;

        boost::system::error_code ec;
        auto value = json::parse(line, ec);
        if (ec || !value.is_object()) {
            ++out.malformed_lines;
            append_diagnostic(
                out,
                "line " + std::to_string(line_number) + ": invalid JSON event");
            continue;
        }

        EventRecord event;
        std::string validation_error;
        if (!parse_event(value.as_object(), project_id, event, validation_error)) {
            ++out.malformed_lines;
            append_diagnostic(
                out,
                "line " + std::to_string(line_number) + ": " + validation_error);
            continue;
        }

        if (!seen_event_ids.insert(event.event_id).second) {
            ++out.duplicate_events;
            continue;
        }

        update_issue_projection(out, issue_index, event);
        out.events.push_back(std::move(event));
        if (max_events > 0 && out.events.size() > max_events) {
            const auto remove_count = out.events.size() - max_events;
            out.events.erase(
                out.events.begin(),
                out.events.begin() + static_cast<std::ptrdiff_t>(remove_count));
        }
    }

    return out;
}

std::string event_display_name(const std::string& event_type) {
    static const std::map<std::string, std::string> names = {
        {"project.completed", "项目完成"},
        {"monitor.check_completed", "监控检查完成"},
        {"task.started", "任务开始"},
        {"task.progress", "任务进度"},
        {"task.failed", "任务失败"},
        {"task.restarted", "任务已重启"},
        {"task.completed", "任务完成"},
        {"issue.detected", "发现问题"},
        {"issue.classified", "问题已分类"},
        {"issue.assigned", "问题已分派"},
        {"issue.investigating", "正在调查"},
        {"issue.action_selected", "已选择处理动作"},
        {"issue.action_applied", "已执行处理动作"},
        {"issue.recovery_started", "恢复流程已开始"},
        {"issue.recovery_verified", "恢复已验证"},
        {"issue.resolved", "问题已解决"},
        {"issue.escalated", "问题已升级"},
        {"issue.user_action_required", "需要用户/主 Agent 决策"},
        {"agent.started", "Child Agent 已启动"},
        {"agent.evidence_recorded", "Agent 已记录证据"},
        {"agent.analysis_recorded", "Agent 已记录分析"},
        {"agent.action_proposed", "Agent 提出动作"},
        {"agent.action_started", "Agent 开始执行"},
        {"agent.action_finished", "Agent 动作完成"},
        {"agent.completed", "Agent 处理完成"},
        {"agent.failed", "Agent 处理失败"},
        {"notification.requested", "已请求通知"},
        {"notification.delivered", "通知已送达"},
        {"notification.acknowledged", "通知已确认"},
    };
    const auto found = names.find(event_type);
    return found == names.end() ? event_type : found->second;
}

std::string issue_state_display_name(const std::string& state) {
    static const std::map<std::string, std::string> names = {
        {"detected", "已发现"},
        {"classified", "已分类"},
        {"assigned", "已分派"},
        {"investigating", "调查中"},
        {"action_selected", "已选择动作"},
        {"action_applied", "动作已执行"},
        {"recovery_started", "恢复中"},
        {"recovery_verified", "恢复已验证"},
        {"resolved", "已解决"},
        {"escalated", "已升级"},
        {"user_action_required", "等待决策"},
    };
    const auto found = names.find(state);
    return found == names.end() ? state : found->second;
}

std::string issue_recovery_stage(const IssueProjection& issue) {
    if (issue.resolved) return "resolved";
    if (issue.user_action_required) return "needs_user";
    if (issue.agent_failed) return "failed";
    if (issue.recovery_verified) return "recovery_verified";
    if (issue.recovery_started) return "recovery_verification";
    if (issue.task_restarted || issue.action_applied)
        return "waiting_verification";
    if (issue.agent_active) return "agent_handling";
    if (issue.agent_completed) return "agent_completed";
    if (issue.state == "action_selected") return "action_selected";
    if (issue.state == "investigating") return "investigating";
    if (issue.state == "assigned") return "assigned";
    if (issue.state == "classified") return "classified";
    if (issue.state == "escalated") return "escalated";
    if (issue.state == "detected") return "detected";
    return issue.state.empty() ? "waiting" : issue.state;
}

std::string issue_recovery_stage_display_name(
    const std::string& stage) {
    static const std::map<std::string, std::string> names = {
        {"detected", "已发现问题"},
        {"classified", "已完成分类"},
        {"assigned", "已完成分派"},
        {"investigating", "正在调查"},
        {"agent_handling", "Child Agent 正在处理"},
        {"agent_completed", "Agent 已完成，等待后续处理"},
        {"action_selected", "已选择恢复动作"},
        {"waiting_verification", "动作已完成，等待恢复验证"},
        {"recovery_verification", "正在验证恢复"},
        {"recovery_verified", "恢复已验证"},
        {"resolved", "问题已解决"},
        {"needs_user", "等待用户/主 Agent 决策"},
        {"failed", "自动处理失败"},
        {"escalated", "问题已升级"},
        {"waiting", "等待更多事件"},
    };
    const auto found = names.find(stage);
    return found == names.end() ? stage : found->second;
}

std::string issue_recovery_next_step(const IssueProjection& issue) {
    const auto stage = issue_recovery_stage(issue);
    if (stage == "resolved") return "无";
    if (stage == "needs_user")
        return "等待用户/主 Agent 记录明确决定";
    if (stage == "failed")
        return "查看失败证据并升级或重新选择受权动作";
    if (stage == "recovery_verified")
        return "等待 issue.resolved 关闭问题";
    if (stage == "recovery_verification")
        return "按完成标准检查下游运行证据";
    if (stage == "waiting_verification")
        return "Monitor 独立验证恢复，不以动作成功代替恢复";
    if (stage == "agent_handling")
        return "等待 Child Agent 记录证据、分析或受限动作";
    if (stage == "agent_completed")
        return "Monitor/Policy 根据 Agent 结论继续处理";
    if (stage == "action_selected")
        return "执行已授权的恢复动作";
    if (stage == "assigned" || stage == "investigating")
        return "收集证据并在 Policy 权限内选择动作";
    if (stage == "classified")
        return "按 Recovery Policy 分派 L1/L2/L3";
    if (stage == "escalated")
        return "等待升级路径接管";
    if (stage == "detected")
        return "分类问题并匹配 Recovery Policy";
    return "等待下一条协议事件";
}

}  // namespace monitor_hub
