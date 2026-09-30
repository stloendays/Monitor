#include "monitor_hub/command_control.hpp"

#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
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

const json::object* object(const json::value* value) {
    return value && value->is_object() ? &value->as_object() : nullptr;
}

const json::array* array(const json::value* value) {
    return value && value->is_array() ? &value->as_array() : nullptr;
}

std::vector<std::string> string_array(const json::value* value) {
    std::vector<std::string> result;
    const auto* values = array(value);
    if (!values) return result;
    for (const auto& item : *values) {
        const auto text = str(&item);
        if (!text.empty()) result.push_back(text);
    }
    return result;
}

json::array json_strings(const std::vector<std::string>& values) {
    json::array result;
    for (const auto& value : values) result.emplace_back(value);
    return result;
}

void append_unique(
    std::vector<std::string>& target,
    const std::vector<std::string>& source) {
    std::set<std::string> seen(target.begin(), target.end());
    for (const auto& item : source) {
        if (item.empty() || !seen.insert(item).second) continue;
        target.push_back(item);
    }
}

bool contains(
    const std::vector<std::string>& values,
    const std::string& expected) {
    return std::find(values.begin(), values.end(), expected) != values.end();
}

fs::path commands_dir(const RuntimePaths& paths) {
    return paths.hub_data / "commands";
}

fs::path inbox_path(const RuntimePaths& paths) {
    return commands_dir(paths) / "inbox.jsonl";
}

fs::path receipt_path(const RuntimePaths& paths) {
    return commands_dir(paths) / "receipts.jsonl";
}

fs::path control_lock_path(const RuntimePaths& paths) {
    return commands_dir(paths) / ".control.lock";
}

class ControlLock {
public:
    explicit ControlLock(const RuntimePaths& paths)
        : path_(control_lock_path(paths)) {
        std::error_code ec;
        fs::create_directories(path_.parent_path(), ec);
        if (ec)
            throw std::runtime_error(
                "cannot create command directory: " + ec.message());

        for (int attempt = 0; attempt < 2; ++attempt) {
            ec.clear();
            if (fs::create_directory(path_, ec)) {
                held_ = true;
                return;
            }
            if (ec && ec != std::errc::file_exists)
                throw std::runtime_error(
                    "cannot acquire command control lock: " + ec.message());

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
        throw std::runtime_error("command control plane is busy");
    }

    ControlLock(const ControlLock&) = delete;
    ControlLock& operator=(const ControlLock&) = delete;

    ~ControlLock() {
        if (!held_) return;
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

private:
    fs::path path_;
    bool held_ = false;
};

void append_json_line(const fs::path& path, const json::object& value) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
        throw std::runtime_error(
            "cannot create directory for " + path.string() + ": " +
            ec.message());

    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out)
        throw std::runtime_error("cannot open " + path.string());
    out << json::serialize(value) << '\n';
    out.flush();
    if (!out)
        throw std::runtime_error("failed writing " + path.string());
}

void write_json_atomic(const fs::path& path, const json::object& value) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
        throw std::runtime_error(
            "cannot create dispatch directory: " + ec.message());

    if (fs::exists(path, ec) && !ec) return;

    const auto tmp = fs::path(path.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot write " + tmp.string());
        out << json::serialize(value) << '\n';
        out.flush();
        if (!out)
            throw std::runtime_error("failed writing " + tmp.string());
    }

    ec.clear();
    fs::rename(tmp, path, ec);
    if (!ec) return;

    if (fs::exists(path)) {
        std::error_code cleanup;
        fs::remove(tmp, cleanup);
        return;
    }

    std::error_code cleanup;
    fs::remove(tmp, cleanup);
    throw std::runtime_error(
        "cannot publish dispatch file " + path.string() + ": " +
        ec.message());
}

std::string safe_filename(const std::string& value) {
    std::string safe;
    safe.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.')
            safe.push_back(static_cast<char>(ch));
        else
            safe.push_back('_');
    }
    if (safe.empty()) safe = "command";
    if (safe.size() > 72) safe.resize(72);

    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    std::ostringstream suffix;
    suffix << std::hex << std::setw(16) << std::setfill('0') << hash;
    return safe + "-" + suffix.str() + ".json";
}

bool safe_project_id(const std::string& project_id) {
    if (project_id.empty()) return false;
    return std::all_of(
        project_id.begin(),
        project_id.end(),
        [](const unsigned char ch) {
            return std::isalnum(ch) ||
                   ch == '-' || ch == '_' || ch == '.';
        });
}

bool safe_policy_ref(const std::string& policy_ref) {
    if (policy_ref.empty() ||
        policy_ref.find('\\') != std::string::npos)
        return false;

    const fs::path path(policy_ref);
    if (path.is_absolute()) return false;

    std::size_t meaningful_parts = 0;
    bool first = true;
    for (const auto& part : path) {
        const auto text = part.string();
        if (text.empty() || text == ".") continue;
        if (text == "..") return false;
        ++meaningful_parts;
        if (first) {
            if (text != "policies") return false;
            first = false;
        }
    }
    return !first && meaningful_parts >= 2;
}

fs::path resolve_policy_path(
    const RuntimePaths& paths,
    const std::string& policy_ref) {

    if (!safe_policy_ref(policy_ref))
        return {};

    auto path = (paths.hub_data / fs::path(policy_ref)).lexically_normal();
    std::error_code ec;
    if (fs::is_regular_file(path, ec) && !ec)
        return path;

    if (!path.has_extension()) {
        auto with_json = path;
        with_json += ".json";
        ec.clear();
        if (fs::is_regular_file(with_json, ec) && !ec)
            return with_json;
        return with_json;
    }
    return path;
}

std::optional<RecoveryPolicy> parse_policy(
    const json::object& root,
    const fs::path& source_path,
    std::string& error) {

    RecoveryPolicy policy;
    policy.schema_version = integer(root.if_contains("schema_version"));
    policy.policy_id = str(root.if_contains("policy_id"));
    policy.project_id = str(root.if_contains("project_id"));
    policy.source_path = source_path;

    if (policy.schema_version != kSchemaVersion) {
        error = "unsupported policy schema_version";
        return std::nullopt;
    }
    if (policy.policy_id.empty()) {
        error = "policy missing policy_id";
        return std::nullopt;
    }
    if (policy.project_id.empty()) {
        error = "policy missing project_id";
        return std::nullopt;
    }

    const auto* actions = array(root.if_contains("actions"));
    if (!actions) {
        error = "policy missing actions array";
        return std::nullopt;
    }

    std::set<std::string> action_ids;
    for (const auto& value : *actions) {
        if (!value.is_object()) {
            error = "policy action must be an object";
            return std::nullopt;
        }
        const auto& item = value.as_object();
        PolicyAction action;
        action.action_id = str(item.if_contains("action_id"));
        action.authority = str(item.if_contains("authority"));
        action.dispatch_kind = str(item.if_contains("dispatch_kind"));
        action.handler = str(item.if_contains("handler"));
        action.agent_profile = str(item.if_contains("agent_profile"));
        action.allowed_command_types =
            string_array(item.if_contains("allowed_command_types"));
        action.constraints = string_array(item.if_contains("constraints"));
        action.completion_criteria =
            string_array(item.if_contains("completion_criteria"));
        action.enabled = boolean(item.if_contains("enabled"), true);

        if (action.action_id.empty()) {
            error = "policy action missing action_id";
            return std::nullopt;
        }
        if (!action_ids.insert(action.action_id).second) {
            error = "duplicate policy action_id: " + action.action_id;
            return std::nullopt;
        }
        if (action.authority != "L1" && action.authority != "L2") {
            error = "policy action authority must be L1 or L2";
            return std::nullopt;
        }
        if (action.authority == "L1" &&
            action.dispatch_kind != "deterministic") {
            error =
                "L1 policy action must use dispatch_kind=deterministic";
            return std::nullopt;
        }
        if (action.authority == "L2" &&
            action.dispatch_kind != "child_agent") {
            error =
                "L2 policy action must use dispatch_kind=child_agent";
            return std::nullopt;
        }
        if (action.authority == "L1" && action.handler.empty()) {
            error = "L1 policy action missing deterministic handler";
            return std::nullopt;
        }
        if (action.authority == "L2" && action.agent_profile.empty()) {
            error = "L2 policy action missing agent_profile";
            return std::nullopt;
        }
        if (action.allowed_command_types.empty()) {
            error =
                "policy action missing allowed_command_types: " +
                action.action_id;
            return std::nullopt;
        }
        policy.actions.push_back(std::move(action));
    }

    return policy;
}

std::set<std::string> load_processed_command_ids(
    const RuntimePaths& paths) {

    std::set<std::string> result;
    std::ifstream in(receipt_path(paths), std::ios::binary);
    if (!in) return result;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        boost::system::error_code ec;
        const auto value = json::parse(line, ec);
        if (ec || !value.is_object()) continue;
        const auto command_id =
            str(value.as_object().if_contains("command_id"));
        if (!command_id.empty()) result.insert(command_id);
    }
    return result;
}

std::set<std::string> load_inbox_command_ids(
    const RuntimePaths& paths) {

    std::set<std::string> result;
    std::ifstream in(inbox_path(paths), std::ios::binary);
    if (!in) return result;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        boost::system::error_code ec;
        const auto value = json::parse(line, ec);
        if (ec || !value.is_object()) continue;
        const auto command_id =
            str(value.as_object().if_contains("command_id"));
        if (!command_id.empty()) result.insert(command_id);
    }
    return result;
}

json::object receipt_line(const CommandReceipt& receipt) {
    json::object out;
    out["schema_version"] = kSchemaVersion;
    out["command_id"] = receipt.command_id;
    out["command_type"] = receipt.command_type;
    out["project_id"] = receipt.project_id;
    out["state"] = receipt.state;
    out["reason"] = receipt.reason;
    out["processed_at"] = receipt.processed_at;
    if (!receipt.policy_id.empty()) out["policy_id"] = receipt.policy_id;
    if (!receipt.action_id.empty()) out["action_id"] = receipt.action_id;
    if (!receipt.dispatch_kind.empty())
        out["dispatch_kind"] = receipt.dispatch_kind;
    if (!receipt.dispatch_path.empty())
        out["dispatch_path"] = receipt.dispatch_path.string();
    return out;
}

void record_receipt(
    const RuntimePaths& paths,
    CommandControlResult& result,
    CommandReceipt receipt) {

    append_json_line(receipt_path(paths), receipt_line(receipt));
    if (receipt.state == "queued_l1") ++result.queued_l1;
    else if (receipt.state == "queued_l2") ++result.queued_l2;
    else if (receipt.state == "needs_user") ++result.escalated_l3;
    else if (receipt.state == "rejected") ++result.rejected;
    result.receipts.push_back(std::move(receipt));
}

json::object command_to_json(const CommandEnvelope& command) {
    json::object requested_by;
    requested_by["kind"] = command.requested_by_kind;
    requested_by["id"] = command.requested_by_id;

    json::object out;
    out["schema_version"] = command.schema_version;
    out["command_id"] = command.command_id;
    out["command_type"] = command.command_type;
    out["requested_at"] = command.requested_at;
    out["project_id"] = command.project_id;
    if (!command.task_id.empty()) out["task_id"] = command.task_id;
    if (!command.issue_id.empty()) out["issue_id"] = command.issue_id;
    if (!command.correlation_id.empty())
        out["correlation_id"] = command.correlation_id;
    out["requested_by"] = std::move(requested_by);
    out["authority"] = command.authority;
    if (!command.policy_ref.empty()) out["policy_ref"] = command.policy_ref;
    out["constraints"] = json_strings(command.constraints);
    out["completion_criteria"] =
        json_strings(command.completion_criteria);
    out["context_refs"] = json_strings(command.context_refs);
    out["payload"] = command.payload;
    return out;
}

std::string event_id_for(
    const CommandEnvelope& command,
    const std::string& suffix) {
    return "evt:cmd:" + command.command_id + ":" + suffix;
}

bool event_id_exists(
    const fs::path& event_path,
    const std::string& event_id) {

    std::ifstream in(event_path, std::ios::binary);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        boost::system::error_code ec;
        const auto value = json::parse(line, ec);
        if (ec || !value.is_object()) continue;
        if (str(value.as_object().if_contains("event_id")) == event_id)
            return true;
    }
    return false;
}

void append_event_if_missing(
    const RuntimePaths& paths,
    const std::string& project_id,
    const json::object& event) {

    const auto event_id = str(event.if_contains("event_id"));
    if (event_id.empty()) return;

    const auto path =
        paths.hub_data / "events" / (project_id + ".jsonl");
    if (event_id_exists(path, event_id)) return;
    append_json_line(path, event);
}

json::object event_source() {
    json::object source;
    source["kind"] = "core";
    source["id"] = "monitor-hub-command-dispatcher";
    return source;
}

void emit_l3_escalation(
    const RuntimePaths& paths,
    const CommandEnvelope& command) {

    const auto issue_id = command.issue_id.empty()
        ? "iss:cmd:" + command.command_id
        : command.issue_id;
    const auto correlation_id = command.correlation_id.empty()
        ? "corr:cmd:" + command.command_id
        : command.correlation_id;
    const auto occurred_at = iso_now_local();

    json::object issue_payload;
    issue_payload["summary"] =
        "Command requires main-Agent/user authority: " +
        command.command_type;
    issue_payload["authority"] = "L3";
    issue_payload["command_id"] = command.command_id;

    json::object issue_event;
    issue_event["schema_version"] = kSchemaVersion;
    issue_event["event_id"] =
        event_id_for(command, "user_action_required");
    issue_event["event_type"] = "issue.user_action_required";
    issue_event["occurred_at"] = occurred_at;
    issue_event["project_id"] = command.project_id;
    if (!command.task_id.empty())
        issue_event["task_id"] = command.task_id;
    issue_event["issue_id"] = issue_id;
    issue_event["correlation_id"] = correlation_id;
    issue_event["source"] = event_source();
    issue_event["severity"] = "warning";
    issue_event["payload"] = std::move(issue_payload);
    append_event_if_missing(
        paths,
        command.project_id,
        issue_event);

    json::object notification_payload;
    notification_payload["notification_id"] =
        "ntf:cmd:" + command.command_id;
    notification_payload["target"] = "main_agent";
    notification_payload["reason"] = "decision_required";
    notification_payload["summary"] =
        "L3 decision required for command " + command.command_type;
    notification_payload["command_id"] = command.command_id;
    notification_payload["project_id"] = command.project_id;
    if (!command.task_id.empty())
        notification_payload["task_id"] = command.task_id;
    notification_payload["issue_id"] = issue_id;

    json::object notification_event;
    notification_event["schema_version"] = kSchemaVersion;
    notification_event["event_id"] =
        event_id_for(command, "notification_requested");
    notification_event["event_type"] = "notification.requested";
    notification_event["occurred_at"] = occurred_at;
    notification_event["project_id"] = command.project_id;
    if (!command.task_id.empty())
        notification_event["task_id"] = command.task_id;
    notification_event["issue_id"] = issue_id;
    notification_event["correlation_id"] = correlation_id;
    notification_event["source"] = event_source();
    notification_event["severity"] = "warning";
    notification_event["payload"] =
        std::move(notification_payload);
    append_event_if_missing(
        paths,
        command.project_id,
        notification_event);
}

json::object dispatch_record(
    const CommandEnvelope& command,
    const RecoveryPolicy& policy,
    const PolicyAction& action,
    const std::vector<std::string>& constraints,
    const std::vector<std::string>& completion_criteria) {

    json::object out;
    out["schema_version"] = kSchemaVersion;
    out["dispatch_id"] = "dsp:" + command.command_id;
    out["command_id"] = command.command_id;
    out["command_type"] = command.command_type;
    out["dispatched_at"] = iso_now_local();
    out["project_id"] = command.project_id;
    if (!command.task_id.empty()) out["task_id"] = command.task_id;
    if (!command.issue_id.empty()) out["issue_id"] = command.issue_id;
    if (!command.correlation_id.empty())
        out["correlation_id"] = command.correlation_id;
    out["authority"] = command.authority;
    out["policy_id"] = policy.policy_id;
    out["policy_ref"] = command.policy_ref;
    out["action_id"] = action.action_id;
    out["dispatch_kind"] = action.dispatch_kind;
    if (!action.handler.empty()) out["handler"] = action.handler;
    if (!action.agent_profile.empty())
        out["agent_profile"] = action.agent_profile;
    out["constraints"] = json_strings(constraints);
    out["completion_criteria"] =
        json_strings(completion_criteria);
    out["context_refs"] = json_strings(command.context_refs);
    out["requested_command"] = command_to_json(command);
    return out;
}

CommandReceipt rejected_receipt(
    const CommandEnvelope& command,
    std::string reason) {

    CommandReceipt receipt;
    receipt.command_id = command.command_id;
    receipt.command_type = command.command_type;
    receipt.project_id = command.project_id;
    receipt.state = "rejected";
    receipt.reason = std::move(reason);
    receipt.processed_at = iso_now_local();
    return receipt;
}

void append_diagnostic(
    CommandControlResult& result,
    std::string message) {
    constexpr std::size_t kMaxDiagnostics = 64;
    if (result.diagnostics.size() < kMaxDiagnostics)
        result.diagnostics.push_back(std::move(message));
}

}  // namespace

std::optional<CommandEnvelope> parse_command_envelope(
    const json::object& root,
    std::string& error) {

    CommandEnvelope command;
    command.schema_version =
        integer(root.if_contains("schema_version"));
    command.command_id = str(root.if_contains("command_id"));
    command.command_type = str(root.if_contains("command_type"));
    command.requested_at = str(root.if_contains("requested_at"));
    command.project_id = str(root.if_contains("project_id"));
    command.task_id = str(root.if_contains("task_id"));
    command.issue_id = str(root.if_contains("issue_id"));
    command.correlation_id = str(root.if_contains("correlation_id"));
    command.authority = str(root.if_contains("authority"));
    command.policy_ref = str(root.if_contains("policy_ref"));
    command.constraints =
        string_array(root.if_contains("constraints"));
    command.completion_criteria =
        string_array(root.if_contains("completion_criteria"));
    command.context_refs =
        string_array(root.if_contains("context_refs"));

    const auto* requested_by =
        object(root.if_contains("requested_by"));
    if (requested_by) {
        command.requested_by_kind =
            str(requested_by->if_contains("kind"));
        command.requested_by_id =
            str(requested_by->if_contains("id"));
    }

    const auto* payload = object(root.if_contains("payload"));
    if (payload) command.payload = *payload;

    if (command.schema_version != kSchemaVersion)
        error = "unsupported command schema_version";
    else if (command.command_id.empty())
        error = "missing command_id";
    else if (command.command_type.empty())
        error = "missing command_type";
    else if (command.requested_at.empty())
        error = "missing requested_at";
    else if (command.project_id.empty())
        error = "missing project_id";
    else if (!safe_project_id(command.project_id))
        error = "project_id must be a filesystem-safe stable ID";
    else if (!requested_by ||
             command.requested_by_kind.empty() ||
             command.requested_by_id.empty())
        error = "missing requested_by identity";
    else if (command.authority != "L1" &&
             command.authority != "L2" &&
             command.authority != "L3")
        error = "authority must be L1, L2, or L3";
    else if (!payload)
        error = "missing payload object";
    else if ((command.authority == "L1" ||
              command.authority == "L2") &&
             command.policy_ref.empty())
        error = "L1/L2 command missing policy_ref";
    else
        return command;

    return std::nullopt;
}

std::optional<RecoveryPolicy> load_recovery_policy(
    const RuntimePaths& paths,
    const std::string& policy_ref,
    std::string& error) {

    if (!safe_policy_ref(policy_ref)) {
        error =
            "policy_ref must stay under HUB_DATA/policies and cannot contain '..'";
        return std::nullopt;
    }

    const auto path = resolve_policy_path(paths, policy_ref);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "policy not found: " + path.string();
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    boost::system::error_code ec;
    auto value = json::parse(buffer.str(), ec);
    if (ec || !value.is_object()) {
        error = "invalid policy JSON: " + path.string();
        return std::nullopt;
    }
    return parse_policy(value.as_object(), path, error);
}

const PolicyAction* find_policy_action(
    const RecoveryPolicy& policy,
    const std::string& action_id) {

    const auto found = std::find_if(
        policy.actions.begin(),
        policy.actions.end(),
        [&](const PolicyAction& action) {
            return action.action_id == action_id;
        });
    return found == policy.actions.end() ? nullptr : &*found;
}

CommandSubmitResult submit_command(
    const RuntimePaths& paths,
    const json::object& command_json) {

    CommandSubmitResult result;
    result.inbox_path = inbox_path(paths);

    std::string error;
    const auto command = parse_command_envelope(command_json, error);
    if (!command) {
        result.reason = error;
        return result;
    }
    result.command_id = command->command_id;

    ControlLock lock(paths);
    const auto existing = load_inbox_command_ids(paths);
    if (existing.contains(command->command_id)) {
        result.duplicate = true;
        result.reason = "command_id already exists in inbox";
        return result;
    }

    append_json_line(result.inbox_path, command_json);
    result.accepted = true;
    result.reason = "queued";
    return result;
}

CommandSubmitResult submit_command_file(
    const RuntimePaths& paths,
    const fs::path& command_file) {

    CommandSubmitResult result;
    result.inbox_path = inbox_path(paths);

    std::ifstream in(command_file, std::ios::binary);
    if (!in) {
        result.reason =
            "cannot read command file: " + command_file.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();

    boost::system::error_code ec;
    auto value = json::parse(buffer.str(), ec);
    if (ec || !value.is_object()) {
        result.reason =
            "command file must contain one JSON object";
        return result;
    }
    return submit_command(paths, value.as_object());
}

CommandControlResult dispatch_pending_commands(
    const RuntimePaths& paths) {

    CommandControlResult result;
    result.inbox_path = inbox_path(paths);
    result.receipt_path = receipt_path(paths);

    ControlLock lock(paths);
    auto processed = load_processed_command_ids(paths);
    std::set<std::string> seen_inbox;

    std::ifstream in(result.inbox_path, std::ios::binary);
    if (!in) return result;

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        if (line.empty()) continue;

        boost::system::error_code ec;
        auto value = json::parse(line, ec);
        if (ec || !value.is_object()) {
            ++result.malformed_commands;
            append_diagnostic(
                result,
                "line " + std::to_string(line_number) +
                    ": invalid command JSON");
            continue;
        }

        const auto& root = value.as_object();
        const auto raw_id = str(root.if_contains("command_id"));
        if (!raw_id.empty() &&
            !seen_inbox.insert(raw_id).second) {
            ++result.duplicate_commands;
            continue;
        }
        if (!raw_id.empty() && processed.contains(raw_id)) {
            ++result.already_processed;
            continue;
        }

        std::string parse_error;
        const auto command =
            parse_command_envelope(root, parse_error);
        if (!command) {
            ++result.malformed_commands;
            append_diagnostic(
                result,
                "line " + std::to_string(line_number) +
                    ": " + parse_error);
            if (!raw_id.empty()) {
                CommandEnvelope partial;
                partial.command_id = raw_id;
                partial.command_type =
                    str(root.if_contains("command_type"));
                partial.project_id =
                    str(root.if_contains("project_id"));
                record_receipt(
                    paths,
                    result,
                    rejected_receipt(
                        partial,
                        "invalid command envelope: " +
                            parse_error));
                processed.insert(raw_id);
            }
            continue;
        }

        if (command->authority == "L3") {
            emit_l3_escalation(paths, *command);

            CommandReceipt receipt;
            receipt.command_id = command->command_id;
            receipt.command_type = command->command_type;
            receipt.project_id = command->project_id;
            receipt.state = "needs_user";
            receipt.reason =
                "L3 command requires main-Agent/user decision";
            receipt.processed_at = iso_now_local();
            record_receipt(paths, result, std::move(receipt));
            processed.insert(command->command_id);
            continue;
        }

        std::string policy_error;
        const auto policy =
            load_recovery_policy(
                paths,
                command->policy_ref,
                policy_error);
        if (!policy) {
            record_receipt(
                paths,
                result,
                rejected_receipt(
                    *command,
                    policy_error));
            processed.insert(command->command_id);
            continue;
        }
        if (policy->project_id != command->project_id) {
            record_receipt(
                paths,
                result,
                rejected_receipt(
                    *command,
                    "policy project_id does not match command project_id"));
            processed.insert(command->command_id);
            continue;
        }

        const auto action_id =
            str(command->payload.if_contains("action_id"));
        if (action_id.empty()) {
            record_receipt(
                paths,
                result,
                rejected_receipt(
                    *command,
                    "L1/L2 command payload missing action_id"));
            processed.insert(command->command_id);
            continue;
        }

        const auto* action =
            find_policy_action(*policy, action_id);
        if (!action) {
            record_receipt(
                paths,
                result,
                rejected_receipt(
                    *command,
                    "action_id is not present in policy"));
            processed.insert(command->command_id);
            continue;
        }
        if (!action->enabled) {
            record_receipt(
                paths,
                result,
                rejected_receipt(
                    *command,
                    "policy action is disabled"));
            processed.insert(command->command_id);
            continue;
        }
        if (action->authority != command->authority) {
            record_receipt(
                paths,
                result,
                rejected_receipt(
                    *command,
                    "command authority does not match policy action"));
            processed.insert(command->command_id);
            continue;
        }
        if (!contains(
                action->allowed_command_types,
                command->command_type)) {
            record_receipt(
                paths,
                result,
                rejected_receipt(
                    *command,
                    "command_type is not allowed by policy action"));
            processed.insert(command->command_id);
            continue;
        }

        std::vector<std::string> constraints =
            action->constraints;
        append_unique(constraints, command->constraints);
        std::vector<std::string> completion_criteria =
            action->completion_criteria;
        append_unique(
            completion_criteria,
            command->completion_criteria);

        const auto lane =
            command->authority == "L1" ? "l1" : "l2";
        const auto path =
            paths.hub_data / "dispatch" / lane /
            safe_filename(command->command_id);
        const auto dispatch = dispatch_record(
            *command,
            *policy,
            *action,
            constraints,
            completion_criteria);
        write_json_atomic(path, dispatch);

        CommandReceipt receipt;
        receipt.command_id = command->command_id;
        receipt.command_type = command->command_type;
        receipt.project_id = command->project_id;
        receipt.state =
            command->authority == "L1"
            ? "queued_l1"
            : "queued_l2";
        receipt.reason =
            command->authority == "L1"
            ? "validated deterministic action queued"
            : "validated child-Agent request queued";
        receipt.processed_at = iso_now_local();
        receipt.policy_id = policy->policy_id;
        receipt.action_id = action->action_id;
        receipt.dispatch_kind = action->dispatch_kind;
        receipt.dispatch_path = path;
        record_receipt(paths, result, std::move(receipt));
        processed.insert(command->command_id);
    }

    return result;
}

json::object command_receipt_to_json(
    const CommandReceipt& receipt) {
    return receipt_line(receipt);
}

json::object command_control_result_to_json(
    const CommandControlResult& result) {

    json::array receipts;
    for (const auto& receipt : result.receipts)
        receipts.emplace_back(
            command_receipt_to_json(receipt));

    json::array diagnostics;
    for (const auto& item : result.diagnostics)
        diagnostics.emplace_back(item);

    json::object out;
    out["schema_version"] = kSchemaVersion;
    out["inbox_path"] = result.inbox_path.string();
    out["receipt_path"] = result.receipt_path.string();
    out["processed_this_run"] =
        static_cast<std::uint64_t>(result.receipts.size());
    out["queued_l1"] =
        static_cast<std::uint64_t>(result.queued_l1);
    out["queued_l2"] =
        static_cast<std::uint64_t>(result.queued_l2);
    out["escalated_l3"] =
        static_cast<std::uint64_t>(result.escalated_l3);
    out["rejected"] =
        static_cast<std::uint64_t>(result.rejected);
    out["already_processed"] =
        static_cast<std::uint64_t>(result.already_processed);
    out["duplicate_commands"] =
        static_cast<std::uint64_t>(result.duplicate_commands);
    out["malformed_commands"] =
        static_cast<std::uint64_t>(result.malformed_commands);
    out["receipts"] = std::move(receipts);
    out["diagnostics"] = std::move(diagnostics);
    return out;
}

json::object command_submit_result_to_json(
    const CommandSubmitResult& result) {

    json::object out;
    out["accepted"] = result.accepted;
    out["duplicate"] = result.duplicate;
    out["command_id"] = result.command_id;
    out["reason"] = result.reason;
    out["inbox_path"] = result.inbox_path.string();
    return out;
}

std::string command_policy_example() {
    return R"({
  "schema_version": 1,
  "policy_id": "example-recovery-v1",
  "project_id": "example-project",
  "actions": [
    {
      "action_id": "restart_same_parameters",
      "authority": "L1",
      "dispatch_kind": "deterministic",
      "handler": "restart_same_parameters",
      "allowed_command_types": ["task.restart.requested"],
      "constraints": [
        "reuse validated checkpoint",
        "do not change scientific parameters"
      ],
      "completion_criteria": [
        "replacement job is submitted",
        "monitor observes the replacement job"
      ],
      "enabled": true
    },
    {
      "action_id": "troubleshoot_known_failure",
      "authority": "L2",
      "dispatch_kind": "child_agent",
      "agent_profile": "bounded-troubleshooter",
      "allowed_command_types": ["agent.troubleshoot.request"],
      "constraints": [
        "do not change scientific method",
        "do not create new project scope"
      ],
      "completion_criteria": [
        "evidence and analysis are recorded",
        "bounded action is queued or L3 escalation is emitted"
      ],
      "enabled": true
    }
  ]
})";
}

}  // namespace monitor_hub
