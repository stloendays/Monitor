#include "monitor_hub/qt_desktop_settings.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
#include <QtGlobal>

namespace monitor_hub {
namespace {

constexpr auto kCloseToTray = "desktop/close_to_tray";
constexpr auto kNotifications = "desktop/notifications";
constexpr auto kAutomaticControl = "desktop/automatic_control";
constexpr auto kStartupValueName = "Monitor Hub";

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

}  // namespace

DesktopSettings load_desktop_settings() {
    QSettings settings;
    DesktopSettings out;
    out.close_to_tray = settings.value(kCloseToTray, true).toBool();
    out.notifications = settings.value(kNotifications, true).toBool();
    out.automatic_control = settings.value(kAutomaticControl, true).toBool();
    out.launch_at_login = launch_at_login_enabled();
    return out;
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
    settings.sync();

    if (settings.status() != QSettings::NoError) {
        if (error_message) *error_message = settings_error_text(settings.status());
        return false;
    }
    return true;
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

    QStringList lines;
    lines << QStringLiteral("Monitor Hub desktop diagnostics")
          << QStringLiteral("version=%1").arg(QCoreApplication::applicationVersion())
          << QStringLiteral("qt=%1").arg(QString::fromLatin1(qVersion()))
          << QStringLiteral("executable=%1").arg(
                 QDir::toNativeSeparators(QCoreApplication::applicationFilePath()))
          << QStringLiteral("settings=%1").arg(desktop_settings_storage())
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
          << QStringLiteral("launch_at_login=%1").arg(
                 settings.launch_at_login ? QStringLiteral("true")
                                          : QStringLiteral("false"));
    return lines.join(QLatin1Char('\n'));
}

}  // namespace monitor_hub
