#pragma once

#include "monitor_hub/core.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace monitor_hub {

struct ClaudeUsageWindow {
    std::optional<double> used_percentage;
    std::optional<double> resets_at;
};

struct ClaudeCliStatus {
    bool cli_found = false;
    int running_processes = 0;
    std::filesystem::path executable;
    std::filesystem::path config_dir;
    std::filesystem::path status_file;

    std::string source;
    std::string version;
    std::string model;
    std::string cwd;
    std::optional<double> observed_at;
    std::optional<double> context_used_percentage;
    std::optional<double> session_cost_usd;

    ClaudeUsageWindow five_hour;
    ClaudeUsageWindow seven_day;
    bool rate_limits_available = false;
};

ClaudeCliStatus load_claude_cli_status(
    const SystemInfo& system,
    const RuntimePaths& paths);

}  // namespace monitor_hub
