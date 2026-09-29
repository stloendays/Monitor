#include "monitor_hub/core.hpp"

#include <boost/json.hpp>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    try {
        auto paths = monitor_hub::runtime_paths_from_env(argc > 0 ? std::filesystem::path(argv[0]) : std::filesystem::path{});
        monitor_hub::SystemInfo system;
        bool dump = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--dump") dump = true;
            else if (arg == "--registry" && i + 1 < argc) paths.registry = argv[++i];
            else if (arg == "--hub-data" && i + 1 < argc) paths.hub_data = argv[++i];
            else if (arg == "--job-root" && i + 1 < argc) paths.job_root = argv[++i];
            else if (arg == "--no-discovery") paths.discovery = false;
            else if (arg == "--system-info" && i + 1 < argc) system = monitor_hub::load_system_info_fixture(argv[++i]);
            else if (arg == "--help" || arg == "-h") {
                std::cout << "monitor_hub_cli [--dump] [--registry FILE] [--hub-data DIR] [--no-discovery] [--system-info FILE]\n";
                return 0;
            } else {
                std::cerr << "unknown argument: " << arg << "\n";
                return 2;
            }
        }
        if (!dump) {
            std::cerr << "phase 1 currently exposes the compatibility CLI only; use --dump\n";
            return 2;
        }
        std::cout << boost::json::serialize(monitor_hub::dump_all(system, paths)) << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "monitor_hub_cli: " << e.what() << "\n";
        return 1;
    }
}
