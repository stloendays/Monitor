#include "monitor_hub/core.hpp"
#include "monitor_hub/windows_probe.hpp"

#include <boost/json.hpp>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    try {
        auto paths = monitor_hub::runtime_paths_from_env(argc > 0 ? std::filesystem::path(argv[0]) : std::filesystem::path{});
        monitor_hub::SystemInfo system;
        bool system_fixture = false;
        bool dump = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--dump") dump = true;
            else if (arg == "--registry" && i + 1 < argc) paths.registry = argv[++i];
            else if (arg == "--hub-data" && i + 1 < argc) paths.hub_data = argv[++i];
            else if (arg == "--job-root" && i + 1 < argc) paths.job_root = argv[++i];
            else if (arg == "--no-discovery") paths.discovery = false;
            else if (arg == "--system-info" && i + 1 < argc) {
                system = monitor_hub::load_system_info_fixture(argv[++i]);
                system_fixture = true;
            }
            else if (arg == "--help" || arg == "-h") {
                std::cout << "monitor_hub_cli [--dump] [--registry FILE] [--hub-data DIR] [--job-root DIR] [--no-discovery] [--system-info FILE]\n"
                             "  Without --system-info, Windows Task Scheduler and Win32_Process are probed live through COM/WMI.\n";
                return 0;
            } else {
                std::cerr << "unknown argument: " << arg << "\n";
                return 2;
            }
        }
        if (!dump) {
            std::cerr << "the compatibility CLI currently requires --dump\n";
            return 2;
        }
        if (!system_fixture) system = monitor_hub::probe_system_info();
        std::cout << boost::json::serialize(monitor_hub::dump_all(system, paths)) << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "monitor_hub_cli: " << e.what() << "\n";
        return 1;
    }
}
