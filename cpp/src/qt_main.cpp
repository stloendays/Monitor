#include "monitor_hub/qt_main_window.hpp"

#include <QApplication>

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Monitor Hub");
    QApplication::setOrganizationName("Monitor");

    auto paths = monitor_hub::runtime_paths_from_env(
        argc > 0 ? std::filesystem::path(argv[0]) : std::filesystem::path{});

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--registry" && i + 1 < argc) paths.registry = argv[++i];
        else if (arg == "--hub-data" && i + 1 < argc) paths.hub_data = argv[++i];
        else if (arg == "--job-root" && i + 1 < argc) paths.job_root = argv[++i];
        else if (arg == "--no-discovery") paths.discovery = false;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "monitor_hub_qt [--registry FILE] [--hub-data DIR] [--job-root DIR] [--no-discovery]\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }

    monitor_hub::QtMainWindow window(std::move(paths));
    window.show();
    return app.exec();
}
