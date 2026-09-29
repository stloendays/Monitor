#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <QVector>

#include <stdexcept>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

class UpdateFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void log_line(const QString& log_path, const QString& text) {
    QFileInfo info(log_path);
    QDir().mkpath(info.absolutePath());
    QFile f(log_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    QTextStream out(&f);
    out << QDateTime::currentDateTime().toString(Qt::ISODate)
        << ' ' << text << '\n';
}

QJsonObject read_object(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        throw UpdateFailure(
            QStringLiteral("cannot open JSON: %1").arg(path).toStdString());
    }
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        throw UpdateFailure(
            QStringLiteral("invalid JSON: %1").arg(path).toStdString());
    }
    return doc.object();
}

QString safe_rel_path(QString text) {
    text.replace('\\', '/');
    const auto cleaned = QDir::cleanPath(text);
    if (text.isEmpty() ||
        QDir::isAbsolutePath(text) ||
        cleaned == QStringLiteral(".") ||
        cleaned == QStringLiteral("..") ||
        cleaned.startsWith(QStringLiteral("../")) ||
        cleaned.contains(QStringLiteral("/../")) ||
        cleaned.contains(QLatin1Char(':'))) {
        throw UpdateFailure(
            QStringLiteral("unsafe manifest path: %1").arg(text).toStdString());
    }
    return cleaned;
}

QByteArray sha256_file(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        throw UpdateFailure(
            QStringLiteral("cannot hash file: %1").arg(path).toStdString());
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!f.atEnd()) {
        const auto block = f.read(1024 * 1024);
        if (block.isEmpty() && f.error() != QFile::NoError) {
            throw UpdateFailure(
                QStringLiteral("read failed while hashing: %1")
                    .arg(path)
                    .toStdString());
        }
        hash.addData(block);
    }
    return hash.result().toHex();
}

QString verify_stage(const QString& stage_dir) {
    const QDir stage(stage_dir);
    const auto manifest_path = stage.filePath(QStringLiteral("manifest.json"));
    const auto manifest = read_object(manifest_path);

    if (manifest.value(QStringLiteral("format")).toInt(-1) != 1) {
        throw UpdateFailure("unsupported update manifest format");
    }

    const auto version = manifest.value(QStringLiteral("version")).toString();
    static const QRegularExpression semver(
        QStringLiteral(R"(^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$)"));
    if (!semver.match(version).hasMatch()) {
        throw UpdateFailure("invalid update version");
    }

    const auto files = manifest.value(QStringLiteral("files")).toArray();
    if (files.isEmpty()) {
        throw UpdateFailure("update manifest contains no files");
    }

    const QDir payload(stage.filePath(QStringLiteral("payload")));
    QSet<QString> seen;
    for (const auto& value : files) {
        if (!value.isObject()) throw UpdateFailure("invalid manifest file entry");
        const auto entry = value.toObject();
        const auto rel = safe_rel_path(entry.value(QStringLiteral("path")).toString());
        if (seen.contains(rel)) {
            throw UpdateFailure(
                QStringLiteral("duplicate manifest path: %1").arg(rel).toStdString());
        }
        seen.insert(rel);

        const QFileInfo src(payload.filePath(rel));
        if (!src.isFile()) {
            throw UpdateFailure(
                QStringLiteral("payload file missing: %1").arg(rel).toStdString());
        }

        const auto expected_size =
            static_cast<qint64>(entry.value(QStringLiteral("size")).toDouble(-1));
        if (expected_size < 0 || src.size() != expected_size) {
            throw UpdateFailure(
                QStringLiteral("payload size mismatch: %1").arg(rel).toStdString());
        }

        const auto expected_hash =
            entry.value(QStringLiteral("sha256"))
                .toString()
                .toLatin1()
                .toLower();
        if (expected_hash.size() != 64 ||
            sha256_file(src.absoluteFilePath()) != expected_hash) {
            throw UpdateFailure(
                QStringLiteral("payload hash mismatch: %1").arg(rel).toStdString());
        }
    }

    return version;
}

bool has_git_ancestor(QString path) {
    QDir dir(path);
    for (int i = 0; i < 12; ++i) {
        if (QFileInfo(dir.filePath(QStringLiteral(".git"))).exists()) return true;
        if (!dir.cdUp()) break;
    }
    return false;
}

void verify_install_root(const QString& install_root) {
    if (has_git_ancestor(install_root)) {
        throw UpdateFailure("refusing to update a Git development checkout");
    }

    const QDir root(install_root);
    const auto marker_path =
        root.filePath(QStringLiteral(".monitor-hub-release.json"));
    const auto marker = read_object(marker_path);
    if (marker.value(QStringLiteral("format")).toInt(-1) != 1 ||
        marker.value(QStringLiteral("version")).toString().isEmpty()) {
        throw UpdateFailure("install root is not a managed Monitor Hub release");
    }
}

void atomic_copy(const QString& source, const QString& destination) {
    QFile src(source);
    if (!src.open(QIODevice::ReadOnly)) {
        throw UpdateFailure(
            QStringLiteral("cannot open source file: %1").arg(source).toStdString());
    }

    QFileInfo dst_info(destination);
    if (!QDir().mkpath(dst_info.absolutePath())) {
        throw UpdateFailure(
            QStringLiteral("cannot create destination directory: %1")
                .arg(dst_info.absolutePath())
                .toStdString());
    }

    QSaveFile dst(destination);
    if (!dst.open(QIODevice::WriteOnly)) {
        throw UpdateFailure(
            QStringLiteral("cannot open destination file: %1")
                .arg(destination)
                .toStdString());
    }

    while (!src.atEnd()) {
        const auto block = src.read(1024 * 1024);
        if (block.isEmpty() && src.error() != QFile::NoError) {
            dst.cancelWriting();
            throw UpdateFailure(
                QStringLiteral("read failed: %1").arg(source).toStdString());
        }
        if (dst.write(block) != block.size()) {
            dst.cancelWriting();
            throw UpdateFailure(
                QStringLiteral("write failed: %1").arg(destination).toStdString());
        }
    }

    if (!dst.commit()) {
        throw UpdateFailure(
            QStringLiteral("atomic replace failed: %1")
                .arg(destination)
                .toStdString());
    }
}

void wait_for_pid(qint64 pid) {
    if (pid <= 0) return;
#ifdef Q_OS_WIN
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!handle) return;
    const DWORD result = WaitForSingleObject(handle, 120000);
    CloseHandle(handle);
    if (result == WAIT_TIMEOUT) {
        throw UpdateFailure("timed out waiting for Monitor Hub to exit");
    }
    if (result != WAIT_OBJECT_0) {
        throw UpdateFailure("failed waiting for Monitor Hub to exit");
    }
#else
    Q_UNUSED(pid);
#endif
}

QString apply_update(
    const QString& stage_dir,
    const QString& install_root,
    const QString& backup_root,
    const QString& log_path) {
    verify_install_root(install_root);
    const auto version = verify_stage(stage_dir);

    const auto manifest =
        read_object(QDir(stage_dir).filePath(QStringLiteral("manifest.json")));
    const auto files = manifest.value(QStringLiteral("files")).toArray();

    const QDir payload(QDir(stage_dir).filePath(QStringLiteral("payload")));
    const QDir root(install_root);

    const auto backup_dir =
        QDir(backup_root).filePath(
            QStringLiteral("v%1-%2")
                .arg(version)
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    if (!QDir().mkpath(backup_dir)) {
        throw UpdateFailure("cannot create update backup directory");
    }

    struct AppliedFile {
        QString rel;
        bool existed = false;
    };
    QVector<AppliedFile> applied;

    try {
        for (const auto& value : files) {
            const auto entry = value.toObject();
            const auto rel =
                safe_rel_path(entry.value(QStringLiteral("path")).toString());
            const auto source = payload.filePath(rel);
            const auto destination = root.filePath(rel);
            const bool existed = QFileInfo::exists(destination);

            if (existed) {
                atomic_copy(
                    destination,
                    QDir(backup_dir).filePath(rel));
            }

            atomic_copy(source, destination);
            applied.push_back({rel, existed});
        }
    } catch (...) {
        log_line(log_path, QStringLiteral("apply failed; rolling back"));
        for (auto it = applied.crbegin(); it != applied.crend(); ++it) {
            const auto destination = root.filePath(it->rel);
            const auto backup = QDir(backup_dir).filePath(it->rel);
            try {
                if (it->existed && QFileInfo::exists(backup)) {
                    atomic_copy(backup, destination);
                } else if (!it->existed) {
                    QFile::remove(destination);
                }
            } catch (...) {
                log_line(
                    log_path,
                    QStringLiteral("rollback warning: %1").arg(it->rel));
            }
        }
        throw;
    }

    log_line(
        log_path,
        QStringLiteral("updated successfully to %1; backup=%2")
            .arg(version, backup_dir));
    return version;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Monitor Hub Updater"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Monitor Hub release update helper"));
    parser.addHelpOption();

    QCommandLineOption verify_option(
        QStringList{QStringLiteral("verify-stage")},
        QStringLiteral("Verify an extracted update stage and exit."),
        QStringLiteral("dir"));
    QCommandLineOption pid_option(
        QStringList{QStringLiteral("pid")},
        QStringLiteral("PID of the Monitor Hub process to wait for."),
        QStringLiteral("pid"));
    QCommandLineOption stage_option(
        QStringList{QStringLiteral("stage")},
        QStringLiteral("Extracted update stage directory."),
        QStringLiteral("dir"));
    QCommandLineOption root_option(
        QStringList{QStringLiteral("install-root")},
        QStringLiteral("Managed Monitor Hub install root."),
        QStringLiteral("dir"));
    QCommandLineOption backup_option(
        QStringList{QStringLiteral("backup-root")},
        QStringLiteral("Directory for rollback backups."),
        QStringLiteral("dir"));
    QCommandLineOption restart_exe_option(
        QStringList{QStringLiteral("restart-exe")},
        QStringLiteral("Executable to restart after a successful apply."),
        QStringLiteral("path"));
    QCommandLineOption restart_arg_option(
        QStringList{QStringLiteral("restart-arg")},
        QStringLiteral("Argument passed to the restarted process; may repeat."),
        QStringLiteral("arg"));

    parser.addOption(verify_option);
    parser.addOption(pid_option);
    parser.addOption(stage_option);
    parser.addOption(root_option);
    parser.addOption(backup_option);
    parser.addOption(restart_exe_option);
    parser.addOption(restart_arg_option);
    parser.process(app);

    try {
        if (parser.isSet(verify_option)) {
            const auto version = verify_stage(parser.value(verify_option));
            QTextStream(stdout) << version << '\n';
            return 0;
        }

        bool pid_ok = false;
        const auto pid = parser.value(pid_option).toLongLong(&pid_ok);
        const auto stage = parser.value(stage_option);
        const auto install_root = parser.value(root_option);
        const auto backup_root = parser.value(backup_option);
        const auto restart_exe = parser.value(restart_exe_option);

        if (!pid_ok || stage.isEmpty() || install_root.isEmpty() ||
            backup_root.isEmpty() || restart_exe.isEmpty()) {
            throw UpdateFailure(
                "apply mode requires --pid, --stage, --install-root, "
                "--backup-root, and --restart-exe");
        }

        const auto log_path =
            QDir(backup_root).filePath(QStringLiteral("../update.log"));
        log_line(log_path, QStringLiteral("waiting for pid %1").arg(pid));
        wait_for_pid(pid);

        const auto version =
            apply_update(stage, install_root, backup_root, log_path);

        if (!QProcess::startDetached(
                restart_exe,
                parser.values(restart_arg_option),
                install_root)) {
            throw UpdateFailure("updated successfully but restart launch failed");
        }

        log_line(
            log_path,
            QStringLiteral("finished v%1; restart launched").arg(version));
        return 0;
    } catch (const std::exception& e) {
        QTextStream(stderr) << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
