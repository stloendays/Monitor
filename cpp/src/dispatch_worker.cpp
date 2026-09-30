#include "monitor_hub/dispatch_worker.hpp"
#include "monitor_hub/deterministic_handlers.hpp"

#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

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

fs::path worker_receipt_path(const RuntimePaths& paths) {
    return paths.hub_data / "dispatch" / "worker-receipts.jsonl";
}

fs::path worker_lock_path(const RuntimePaths& paths) {
    return paths.hub_data / "dispatch" / ".worker.lock";
}

class WorkerLock {
public:
    explicit WorkerLock(const RuntimePaths& paths)
        : path_(worker_lock_path(paths)) {
        std::error_code ec;
        fs::create_directories(path_.parent_path(), ec);
        if (ec)
            throw std::runtime_error(
                "cannot create dispatch directory: " + ec.message());

        for (int attempt = 0; attempt < 2; ++attempt) {
            ec.clear();
            if (fs::create_directory(path_, ec)) {
                held_ = true;
                return;
            }
            if (ec && ec != std::errc::file_exists)
                throw std::runtime_error(
                    "cannot acquire dispatch worker lock: " +
                    ec.message());

            std::error_code time_ec;
            const auto modified = fs::last_write_time(path_, time_ec);
            if (!time_ec &&
                fs::file_time_type::clock::now() - modified >
                    kStaleLockAge) {
                std::error_code remove_ec;
                fs::remove_all(path_, remove_ec);
                if (!remove_ec) continue;
            }
            break;
        }
        throw std::runtime_error("dispatch worker is busy");
    }

    ~WorkerLock() {
        if (!held_) return;
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    WorkerLock(const WorkerLock&) = delete;
    WorkerLock& operator=(const WorkerLock&) = delete;

private:
    fs::path path_;
    bool held_ = false;
};

void append_json_line(const fs::path& path, const json::object& value) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
        throw std::runtime_error(
            "cannot create directory for " + path.string());

    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out)
        throw std::runtime_error("cannot open " + path.string());
    out << json::serialize(value) << '\n';
    out.flush();
    if (!out)
        throw std::runtime_error("failed writing " + path.string());
}

void write_text_atomic(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
        throw std::runtime_error(
            "cannot create directory for " + path.string());

    const auto tmp = fs::path(path.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot write " + tmp.string());
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out)
            throw std::runtime_error("failed writing " + tmp.string());
    }

    ec.clear();
    fs::rename(tmp, path, ec);
    if (!ec) return;

    std::error_code remove_ec;
    fs::remove(path, remove_ec);
    ec.clear();
    fs::rename(tmp, path, ec);
    if (!ec) return;

    fs::remove(tmp, remove_ec);
    throw std::runtime_error(
        "cannot publish " + path.string() + ": " + ec.message());
}

std::string safe_component(const std::string& value) {
    std::string safe;
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.')
            safe.push_back(static_cast<char>(ch));
        else
            safe.push_back('_');
    }
    if (safe.empty()) safe = "dispatch";
    if (safe.size() > 72) safe.resize(72);
    return safe;
}

std::string powershell_literal(const fs::path& path) {
    const auto value = path.string();
    std::string escaped;
    escaped.reserve(value.size() + 4);
    for (const auto ch : value) {
        if (ch == '\'') escaped += "''";
        else escaped += ch;
    }
    return "'" + escaped + "'";
}

std::string powershell_literal_text(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 4);
    for (const auto ch : value) {
        if (ch == '\'') escaped += "''";
        else escaped += ch;
    }
    return "'" + escaped + "'";
}

std::string agent_model() {
    if (const auto* value = std::getenv("MONITOR_HUB_AGENT_MODEL");
        value && *value)
        return value;
    return "sonnet";
}

fs::path choose_working_directory(
    const RuntimePaths& paths,
    const DispatchRecord& dispatch) {

    for (const auto& ref : dispatch.context_refs) {
        const fs::path path(ref);
        if (!path.is_absolute()) continue;

        std::error_code ec;
        if (fs::is_directory(path, ec) && !ec)
            return path;

        const auto parent = path.parent_path();
        ec.clear();
        if (!parent.empty() &&
            fs::is_directory(parent, ec) && !ec)
            return parent;
    }
    return paths.hub_data;
}

std::string l2_prompt(
    const DispatchRecord& dispatch,
    const fs::path& result_file) {

    std::ostringstream out;
    out
        << "You are a bounded child Agent launched by Monitor Hub for one issue.\n"
        << "You have L2 authority only. You must not expand project scope or make L3 decisions.\n\n"
        << "Identity:\n"
        << "- dispatch_id: " << dispatch.dispatch_id << "\n"
        << "- project_id: " << dispatch.project_id << "\n"
        << "- task_id: " << dispatch.task_id << "\n"
        << "- issue_id: " << dispatch.issue_id << "\n"
        << "- action_id: " << dispatch.action_id << "\n"
        << "- agent_profile: " << dispatch.agent_profile << "\n\n"
        << "Hard constraints:\n";
    for (const auto& item : dispatch.constraints)
        out << "- " << item << "\n";

    out << "\nCompletion criteria:\n";
    for (const auto& item : dispatch.completion_criteria)
        out << "- " << item << "\n";

    out << "\nEvidence/context references:\n";
    for (const auto& item : dispatch.context_refs)
        out << "- " << item << "\n";

    out
        << "\nRules:\n"
        << "1. Read relevant project governance (AGENTS.md/CLAUDE.md/handoff) before changing files.\n"
        << "2. Inspect evidence first; do not guess from filenames or prior assumptions.\n"
        << "3. You may only perform actions inside the listed constraints and completion criteria.\n"
        << "4. Never change scientific/business method, critical parameters, project scope, or irreversibly delete/overwrite data unless explicitly authorized by these constraints.\n"
        << "5. If a materially different choice or missing authority is required, do not choose it. Report NEEDS_USER instead.\n"
        << "6. Agent process exit does not mean the Issue is resolved. The monitor will independently verify recovery.\n"
        << "7. Write the final machine-readable result atomically to: "
        << result_file.string() << "\n\n"
        << "Result JSON schema:\n"
        << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"dispatch_id\": \"" << dispatch.dispatch_id << "\",\n"
        << "  \"success\": true,\n"
        << "  \"summary\": \"what was found/done\",\n"
        << "  \"action_type\": \"stable action name or empty\",\n"
        << "  \"action_applied\": false,\n"
        << "  \"evidence_refs\": [\"path or artifact\"],\n"
        << "  \"uncertainty\": \"remaining uncertainty or empty\",\n"
        << "  \"needs_user\": \"reason or none\"\n"
        << "}\n"
        << "Write to a temporary file first and rename it to the final result path only after the JSON is complete.\n";

    return out.str();
}

int spawn_wait(
    const fs::path& program,
    const std::vector<std::string>& arguments,
    std::string& error) {

    const auto program_string = program.string();
    std::vector<std::string> owned;
    owned.reserve(arguments.size() + 1);
    owned.push_back(program_string);
    owned.insert(owned.end(), arguments.begin(), arguments.end());

#ifdef _WIN32
    std::vector<const char*> argv;
    argv.reserve(owned.size() + 1);
    for (const auto& item : owned) argv.push_back(item.c_str());
    argv.push_back(nullptr);

    errno = 0;
    const auto result = _spawnvp(
        _P_WAIT,
        program_string.c_str(),
        argv.data());
    if (result == -1) {
        error =
            "process launch failed: " +
            std::string(std::strerror(errno));
        return -1;
    }
    return static_cast<int>(result);
#else
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (auto& item : owned) argv.push_back(item.data());
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawn_error = posix_spawnp(
        &pid,
        program_string.c_str(),
        nullptr,
        nullptr,
        argv.data(),
        environ);
    if (spawn_error != 0) {
        error =
            "process launch failed: " +
            std::string(std::strerror(spawn_error));
        return -1;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        error =
            "waitpid failed: " +
            std::string(std::strerror(errno));
        return -1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    error = "process terminated abnormally";
    return -1;
#endif
}

json::object worker_source() {
    json::object source;
    source["kind"] = "core";
    source["id"] = "monitor-hub-dispatch-worker";
    return source;
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
    const DispatchRecord& dispatch,
    json::object event) {

    const auto event_id = str(event.if_contains("event_id"));
    const auto path =
        paths.hub_data / "events" /
        (dispatch.project_id + ".jsonl");
    if (event_id.empty() || event_id_exists(path, event_id))
        return;
    append_json_line(path, event);
}

std::string worker_event_id(
    const DispatchRecord& dispatch,
    const std::string& suffix) {
    return "evt:dsp:" + dispatch.dispatch_id + ":" + suffix;
}

json::object event_envelope(
    const DispatchRecord& dispatch,
    const std::string& event_type,
    const std::string& suffix,
    const std::string& severity,
    json::object payload,
    json::object source = {}) {

    if (source.empty()) source = worker_source();

    json::object event;
    event["schema_version"] = kSchemaVersion;
    event["event_id"] = worker_event_id(dispatch, suffix);
    event["event_type"] = event_type;
    event["occurred_at"] = iso_now_local();
    event["project_id"] = dispatch.project_id;
    if (!dispatch.task_id.empty())
        event["task_id"] = dispatch.task_id;
    if (!dispatch.issue_id.empty())
        event["issue_id"] = dispatch.issue_id;
    if (!dispatch.correlation_id.empty())
        event["correlation_id"] = dispatch.correlation_id;
    event["source"] = std::move(source);
    event["severity"] = severity;
    event["payload"] = std::move(payload);
    return event;
}

void emit_escalation(
    const RuntimePaths& paths,
    const DispatchRecord& dispatch,
    const std::string& reason,
    const std::string& summary,
    const std::string& suffix) {

    const auto issue_id = dispatch.issue_id.empty()
        ? "iss:dsp:" + dispatch.dispatch_id
        : dispatch.issue_id;

    json::object issue_payload;
    issue_payload["summary"] = summary;
    issue_payload["authority"] = "L3";
    issue_payload["reason"] = reason;
    issue_payload["dispatch_id"] = dispatch.dispatch_id;

    auto issue_event = event_envelope(
        dispatch,
        "issue.user_action_required",
        suffix + ":issue",
        "warning",
        std::move(issue_payload));
    issue_event["issue_id"] = issue_id;
    append_event_if_missing(paths, dispatch, std::move(issue_event));

    json::object notification_payload;
    notification_payload["notification_id"] =
        "ntf:dsp:" + dispatch.dispatch_id + ":" + suffix;
    notification_payload["target"] = "main_agent";
    notification_payload["reason"] = reason;
    notification_payload["summary"] = summary;
    notification_payload["project_id"] = dispatch.project_id;
    if (!dispatch.task_id.empty())
        notification_payload["task_id"] = dispatch.task_id;
    notification_payload["issue_id"] = issue_id;

    auto notification_event = event_envelope(
        dispatch,
        "notification.requested",
        suffix + ":notification",
        "warning",
        std::move(notification_payload));
    notification_event["issue_id"] = issue_id;
    append_event_if_missing(
        paths,
        dispatch,
        std::move(notification_event));
}

json::object receipt_line(const WorkerReceipt& receipt) {
    json::object out;
    out["schema_version"] = kSchemaVersion;
    out["dispatch_id"] = receipt.dispatch_id;
    out["command_id"] = receipt.command_id;
    out["project_id"] = receipt.project_id;
    out["authority"] = receipt.authority;
    out["state"] = receipt.state;
    out["reason"] = receipt.reason;
    out["updated_at"] = receipt.updated_at;
    if (!receipt.run_id.empty()) out["run_id"] = receipt.run_id;
    if (!receipt.result_file.empty())
        out["result_file"] = receipt.result_file.string();
    return out;
}

void record_receipt(
    const RuntimePaths& paths,
    DispatchWorkerResult& result,
    WorkerReceipt receipt) {

    append_json_line(worker_receipt_path(paths), receipt_line(receipt));

    if (receipt.authority == "L1") {
        if (receipt.state == "completed") ++result.l1_completed;
        else if (receipt.state == "failed") ++result.l1_failed;
    } else if (receipt.authority == "L2") {
        if (receipt.state == "launched") ++result.l2_launched;
        else if (receipt.state == "completed") ++result.l2_completed;
        else if (receipt.state == "needs_user")
            ++result.l2_needs_user;
        else if (receipt.state == "failed")
            ++result.l2_failed;
    }
    result.receipts.push_back(std::move(receipt));
}

std::map<std::string, WorkerReceipt> load_latest_receipts(
    const RuntimePaths& paths) {

    std::map<std::string, WorkerReceipt> latest;
    std::ifstream in(worker_receipt_path(paths), std::ios::binary);
    if (!in) return latest;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        boost::system::error_code ec;
        const auto value = json::parse(line, ec);
        if (ec || !value.is_object()) continue;
        const auto& root = value.as_object();

        WorkerReceipt receipt;
        receipt.dispatch_id = str(root.if_contains("dispatch_id"));
        receipt.command_id = str(root.if_contains("command_id"));
        receipt.project_id = str(root.if_contains("project_id"));
        receipt.authority = str(root.if_contains("authority"));
        receipt.state = str(root.if_contains("state"));
        receipt.reason = str(root.if_contains("reason"));
        receipt.updated_at = str(root.if_contains("updated_at"));
        receipt.run_id = str(root.if_contains("run_id"));
        receipt.result_file = str(root.if_contains("result_file"));
        if (!receipt.dispatch_id.empty())
            latest[receipt.dispatch_id] = std::move(receipt);
    }
    return latest;
}

std::vector<fs::path> dispatch_files(
    const RuntimePaths& paths,
    const std::string& lane) {

    std::vector<fs::path> files;
    const auto dir = paths.hub_data / "dispatch" / lane;
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return files;

    for (fs::directory_iterator it(dir, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        if (it->path().extension() == ".json")
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

void add_diagnostic(
    DispatchWorkerResult& result,
    std::string message) {
    constexpr std::size_t kMaxDiagnostics = 64;
    if (result.diagnostics.size() < kMaxDiagnostics)
        result.diagnostics.push_back(std::move(message));
}

void run_l1_handler(
    const RuntimePaths& paths,
    const DispatchRecord& dispatch,
    DispatchWorkerResult& result) {

    const auto execution =
        execute_deterministic_handler(dispatch);

    if (!execution.supported) {
        const auto reason =
            execution.error.empty()
            ? "No audited L1 worker is registered for handler: " +
                  dispatch.handler
            : execution.error;
        emit_escalation(
            paths,
            dispatch,
            "unsupported_l1_handler",
            reason,
            "unsupported_handler");

        WorkerReceipt receipt;
        receipt.dispatch_id = dispatch.dispatch_id;
        receipt.command_id = dispatch.command_id;
        receipt.project_id = dispatch.project_id;
        receipt.authority = dispatch.authority;
        receipt.state = "failed";
        receipt.reason = reason;
        receipt.updated_at = iso_now_local();
        record_receipt(paths, result, std::move(receipt));
        return;
    }

    if (!execution.success) {
        const auto reason =
            execution.error.empty()
            ? "deterministic handler failed"
            : execution.error;
        emit_escalation(
            paths,
            dispatch,
            "deterministic_handler_failed",
            reason,
            "deterministic_failed");

        WorkerReceipt receipt;
        receipt.dispatch_id = dispatch.dispatch_id;
        receipt.command_id = dispatch.command_id;
        receipt.project_id = dispatch.project_id;
        receipt.authority = dispatch.authority;
        receipt.state = "failed";
        receipt.reason = reason;
        receipt.updated_at = iso_now_local();
        record_receipt(paths, result, std::move(receipt));
        return;
    }

    json::object payload;
    payload["summary"] = execution.summary;
    payload["handler"] = execution.handler;
    payload["evidence_refs"] =
        json_strings(execution.evidence_refs);
    payload["handler_metadata"] = execution.metadata;
    if (execution.process_id != 0)
        payload["process_id"] = execution.process_id;
    if (execution.exit_code >= 0)
        payload["exit_code"] = execution.exit_code;

    if (!execution.action_applied) {
        append_event_if_missing(
            paths,
            dispatch,
            event_envelope(
                dispatch,
                "monitor.check_completed",
                "deterministic_check_completed",
                "info",
                std::move(payload)));
    } else {
        if (!dispatch.issue_id.empty()) {
            auto action_payload = payload;
            action_payload["action_type"] = execution.handler;
            action_payload["success"] = true;
            append_event_if_missing(
                paths,
                dispatch,
                event_envelope(
                    dispatch,
                    "issue.action_applied",
                    "deterministic_action_applied",
                    "info",
                    std::move(action_payload)));
        }

        payload["action_type"] = execution.handler;
        payload["success"] = true;
        append_event_if_missing(
            paths,
            dispatch,
            event_envelope(
                dispatch,
                "task.restarted",
                "task_restarted",
                "info",
                std::move(payload)));
    }

    WorkerReceipt receipt;
    receipt.dispatch_id = dispatch.dispatch_id;
    receipt.command_id = dispatch.command_id;
    receipt.project_id = dispatch.project_id;
    receipt.authority = dispatch.authority;
    receipt.state = "completed";
    receipt.reason = execution.summary;
    receipt.updated_at = iso_now_local();
    record_receipt(paths, result, std::move(receipt));
}

struct AgentResult {
    bool success = false;
    std::string summary;
    std::string action_type;
    bool action_applied = false;
    std::vector<std::string> evidence_refs;
    std::string uncertainty;
    std::string needs_user;
};

std::optional<AgentResult> load_agent_result(
    const fs::path& path,
    const std::string& dispatch_id,
    std::string& error) {

    const auto value = read_json(path);
    if (!value || !value->is_object()) {
        error = "result file is not complete JSON yet";
        return std::nullopt;
    }

    const auto& root = value->as_object();
    if (integer(root.if_contains("schema_version")) != kSchemaVersion) {
        error = "unsupported agent result schema_version";
        return std::nullopt;
    }
    if (str(root.if_contains("dispatch_id")) != dispatch_id) {
        error = "agent result dispatch_id mismatch";
        return std::nullopt;
    }

    AgentResult result;
    result.success = boolean(root.if_contains("success"));
    result.summary = str(root.if_contains("summary"));
    result.action_type = str(root.if_contains("action_type"));
    result.action_applied =
        boolean(root.if_contains("action_applied"));
    result.evidence_refs =
        string_array(root.if_contains("evidence_refs"));
    result.uncertainty = str(root.if_contains("uncertainty"));
    result.needs_user = str(root.if_contains("needs_user"));

    if (result.summary.empty()) {
        error = "agent result missing summary";
        return std::nullopt;
    }
    return result;
}

bool needs_user(const std::string& value) {
    if (value.empty()) return false;
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char ch : value)
        normalized.push_back(
            static_cast<char>(std::tolower(ch)));
    return normalized != "none" &&
           normalized != "no" &&
           normalized != "false";
}

void reconcile_l2_result(
    const RuntimePaths& paths,
    const DispatchRecord& dispatch,
    const AgentRunPlan& plan,
    const AgentResult& agent_result,
    DispatchWorkerResult& result) {

    json::object source;
    source["kind"] = "child_agent";
    source["id"] = dispatch.agent_profile;

    if (!agent_result.evidence_refs.empty()) {
        json::object payload;
        payload["summary"] = agent_result.summary;
        payload["evidence_refs"] =
            json_strings(agent_result.evidence_refs);
        append_event_if_missing(
            paths,
            dispatch,
            event_envelope(
                dispatch,
                "agent.evidence_recorded",
                "evidence_recorded",
                "info",
                std::move(payload),
                source));
    }

    if (needs_user(agent_result.needs_user)) {
        json::object payload;
        payload["summary"] = agent_result.summary;
        payload["uncertainty"] = agent_result.uncertainty;
        payload["needs_user"] = agent_result.needs_user;
        append_event_if_missing(
            paths,
            dispatch,
            event_envelope(
                dispatch,
                "agent.analysis_recorded",
                "analysis_needs_user",
                "warning",
                std::move(payload),
                source));

        emit_escalation(
            paths,
            dispatch,
            "child_agent_needs_user",
            agent_result.needs_user,
            "agent_needs_user");

        WorkerReceipt receipt;
        receipt.dispatch_id = dispatch.dispatch_id;
        receipt.command_id = dispatch.command_id;
        receipt.project_id = dispatch.project_id;
        receipt.authority = dispatch.authority;
        receipt.state = "needs_user";
        receipt.reason = agent_result.needs_user;
        receipt.updated_at = iso_now_local();
        receipt.run_id = plan.run_id;
        receipt.result_file = plan.result_file;
        record_receipt(paths, result, std::move(receipt));
        return;
    }

    if (!agent_result.success) {
        emit_escalation(
            paths,
            dispatch,
            "child_agent_failed",
            agent_result.summary,
            "agent_failed");

        WorkerReceipt receipt;
        receipt.dispatch_id = dispatch.dispatch_id;
        receipt.command_id = dispatch.command_id;
        receipt.project_id = dispatch.project_id;
        receipt.authority = dispatch.authority;
        receipt.state = "failed";
        receipt.reason = agent_result.summary;
        receipt.updated_at = iso_now_local();
        receipt.run_id = plan.run_id;
        receipt.result_file = plan.result_file;
        record_receipt(paths, result, std::move(receipt));
        return;
    }

    if (agent_result.action_applied) {
        json::object payload;
        payload["summary"] = agent_result.summary;
        payload["action_type"] = agent_result.action_type;
        payload["success"] = true;
        payload["evidence_refs"] =
            json_strings(agent_result.evidence_refs);
        append_event_if_missing(
            paths,
            dispatch,
            event_envelope(
                dispatch,
                "agent.action_finished",
                "action_finished",
                "info",
                std::move(payload),
                source));
    }

    json::object payload;
    payload["summary"] = agent_result.summary;
    payload["success"] = true;
    payload["uncertainty"] = agent_result.uncertainty;
    append_event_if_missing(
        paths,
        dispatch,
        event_envelope(
            dispatch,
            "agent.completed",
            "completed",
            "info",
            std::move(payload),
            source));

    WorkerReceipt receipt;
    receipt.dispatch_id = dispatch.dispatch_id;
    receipt.command_id = dispatch.command_id;
    receipt.project_id = dispatch.project_id;
    receipt.authority = dispatch.authority;
    receipt.state = "completed";
    receipt.reason =
        "child Agent completed; monitor recovery verification still required";
    receipt.updated_at = iso_now_local();
    receipt.run_id = plan.run_id;
    receipt.result_file = plan.result_file;
    record_receipt(paths, result, std::move(receipt));
}

}  // namespace

std::optional<DispatchRecord> load_dispatch_record(
    const fs::path& path,
    std::string& error) {

    const auto value = read_json(path);
    if (!value || !value->is_object()) {
        error = "invalid dispatch JSON: " + path.string();
        return std::nullopt;
    }

    const auto& root = value->as_object();
    DispatchRecord dispatch;
    dispatch.schema_version =
        integer(root.if_contains("schema_version"));
    dispatch.dispatch_id = str(root.if_contains("dispatch_id"));
    dispatch.command_id = str(root.if_contains("command_id"));
    dispatch.command_type = str(root.if_contains("command_type"));
    dispatch.dispatched_at = str(root.if_contains("dispatched_at"));
    dispatch.project_id = str(root.if_contains("project_id"));
    dispatch.task_id = str(root.if_contains("task_id"));
    dispatch.issue_id = str(root.if_contains("issue_id"));
    dispatch.correlation_id = str(root.if_contains("correlation_id"));
    dispatch.authority = str(root.if_contains("authority"));
    dispatch.policy_id = str(root.if_contains("policy_id"));
    dispatch.policy_ref = str(root.if_contains("policy_ref"));
    dispatch.action_id = str(root.if_contains("action_id"));
    dispatch.dispatch_kind = str(root.if_contains("dispatch_kind"));
    dispatch.handler = str(root.if_contains("handler"));
    if (const auto* handler_config =
            object(root.if_contains("handler_config")))
        dispatch.handler_config = *handler_config;
    dispatch.agent_profile = str(root.if_contains("agent_profile"));
    dispatch.constraints =
        string_array(root.if_contains("constraints"));
    dispatch.completion_criteria =
        string_array(root.if_contains("completion_criteria"));
    dispatch.context_refs =
        string_array(root.if_contains("context_refs"));

    const auto* requested =
        object(root.if_contains("requested_command"));
    if (requested) dispatch.requested_command = *requested;

    if (dispatch.schema_version != kSchemaVersion)
        error = "unsupported dispatch schema_version";
    else if (dispatch.dispatch_id.empty())
        error = "missing dispatch_id";
    else if (dispatch.command_id.empty())
        error = "missing command_id";
    else if (dispatch.project_id.empty())
        error = "missing project_id";
    else if (dispatch.authority != "L1" &&
             dispatch.authority != "L2")
        error = "worker only accepts L1/L2 dispatches";
    else if (dispatch.authority == "L1" &&
             dispatch.dispatch_kind != "deterministic")
        error = "L1 dispatch_kind must be deterministic";
    else if (dispatch.authority == "L2" &&
             dispatch.dispatch_kind != "child_agent")
        error = "L2 dispatch_kind must be child_agent";
    else if (dispatch.authority == "L1" &&
             dispatch.handler.empty())
        error = "L1 dispatch missing handler";
    else if (dispatch.authority == "L2" &&
             dispatch.agent_profile.empty())
        error = "L2 dispatch missing agent_profile";
    else
        return dispatch;

    return std::nullopt;
}

std::optional<AgentRunPlan> prepare_l2_agent_run(
    const RuntimePaths& paths,
    const DispatchRecord& dispatch,
    std::string& error) {

    if (dispatch.authority != "L2" ||
        dispatch.dispatch_kind != "child_agent") {
        error = "dispatch is not an L2 child-Agent request";
        return std::nullopt;
    }

    AgentRunPlan plan;
    plan.dispatch_id = dispatch.dispatch_id;
    plan.run_id = "run:" + dispatch.dispatch_id;
    plan.job_name =
        "hub-agent-" + safe_component(dispatch.command_id);
    if (plan.job_name.size() > 96)
        plan.job_name.resize(96);

    plan.run_dir =
        paths.hub_data / "agent-runs" /
        safe_component(dispatch.dispatch_id);
    plan.prompt_file = plan.run_dir / "prompt.txt";
    plan.result_file = plan.run_dir / "result.json";
    plan.working_directory =
        choose_working_directory(paths, dispatch);
    plan.runtime = setup_agent_runtime_from_env();

    const auto prompt =
        l2_prompt(dispatch, plan.result_file);
    write_text_atomic(plan.prompt_file, prompt);

    plan.command =
        "[Console]::OutputEncoding = [Text.Encoding]::UTF8; "
        "$OutputEncoding = [Text.Encoding]::UTF8; "
        "Remove-Item Env:CLAUDE_CONFIG_DIR -ErrorAction SilentlyContinue; "
        "Get-Content -Raw -Encoding utf8 -LiteralPath " +
        powershell_literal(plan.prompt_file) +
        " | & " +
        powershell_literal(plan.runtime.claude_executable) +
        " -p --dangerously-skip-permissions "
        "--output-format stream-json --verbose --model " +
        powershell_literal_text(agent_model());

    plan.arguments = {
        "-NoProfile",
        "-NonInteractive",
        "-File",
        plan.runtime.detach_script.string(),
        "-Name",
        plan.job_name,
        "-Command",
        plan.command,
        "-WorkDir",
        plan.working_directory.string(),
    };
    return plan;
}

DispatchWorkerResult run_dispatch_workers(
    const RuntimePaths& paths,
    bool launch_agents) {

    DispatchWorkerResult result;
    result.receipt_path = worker_receipt_path(paths);

    WorkerLock lock(paths);
    auto latest = load_latest_receipts(paths);

    for (const auto& path : dispatch_files(paths, "l1")) {
        std::string error;
        const auto dispatch = load_dispatch_record(path, error);
        if (!dispatch) {
            ++result.malformed_dispatches;
            add_diagnostic(result, error);
            continue;
        }

        const auto prior = latest.find(dispatch->dispatch_id);
        if (prior != latest.end() &&
            (prior->second.state == "completed" ||
             prior->second.state == "failed" ||
             prior->second.state == "needs_user")) {
            ++result.already_terminal;
            continue;
        }

        run_l1_handler(paths, *dispatch, result);
    }

    for (const auto& path : dispatch_files(paths, "l2")) {
        std::string error;
        const auto dispatch = load_dispatch_record(path, error);
        if (!dispatch) {
            ++result.malformed_dispatches;
            add_diagnostic(result, error);
            continue;
        }

        const auto prior = latest.find(dispatch->dispatch_id);
        if (prior != latest.end() &&
            (prior->second.state == "completed" ||
             prior->second.state == "failed" ||
             prior->second.state == "needs_user")) {
            ++result.already_terminal;
            continue;
        }

        auto plan = prepare_l2_agent_run(paths, *dispatch, error);
        if (!plan) {
            add_diagnostic(
                result,
                dispatch->dispatch_id + ": " + error);
            continue;
        }

        if (prior != latest.end() &&
            prior->second.state == "launched") {
            if (!fs::exists(plan->result_file)) {
                ++result.active_l2;
                continue;
            }

            auto agent_result = load_agent_result(
                plan->result_file,
                dispatch->dispatch_id,
                error);
            if (!agent_result) {
                ++result.active_l2;
                add_diagnostic(
                    result,
                    dispatch->dispatch_id + ": " + error);
                continue;
            }

            reconcile_l2_result(
                paths,
                *dispatch,
                *plan,
                *agent_result,
                result);
            continue;
        }

        if (!launch_agents) {
            add_diagnostic(
                result,
                dispatch->dispatch_id +
                    ": child-Agent launch disabled");
            continue;
        }

        const auto runtime_errors =
            validate_setup_agent_runtime(plan->runtime);
        if (!runtime_errors.empty()) {
            std::ostringstream message;
            for (std::size_t i = 0;
                 i < runtime_errors.size(); ++i) {
                if (i) message << "; ";
                message << runtime_errors[i];
            }
            const auto reason = message.str();
            emit_escalation(
                paths,
                *dispatch,
                "agent_runtime_unavailable",
                reason,
                "runtime_unavailable");

            WorkerReceipt receipt;
            receipt.dispatch_id = dispatch->dispatch_id;
            receipt.command_id = dispatch->command_id;
            receipt.project_id = dispatch->project_id;
            receipt.authority = dispatch->authority;
            receipt.state = "failed";
            receipt.reason = reason;
            receipt.updated_at = iso_now_local();
            receipt.run_id = plan->run_id;
            receipt.result_file = plan->result_file;
            record_receipt(paths, result, std::move(receipt));
            continue;
        }

        std::string launch_error;
        const auto exit_code = spawn_wait(
            plan->runtime.powershell,
            plan->arguments,
            launch_error);
        if (exit_code != 0) {
            const auto reason =
                launch_error.empty()
                ? "detached Agent launcher returned exit code " +
                      std::to_string(exit_code)
                : launch_error;
            emit_escalation(
                paths,
                *dispatch,
                "agent_launch_failed",
                reason,
                "launch_failed");

            WorkerReceipt receipt;
            receipt.dispatch_id = dispatch->dispatch_id;
            receipt.command_id = dispatch->command_id;
            receipt.project_id = dispatch->project_id;
            receipt.authority = dispatch->authority;
            receipt.state = "failed";
            receipt.reason = reason;
            receipt.updated_at = iso_now_local();
            receipt.run_id = plan->run_id;
            receipt.result_file = plan->result_file;
            record_receipt(paths, result, std::move(receipt));
            continue;
        }

        json::object payload;
        payload["summary"] =
            "Bounded child Agent launched for " +
            dispatch->action_id;
        payload["agent_profile"] = dispatch->agent_profile;
        payload["run_id"] = plan->run_id;
        payload["result_file"] = plan->result_file.string();
        append_event_if_missing(
            paths,
            *dispatch,
            event_envelope(
                *dispatch,
                "agent.started",
                "started",
                "info",
                std::move(payload)));

        WorkerReceipt receipt;
        receipt.dispatch_id = dispatch->dispatch_id;
        receipt.command_id = dispatch->command_id;
        receipt.project_id = dispatch->project_id;
        receipt.authority = dispatch->authority;
        receipt.state = "launched";
        receipt.reason =
            "bounded child Agent launched";
        receipt.updated_at = iso_now_local();
        receipt.run_id = plan->run_id;
        receipt.result_file = plan->result_file;
        record_receipt(paths, result, std::move(receipt));
    }

    return result;
}

json::object worker_receipt_to_json(
    const WorkerReceipt& receipt) {
    return receipt_line(receipt);
}

json::object dispatch_worker_result_to_json(
    const DispatchWorkerResult& result) {

    json::array receipts;
    for (const auto& receipt : result.receipts)
        receipts.emplace_back(worker_receipt_to_json(receipt));

    json::array diagnostics;
    for (const auto& item : result.diagnostics)
        diagnostics.emplace_back(item);

    json::object out;
    out["schema_version"] = kSchemaVersion;
    out["receipt_path"] = result.receipt_path.string();
    out["l1_completed"] =
        static_cast<std::uint64_t>(result.l1_completed);
    out["l1_failed"] =
        static_cast<std::uint64_t>(result.l1_failed);
    out["l2_launched"] =
        static_cast<std::uint64_t>(result.l2_launched);
    out["l2_completed"] =
        static_cast<std::uint64_t>(result.l2_completed);
    out["l2_needs_user"] =
        static_cast<std::uint64_t>(result.l2_needs_user);
    out["l2_failed"] =
        static_cast<std::uint64_t>(result.l2_failed);
    out["active_l2"] =
        static_cast<std::uint64_t>(result.active_l2);
    out["already_terminal"] =
        static_cast<std::uint64_t>(result.already_terminal);
    out["malformed_dispatches"] =
        static_cast<std::uint64_t>(result.malformed_dispatches);
    out["receipts"] = std::move(receipts);
    out["diagnostics"] = std::move(diagnostics);
    return out;
}

}  // namespace monitor_hub
