#include "monitor_hub/control_orchestrator.hpp"
#include "monitor_hub/core.hpp"

#include <boost/json.hpp>

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    try {
        auto paths = monitor_hub::runtime_paths_from_env(
            argc > 0 ? std::filesystem::path(argv[0])
                     : std::filesystem::path{});

        bool tick = false;
        monitor_hub::OrchestratorOptions options;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--tick") {
                tick = true;
            } else if (arg == "--no-launch-agents") {
                options.launch_agents = false;
            } else if (arg == "--hub-data" && i + 1 < argc) {
                paths.hub_data = argv[++i];
            } else if (arg == "--registry" && i + 1 < argc) {
                paths.registry = argv[++i];
            } else if (arg == "--job-root" && i + 1 < argc) {
                paths.job_root = argv[++i];
            } else if (arg == "--help" || arg == "-h") {
                std::cout
                    << "monitor_hub_orchestrator --tick [--no-launch-agents] [--hub-data DIR]\n"
                       "\n"
                       "One idempotent control-plane tick performs:\n"
                       "  1. command policy dispatch\n"
                       "  2. deterministic/child-Agent worker processing\n"
                       "  3. durable main-Agent outbox synchronization\n";
                return 0;
            } else {
                std::cerr << "unknown argument: " << arg << "\n";
                return 2;
            }
        }

        if (!tick) {
            std::cerr
                << "monitor_hub_orchestrator currently requires --tick\n";
            return 2;
        }

        const auto result =
            monitor_hub::run_control_tick(paths, options);
        std::cout << boost::json::serialize(
            monitor_hub::orchestrator_tick_to_json(result))
                  << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "monitor_hub_orchestrator: "
                  << error.what() << "\n";
        return 1;
    }
}
