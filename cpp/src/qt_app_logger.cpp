#include "monitor_hub/qt_app_logger.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <QtGlobal>

#include <cstddef>
#include <cstdio>

namespace monitor_hub {
namespace {

constexpr qint64 kMaxLogBytes = 4 * 1024 * 1024;
constexpr int kLogBackups = 3;

QMutex g_log_mutex;
QFile g_log_file;
QtMessageHandler g_previous_handler = nullptr;
bool g_logging_initialized = false;

QString message_level(QtMsgType type) {
    switch (type) {
    case QtDebugMsg:
        return QStringLiteral("DEBUG");
    case QtInfoMsg:
        return QStringLiteral("INFO");
    case QtWarningMsg:
        return QStringLiteral("WARN");
    case QtCriticalMsg:
        return QStringLiteral("ERROR");
    case QtFatalMsg:
        return QStringLiteral("FATAL");
    }
    return QStringLiteral("INFO");
}

bool ensure_log_directory(QString* error_message) {
    const auto directory = desktop_log_directory();
    if (directory.isEmpty()) {
        if (error_message)
            *error_message = QStringLiteral("无法确定桌面日志目录。");
        return false;
    }
    if (QDir().mkpath(directory)) return true;
    if (error_message)
        *error_message = QStringLiteral("无法创建日志目录：%1").arg(directory);
    return false;
}

bool rotate_logs(QString* error_message) {
    const auto path = desktop_log_file();
    const QFileInfo current(path);
    if (!current.exists() || current.size() < kMaxLogBytes) return true;

    QFile::remove(path + QStringLiteral(".%1").arg(kLogBackups));
    for (int index = kLogBackups - 1; index >= 1; --index) {
        const auto from = path + QStringLiteral(".%1").arg(index);
        const auto to = path + QStringLiteral(".%1").arg(index + 1);
        if (QFileInfo::exists(from) && !QFile::rename(from, to)) {
            if (error_message)
                *error_message =
                    QStringLiteral("日志轮转失败：%1").arg(from);
            return false;
        }
    }

    const auto first_backup = path + QStringLiteral(".1");
    QFile::remove(first_backup);
    if (!QFile::rename(path, first_backup)) {
        if (error_message)
            *error_message = QStringLiteral("日志轮转失败：%1").arg(path);
        return false;
    }
    return true;
}

void desktop_message_handler(
    QtMsgType type,
    const QMessageLogContext& context,
    const QString& message) {
    const auto timestamp =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const auto category =
        context.category && context.category[0] != '\0'
            ? QString::fromUtf8(context.category)
            : QStringLiteral("default");
    const auto line =
        QStringLiteral("%1 [%2] [%3] %4\n")
            .arg(timestamp, message_level(type), category, message);

    {
        QMutexLocker locker(&g_log_mutex);
        if (g_log_file.isOpen()) {
            QTextStream stream(&g_log_file);
            stream << line;
            stream.flush();
        }
    }

    if (g_previous_handler) {
        g_previous_handler(type, context, message);
    } else {
        const auto utf8 = line.toUtf8();
        std::fwrite(
            utf8.constData(),
            sizeof(char),
            static_cast<std::size_t>(utf8.size()),
            stderr);
        std::fflush(stderr);
    }
}

}  // namespace

QString desktop_log_directory() {
    auto root =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty()) {
        root = QDir::home().filePath(QStringLiteral(".monitor-hub"));
    }
    return QDir(root).filePath(QStringLiteral("logs"));
}

QString desktop_log_file() {
    return QDir(desktop_log_directory())
        .filePath(QStringLiteral("monitor-hub.log"));
}

bool initialize_desktop_logging(QString* error_message) {
    QMutexLocker locker(&g_log_mutex);
    if (g_logging_initialized) return true;

    if (!ensure_log_directory(error_message)) return false;
    if (!rotate_logs(error_message)) return false;

    g_log_file.setFileName(desktop_log_file());
    if (!g_log_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (error_message) {
            *error_message =
                QStringLiteral("无法打开日志文件：%1").arg(g_log_file.errorString());
        }
        return false;
    }

    g_previous_handler = qInstallMessageHandler(desktop_message_handler);
    g_logging_initialized = true;
    return true;
}

void shutdown_desktop_logging() {
    QMutexLocker locker(&g_log_mutex);
    if (!g_logging_initialized) return;

    qInstallMessageHandler(g_previous_handler);
    g_previous_handler = nullptr;
    g_log_file.flush();
    g_log_file.close();
    g_logging_initialized = false;
}

bool open_desktop_log_directory(QString* error_message) {
    if (!ensure_log_directory(error_message)) return false;
    if (QDesktopServices::openUrl(
            QUrl::fromLocalFile(desktop_log_directory()))) {
        return true;
    }
    if (error_message) {
        *error_message =
            QStringLiteral("系统无法打开日志目录：%1").arg(desktop_log_directory());
    }
    return false;
}

}  // namespace monitor_hub
