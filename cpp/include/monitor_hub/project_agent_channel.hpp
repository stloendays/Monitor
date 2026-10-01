#pragma once

#include "monitor_hub/core.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace monitor_hub {

struct ProjectAgentBinding {
    int schema_version = 1;
    std::string provider;
    std::string agent_id;
    std::string session_id;
    std::string session_name;
    std::string workspace;
    std::string transcript_path;
    std::string request_id;
    std::string bound_at;

    bool empty() const noexcept;
};

struct ProjectAgentMessage {
    int schema_version = 1;
    std::string message_id;
    std::string project_id;
    std::string created_at;
    std::string sender;
    std::string target;
    std::string kind;
    std::string body;
    std::string correlation_id;
    std::string reply_to;
    std::string source;
};

struct ProjectAgentChannel {
    std::string project_id;
    fs::path directory;
    fs::path binding_path;
    fs::path messages_path;
    std::optional<ProjectAgentBinding> binding;
    std::vector<ProjectAgentMessage> messages;
    std::vector<std::string> diagnostics;
    std::size_t malformed_lines = 0;
    std::size_t duplicate_messages = 0;
};

bool valid_project_agent_id(const std::string& project_id);

fs::path project_agent_channel_directory(
    const RuntimePaths& paths,
    const std::string& project_id);

ProjectAgentChannel load_project_agent_channel(
    const RuntimePaths& paths,
    const std::string& project_id,
    std::size_t max_messages = 500);

bool save_project_agent_binding(
    const RuntimePaths& paths,
    const std::string& project_id,
    const ProjectAgentBinding& binding,
    std::string* error = nullptr);

ProjectAgentMessage append_project_agent_message(
    const RuntimePaths& paths,
    ProjectAgentMessage message);

std::vector<ProjectAgentMessage> pending_project_agent_questions(
    const ProjectAgentChannel& channel);

std::optional<ProjectAgentBinding> project_agent_binding_from_json(
    const json::value& value,
    std::string* error = nullptr);

json::object project_agent_binding_to_json(
    const ProjectAgentBinding& binding);
json::object project_agent_message_to_json(
    const ProjectAgentMessage& message);
json::object project_agent_channel_to_json(
    const ProjectAgentChannel& channel,
    bool pending_only = false);

}  // namespace monitor_hub
