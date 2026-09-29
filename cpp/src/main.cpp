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
        bool probe_system = false;
        bool have_fixture = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--dump") dump = true;
            else if (arg == "--probe-system") probe_system = true;
            else if (arg == "--registry" && i + 1 < argc) paths.registry = argv[++i];
            else if (arg == "--hub-data" && i + 1 < argc) paths.hub_data = argv[++i];
            else if (arg == "--job-root" && i + 1 < argc) paths.job_root = argv[++i];
            else if (arg == "--no-discovery") paths.discovery = false;
            else if (arg == "--system-info" && i + 1 < argc) {
                system = monitor_hub::load_system_info_fixture(argv[++i]);
                have_fixture = true;
            } else if (arg == "--help" || arg == "-h") {
                std::cout
                    << "monitor_hub_cli [--dump | --probe-system] [--registry FILE] [--hub-data DIR]\n"
                    << "                [--job-root DIR] [--no-discovery] [--system-info FILE]\n"
                    << "\n"
                    << "--probe-system  Read Task Scheduler + selected process information through COM/WMI.\n"
                    << "--system-info   Use a JSON fixture instead of the live Windows probe.\n";
                return 0;
            } else {
                std::cerr << "unknown argument: " << arg << "\n";
                return 2;
            }
        }

        if (!have_fixture && (dump || probe_system)) system = monitor_hub::probe_system_info();

        if (probe_system) {
            std::cout << boost::json::serialize(monitor_hub::system_info_json(system)) << "\n";
            return system.error.empty() ? 0 : 1;
        }
        if (dump) {
            std::cout << boost::json::serialize(monitor_hub::dump_all(system, paths)) << "\n";
            return 0;
        }

        std::cerr << "use --dump or --probe-system\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "monitor_hub_cli: " << e.what() << "\n";
        return 1;
    }
}
