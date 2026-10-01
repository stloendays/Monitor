#include "monitor_hub/qt_desktop_controller.hpp"
#include "monitor_hub/qt_desktop_settings.hpp"
#include "monitor_hub/qt_main_window.hpp"
#include "monitor_hub/qt_app_logger.hpp"

#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QMessageBox>
#include <QDebug>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Monitor Hub");
    QApplication::setApplicationDisplayName("Monitor Hub");
    QApplication::setOrganizationName("Monitor");
    QApplication::setApplicationVersion(QStringLiteral(MONITOR_HUB_VERSION));

    QString logging_error;
    if (!monitor_hub::initialize_desktop_logging(&logging_error)) {
        std::cerr << "warning: desktop logging unavailable: "
                  << logging_error.toStdString() << "\n";
    } else {
        qInfo().noquote()
            << "Monitor Hub desktop starting, version"
            << QCoreApplication::applicationVersion();
    }

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
    const auto saved_settings = monitor_hub::load_desktop_settings();
    if (std::getenv("MONITOR_HUB_REGISTRY") == nullptr &&
        !saved_settings.registry_path.trimmed().isEmpty()) {
        paths.registry = saved_settings.registry_path.toUtf8().toStdString();
    }
    if (std::getenv("MONITOR_HUB_DATA") == nullptr &&
        !saved_settings.hub_data_path.trimmed().isEmpty()) {
        paths.hub_data = saved_settings.hub_data_path.toUtf8().toStdString();
    }

    bool background_requested = false;
    bool diagnostics_requested = false;
    bool registry_cli_override = false;
    bool hub_data_cli_override = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--registry" && i + 1 < argc) {
            paths.registry = argv[++i];
            registry_cli_override = true;
        }
        else if (arg == "--hub-data" && i + 1 < argc) {
            paths.hub_data = argv[++i];
            hub_data_cli_override = true;
        }
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

    if (!background_requested &&
        !registry_cli_override &&
        std::getenv("MONITOR_HUB_REGISTRY") == nullptr) {
        const QFileInfo registry_info(
            QString::fromUtf8(paths.registry.string().c_str()));
        if (!registry_info.exists()) {
            const auto answer = QMessageBox::question(
                nullptr,
                QStringLiteral("导入已有监控登记表"),
                QStringLiteral(
                    "没有找到当前登记表：\n%1\n\n"
                    "如果你已经在 Python 版 Monitor Hub 中使用 monitor_hub_projects.json，"
                    "可以现在选择它；选择后路径会保存到设置。")
                    .arg(registry_info.filePath()),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::Yes);
            if (answer == QMessageBox::Yes) {
                const auto chosen = QFileDialog::getOpenFileName(
                    nullptr,
                    QStringLiteral("选择 monitor_hub_projects.json"),
                    registry_info.absolutePath(),
                    QStringLiteral("JSON (*.json);;All files (*)"));
                if (!chosen.isEmpty()) {
                    paths.registry = chosen.toUtf8().toStdString();
                    QString error;
                    monitor_hub::save_runtime_locations(
                        chosen,
                        hub_data_cli_override
                            ? QString{}
                            : saved_settings.hub_data_path,
                        &error);
                    if (!error.isEmpty()) {
                        QMessageBox::warning(
                            nullptr,
                            QStringLiteral("Monitor Hub"),
                            QStringLiteral("登记表已选择，但路径保存失败：%1").arg(error));
                    }
                }
            }
        }
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
    const int exit_code = app.exec();
    monitor_hub::shutdown_desktop_logging();
    return exit_code;
}
