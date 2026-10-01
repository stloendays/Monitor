#include "monitor_hub/project_agent_channel.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    using namespace monitor_hub;

    const auto root =
        fs::temp_directory_path() / "monitor-hub-project-agent-channel-test";
    fs::remove_all(root);

    RuntimePaths paths;
    paths.hub_data = root / "hub";
    paths.registry = root / "registry.json";
    paths.job_root = root / "jobs";
    paths.discovery = false;

    assert(valid_project_agent_id("alpha"));
    assert(valid_project_agent_id("project:alpha"));
    assert(!valid_project_agent_id("../alpha"));
    assert(!valid_project_agent_id("alpha/beta"));

    ProjectAgentBinding binding;
    binding.provider = "claude_code";
    binding.agent_id = "originating-agent";
    binding.session_id = "session-123";
    binding.session_name = "Alpha research";
    binding.workspace = "C:/Research/Alpha";
    binding.transcript_path = "C:/Users/test/.claude/projects/alpha/session.jsonl";
    binding.request_id = "request-001";

    std::string error;
    assert(save_project_agent_binding(paths, "alpha", binding, &error));
    assert(error.empty());

    ProjectAgentMessage question;
    question.project_id = "alpha";
    question.sender = "user";
    question.target = "project_agent";
    question.kind = "question";
    question.body = "现在跑到哪里了？";
    question.source = "qt";
    question = append_project_agent_message(paths, question);
    assert(!question.message_id.empty());

    auto channel = load_project_agent_channel(paths, "alpha");
    assert(channel.binding);
    assert(channel.binding->session_id == "session-123");
    assert(channel.messages.size() == 1);

    auto pending = pending_project_agent_questions(channel);
    assert(pending.size() == 1);
    assert(pending[0].message_id == question.message_id);

    ProjectAgentMessage reply;
    reply.project_id = "alpha";
    reply.sender = "agent";
    reply.target = "user";
    reply.kind = "answer";
    reply.body = "fold2 正在运行。";
    reply.reply_to = question.message_id;
    reply.source = "monitor-hub-mcp";
    append_project_agent_message(paths, reply);

    channel = load_project_agent_channel(paths, "alpha");
    assert(channel.messages.size() == 2);
    assert(pending_project_agent_questions(channel).empty());

    const auto serialized =
        project_agent_channel_to_json(channel, false);
    assert(serialized.at("binding").as_object().at("session_id").as_string() ==
           "session-123");
    assert(serialized.at("messages").as_array().size() == 2);

    fs::remove_all(root);
    std::cout << "project agent channel tests passed\n";
    return 0;
}
