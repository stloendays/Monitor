#include "monitor_hub/core.hpp"
#include "monitor_hub/dispatch_worker.hpp"

#include <boost/json.hpp>

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    try {
        auto paths = monitor_hub::runtime_paths_from_env(
            argc > 0 ? std::filesystem::path(argv[0])
                     : std::filesystem::path{});

        bool run_once = false;
        bool launch_agents = true;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--run-once") {
                run_once = true;
            } else if (arg == "--no-launch-agents") {
                launch_agents = false;
            } else if (arg == "--hub-data" && i + 1 < argc) {
                paths.hub_data = argv[++i];
            } else if (arg == "--registry" && i + 1 < argc) {
                paths.registry = argv[++i];
            } else if (arg == "--job-root" && i + 1 < argc) {
                paths.job_root = argv[++i];
            } else if (arg == "--help" || arg == "-h") {
                std::cout
                    << "monitor_hub_worker --run-once [--no-launch-agents] [--hub-data DIR]\n"
                       "  L1: execute audited built-in deterministic handlers.\n"
                       "  L2: launch/reconcile bounded child-Agent runs.\n"
                       "  Unsupported L1 handlers and L2 runtime failures escalate instead of executing unsafely.\n";
                return 0;
            } else {
                std::cerr << "unknown argument: " << arg << "\n";
                return 2;
            }
        }

        if (!run_once) {
            std::cerr << "monitor_hub_worker currently requires --run-once\n";
            return 2;
        }

        const auto result =
            monitor_hub::run_dispatch_workers(
                paths,
                launch_agents);
        std::cout << boost::json::serialize(
            monitor_hub::dispatch_worker_result_to_json(result))
                  << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "monitor_hub_worker: "
                  << error.what() << "\n";
        return 1;
    }
}
