#include "monitor_hub/qt_desktop_settings.hpp"
#include "monitor_hub/qt_app_logger.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>

namespace monitor_hub {
namespace {

constexpr auto kCloseToTray = "desktop/close_to_tray";
constexpr auto kNotifications = "desktop/notifications";
constexpr auto kAutomaticControl = "desktop/automatic_control";
constexpr auto kRegistryPath = "runtime/registry_path";
constexpr auto kHubDataPath = "runtime/hub_data_path";
constexpr auto kWindowGeometry = "ui/window_geometry";
constexpr auto kWindowState = "ui/window_state";
constexpr auto kSelectedProject = "ui/selected_project";
constexpr auto kSelectedTab = "ui/selected_tab";
constexpr auto kRecentPaths = "recent/paths";
constexpr auto kCleanShutdown = "session/clean_shutdown";
constexpr auto kStartupValueName = "Monitor Hub";
constexpr int kConfigBackupSchemaVersion = 1;
constexpr int kMaxConfigBackups = 5;
constexpr int kMaxRecentPaths = 15;

#ifdef Q_OS_WIN
constexpr auto kWindowsRunKey =
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
#endif

QString settings_error_text(QSettings::Status status) {
    switch (status) {
    case QSettings::AccessError:
        return QStringLiteral("无法写入设置存储。");
    case QSettings::FormatError:
        return QStringLiteral("设置存储格式无效。");
    case QSettings::NoError:
        return {};
    }
    return QStringLiteral("未知设置错误。");
}

bool sync_settings(QSettings& settings, QString* error_message) {
    settings.sync();
    if (settings.status() == QSettings::NoError) return true;
    if (error_message) *error_message = settings_error_text(settings.status());
    return false;
}

QString normalized_recent_path(const QString& value) {
    const auto trimmed = value.trimmed();
    if (trimmed.isEmpty()) return {};
    const QFileInfo info(trimmed);
    return QDir::cleanPath(info.absoluteFilePath());
}

bool ensure_backup_directory(QString* error_message = nullptr) {
    const auto path = desktop_config_backup_directory();
    if (path.isEmpty()) {
        if (error_message)
            *error_message = QStringLiteral("无法确定配置备份目录。");
        return false;
    }
    if (QDir().mkpath(path)) return true;
    if (error_message)
        *error_message = QStringLiteral("无法创建配置备份目录：%1").arg(path);
    return false;
}

QJsonArray recent_paths_json() {
    QJsonArray items;
    for (const auto& path : load_recent_paths())
        items.append(path);
    return items;
}

QString backup_capture_time(const QString& path) {
    QFile file(path);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error == QJsonParseError::NoError && document.isObject()) {
            const auto captured =
                document.object().value(QStringLiteral("captured_at")).toString();
            if (!captured.isEmpty()) return captured;
        }
    }
    return QFileInfo(path).lastModified().toUTC().toString(Qt::ISODateWithMs);
}

void prune_config_backups() {
    QDir directory(desktop_config_backup_directory());
    const auto entries = directory.entryInfoList(
        QStringList{QStringLiteral("desktop-config-*.json")},
        QDir::Files,
        QDir::Name | QDir::Reversed);
    for (int index = kMaxConfigBackups; index < entries.size(); ++index)
        QFile::remove(entries[index].absoluteFilePath());
}

}  // namespace

DesktopSettings load_desktop_settings() {
    QSettings settings;
    DesktopSettings out;
    out.close_to_tray = settings.value(kCloseToTray, true).toBool();
    out.notifications = settings.value(kNotifications, true).toBool();
    out.automatic_control = settings.value(kAutomaticControl, true).toBool();
    out.registry_path = settings.value(kRegistryPath).toString();
    out.hub_data_path = settings.value(kHubDataPath).toString();
    out.launch_at_login = launch_at_login_enabled();
    return out;
}

DesktopUiState load_desktop_ui_state() {
    QSettings settings;
    DesktopUiState out;
    out.window_geometry = settings.value(kWindowGeometry).toByteArray();
    out.window_state = settings.value(kWindowState).toByteArray();
    out.project_id = settings.value(kSelectedProject).toString();
    out.tab_index = settings.value(kSelectedTab, 0).toInt();
    return out;
}

QStringList load_recent_paths(int limit) {
    QSettings settings;
    auto paths = settings.value(kRecentPaths).toStringList();
    if (limit > 0 && paths.size() > limit)
        paths = paths.mid(0, limit);
    return paths;
}

bool remember_recent_path(
    const QString& path,
    QString* error_message) {

    const auto normalized = normalized_recent_path(path);
    if (normalized.isEmpty() || !QFileInfo::exists(normalized)) {
        if (error_message) {
            *error_message = normalized.isEmpty()
                ? QStringLiteral("最近文件路径为空。")
                : QStringLiteral("路径不存在：%1").arg(normalized);
        }
        return false;
    }

    QSettings settings;
    auto paths = settings.value(kRecentPaths).toStringList();

    for (int index = paths.size() - 1; index >= 0; --index) {
        if (QDir::cleanPath(paths[index]).compare(
                normalized,
                Qt::CaseInsensitive) == 0) {
            paths.removeAt(index);
        }
    }

    paths.prepend(normalized);
    if (paths.size() > kMaxRecentPaths)
        paths = paths.mid(0, kMaxRecentPaths);
    settings.setValue(kRecentPaths, paths);
    return sync_settings(settings, error_message);
}

bool clear_recent_paths(QString* error_message) {
    QSettings settings;
    settings.remove(kRecentPaths);
    return sync_settings(settings, error_message);
}

QString desktop_config_backup_directory() {
    auto root =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty())
        root = QDir::home().filePath(QStringLiteral(".monitor-hub"));
    return QDir(root).filePath(QStringLiteral("config-backups"));
}

QString create_desktop_config_backup(QString* error_message) {
    if (!ensure_backup_directory(error_message)) return {};

    const auto desktop = load_desktop_settings();
    const auto captured_at =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    QJsonObject settings_json;
    settings_json.insert(
        QStringLiteral("close_to_tray"),
        desktop.close_to_tray);
    settings_json.insert(
        QStringLiteral("notifications"),
        desktop.notifications);
    settings_json.insert(
        QStringLiteral("automatic_control"),
        desktop.automatic_control);
    settings_json.insert(
        QStringLiteral("launch_at_login"),
        desktop.launch_at_login);
    settings_json.insert(
        QStringLiteral("registry_path"),
        desktop.registry_path);
    settings_json.insert(
        QStringLiteral("hub_data_path"),
        desktop.hub_data_path);

    QJsonObject root;
    root.insert(
        QStringLiteral("schema_version"),
        kConfigBackupSchemaVersion);
    root.insert(
        QStringLiteral("captured_at"),
        captured_at);
    root.insert(
        QStringLiteral("app_version"),
        QCoreApplication::applicationVersion());
    root.insert(
        QStringLiteral("settings"),
        settings_json);
    root.insert(
        QStringLiteral("recent_paths"),
        recent_paths_json());

    const auto stamp =
        QDateTime::currentDateTimeUtc().toString(
            QStringLiteral("yyyyMMdd-HHmmsszzz"));
    QDir directory(desktop_config_backup_directory());
    auto path = directory.filePath(
        QStringLiteral("desktop-config-%1.json").arg(stamp));
    for (int suffix = 1; QFileInfo::exists(path); ++suffix) {
        path = directory.filePath(
            QStringLiteral("desktop-config-%1-%2.json")
                .arg(stamp)
                .arg(suffix));
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error_message)
            *error_message =
                QStringLiteral("无法创建配置备份：%1")
                    .arg(file.errorString());
        return {};
    }

    const auto payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size() || !file.commit()) {
        if (error_message)
            *error_message =
                QStringLiteral("无法写入配置备份：%1")
                    .arg(file.errorString());
        return {};
    }

    prune_config_backups();
    return path;
}

std::vector<DesktopConfigBackupInfo> list_desktop_config_backups() {
    std::vector<DesktopConfigBackupInfo> backups;
    QDir directory(desktop_config_backup_directory());
    const auto entries = directory.entryInfoList(
        QStringList{QStringLiteral("desktop-config-*.json")},
        QDir::Files,
        QDir::Name | QDir::Reversed);
    backups.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto& entry : entries) {
        backups.push_back({
            entry.absoluteFilePath(),
            backup_capture_time(entry.absoluteFilePath()),
        });
    }
    return backups;
}

bool restore_desktop_config_backup(
    const QString& backup_path,
    QString* error_message) {

    QFile file(backup_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error_message)
            *error_message =
                QStringLiteral("无法读取配置备份：%1")
                    .arg(file.errorString());
        return false;
    }

    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        if (error_message)
            *error_message =
                QStringLiteral("配置备份 JSON 无效：%1")
                    .arg(parse_error.errorString());
        return false;
    }

    const auto root = document.object();
    if (root.value(QStringLiteral("schema_version")).toInt() !=
        kConfigBackupSchemaVersion) {
        if (error_message)
            *error_message = QStringLiteral("配置备份版本不受支持。");
        return false;
    }

    const auto settings_value = root.value(QStringLiteral("settings"));
    if (!settings_value.isObject()) {
        if (error_message)
            *error_message = QStringLiteral("配置备份缺少 settings。");
        return false;
    }

    const auto saved = settings_value.toObject();
    const bool close_to_tray =
        saved.value(QStringLiteral("close_to_tray")).toBool(true);
    const bool notifications =
        saved.value(QStringLiteral("notifications")).toBool(true);
    const bool automatic_control =
        saved.value(QStringLiteral("automatic_control")).toBool(true);
    const bool launch_at_login =
        saved.value(QStringLiteral("launch_at_login")).toBool(false);
    const auto registry_path =
        saved.value(QStringLiteral("registry_path")).toString();
    const auto hub_data_path =
        saved.value(QStringLiteral("hub_data_path")).toString();

    QSettings settings;
    settings.setValue(kCloseToTray, close_to_tray);
    settings.setValue(kNotifications, notifications);
    settings.setValue(kAutomaticControl, automatic_control);
    settings.setValue(kRegistryPath, registry_path);
    settings.setValue(kHubDataPath, hub_data_path);

    const auto recent = root.value(QStringLiteral("recent_paths"));
    if (recent.isArray()) {
        QStringList recent_paths;
        for (const auto& item : recent.toArray()) {
            if (item.isString() && !item.toString().trimmed().isEmpty())
                recent_paths << item.toString();
        }
        settings.setValue(kRecentPaths, recent_paths);
    }

    if (!sync_settings(settings, error_message))
        return false;

    if (launch_at_login_enabled() != launch_at_login) {
        QString startup_error;
        if (!set_launch_at_login(launch_at_login, &startup_error)) {
            if (error_message) {
                *error_message =
                    QStringLiteral(
                        "桌面配置已恢复，但开机启动状态恢复失败：%1")
                        .arg(startup_error);
            }
            return false;
        }
    }

    return true;
}

bool save_desktop_preferences(
    bool close_to_tray,
    bool notifications,
    bool automatic_control,
    QString* error_message) {
    QSettings settings;
    settings.setValue(kCloseToTray, close_to_tray);
    settings.setValue(kNotifications, notifications);
    settings.setValue(kAutomaticControl, automatic_control);
    return sync_settings(settings, error_message);
}

bool save_runtime_locations(
    const QString& registry_path,
    const QString& hub_data_path,
    QString* error_message) {
    QSettings settings;
    settings.setValue(kRegistryPath, registry_path.trimmed());
    settings.setValue(kHubDataPath, hub_data_path.trimmed());
    return sync_settings(settings, error_message);
}

bool save_desktop_ui_state(
    const DesktopUiState& state,
    QString* error_message) {
    QSettings settings;
    settings.setValue(kWindowGeometry, state.window_geometry);
    settings.setValue(kWindowState, state.window_state);
    settings.setValue(kSelectedProject, state.project_id);
    settings.setValue(kSelectedTab, state.tab_index);
    return sync_settings(settings, error_message);
}

bool begin_desktop_session(
    bool* previous_session_clean,
    QString* error_message) {
    QSettings settings;
    const bool clean = settings.value(kCleanShutdown, true).toBool();
    if (previous_session_clean) *previous_session_clean = clean;
    settings.setValue(kCleanShutdown, false);
    return sync_settings(settings, error_message);
}

bool end_desktop_session(QString* error_message) {
    QSettings settings;
    settings.setValue(kCleanShutdown, true);
    return sync_settings(settings, error_message);
}

QString desktop_startup_command() {
    const auto executable =
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QStringLiteral("\"%1\" --background").arg(executable);
}

bool launch_at_login_enabled() {
#ifdef Q_OS_WIN
    QSettings run_key(QString::fromLatin1(kWindowsRunKey), QSettings::NativeFormat);
    return run_key.value(QString::fromLatin1(kStartupValueName)).toString() ==
           desktop_startup_command();
#else
    return false;
#endif
}

bool set_launch_at_login(bool enabled, QString* error_message) {
#ifndef Q_OS_WIN
    if (enabled) {
        if (error_message)
            *error_message = QStringLiteral("当前平台尚未实现开机启动注册。");
        return false;
    }
    return true;
#else
    if (enabled && running_from_development_checkout()) {
        if (error_message) {
            *error_message = QStringLiteral(
                "开发工作区中的程序不会注册开机启动。请先安装正式 Release，"
                "再从安装版设置开机启动。");
        }
        return false;
    }

    QSettings run_key(QString::fromLatin1(kWindowsRunKey), QSettings::NativeFormat);
    const auto name = QString::fromLatin1(kStartupValueName);
    if (enabled) run_key.setValue(name, desktop_startup_command());
    else run_key.remove(name);
    run_key.sync();

    if (run_key.status() != QSettings::NoError) {
        if (error_message) *error_message = settings_error_text(run_key.status());
        return false;
    }
    return true;
#endif
}

bool running_from_development_checkout() {
    QDir dir(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 10; ++depth) {
        const auto marker = QFileInfo(dir.filePath(QStringLiteral(".git")));
        if (marker.exists()) return true;
        if (!dir.cdUp()) break;
    }
    return false;
}

QString desktop_settings_storage() {
    QSettings settings;
    return settings.fileName();
}

QString desktop_orchestrator_program() {
#ifdef Q_OS_WIN
    constexpr auto kName = "monitor_hub_orchestrator.exe";
#else
    constexpr auto kName = "monitor_hub_orchestrator";
#endif
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QString::fromLatin1(kName));
}

QString desktop_diagnostics_text() {
    const auto settings = load_desktop_settings();
    const auto ui = load_desktop_ui_state();

    QStringList lines;
    lines << QStringLiteral("Monitor Hub desktop diagnostics")
          << QStringLiteral("version=%1").arg(QCoreApplication::applicationVersion())
          << QStringLiteral("qt=%1").arg(QString::fromLatin1(qVersion()))
          << QStringLiteral("executable=%1").arg(
                 QDir::toNativeSeparators(QCoreApplication::applicationFilePath()))
          << QStringLiteral("settings=%1").arg(desktop_settings_storage())
          << QStringLiteral("log_file=%1").arg(
                 QDir::toNativeSeparators(desktop_log_file()))
          << QStringLiteral("config_backup_dir=%1").arg(
                 QDir::toNativeSeparators(desktop_config_backup_directory()))
          << QStringLiteral("recent_paths=%1").arg(load_recent_paths().size())
          << QStringLiteral("orchestrator=%1").arg(
                 QDir::toNativeSeparators(desktop_orchestrator_program()))
          << QStringLiteral("orchestrator_exists=%1").arg(
                 QFileInfo::exists(desktop_orchestrator_program())
                     ? QStringLiteral("true")
                     : QStringLiteral("false"))
          << QStringLiteral("development_checkout=%1").arg(
                 running_from_development_checkout() ? QStringLiteral("true")
                                                     : QStringLiteral("false"))
          << QStringLiteral("close_to_tray=%1").arg(
                 settings.close_to_tray ? QStringLiteral("true")
                                        : QStringLiteral("false"))
          << QStringLiteral("notifications=%1").arg(
                 settings.notifications ? QStringLiteral("true")
                                        : QStringLiteral("false"))
          << QStringLiteral("automatic_control=%1").arg(
                 settings.automatic_control ? QStringLiteral("true")
                                            : QStringLiteral("false"))
          << QStringLiteral("registry_path=%1").arg(settings.registry_path)
          << QStringLiteral("hub_data_path=%1").arg(settings.hub_data_path)
          << QStringLiteral("selected_project=%1").arg(ui.project_id)
          << QStringLiteral("selected_tab=%1").arg(ui.tab_index)
          << QStringLiteral("launch_at_login=%1").arg(
                 settings.launch_at_login ? QStringLiteral("true")
                                          : QStringLiteral("false"));
    return lines.join(QLatin1Char('\n'));
}

}  // namespace monitor_hub
