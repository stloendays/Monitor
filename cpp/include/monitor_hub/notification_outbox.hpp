#pragma once

#include "monitor_hub/core.hpp"
#include "monitor_hub/event_store.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace monitor_hub {

struct NotificationRecord {
    std::string notification_id;
    std::string source_event_id;
    std::string correlation_id;
    std::string target;
    std::string reason;
    std::string project_id;
    std::string task_id;
    std::string issue_id;
    std::string summary;
    std::string requested_at;
    std::string state;
    std::string state_at;
    std::string actor;
    bool synthetic = false;
};

struct NotificationOutbox {
    fs::path source_path;
    std::vector<NotificationRecord> items;
    std::vector<std::string> diagnostics;
    std::size_t malformed_lines = 0;
    std::size_t duplicate_records = 0;

    std::size_t pending_count() const;
};

NotificationOutbox load_notification_outbox(const RuntimePaths& paths);
NotificationOutbox sync_notification_outbox(const RuntimePaths& paths);

bool acknowledge_notification(
    const RuntimePaths& paths,
    const std::string& notification_id,
    const std::string& actor = "main_agent");

json::object notification_to_json(const NotificationRecord& notification);
json::object notification_outbox_to_json(
    const NotificationOutbox& outbox,
    bool include_acknowledged = false);

}  // namespace monitor_hub
