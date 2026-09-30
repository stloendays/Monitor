#pragma once

#include "monitor_hub/core.hpp"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

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
    std::string project_dir;
    std::string git_worktree;

    std::string session_id;
    std::string session_name;
    std::string prompt_id;
    std::filesystem::path transcript_path;
    std::string agent_name;
    std::string agent_type;

    std::string recent_tool;
    std::optional<double> recent_tool_at;
    std::string recent_agent;
    std::optional<double> recent_agent_at;

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

std::string match_claude_workspace_project(
    const ClaudeCliStatus& status,
    const std::vector<json::object>& projects);

int run_claude_statusline_bridge(
    std::istream& input,
    std::ostream& output,
    const RuntimePaths& paths,
    std::string* diagnostic = nullptr);

}  // namespace monitor_hub
