#pragma once

#include "monitor_hub/core.hpp"
#include "monitor_hub/project_agent_channel.hpp"

#include <optional>
#include <string>
#include <vector>

namespace monitor_hub {

struct SetupRequestFields {
    std::string project_name;
    std::string workdir;
};

struct SetupAgentRuntime {
    fs::path powershell;
    fs::path detach_script;
    fs::path claude_executable;
};

struct SetupRequestLaunch {
    std::string stamp;
    std::string request_id;
    std::string job_name;
    fs::path request_file;
    fs::path report_file;
    fs::path prompt_file;
    fs::path origin_agent_file;
    fs::path working_directory;
    SetupAgentRuntime runtime;
    std::string command;
    std::vector<std::string> arguments;
};

std::string setup_request_template();
std::string setup_request_example();

std::optional<SetupRequestFields> parse_setup_request_fields(
    const std::string& body);

SetupAgentRuntime setup_agent_runtime_from_env();

std::vector<std::string> validate_setup_agent_runtime(
    const SetupAgentRuntime& runtime);

SetupRequestLaunch prepare_setup_request(
    const RuntimePaths& paths,
    const std::string& body,
    const fs::path& requested_workdir,
    const std::optional<std::string>& stamp_override = std::nullopt,
    const std::optional<ProjectAgentBinding>& origin_agent = std::nullopt);

}  // namespace monitor_hub
