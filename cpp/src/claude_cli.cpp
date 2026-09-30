#include "monitor_hub/claude_cli.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace monitor_hub {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string normalized_path_key(std::string value) {
    if (value.empty()) return {};
    std::replace(value.begin(), value.end(), '\\', '/');
    std::filesystem::path path(value);
    auto normalized = path.lexically_normal().generic_string();
    while (normalized.size() > 1 && normalized.back() == '/')
        normalized.pop_back();
    return lower(normalized);
}

bool paths_related(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty() || rhs.empty()) return false;
    if (lhs == rhs) return true;
    return (lhs.size() > rhs.size() &&
            lhs.compare(0, rhs.size(), rhs) == 0 &&
            lhs[rhs.size()] == '/') ||
           (rhs.size() > lhs.size() &&
            rhs.compare(0, lhs.size(), lhs) == 0 &&
            rhs[lhs.size()] == '/');
}

std::string parent_path_string(const std::string& value) {
    if (value.empty()) return {};
    auto normalized = value;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return std::filesystem::path(normalized).parent_path().generic_string();
}

std::string str(const json::value* value) {
    if (!value) return {};
    if (value->is_string()) return std::string(value->as_string());
    return {};
}

std::optional<double> number(const json::value* value) {
    if (!value) return std::nullopt;
    if (value->is_double()) return value->as_double();
    if (value->is_int64()) return static_cast<double>(value->as_int64());
    if (value->is_uint64()) return static_cast<double>(value->as_uint64());
    return std::nullopt;
}

const json::object* object(const json::value* value) {
    return value && value->is_object() ? &value->as_object() : nullptr;
}

std::filesystem::path env_path(const char* name) {
    const auto* value = std::getenv(name);
    return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
}

bool path_exists(const std::filesystem::path& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

std::filesystem::path first_existing(
    const std::vector<std::filesystem::path>& candidates) {
    for (const auto& candidate : candidates)
        if (path_exists(candidate)) return candidate;
    return {};
}

std::optional<double> parse_observed_at(const json::object& root,
                                        const std::filesystem::path& file) {
    const auto captured = str(root.if_contains("captured_at"));
    if (!captured.empty()) {
        if (const auto parsed = parse_iso_local_seconds(captured)) return parsed;
    }
    return mtime_seconds(file);
}

void read_window(const json::object* rate_limits,
                 const char* key,
                 ClaudeUsageWindow& out) {
    if (!rate_limits) return;
    const auto* window = object(rate_limits->if_contains(key));
    if (!window) return;
    if (const auto used = number(window->if_contains("used_percentage"));
        used && *used >= 0.0 && *used <= 100.0) {
        out.used_percentage = used;
    }
    out.resets_at = number(window->if_contains("resets_at"));
}

void read_statusline_snapshot(const json::object& root,
                              ClaudeCliStatus& status) {
    status.source = str(root.if_contains("source"));
    if (status.source.empty()) status.source = "statusline";

    status.version = str(root.if_contains("version"));
    if (const auto* model = object(root.if_contains("model"))) {
        status.model = str(model->if_contains("display_name"));
        if (status.model.empty()) status.model = str(model->if_contains("id"));
    }
    if (const auto* workspace = object(root.if_contains("workspace"))) {
        status.cwd = str(workspace->if_contains("current_dir"));
        status.project_dir = str(workspace->if_contains("project_dir"));
        status.git_worktree = str(workspace->if_contains("git_worktree"));
    }

    if (const auto* session = object(root.if_contains("session"))) {
        status.session_id = str(session->if_contains("id"));
        status.session_name = str(session->if_contains("name"));
        status.prompt_id = str(session->if_contains("prompt_id"));
        const auto transcript = str(session->if_contains("transcript_path"));
        if (!transcript.empty()) status.transcript_path = transcript;
    }

    if (const auto* agent = object(root.if_contains("agent"))) {
        status.agent_name = str(agent->if_contains("name"));
        status.agent_type = str(agent->if_contains("type"));
    }

    if (const auto* context = object(root.if_contains("context_window")))
        status.context_used_percentage =
            number(context->if_contains("used_percentage"));

    if (const auto* cost = object(root.if_contains("cost")))
        status.session_cost_usd = number(cost->if_contains("total_cost_usd"));

    const auto* limits = object(root.if_contains("rate_limits"));
    read_window(limits, "five_hour", status.five_hour);
    read_window(limits, "seven_day", status.seven_day);

    if (const auto* available = root.if_contains("rate_limits_available");
        available && available->is_bool()) {
        status.rate_limits_available = available->as_bool();
    } else {
        status.rate_limits_available =
            status.five_hour.used_percentage.has_value() ||
            status.seven_day.used_percentage.has_value() ||
            status.five_hour.resets_at.has_value() ||
            status.seven_day.resets_at.has_value();
    }
}

std::string read_tail(const std::filesystem::path& path,
                      std::size_t max_bytes = 350000) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto end = in.tellg();
    if (end <= 0) return {};
    const auto size = static_cast<std::uint64_t>(end);
    const auto start = size > max_bytes ? size - max_bytes : 0;
    in.seekg(static_cast<std::streamoff>(start), std::ios::beg);
    std::string data(static_cast<std::size_t>(size - start), '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    return data;
}

std::optional<double> transcript_event_time(const json::object& root) {
    if (const auto text = str(root.if_contains("timestamp")); !text.empty()) {
        if (const auto parsed = parse_iso_local_seconds(text)) return parsed;
    }
    if (const auto value = number(root.if_contains("timestamp"))) return value;
    if (const auto value = number(root.if_contains("time"))) return value;
    return std::nullopt;
}

void capture_tool_block(const json::object& block,
                        const std::optional<double>& event_time,
                        ClaudeCliStatus& status) {
    if (str(block.if_contains("type")) != "tool_use") return;
    const auto name = str(block.if_contains("name"));
    if (name.empty()) return;

    if (status.recent_tool.empty()) {
        status.recent_tool = name;
        status.recent_tool_at = event_time;
    }

    if (status.recent_agent.empty() && (name == "Agent" || name == "Task")) {
        status.recent_agent = name;
        if (const auto* input = object(block.if_contains("input"))) {
            const auto subtype = str(input->if_contains("subagent_type"));
            if (!subtype.empty()) status.recent_agent += " · " + subtype;
        }
        status.recent_agent_at = event_time;
    }
}

void read_recent_transcript_activity(ClaudeCliStatus& status) {
    if (status.transcript_path.empty() || !path_exists(status.transcript_path)) return;

    const auto text = read_tail(status.transcript_path, 600000);
    if (text.empty()) return;

    std::vector<std::string> lines;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);)
        if (!line.empty()) lines.push_back(std::move(line));

    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        boost::system::error_code parse_error;
        auto parsed = json::parse(*it, parse_error);
        if (parse_error || !parsed.is_object()) continue;

        const auto& root = parsed.as_object();
        const auto event_time = transcript_event_time(root);

        if (str(root.if_contains("type")) == "tool_use")
            capture_tool_block(root, event_time, status);

        const json::object* message = object(root.if_contains("message"));
        const json::value* content = message
            ? message->if_contains("content")
            : root.if_contains("content");

        if (content && content->is_array()) {
            const auto& blocks = content->as_array();
            for (auto bit = blocks.rbegin(); bit != blocks.rend(); ++bit) {
                if (!bit->is_object()) continue;
                capture_tool_block(bit->as_object(), event_time, status);
                if (!status.recent_tool.empty() && !status.recent_agent.empty()) break;
            }
        }

        if (!status.recent_tool.empty() && !status.recent_agent.empty()) return;
    }

    if (!status.recent_tool.empty() && !status.recent_tool_at)
        status.recent_tool_at = mtime_seconds(status.transcript_path);
    if (!status.recent_agent.empty() && !status.recent_agent_at)
        status.recent_agent_at = mtime_seconds(status.transcript_path);
}

void read_stream_rate_limit(const json::object& event,
                            ClaudeCliStatus& status) {
    const auto* info = object(event.if_contains("rate_limit_info"));
    if (!info) return;

    status.source = "stream-json";
    status.rate_limits_available = true;

    if (const auto* windows = object(info->if_contains("unifiedWindows"))) {
        auto read_unified = [&](const char* key, ClaudeUsageWindow& out) {
            const auto* window = object(windows->if_contains(key));
            if (!window) return;
            if (const auto utilization = number(window->if_contains("utilization"))) {
                const auto percentage =
                    *utilization <= 1.0 ? *utilization * 100.0 : *utilization;
                if (percentage >= 0.0 && percentage <= 100.0)
                    out.used_percentage = percentage;
            }
            out.resets_at = number(window->if_contains("resetsAt"));
        };
        read_unified("five_hour", status.five_hour);
        read_unified("seven_day", status.seven_day);
    }

    const auto type = str(info->if_contains("rateLimitType"));
    const auto reset = number(info->if_contains("resetsAt"));
    const auto utilization = number(info->if_contains("utilization"));
    ClaudeUsageWindow* representative = nullptr;
    if (type == "five_hour") representative = &status.five_hour;
    else if (type == "seven_day") representative = &status.seven_day;
    if (representative) {
        if (reset) representative->resets_at = reset;
        if (utilization) {
            const auto percentage =
                *utilization <= 1.0 ? *utilization * 100.0 : *utilization;
            if (percentage >= 0.0 && percentage <= 100.0)
                representative->used_percentage = percentage;
        }
    }
}

bool read_latest_stream_snapshot(const RuntimePaths& paths,
                                 ClaudeCliStatus& status) {
    std::vector<std::pair<double, std::filesystem::path>> logs;
    std::error_code ec;
    if (!std::filesystem::is_directory(paths.job_root, ec) || ec) return false;

    for (const auto& entry : std::filesystem::directory_iterator(paths.job_root, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec) || ec) continue;
        const auto log = entry.path() / "output.log";
        if (!path_exists(log)) continue;
        logs.emplace_back(mtime_seconds(log).value_or(0.0), log);
    }

    std::sort(logs.begin(), logs.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    const std::size_t inspect = std::min<std::size_t>(logs.size(), 30);
    for (std::size_t i = 0; i < inspect; ++i) {
        const auto text = read_tail(logs[i].second);
        std::vector<std::string> lines;
        std::istringstream input(text);
        for (std::string line; std::getline(input, line);) lines.push_back(std::move(line));

        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            if (it->find("rate_limit_event") == std::string::npos) continue;
            boost::system::error_code parse_error;
            auto parsed = json::parse(*it, parse_error);
            if (parse_error || !parsed.is_object()) continue;
            const auto& event = parsed.as_object();
            if (str(event.if_contains("type")) != "rate_limit_event") continue;
            read_stream_rate_limit(event, status);
            status.observed_at = logs[i].first;
            return true;
        }
    }
    return false;
}

std::filesystem::path detect_claude_executable() {
    const auto user = env_path("USERPROFILE");
    const auto appdata = env_path("APPDATA");
    return first_existing({
        env_path("MONITOR_HUB_CLAUDE_EXE"),
        std::filesystem::path(
            R"(D:\Download\npm-global\node_modules\@anthropic-ai\claude-code\bin\claude.exe)"),
        user.empty() ? std::filesystem::path{} : user / ".local" / "bin" / "claude.exe",
        appdata.empty() ? std::filesystem::path{} : appdata / "npm" / "claude.cmd",
    });
}

std::filesystem::path detect_claude_config_dir() {
    const auto configured = env_path("CLAUDE_CONFIG_DIR");
    if (!configured.empty()) return configured;
    const auto user = env_path("USERPROFILE");
    if (!user.empty()) return user / ".claude";
    return {};
}

}

std::filesystem::path statusline_status_path(const RuntimePaths& paths) {
    const auto custom = env_path("MONITOR_HUB_CLAUDE_STATUS");
    return custom.empty()
        ? paths.hub_data / "claude" / "cli_status.json"
        : custom;
}

json::object sanitized_usage_window(const json::object* limits,
                                    const char* key) {
    json::object out;
    if (!limits) return out;
    const auto* source = object(limits->if_contains(key));
    if (!source) return out;

    if (const auto used = number(source->if_contains("used_percentage"));
        used && *used >= 0.0 && *used <= 100.0) {
        out["used_percentage"] = *used;
    }
    if (const auto reset = number(source->if_contains("resets_at")))
        out["resets_at"] = *reset;
    return out;
}

json::object sanitize_statusline_payload(const json::object& payload) {
    json::object snapshot;
    snapshot["schema_version"] = 1;
    snapshot["source"] = "claude_statusline";
    snapshot["captured_at"] = iso_now_local();
    snapshot["version"] = str(payload.if_contains("version"));

    json::object session;
    session["id"] = str(payload.if_contains("session_id"));
    session["name"] = str(payload.if_contains("session_name"));
    session["prompt_id"] = str(payload.if_contains("prompt_id"));
    session["transcript_path"] = str(payload.if_contains("transcript_path"));
    snapshot["session"] = std::move(session);

    json::object model_out;
    if (const auto* model = object(payload.if_contains("model"))) {
        model_out["id"] = str(model->if_contains("id"));
        auto display = str(model->if_contains("display_name"));
        if (display.empty()) display = str(model->if_contains("id"));
        model_out["display_name"] = display;
    } else {
        model_out["id"] = "";
        model_out["display_name"] = "";
    }
    snapshot["model"] = std::move(model_out);

    const auto* workspace = object(payload.if_contains("workspace"));
    json::object workspace_out;
    auto current_dir = workspace
        ? str(workspace->if_contains("current_dir"))
        : std::string{};
    if (current_dir.empty()) current_dir = str(payload.if_contains("cwd"));
    workspace_out["current_dir"] = current_dir;
    workspace_out["project_dir"] =
        workspace ? str(workspace->if_contains("project_dir")) : std::string{};
    workspace_out["git_worktree"] =
        workspace ? str(workspace->if_contains("git_worktree")) : std::string{};
    snapshot["workspace"] = std::move(workspace_out);

    json::object agent_out;
    if (const auto* agent = object(payload.if_contains("agent"))) {
        agent_out["name"] = str(agent->if_contains("name"));
        agent_out["type"] = str(agent->if_contains("type"));
    } else {
        agent_out["name"] = "";
        agent_out["type"] = "";
    }
    snapshot["agent"] = std::move(agent_out);

    json::object context_out;
    if (const auto* context = object(payload.if_contains("context_window"))) {
        if (const auto used = number(context->if_contains("used_percentage"));
            used && *used >= 0.0 && *used <= 100.0) {
            context_out["used_percentage"] = *used;
        }
    }
    snapshot["context_window"] = std::move(context_out);

    json::object cost_out;
    if (const auto* cost = object(payload.if_contains("cost"))) {
        if (const auto value = number(cost->if_contains("total_cost_usd")))
            cost_out["total_cost_usd"] = *value;
    }
    snapshot["cost"] = std::move(cost_out);

    const auto* limits = object(payload.if_contains("rate_limits"));
    auto five = sanitized_usage_window(limits, "five_hour");
    auto seven = sanitized_usage_window(limits, "seven_day");
    json::object rate_limits;
    if (!five.empty()) rate_limits["five_hour"] = five;
    if (!seven.empty()) rate_limits["seven_day"] = seven;
    snapshot["rate_limits_available"] = !rate_limits.empty();
    snapshot["rate_limits"] = std::move(rate_limits);

    return snapshot;
}

bool write_statusline_snapshot_atomic(const std::filesystem::path& target,
                                      const json::object& snapshot,
                                      std::string* diagnostic) {
    std::error_code ec;
    const auto parent = target.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (diagnostic) *diagnostic = "create status directory failed: " + ec.message();
            return false;
        }
    }

    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    auto temp = target;
    temp += ".tmp." + std::to_string(stamp);

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (diagnostic) *diagnostic = "open temporary status file failed";
            return false;
        }
        out << json::serialize(snapshot) << '\n';
        out.flush();
        if (!out) {
            if (diagnostic) *diagnostic = "write temporary status file failed";
            out.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
    }

#ifdef _WIN32
    const auto from = temp.wstring();
    const auto to = target.wstring();
    if (!MoveFileExW(
            from.c_str(),
            to.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto code = GetLastError();
        std::filesystem::remove(temp, ec);
        if (diagnostic)
            *diagnostic = "replace status file failed: Win32 " + std::to_string(code);
        return false;
    }
#else
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        if (diagnostic) *diagnostic = "replace status file failed: " + ec.message();
        return false;
    }
#endif
    return true;
}

std::optional<double> snapshot_usage(const json::object& snapshot,
                                     const char* window) {
    const auto* limits = object(snapshot.if_contains("rate_limits"));
    const auto* value = limits ? object(limits->if_contains(window)) : nullptr;
    return value ? number(value->if_contains("used_percentage")) : std::nullopt;
}

std::string format_percent(const std::optional<double>& value) {
    if (!value) return "?";
    std::ostringstream out;
    out << std::fixed << std::setprecision(0) << *value << "%";
    return out.str();
}

std::string statusline_display(const json::object& snapshot) {
    std::string model_name = "Claude";
    if (const auto* model = object(snapshot.if_contains("model"))) {
        const auto display = str(model->if_contains("display_name"));
        if (!display.empty()) model_name = display;
    }

    std::vector<std::string> parts{model_name};
    const auto five = snapshot_usage(snapshot, "five_hour");
    const auto seven = snapshot_usage(snapshot, "seven_day");
    if (five || seven) {
        parts.push_back("5h " + format_percent(five));
        parts.push_back("7d " + format_percent(seven));
    }
    if (const auto* context = object(snapshot.if_contains("context_window"))) {
        if (const auto used = number(context->if_contains("used_percentage"))) {
            parts.push_back("ctx " + format_percent(used));
        }
    }

    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) out += " · ";
        out += part;
    }
    return out;
}

}  // namespace

ClaudeCliStatus load_claude_cli_status(
    const SystemInfo& system,
    const RuntimePaths& paths) {

    ClaudeCliStatus status;
    status.executable = detect_claude_executable();
    status.cli_found = !status.executable.empty();
    status.config_dir = detect_claude_config_dir();

    const auto custom_status = env_path("MONITOR_HUB_CLAUDE_STATUS");
    status.status_file = custom_status.empty()
        ? paths.hub_data / "claude" / "cli_status.json"
        : custom_status;

    for (const auto& process : system.procs) {
        const auto name = lower(process.name);
        const auto command = lower(process.cmd);
        if (name == "claude.exe" ||
            command.find("@anthropic-ai\\claude-code") != std::string::npos ||
            command.find("claude-code") != std::string::npos) {
            ++status.running_processes;
        }
    }

    if (const auto root = read_json(status.status_file);
        root && root->is_object()) {
        read_statusline_snapshot(root->as_object(), status);
        status.observed_at = parse_observed_at(root->as_object(), status.status_file);
        read_recent_transcript_activity(status);
        return status;
    }

    read_latest_stream_snapshot(paths, status);
    return status;
}

std::string match_claude_workspace_project(
    const ClaudeCliStatus& status,
    const std::vector<json::object>& projects) {

    const auto workspace = !status.project_dir.empty()
        ? status.project_dir
        : status.cwd;
    const auto workspace_key = normalized_path_key(workspace);
    if (workspace_key.empty()) return {};

    std::string best_id;
    std::size_t best_score = 0;

    for (const auto& project : projects) {
        const auto id = str(project.if_contains("id"));
        if (id.empty()) continue;

        std::vector<std::string> candidates;
        for (const auto* key : {"dir", "qa_cwd"}) {
            const auto value = str(project.if_contains(key));
            if (!value.empty()) candidates.push_back(value);
        }
        if (const auto* runner = object(project.if_contains("runner"))) {
            const auto workdir = str(runner->if_contains("workdir"));
            if (!workdir.empty()) candidates.push_back(workdir);
        }
        for (const auto* key : {"status_json", "status_md"}) {
            const auto value = str(project.if_contains(key));
            const auto parent = parent_path_string(value);
            if (!parent.empty()) candidates.push_back(parent);
        }

        for (const auto& candidate : candidates) {
            const auto candidate_key = normalized_path_key(candidate);
            if (!paths_related(workspace_key, candidate_key)) continue;
            if (candidate_key.size() <= best_score) continue;
            best_score = candidate_key.size();
            best_id = id;
        }
    }
    return best_id;
}

int run_claude_statusline_bridge(
    std::istream& input,
    std::ostream& output,
    const RuntimePaths& paths,
    std::string* diagnostic) {

    try {
        const std::string text{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};

        boost::system::error_code parse_error;
        auto parsed = json::parse(text, parse_error);
        if (parse_error || !parsed.is_object()) {
            if (diagnostic)
                *diagnostic = parse_error
                    ? "invalid statusLine JSON: " + parse_error.message()
                    : "statusLine payload is not an object";
            output << "Claude\n";
            return 0;
        }

        auto snapshot = sanitize_statusline_payload(parsed.as_object());
        std::string write_diagnostic;
        write_statusline_snapshot_atomic(
            statusline_status_path(paths),
            snapshot,
            &write_diagnostic);
        if (diagnostic && !write_diagnostic.empty())
            *diagnostic = write_diagnostic;

        output << statusline_display(snapshot) << '\n';
        return 0;
    } catch (const std::exception& e) {
        if (diagnostic) *diagnostic = e.what();
        output << "Claude\n";
        return 0;
    }
}

}  // namespace monitor_hub
