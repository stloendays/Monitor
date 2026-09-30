#pragma once

#include "monitor_hub/core.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace monitor_hub {

struct EventRecord {
    int schema_version = 0;
    std::string event_id;
    std::string event_type;
    std::string occurred_at;
    std::string project_id;
    std::string task_id;
    std::string issue_id;
    std::string agent_run_id;
    std::string correlation_id;
    std::string severity;
    std::string source_kind;
    std::string source_id;
    std::string summary;
    std::string authority;
    std::string notification_id;
    std::string notification_target;
    std::string notification_reason;
    std::vector<std::string> evidence_refs;
};

struct IssueProjection {
    std::string issue_id;
    std::string task_id;
    std::string state;
    std::string authority;
    std::string summary;
    std::string current_action;
    std::string agent_run_id;
    std::string last_event_at;
    bool user_action_required = false;
    bool resolved = false;
};

struct ProjectEventProjection {
    std::string project_id;
    fs::path source_path;
    std::vector<EventRecord> events;
    std::vector<IssueProjection> issues;
    std::vector<std::string> diagnostics;
    std::size_t duplicate_events = 0;
    std::size_t malformed_lines = 0;

    bool has_user_attention() const;
};

ProjectEventProjection load_project_event_projection(
    const RuntimePaths& paths,
    const std::string& project_id,
    std::size_t max_events = 500);

std::string event_display_name(const std::string& event_type);
std::string issue_state_display_name(const std::string& state);

}  // namespace monitor_hub
