#pragma once

#include <QString>

namespace monitor_hub {

bool initialize_desktop_logging(QString* error_message = nullptr);
void shutdown_desktop_logging();

QString desktop_log_directory();
QString desktop_log_file();
bool open_desktop_log_directory(QString* error_message = nullptr);

}  // namespace monitor_hub
