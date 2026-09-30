#include "monitor_hub/notification_outbox.hpp"

#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace monitor_hub {
namespace {

constexpr int kSchemaVersion = 1;
constexpr auto kStaleLockAge = std::chrono::minutes(5);

std::string str(const json::value* value, std::string fallback = {}) {
    if (!value) return fallback;
    if (value->is_string()) return std::string(value->as_string());
    if (value->is_int64()) return std::to_string(value->as_int64());
    if (value->is_uint64()) return std::to_string(value->as_uint64());
    if (value->is_bool()) return value->as_bool() ? "true" : "false";
    return fallback;
}

bool boolean(const json::value* value, bool fallback = false) {
    return value && value->is_bool() ? value->as_bool() : fallback;
}

int integer(const json::value* value, int fallback = 0) {
    if (!value) return fallback;
    if (value->is_int64()) return static_cast<int>(value->as_int64());
    if (value->is_uint64()) return static_cast<int>(value->as_uint64());
    return fallback;
}

std::string utc_now_iso() {
    const auto now = std::chrono::system_clock::now();
    const auto value = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &value);
#else
    gmtime_r(&value, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

fs::path outbox_path(const RuntimePaths& paths) {
    return paths.hub_data / "outbox" / "notifications.jsonl";
}

fs::path outbox_lock_path(const RuntimePaths& paths) {
    return paths.hub_data / "outbox" / ".notifications.lock";
}

class OutboxLock {
public:
    explicit OutboxLock(const RuntimePaths& paths)
        : path_(outbox_lock_path(paths)) {
        std::error_code ec;
        fs::create_directories(path_.parent_path(), ec);
        if (ec)
            throw std::runtime_error(
                "cannot create outbox directory: " + ec.message());

        for (int attempt = 0; attempt < 2; ++attempt) {
            ec.clear();
            if (fs::create_directory(path_, ec)) {
                held_ = true;
                return;
            }
            if (ec && ec != std::errc::file_exists)
                throw std::runtime_error(
                    "cannot acquire notification outbox lock: " + ec.message());

            std::error_code time_ec;
            const auto modified = fs::last_write_time(path_, time_ec);
            if (!time_ec) {
                const auto age = fs::file_time_type::clock::now() - modified;
                if (age > kStaleLockAge) {
                    std::error_code remove_ec;
                    fs::remove_all(path_, remove_ec);
                    if (!remove_ec) continue;
                }
            }
            break;
        }
        throw std::runtime_error("notification outbox is busy");
    }

    OutboxLock(const OutboxLock&) = delete;
    OutboxLock& operator=(const OutboxLock&) = delete;

    ~OutboxLock() {
        if (!held_) return;
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

private:
    fs::path path_;
    bool held_ = false;
};

struct LoadedOutbox {
    NotificationOutbox outbox;
    std::set<std::string> record_ids;
    std::map<std::string, std::size_t> notification_index;
};

void append_diagnostic(NotificationOutbox& outbox, std::string message) {
    constexpr std::size_t kMaxDiagnostics = 32;
    if (outbox.diagnostics.size() < kMaxDiagnostics)
        outbox.diagnostics.push_back(std::move(message));
}

NotificationRecord parse_notification_record(
    const json::object& root,
    std::string& record_id,
    std::string& error) {

    NotificationRecord record;
    if (integer(root.if_contains("schema_version")) != kSchemaVersion) {
        error = "unsupported schema_version";
        return record;
    }

    record_id = str(root.if_contains("record_id"));
    record.notification_id = str(root.if_contains("notification_id"));
    record.source_event_id = str(root.if_contains("source_event_id"));
    record.correlation_id = str(root.if_contains("correlation_id"));
    record.target = str(root.if_contains("target"), "main_agent");
    record.reason = str(root.if_contains("reason"));
    record.project_id = str(root.if_contains("project_id"));
    record.task_id = str(root.if_contains("task_id"));
    record.issue_id = str(root.if_contains("issue_id"));
    record.summary = str(root.if_contains("summary"));
    record.requested_at = str(root.if_contains("requested_at"));
    record.state = str(root.if_contains("state"));
    record.state_at = str(root.if_contains("state_at"));
    record.actor = str(root.if_contains("actor"));
    record.synthetic = boolean(root.if_contains("synthetic"));

    if (record_id.empty()) error = "missing record_id";
    else if (record.notification_id.empty()) error = "missing notification_id";
    else if (record.state.empty()) error = "missing state";
    else if (record.state_at.empty()) error = "missing state_at";
    return record;
}

void apply_record(LoadedOutbox& loaded, const NotificationRecord& record) {
    auto found = loaded.notification_index.find(record.notification_id);
    if (found == loaded.notification_index.end()) {
        loaded.notification_index[record.notification_id] =
            loaded.outbox.items.size();
        loaded.outbox.items.push_back(record);
        return;
    }

    auto& current = loaded.outbox.items[found->second];
    if (!record.source_event_id.empty())
        current.source_event_id = record.source_event_id;
    if (!record.correlation_id.empty())
        current.correlation_id = record.correlation_id;
    if (!record.target.empty()) current.target = record.target;
    if (!record.reason.empty()) current.reason = record.reason;
    if (!record.project_id.empty()) current.project_id = record.project_id;
    if (!record.task_id.empty()) current.task_id = record.task_id;
    if (!record.issue_id.empty()) current.issue_id = record.issue_id;
    if (!record.summary.empty()) current.summary = record.summary;
    if (!record.requested_at.empty()) current.requested_at = record.requested_at;
    if (!record.state.empty()) current.state = record.state;
    if (!record.state_at.empty()) current.state_at = record.state_at;
    if (!record.actor.empty()) current.actor = record.actor;
    current.synthetic = current.synthetic || record.synthetic;
}

LoadedOutbox load_state(const RuntimePaths& paths) {
    LoadedOutbox loaded;
    loaded.outbox.source_path = outbox_path(paths);

    std::ifstream in(loaded.outbox.source_path, std::ios::binary);
    if (!in) return loaded;

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        if (line.empty()) continue;

        boost::system::error_code ec;
        auto value = json::parse(line, ec);
        if (ec || !value.is_object()) {
            ++loaded.outbox.malformed_lines;
            append_diagnostic(
                loaded.outbox,
                "line " + std::to_string(line_number) +
                    ": invalid JSON outbox record");
            continue;
        }

        std::string record_id;
        std::string error;
        const auto record =
            parse_notification_record(value.as_object(), record_id, error);
        if (!error.empty()) {
            ++loaded.outbox.malformed_lines;
            append_diagnostic(
                loaded.outbox,
                "line " + std::to_string(line_number) + ": " + error);
            continue;
        }
        if (!loaded.record_ids.insert(record_id).second) {
            ++loaded.outbox.duplicate_records;
            continue;
        }
        apply_record(loaded, record);
    }
    return loaded;
}

json::object outbox_line(
    const std::string& record_id,
    const NotificationRecord& record) {

    json::object root;
    root["schema_version"] = kSchemaVersion;
    root["record_id"] = record_id;
    root["notification_id"] = record.notification_id;
    root["state"] = record.state;
    root["state_at"] = record.state_at;
    root["target"] = record.target;
    root["reason"] = record.reason;
    root["project_id"] = record.project_id;
    if (!record.task_id.empty()) root["task_id"] = record.task_id;
    if (!record.issue_id.empty()) root["issue_id"] = record.issue_id;
    if (!record.source_event_id.empty())
        root["source_event_id"] = record.source_event_id;
    if (!record.correlation_id.empty())
        root["correlation_id"] = record.correlation_id;
    if (!record.summary.empty()) root["summary"] = record.summary;
    if (!record.requested_at.empty())
        root["requested_at"] = record.requested_at;
    if (!record.actor.empty()) root["actor"] = record.actor;
    root["synthetic"] = record.synthetic;
    return root;
}

void append_lines(
    const fs::path& path,
    const std::vector<std::pair<std::string, NotificationRecord>>& records) {

    if (records.empty()) return;
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out)
        throw std::runtime_error(
            "cannot open notification outbox: " + path.string());
    for (const auto& [record_id, record] : records)
        out << json::serialize(outbox_line(record_id, record)) << '\n';
    out.flush();
    if (!out)
        throw std::runtime_error(
            "failed writing notification outbox: " + path.string());
}

std::string decision_key(const EventRecord& event) {
    if (event.issue_id.empty()) return {};
    return "decision|" + event.project_id + "|" + event.issue_id;
}

std::string completion_key(const EventRecord& event) {
    return "completion|" + event.project_id;
}

std::string requested_key(
    const EventRecord& event,
    const std::set<std::string>& completion_correlations) {

    if (event.notification_reason == "decision_required" ||
        (!event.issue_id.empty() &&
         event.notification_reason != "project_completed"))
        return decision_key(event);

    if (event.notification_reason == "project_completed" ||
        event.notification_reason == "completion" ||
        event.notification_reason == "completed" ||
        (!event.correlation_id.empty() &&
         completion_correlations.contains(event.correlation_id)))
        return completion_key(event);
    return {};
}

NotificationRecord pending_from_request(const EventRecord& event) {
    NotificationRecord record;
    record.notification_id = event.notification_id.empty()
        ? "ntf:" + event.event_id
        : event.notification_id;
    record.source_event_id = event.event_id;
    record.correlation_id = event.correlation_id;
    record.target = event.notification_target.empty()
        ? "main_agent"
        : event.notification_target;
    record.reason = event.notification_reason.empty()
        ? (event.issue_id.empty() ? "notification" : "decision_required")
        : event.notification_reason;
    record.project_id = event.project_id;
    record.task_id = event.task_id;
    record.issue_id = event.issue_id;
    record.summary = event.summary.empty()
        ? event_display_name(event.event_type)
        : event.summary;
    record.requested_at = event.occurred_at;
    record.state = "pending";
    record.state_at = event.occurred_at;
    return record;
}

NotificationRecord pending_from_trigger(
    const EventRecord& event,
    const std::string& reason) {

    auto record = pending_from_request(event);
    record.notification_id = "ntf:auto:" + event.event_id;
    record.reason = reason;
    record.synthetic = true;
    return record;
}

std::vector<EventRecord> load_all_events(const RuntimePaths& paths) {
    std::vector<EventRecord> events;
    const auto dir = paths.hub_data / "events";
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return events;

    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const auto path = it->path();
        if (path.extension() == ".jsonl") files.push_back(path);
    }
    std::sort(files.begin(), files.end());

    for (const auto& path : files) {
        const auto project_id = path.stem().string();
        const auto projection =
            load_project_event_projection(paths, project_id, 0);
        events.insert(
            events.end(),
            projection.events.begin(),
            projection.events.end());
    }
    return events;
}

std::string event_notification_id(
    const EventRecord& event,
    const std::map<std::string, std::string>& correlation_to_id) {

    if (!event.notification_id.empty()) return event.notification_id;
    if (!event.correlation_id.empty()) {
        const auto found = correlation_to_id.find(event.correlation_id);
        if (found != correlation_to_id.end()) return found->second;
    }
    return {};
}

std::string transition_record_id(
    const EventRecord& event,
    const std::string& state) {
    return "obx:" + event.event_id + ":" + state;
}

void queue_if_new(
    LoadedOutbox& loaded,
    std::vector<std::pair<std::string, NotificationRecord>>& pending,
    const std::string& record_id,
    const NotificationRecord& record) {

    if (loaded.record_ids.contains(record_id)) return;
    loaded.record_ids.insert(record_id);
    pending.emplace_back(record_id, record);
    apply_record(loaded, record);
}

}  // namespace

std::size_t NotificationOutbox::pending_count() const {
    return static_cast<std::size_t>(std::count_if(
        items.begin(),
        items.end(),
        [](const NotificationRecord& item) {
            return item.state != "acknowledged";
        }));
}

NotificationOutbox load_notification_outbox(const RuntimePaths& paths) {
    return load_state(paths).outbox;
}

NotificationOutbox sync_notification_outbox(const RuntimePaths& paths) {
    const auto events = load_all_events(paths);

    std::set<std::string> completion_correlations;
    for (const auto& event : events)
        if (event.event_type == "project.completed" &&
            !event.correlation_id.empty())
            completion_correlations.insert(event.correlation_id);

    std::set<std::string> explicit_keys;
    std::map<std::string, std::string> correlation_to_id;
    std::map<std::string, NotificationRecord> seed_by_id;

    for (const auto& event : events) {
        if (event.event_type != "notification.requested") continue;
        auto record = pending_from_request(event);
        const auto key = requested_key(event, completion_correlations);
        if (!key.empty()) explicit_keys.insert(key);
        if (!record.correlation_id.empty())
            correlation_to_id[record.correlation_id] = record.notification_id;
        seed_by_id[record.notification_id] = record;
    }

    // Compatibility bridge: older producers may emit the decisive/completion
    // fact without the mandatory notification.requested event. Synthesize a
    // deterministic pending notification only when no explicit request covers
    // the same issue/project.
    for (const auto& event : events) {
        std::string key;
        std::string reason;
        if (event.event_type == "issue.user_action_required") {
            key = decision_key(event);
            reason = "decision_required";
        } else if (event.event_type == "project.completed") {
            key = completion_key(event);
            reason = "project_completed";
        } else {
            continue;
        }
        if (key.empty() || explicit_keys.contains(key)) continue;

        auto record = pending_from_trigger(event, reason);
        if (!record.correlation_id.empty() &&
            !correlation_to_id.contains(record.correlation_id))
            correlation_to_id[record.correlation_id] = record.notification_id;
        seed_by_id[record.notification_id] = record;
    }

    OutboxLock lock(paths);
    auto loaded = load_state(paths);
    std::vector<std::pair<std::string, NotificationRecord>> writes;

    // Requests first, preserving event order for audit readability.
    for (const auto& event : events) {
        if (event.event_type == "notification.requested") {
            const auto record = pending_from_request(event);
            queue_if_new(
                loaded,
                writes,
                "obx:" + event.event_id + ":pending",
                record);
            continue;
        }
        if (event.event_type == "issue.user_action_required") {
            const auto key = decision_key(event);
            if (key.empty() || explicit_keys.contains(key)) continue;
            const auto record = pending_from_trigger(event, "decision_required");
            queue_if_new(
                loaded,
                writes,
                "obx:auto:" + event.event_id + ":pending",
                record);
            continue;
        }
        if (event.event_type == "project.completed") {
            const auto key = completion_key(event);
            if (explicit_keys.contains(key)) continue;
            const auto record = pending_from_trigger(event, "project_completed");
            queue_if_new(
                loaded,
                writes,
                "obx:auto:" + event.event_id + ":pending",
                record);
        }
    }

    // Delivery/acknowledgement facts then advance the durable projection.
    for (const auto& event : events) {
        std::string state;
        if (event.event_type == "notification.delivered")
            state = "delivered";
        else if (event.event_type == "notification.acknowledged")
            state = "acknowledged";
        else
            continue;

        const auto notification_id =
            event_notification_id(event, correlation_to_id);
        if (notification_id.empty()) continue;

        NotificationRecord record;
        const auto seed = seed_by_id.find(notification_id);
        if (seed != seed_by_id.end())
            record = seed->second;
        else {
            const auto current = loaded.notification_index.find(notification_id);
            if (current == loaded.notification_index.end()) continue;
            record = loaded.outbox.items[current->second];
        }
        record.state = state;
        record.state_at = event.occurred_at;
        record.source_event_id = event.event_id;
        if (!event.summary.empty()) record.summary = event.summary;
        if (state == "acknowledged")
            record.actor = event.source_id.empty()
                ? "main_agent"
                : event.source_id;

        queue_if_new(
            loaded,
            writes,
            transition_record_id(event, state),
            record);
    }

    append_lines(loaded.outbox.source_path, writes);
    return loaded.outbox;
}

bool acknowledge_notification(
    const RuntimePaths& paths,
    const std::string& notification_id,
    const std::string& actor) {

    if (notification_id.empty()) return false;
    OutboxLock lock(paths);
    auto loaded = load_state(paths);
    const auto found = loaded.notification_index.find(notification_id);
    if (found == loaded.notification_index.end()) return false;

    auto record = loaded.outbox.items[found->second];
    if (record.state == "acknowledged") return true;
    record.state = "acknowledged";
    record.state_at = utc_now_iso();
    record.actor = actor.empty() ? "main_agent" : actor;

    const auto record_id = "obx:manual-ack:" + notification_id;
    if (!loaded.record_ids.contains(record_id))
        append_lines(loaded.outbox.source_path, {{record_id, record}});
    return true;
}

json::object notification_to_json(const NotificationRecord& notification) {
    json::object out;
    out["notification_id"] = notification.notification_id;
    out["state"] = notification.state;
    out["state_at"] = notification.state_at;
    out["target"] = notification.target;
    out["reason"] = notification.reason;
    out["project_id"] = notification.project_id;
    if (!notification.task_id.empty())
        out["task_id"] = notification.task_id;
    if (!notification.issue_id.empty())
        out["issue_id"] = notification.issue_id;
    out["summary"] = notification.summary;
    out["requested_at"] = notification.requested_at;
    if (!notification.source_event_id.empty())
        out["source_event_id"] = notification.source_event_id;
    if (!notification.correlation_id.empty())
        out["correlation_id"] = notification.correlation_id;
    if (!notification.actor.empty()) out["actor"] = notification.actor;
    out["synthetic"] = notification.synthetic;
    return out;
}

json::object notification_outbox_to_json(
    const NotificationOutbox& outbox,
    bool include_acknowledged) {

    json::array notifications;
    for (auto it = outbox.items.rbegin(); it != outbox.items.rend(); ++it) {
        if (!include_acknowledged && it->state == "acknowledged") continue;
        notifications.emplace_back(notification_to_json(*it));
    }

    json::array diagnostics;
    for (const auto& item : outbox.diagnostics)
        diagnostics.emplace_back(item);

    json::object result;
    result["schema_version"] = kSchemaVersion;
    result["source_path"] = outbox.source_path.string();
    result["pending_count"] =
        static_cast<std::uint64_t>(outbox.pending_count());
    result["notification_count"] =
        static_cast<std::uint64_t>(outbox.items.size());
    result["malformed_lines"] =
        static_cast<std::uint64_t>(outbox.malformed_lines);
    result["duplicate_records"] =
        static_cast<std::uint64_t>(outbox.duplicate_records);
    result["notifications"] = std::move(notifications);
    result["diagnostics"] = std::move(diagnostics);
    return result;
}

}  // namespace monitor_hub
