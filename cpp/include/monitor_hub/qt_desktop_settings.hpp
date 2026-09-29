#pragma once

#include <QString>

namespace monitor_hub {

struct DesktopSettings {
    bool close_to_tray = true;
    bool notifications = true;
    bool launch_at_login = false;
};

DesktopSettings load_desktop_settings();

bool save_desktop_preferences(
    bool close_to_tray,
    bool notifications,
    QString* error_message = nullptr);

bool launch_at_login_enabled();
bool set_launch_at_login(bool enabled, QString* error_message = nullptr);

bool running_from_development_checkout();
QString desktop_settings_storage();
QString desktop_startup_command();
QString desktop_diagnostics_text();

}  // namespace monitor_hub
