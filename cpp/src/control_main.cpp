#include "monitor_hub/command_control.hpp"
#include "monitor_hub/core.hpp"

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

        bool dispatch_once = false;
        bool policy_example = false;
        std::optional<std::filesystem::path> submit_file;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--dispatch-once") {
                dispatch_once = true;
            } else if (arg == "--submit-command" && i + 1 < argc) {
                submit_file = std::filesystem::path(argv[++i]);
            } else if (arg == "--policy-example") {
                policy_example = true;
            } else if (arg == "--hub-data" && i + 1 < argc) {
                paths.hub_data = argv[++i];
            } else if (arg == "--registry" && i + 1 < argc) {
                paths.registry = argv[++i];
            } else if (arg == "--job-root" && i + 1 < argc) {
                paths.job_root = argv[++i];
            } else if (arg == "--help" || arg == "-h") {
                std::cout
                    << "monitor_hub_control ACTION [--hub-data DIR]\n"
                       "Actions:\n"
                       "  --submit-command FILE  Validate and append one Protocol v1 command JSON object to the durable inbox.\n"
                       "  --dispatch-once        Validate all unprocessed inbox commands and route them by policy.\n"
                       "  --policy-example       Print the v1 recovery-policy JSON example.\n"
                       "\n"
                       "Dispatch lanes:\n"
                       "  L1 -> HUB_DATA/dispatch/l1/<command>.json (deterministic handler request)\n"
                       "  L2 -> HUB_DATA/dispatch/l2/<command>.json (bounded child-Agent request)\n"
                       "  L3 -> issue.user_action_required + notification.requested\n";
                return 0;
            } else {
                std::cerr << "unknown argument: " << arg << "\n";
                return 2;
            }
        }

        const int actions =
            (dispatch_once ? 1 : 0) +
            (submit_file ? 1 : 0) +
            (policy_example ? 1 : 0);
        if (actions != 1) {
            std::cerr
                << "choose exactly one action: --submit-command FILE, "
                   "--dispatch-once, or --policy-example\n";
            return 2;
        }

        if (policy_example) {
            std::cout << monitor_hub::command_policy_example() << "\n";
            return 0;
        }

        if (submit_file) {
            const auto result =
                monitor_hub::submit_command_file(paths, *submit_file);
            std::cout << boost::json::serialize(
                monitor_hub::command_submit_result_to_json(result))
                      << "\n";
            return (result.accepted || result.duplicate) ? 0 : 3;
        }

        const auto result =
            monitor_hub::dispatch_pending_commands(paths);
        std::cout << boost::json::serialize(
            monitor_hub::command_control_result_to_json(result))
                  << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "monitor_hub_control: "
                  << error.what() << "\n";
        return 1;
    }
}
