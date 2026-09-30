#include "monitor_hub/qt_desktop_controller.hpp"
#include "monitor_hub/qt_desktop_settings.hpp"
#include "monitor_hub/qt_main_window.hpp"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QMessageBox>

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Monitor Hub");
    QApplication::setApplicationDisplayName("Monitor Hub");
    QApplication::setOrganizationName("Monitor");
    QApplication::setApplicationVersion(QStringLiteral(MONITOR_HUB_VERSION));
    QApplication::setWindowIcon(
        QIcon(QStringLiteral(":/monitor_hub/icons/monitor_hub.png")));

    QFont app_font(QStringLiteral("Segoe UI"));
    app_font.setPointSizeF(10.0);
    app.setFont(app_font);

    QFile theme(QStringLiteral(":/monitor_hub/styles/modern.qss"));
    if (theme.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(theme.readAll()));
    }

    auto paths = monitor_hub::runtime_paths_from_env(
        argc > 0 ? std::filesystem::path(argv[0]) : std::filesystem::path{});
    bool background_requested = false;
    bool diagnostics_requested = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--registry" && i + 1 < argc) paths.registry = argv[++i];
        else if (arg == "--hub-data" && i + 1 < argc) paths.hub_data = argv[++i];
        else if (arg == "--job-root" && i + 1 < argc) paths.job_root = argv[++i];
        else if (arg == "--no-discovery") paths.discovery = false;
        else if (arg == "--background") background_requested = true;
        else if (arg == "--desktop-diagnostics") diagnostics_requested = true;
        else if (arg == "--version") {
            std::cout << MONITOR_HUB_VERSION << "\n";
            return 0;
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout
                << "monitor_hub_qt [--registry FILE] [--hub-data DIR] "
                   "[--job-root DIR] [--no-discovery] [--background] "
                   "[--desktop-diagnostics] [--version]\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }

    if (diagnostics_requested) {
        const auto diagnostics = monitor_hub::desktop_diagnostics_text().toUtf8();
        std::cout.write(diagnostics.constData(), diagnostics.size());
        std::cout << '\n';
        return 0;
    }

    monitor_hub::QtMainWindow window(std::move(paths));
    monitor_hub::QtDesktopController desktop(window, &app);

    QString instance_error;
    switch (desktop.establish_single_instance(&instance_error)) {
    case monitor_hub::InstanceStatus::ActivatedExisting:
        return 0;
    case monitor_hub::InstanceStatus::Error:
        QMessageBox::critical(
            nullptr,
            QStringLiteral("Monitor Hub"),
            QStringLiteral("无法建立单实例控制通道：%1").arg(instance_error));
        return 3;
    case monitor_hub::InstanceStatus::Primary:
        break;
    }

    desktop.install_system_tray(QApplication::windowIcon());
    desktop.start(background_requested);
    return app.exec();
}
