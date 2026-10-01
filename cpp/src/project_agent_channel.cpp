#include "monitor_hub/project_agent_channel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace monitor_hub {
namespace {

std::string string_value(const json::value* value) {
    if (!value) return {};
    if (value->is_string()) return std::string(value->as_string());
    return {};
}

int int_value(const json::value* value, int fallback = 0) {
    if (!value) return fallback;
    if (value->is_int64()) return static_cast<int>(value->as_int64());
    if (value->is_uint64()) return static_cast<int>(value->as_uint64());
    return fallback;
}

std::string make_message_id(const ProjectAgentMessage& message) {
    static std::atomic<unsigned long long> sequence{0};
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto seq = sequence.fetch_add(1, std::memory_order_relaxed);
    const auto seed = message.project_id + "\n" + message.sender + "\n" +
                      message.target + "\n" + message.kind + "\n" +
                      message.body + "\n" + std::to_string(micros) + "\n" +
                      std::to_string(seq);
    const auto hash = std::hash<std::string>{}(seed);
    std::ostringstream out;
    out << "msg-" << micros << "-" << std::hex << hash;
    return out.str();
}

ProjectAgentBinding parse_binding(const json::object& root) {
    ProjectAgentBinding binding;
    binding.schema_version =
        int_value(root.if_contains("schema_version"), 1);
    binding.provider = string_value(root.if_contains("provider"));
    binding.agent_id = string_value(root.if_contains("agent_id"));
    binding.session_id = string_value(root.if_contains("session_id"));
    binding.session_name = string_value(root.if_contains("session_name"));
    binding.workspace = string_value(root.if_contains("workspace"));
    binding.transcript_path =
        string_value(root.if_contains("transcript_path"));
    binding.request_id = string_value(root.if_contains("request_id"));
    binding.bound_at = string_value(root.if_contains("bound_at"));
    return binding;
}

std::optional<ProjectAgentMessage> parse_message(
    const json::object& root,
    std::string* diagnostic) {

    ProjectAgentMessage message;
    message.schema_version =
        int_value(root.if_contains("schema_version"), 1);
    message.message_id = string_value(root.if_contains("message_id"));
    message.project_id = string_value(root.if_contains("project_id"));
    message.created_at = string_value(root.if_contains("created_at"));
    message.sender = string_value(root.if_contains("sender"));
    message.target = string_value(root.if_contains("target"));
    message.kind = string_value(root.if_contains("kind"));
    message.body = string_value(root.if_contains("body"));
    message.correlation_id =
        string_value(root.if_contains("correlation_id"));
    message.reply_to = string_value(root.if_contains("reply_to"));
    message.source = string_value(root.if_contains("source"));

    if (message.message_id.empty() ||
        message.project_id.empty() ||
        message.sender.empty() ||
        message.target.empty() ||
        message.kind.empty() ||
        message.body.empty()) {
        if (diagnostic)
            *diagnostic =
                "project Agent message missing required fields";
        return std::nullopt;
    }
    return message;
}

bool write_json_atomic(
    const fs::path& path,
    const json::object& value,
    std::string* error) {

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "cannot create Agent channel directory: " + ec.message();
        return false;
    }

    const auto temp = fs::path(path.string() + ".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write " + temp.string();
            return false;
        }
        const auto serialized = json::serialize(value);
        out.write(
            serialized.data(),
            static_cast<std::streamsize>(serialized.size()));
        out << '\n';
        if (!out) {
            if (error) *error = "failed writing " + temp.string();
            return false;
        }
    }

    fs::remove(path, ec);
    ec.clear();
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        if (error) *error = "cannot replace " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace

bool ProjectAgentBinding::empty() const noexcept {
    return provider.empty() &&
           agent_id.empty() &&
           session_id.empty() &&
           session_name.empty() &&
           workspace.empty() &&
           transcript_path.empty() &&
           request_id.empty();
}

bool valid_project_agent_id(const std::string& project_id) {
    if (project_id.empty() || project_id.size() > 160) return false;
    for (const unsigned char ch : project_id) {
        if (ch < 0x20 || ch == '/' || ch == '\\')
            return false;
    }
    return true;
}

fs::path project_agent_channel_directory(
    const RuntimePaths& paths,
    const std::string& project_id) {

    if (!valid_project_agent_id(project_id))
        throw std::invalid_argument("invalid project Agent channel id: " + project_id);

    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const unsigned char ch : project_id) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
            encoded << static_cast<char>(ch);
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0')
                    << static_cast<unsigned int>(ch)
                    << std::setfill(' ');
        }
    }
    return paths.hub_data / "agent-channels" / encoded.str();
}

ProjectAgentChannel load_project_agent_channel(
    const RuntimePaths& paths,
    const std::string& project_id,
    std::size_t max_messages) {

    ProjectAgentChannel channel;
    channel.project_id = project_id;
    channel.directory =
        project_agent_channel_directory(paths, project_id);
    channel.binding_path = channel.directory / "binding.json";
    channel.messages_path = channel.directory / "messages.jsonl";

    if (const auto root = read_json(channel.binding_path);
        root && root->is_object()) {
        auto binding = parse_binding(root->as_object());
        if (!binding.empty())
            channel.binding = std::move(binding);
    }

    std::ifstream input(channel.messages_path, std::ios::binary);
    if (!input) return channel;

    std::set<std::string> seen;
    for (std::string line; std::getline(input, line);) {
        if (line.empty()) continue;
        boost::system::error_code parse_error;
        auto parsed = json::parse(line, parse_error);
        if (parse_error || !parsed.is_object()) {
            ++channel.malformed_lines;
            continue;
        }

        std::string diagnostic;
        auto message = parse_message(parsed.as_object(), &diagnostic);
        if (!message || message->project_id != project_id) {
            ++channel.malformed_lines;
            if (!diagnostic.empty())
                channel.diagnostics.push_back(std::move(diagnostic));
            continue;
        }

        if (!seen.insert(message->message_id).second) {
            ++channel.duplicate_messages;
            continue;
        }
        channel.messages.push_back(std::move(*message));
    }

    if (max_messages > 0 && channel.messages.size() > max_messages) {
        channel.messages.erase(
            channel.messages.begin(),
            channel.messages.end() -
                static_cast<std::ptrdiff_t>(max_messages));
    }
    return channel;
}

bool save_project_agent_binding(
    const RuntimePaths& paths,
    const std::string& project_id,
    const ProjectAgentBinding& binding,
    std::string* error) {

    if (!valid_project_agent_id(project_id)) {
        if (error) *error = "invalid project Agent channel id";
        return false;
    }
    auto normalized = binding;
    if (normalized.schema_version <= 0)
        normalized.schema_version = 1;
    if (normalized.agent_id.empty())
        normalized.agent_id = "project-agent:" + project_id;
    if (normalized.bound_at.empty())
        normalized.bound_at = iso_now_local();

    const auto path =
        project_agent_channel_directory(paths, project_id) /
        "binding.json";
    return write_json_atomic(
        path,
        project_agent_binding_to_json(normalized),
        error);
}

ProjectAgentMessage append_project_agent_message(
    const RuntimePaths& paths,
    ProjectAgentMessage message) {

    if (!valid_project_agent_id(message.project_id))
        throw std::invalid_argument(
            "invalid project Agent channel id: " +
            message.project_id);
    if (message.sender.empty() ||
        message.target.empty() ||
        message.kind.empty() ||
        message.body.empty()) {
        throw std::invalid_argument(
            "project Agent message requires sender, target, kind, and body");
    }

    if (message.schema_version <= 0)
        message.schema_version = 1;
    if (message.created_at.empty())
        message.created_at = iso_now_local();
    if (message.message_id.empty())
        message.message_id = make_message_id(message);
    if (message.source.empty())
        message.source = "monitor_hub";

    const auto directory =
        project_agent_channel_directory(paths, message.project_id);
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec)
        throw std::runtime_error(
            "cannot create Agent channel directory: " + ec.message());

    const auto line =
        json::serialize(project_agent_message_to_json(message)) + "\n";
    std::ofstream out(
        directory / "messages.jsonl",
        std::ios::binary | std::ios::app);
    if (!out)
        throw std::runtime_error(
            "cannot append project Agent channel");
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    if (!out)
        throw std::runtime_error(
            "failed appending project Agent channel");
    return message;
}

std::vector<ProjectAgentMessage> pending_project_agent_questions(
    const ProjectAgentChannel& channel) {

    std::set<std::string> answered;
    for (const auto& message : channel.messages) {
        if (message.sender == "agent" &&
            !message.reply_to.empty()) {
            answered.insert(message.reply_to);
        }
    }

    std::vector<ProjectAgentMessage> pending;
    for (const auto& message : channel.messages) {
        if (message.target != "project_agent") continue;
        if (message.kind != "question" &&
            message.kind != "instruction") {
            continue;
        }
        if (answered.count(message.message_id)) continue;
        pending.push_back(message);
    }
    return pending;
}

std::optional<ProjectAgentBinding> project_agent_binding_from_json(
    const json::value& value,
    std::string* error) {

    if (!value.is_object()) {
        if (error) *error = "Agent binding must be a JSON object";
        return std::nullopt;
    }
    auto binding = parse_binding(value.as_object());
    if (binding.empty()) {
        if (error) *error = "Agent binding has no usable identity";
        return std::nullopt;
    }
    if (binding.schema_version != 1) {
        if (error) *error = "unsupported Agent binding schema_version";
        return std::nullopt;
    }
    return binding;
}

json::object project_agent_binding_to_json(
    const ProjectAgentBinding& binding) {

    json::object out;
    out["schema_version"] = binding.schema_version;
    out["provider"] = binding.provider;
    out["agent_id"] = binding.agent_id;
    out["session_id"] = binding.session_id;
    out["session_name"] = binding.session_name;
    out["workspace"] = binding.workspace;
    out["transcript_path"] = binding.transcript_path;
    out["request_id"] = binding.request_id;
    out["bound_at"] = binding.bound_at;
    return out;
}

json::object project_agent_message_to_json(
    const ProjectAgentMessage& message) {

    json::object out;
    out["schema_version"] = message.schema_version;
    out["message_id"] = message.message_id;
    out["project_id"] = message.project_id;
    out["created_at"] = message.created_at;
    out["sender"] = message.sender;
    out["target"] = message.target;
    out["kind"] = message.kind;
    out["body"] = message.body;
    out["correlation_id"] = message.correlation_id;
    out["reply_to"] = message.reply_to;
    out["source"] = message.source;
    return out;
}

json::object project_agent_channel_to_json(
    const ProjectAgentChannel& channel,
    bool pending_only) {

    json::object out;
    out["schema_version"] = 1;
    out["project_id"] = channel.project_id;
    out["directory"] = channel.directory.string();
    out["binding"] = channel.binding
        ? json::value(project_agent_binding_to_json(*channel.binding))
        : json::value(nullptr);

    json::array messages;
    const auto source = pending_only
        ? pending_project_agent_questions(channel)
        : channel.messages;
    for (const auto& message : source)
        messages.emplace_back(project_agent_message_to_json(message));
    out["messages"] = std::move(messages);

    json::array diagnostics;
    for (const auto& item : channel.diagnostics)
        diagnostics.emplace_back(item);
    out["diagnostics"] = std::move(diagnostics);
    out["malformed_lines"] =
        static_cast<std::uint64_t>(channel.malformed_lines);
    out["duplicate_messages"] =
        static_cast<std::uint64_t>(channel.duplicate_messages);
    return out;
}

}  // namespace monitor_hub
