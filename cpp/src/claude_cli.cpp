#include "monitor_hub/claude_cli.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace monitor_hub {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
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
    return std::filesystem::path_exists(path, ec) && !ec;
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
    if (const auto* workspace = object(root.if_contains("workspace")))
        status.cwd = str(workspace->if_contains("current_dir"));

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
        return status;
    }

    read_latest_stream_snapshot(paths, status);
    return status;
}

}  // namespace monitor_hub
