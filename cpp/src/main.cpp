#include "monitor_hub/core.hpp"
#include "monitor_hub/claude_cli.hpp"
#include "monitor_hub/notification_outbox.hpp"
#include "monitor_hub/project_agent_channel.hpp"
#include "monitor_hub/windows_probe.hpp"

#include <boost/json.hpp>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

int main(int argc, char** argv) {
    try {
        auto paths = monitor_hub::runtime_paths_from_env(
            argc > 0 ? std::filesystem::path(argv[0])
                     : std::filesystem::path{});
        monitor_hub::SystemInfo system;
        bool system_fixture = false;
        bool dump = false;
        bool show_version = false;
        bool probe_system = false;
        bool claude_statusline = false;
        bool notifications = false;
        bool include_acknowledged = false;
        std::optional<std::string> acknowledge_id;
        std::string acknowledge_actor = "main_agent";
        std::optional<std::string> agent_channel_project;
        std::optional<std::string> agent_pending_project;
        std::optional<std::string> agent_post_project;
        std::optional<std::string> agent_reply_project;
        std::optional<std::string> agent_bind_project;
        std::optional<std::string> agent_message;
        std::optional<std::string> agent_message_file;
        std::optional<std::string> agent_reply_to;
        std::optional<std::string> agent_binding_file;
        std::string agent_source = "cli";
        std::string agent_kind = "question";
        std::string agent_correlation;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--dump") dump = true;
            else if (arg == "--version") show_version = true;
            else if (arg == "--probe-system") probe_system = true;
            else if (arg == "--claude-statusline") claude_statusline = true;
            else if (arg == "--notifications") notifications = true;
            else if (arg == "--notifications-all") {
                notifications = true;
                include_acknowledged = true;
            }
            else if (arg == "--ack-notification" && i + 1 < argc)
                acknowledge_id = argv[++i];
            else if (arg == "--notification-actor" && i + 1 < argc)
                acknowledge_actor = argv[++i];
            else if (arg == "--agent-channel" && i + 1 < argc)
                agent_channel_project = argv[++i];
            else if (arg == "--agent-pending" && i + 1 < argc)
                agent_pending_project = argv[++i];
            else if (arg == "--agent-post" && i + 1 < argc)
                agent_post_project = argv[++i];
            else if (arg == "--agent-reply" && i + 1 < argc)
                agent_reply_project = argv[++i];
            else if (arg == "--agent-bind" && i + 1 < argc)
                agent_bind_project = argv[++i];
            else if (arg == "--message" && i + 1 < argc)
                agent_message = argv[++i];
            else if (arg == "--message-file" && i + 1 < argc)
                agent_message_file = argv[++i];
            else if (arg == "--reply-to" && i + 1 < argc)
                agent_reply_to = argv[++i];
            else if (arg == "--binding-file" && i + 1 < argc)
                agent_binding_file = argv[++i];
            else if (arg == "--agent-source" && i + 1 < argc)
                agent_source = argv[++i];
            else if (arg == "--agent-kind" && i + 1 < argc)
                agent_kind = argv[++i];
            else if (arg == "--agent-correlation" && i + 1 < argc)
                agent_correlation = argv[++i];
            else if (arg == "--registry" && i + 1 < argc)
                paths.registry = argv[++i];
            else if (arg == "--hub-data" && i + 1 < argc)
                paths.hub_data = argv[++i];
            else if (arg == "--job-root" && i + 1 < argc)
                paths.job_root = argv[++i];
            else if (arg == "--no-discovery")
                paths.discovery = false;
            else if (arg == "--system-info" && i + 1 < argc) {
                system = monitor_hub::load_system_info_fixture(argv[++i]);
                system_fixture = true;
            }
            else if (arg == "--help" || arg == "-h") {
                std::cout
                    << "monitor_hub_cli ACTION [--registry FILE] [--hub-data DIR] [--job-root DIR] [--no-discovery] [--system-info FILE]\n"
                       "Actions:\n"
                       "  --version                 Print the release version and exit.\n"
                       "  --dump                    Dump normalized Monitor Hub state as JSON.\n"
                       "  --probe-system            Dump the raw read-only Task Scheduler/WMI probe as JSON.\n"
                       "  --claude-statusline       Read Claude Code statusLine JSON from stdin, write a sanitized local snapshot, and print the compact status line.\n"
                       "  --notifications           Sync event streams into the durable outbox and list unacknowledged notifications.\n"
                       "  --notifications-all       Sync and list all notifications, including acknowledged records.\n"
                       "  --ack-notification ID     Acknowledge one durable notification.\n"
                       "  --notification-actor NAME Actor recorded for --ack-notification (default: main_agent).\n"
                       "  --agent-channel PROJECT   Dump one project's durable Agent binding and conversation.\n"
                       "  --agent-pending PROJECT   List unanswered messages addressed to that project's Agent.\n"
                       "  --agent-post PROJECT      Append a user question/instruction for the bound project Agent.\n"
                       "  --agent-reply PROJECT     Append an Agent answer; requires --reply-to MESSAGE_ID.\n"
                       "  --agent-bind PROJECT      Save the project's originating Agent binding from --binding-file JSON.\n"
                       "  --message TEXT            Message body for --agent-post/--agent-reply.\n"
                       "  --message-file FILE       UTF-8 message body file (preferred for multiline/non-ASCII).\n"
                       "  --reply-to MESSAGE_ID     Question/instruction answered by --agent-reply.\n"
                       "  --binding-file FILE       Agent binding JSON for --agent-bind.\n"
                       "  --agent-source NAME       Source label stored with Agent messages (default: cli).\n"
                       "  --agent-kind NAME         Kind for --agent-post (default: question; e.g. instruction, monitor_request).\n"
                       "  --agent-correlation ID    Optional correlation id stored with Agent messages.\n"
                       "\n"
                       "Without --system-info, --dump and --probe-system read Windows Task Scheduler and Win32_Process live through COM/WMI.\n"
                       "--probe-system is raw platform evidence; --dump is the normalized Monitor Hub projection.\n";
                return 0;
            } else {
                std::cerr << "unknown argument: " << arg << "\n";
                return 2;
            }
        }

        const int actions =
            (dump ? 1 : 0) +
            (show_version ? 1 : 0) +
            (probe_system ? 1 : 0) +
            (claude_statusline ? 1 : 0) +
            (notifications ? 1 : 0) +
            (acknowledge_id ? 1 : 0) +
            (agent_channel_project ? 1 : 0) +
            (agent_pending_project ? 1 : 0) +
            (agent_post_project ? 1 : 0) +
            (agent_reply_project ? 1 : 0) +
            (agent_bind_project ? 1 : 0);
        if (actions != 1) {
            std::cerr
                << "choose exactly one action: --version, --dump, --probe-system, "
                   "--claude-statusline, --notifications, --notifications-all, "
                   "--ack-notification ID, --agent-channel PROJECT, "
                   "--agent-pending PROJECT, --agent-post PROJECT, "
                   "--agent-reply PROJECT, or --agent-bind PROJECT\n";
            return 2;
        }

        if (show_version) {
            std::cout << MONITOR_HUB_VERSION << "\n";
            return 0;
        }

        if (probe_system) {
            if (!system_fixture)
                system = monitor_hub::probe_system_info();
            std::cout << boost::json::serialize(
                monitor_hub::system_info_json(system))
                      << "\n";
            return system.error.empty() ? 0 : 1;
        }

        if (claude_statusline) {
            std::string diagnostic;
            const int code = monitor_hub::run_claude_statusline_bridge(
                std::cin,
                std::cout,
                paths,
                &diagnostic);
            if (!diagnostic.empty())
                std::cerr << "monitor_hub_cli: claude statusLine: " << diagnostic << "\n";
            return code;
        }

        if (notifications) {
            const auto outbox =
                monitor_hub::sync_notification_outbox(paths);
            std::cout << boost::json::serialize(
                monitor_hub::notification_outbox_to_json(
                    outbox,
                    include_acknowledged))
                      << "\n";
            return 0;
        }

        if (acknowledge_id) {
            // Sync first so an Agent can acknowledge a just-emitted event
            // without requiring a separate manual materialization step.
            monitor_hub::sync_notification_outbox(paths);
            const auto acknowledged = monitor_hub::acknowledge_notification(
                paths,
                *acknowledge_id,
                acknowledge_actor);
            if (!acknowledged) {
                std::cerr
                    << "notification not found: " << *acknowledge_id << "\n";
                return 3;
            }
            const auto outbox =
                monitor_hub::load_notification_outbox(paths);
            boost::json::object response;
            response["acknowledged"] = true;
            response["notification_id"] = *acknowledge_id;
            response["actor"] = acknowledge_actor;
            response["outbox"] =
                monitor_hub::notification_outbox_to_json(outbox, true);
            std::cout << boost::json::serialize(response) << "\n";
            return 0;
        }

        if (agent_channel_project || agent_pending_project) {
            const auto& project_id =
                agent_channel_project ? *agent_channel_project
                                      : *agent_pending_project;
            const auto channel =
                monitor_hub::load_project_agent_channel(
                    paths,
                    project_id,
                    1000);
            std::cout << boost::json::serialize(
                monitor_hub::project_agent_channel_to_json(
                    channel,
                    agent_pending_project.has_value()))
                      << "\n";
            return 0;
        }

        if (agent_bind_project) {
            if (!agent_binding_file) {
                std::cerr << "--agent-bind requires --binding-file FILE\n";
                return 2;
            }
            const auto root =
                monitor_hub::read_json(*agent_binding_file);
            if (!root) {
                std::cerr << "cannot read Agent binding JSON: "
                          << *agent_binding_file << "\n";
                return 3;
            }
            std::string parse_error;
            const auto binding =
                monitor_hub::project_agent_binding_from_json(
                    *root,
                    &parse_error);
            if (!binding) {
                std::cerr << "invalid Agent binding: "
                          << parse_error << "\n";
                return 3;
            }
            std::string save_error;
            if (!monitor_hub::save_project_agent_binding(
                    paths,
                    *agent_bind_project,
                    *binding,
                    &save_error)) {
                std::cerr << "cannot save Agent binding: "
                          << save_error << "\n";
                return 3;
            }
            boost::json::object response;
            response["bound"] = true;
            response["project_id"] = *agent_bind_project;
            response["binding"] =
                monitor_hub::project_agent_binding_to_json(*binding);
            std::cout << boost::json::serialize(response) << "\n";
            return 0;
        }

        if (agent_post_project || agent_reply_project) {
            if (agent_message && agent_message_file) {
                std::cerr << "choose only one of --message or --message-file\n";
                return 2;
            }
            std::string body;
            if (agent_message) body = *agent_message;
            else if (agent_message_file)
                body = monitor_hub::read_text(*agent_message_file);
            if (body.empty()) {
                std::cerr << "--agent-post/--agent-reply requires a non-empty "
                             "--message or --message-file\n";
                return 2;
            }

            monitor_hub::ProjectAgentMessage message;
            message.project_id =
                agent_post_project ? *agent_post_project
                                   : *agent_reply_project;
            message.source = agent_source;
            message.correlation_id = agent_correlation;
            if (agent_post_project) {
                message.sender = "user";
                message.target = "project_agent";
                message.kind = agent_kind;
            } else {
                if (!agent_reply_to || agent_reply_to->empty()) {
                    std::cerr << "--agent-reply requires --reply-to MESSAGE_ID\n";
                    return 2;
                }
                message.sender = "agent";
                message.target = "user";
                message.kind = "answer";
                message.reply_to = *agent_reply_to;
            }

            const auto stored =
                monitor_hub::append_project_agent_message(
                    paths,
                    std::move(message));
            std::cout << boost::json::serialize(
                monitor_hub::project_agent_message_to_json(stored))
                      << "\n";
            return 0;
        }

        if (!system_fixture)
            system = monitor_hub::probe_system_info();
        std::cout << boost::json::serialize(
            monitor_hub::dump_all(system, paths))
                  << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "monitor_hub_cli: " << e.what() << "\n";
        return 1;
    }
}
