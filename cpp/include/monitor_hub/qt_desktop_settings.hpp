#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace monitor_hub {

struct DesktopSettings {
    bool close_to_tray = true;
    bool notifications = true;
    bool automatic_control = true;
    bool launch_at_login = false;
    QString registry_path;
    QString hub_data_path;
};

struct DesktopConfigBackupInfo {
    QString path;
    QString captured_at;
};

struct DesktopUiState {
    QByteArray window_geometry;
    QByteArray window_state;
    QString project_id;
    int tab_index = 0;
};

DesktopSettings load_desktop_settings();
DesktopUiState load_desktop_ui_state();

QStringList load_recent_paths(int limit = 15);
bool remember_recent_path(
    const QString& path,
    QString* error_message = nullptr);
bool clear_recent_paths(QString* error_message = nullptr);

QString desktop_config_backup_directory();
QString create_desktop_config_backup(QString* error_message = nullptr);
std::vector<DesktopConfigBackupInfo> list_desktop_config_backups();
bool restore_desktop_config_backup(
    const QString& backup_path,
    QString* error_message = nullptr);

bool save_desktop_preferences(
    bool close_to_tray,
    bool notifications,
    bool automatic_control,
    QString* error_message = nullptr);

bool save_runtime_locations(
    const QString& registry_path,
    const QString& hub_data_path,
    QString* error_message = nullptr);

bool save_desktop_ui_state(
    const DesktopUiState& state,
    QString* error_message = nullptr);

bool begin_desktop_session(
    bool* previous_session_clean = nullptr,
    QString* error_message = nullptr);
bool end_desktop_session(QString* error_message = nullptr);

bool launch_at_login_enabled();
bool set_launch_at_login(bool enabled, QString* error_message = nullptr);

bool running_from_development_checkout();
QString desktop_settings_storage();
QString desktop_startup_command();
QString desktop_orchestrator_program();
QString desktop_diagnostics_text();

}  // namespace monitor_hub
