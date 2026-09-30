#include "monitor_hub/core.hpp"
#include "monitor_hub/claude_cli.hpp"
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
        bool claude_statusline = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--dump") dump = true;
            else if (arg == "--claude-statusline") claude_statusline = true;
            else if (arg == "--registry" && i + 1 < argc) paths.registry = argv[++i];
            else if (arg == "--hub-data" && i + 1 < argc) paths.hub_data = argv[++i];
            else if (arg == "--job-root" && i + 1 < argc) paths.job_root = argv[++i];
            else if (arg == "--no-discovery") paths.discovery = false;
            else if (arg == "--system-info" && i + 1 < argc) {
                system = monitor_hub::load_system_info_fixture(argv[++i]);
                system_fixture = true;
            }
            else if (arg == "--help" || arg == "-h") {
                std::cout << "monitor_hub_cli [--dump | --claude-statusline] [--registry FILE] [--hub-data DIR] [--job-root DIR] [--no-discovery] [--system-info FILE]\n"
                             "  --dump: emit normalized Monitor Hub JSON; without --system-info, Windows Task Scheduler and Win32_Process are probed live through COM/WMI.\n"
                             "  --claude-statusline: read Claude Code statusLine JSON from stdin, write a sanitized local snapshot, and print the compact status line.\n";
                return 0;
            } else {
                std::cerr << "unknown argument: " << arg << "\n";
                return 2;
            }
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
        if (!dump) {
            std::cerr << "the compatibility CLI currently requires --dump or --claude-statusline\n";
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
