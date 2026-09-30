#include "monitor_hub/command_control.hpp"
#include "monitor_hub/core.hpp"
#include "monitor_hub/deterministic_handlers.hpp"

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
        bool registered_handlers = false;
        std::optional<std::string> validate_policy_ref;
        std::optional<std::filesystem::path> submit_file;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--dispatch-once") {
                dispatch_once = true;
            } else if (arg == "--submit-command" && i + 1 < argc) {
                submit_file = std::filesystem::path(argv[++i]);
            } else if (arg == "--policy-example") {
                policy_example = true;
            } else if (arg == "--registered-handlers") {
                registered_handlers = true;
            } else if (arg == "--validate-policy-ref" && i + 1 < argc) {
                validate_policy_ref = argv[++i];
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
                       "  --policy-example       Print the portable v1 recovery-policy JSON example.\n"
                       "  --registered-handlers  Print audited deterministic handler IDs as JSON.\n"
                       "  --validate-policy-ref REF  Validate HUB_DATA/<REF> and print normalized policy metadata.\n"
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
            (policy_example ? 1 : 0) +
            (registered_handlers ? 1 : 0) +
            (validate_policy_ref ? 1 : 0);
        if (actions != 1) {
            std::cerr
                << "choose exactly one action: --submit-command FILE, "
                   "--dispatch-once, --policy-example, "
                   "--registered-handlers, or --validate-policy-ref REF\n";
            return 2;
        }

        if (policy_example) {
            std::cout << monitor_hub::command_policy_example() << "\n";
            return 0;
        }
        if (registered_handlers) {
            boost::json::array handlers;
            for (const auto& handler :
                 monitor_hub::registered_deterministic_handlers())
                handlers.emplace_back(handler);

            boost::json::object response;
            response["schema_version"] = 1;
            response["handlers"] = std::move(handlers);
            std::cout << boost::json::serialize(response) << "\n";
            return 0;
        }

        if (validate_policy_ref) {
            std::string error;
            const auto policy =
                monitor_hub::load_recovery_policy(
                    paths,
                    *validate_policy_ref,
                    error);

            boost::json::object response;
            response["schema_version"] = 1;
            response["policy_ref"] = *validate_policy_ref;
            response["valid"] = static_cast<bool>(policy);
            if (!policy) {
                response["error"] = error;
                std::cout << boost::json::serialize(response) << "\n";
                return 3;
            }

            response["policy_id"] = policy->policy_id;
            response["project_id"] = policy->project_id;
            response["source_path"] = policy->source_path.string();

            boost::json::array actions;
            for (const auto& action : policy->actions) {
                boost::json::object item;
                item["action_id"] = action.action_id;
                item["authority"] = action.authority;
                item["dispatch_kind"] = action.dispatch_kind;
                if (!action.handler.empty())
                    item["handler"] = action.handler;
                if (!action.agent_profile.empty())
                    item["agent_profile"] = action.agent_profile;
                item["enabled"] = action.enabled;
                actions.emplace_back(std::move(item));
            }
            response["actions"] = std::move(actions);

            std::cout << boost::json::serialize(response) << "\n";
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
