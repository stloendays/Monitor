#include "monitor_hub/core.hpp"
#include "monitor_hub/claude_cli.hpp"
#include "monitor_hub/notification_outbox.hpp"
#include "monitor_hub/windows_probe.hpp"

#include <boost/json.hpp>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

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
            (acknowledge_id ? 1 : 0);
        if (actions != 1) {
            std::cerr
                << "choose exactly one action: --version, --dump, --probe-system, "
                   "--claude-statusline, --notifications, --notifications-all, "
                   "or --ack-notification ID\n";
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
