#include "monitor_hub/qt_update_service.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <memory>

namespace monitor_hub {
namespace {

constexpr auto kRepo = "stloendays/Monitor";
constexpr auto kArchiveName = "monitor-hub-windows-x64.zip";
constexpr auto kChecksumName = "monitor-hub-windows-x64.zip.sha256";

struct StableVersion {
    quint64 major = 0;
    quint64 minor = 0;
    quint64 patch = 0;
};

std::optional<StableVersion> parse_stable_version(QString text) {
    text = text.trimmed();
    if (text.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        text.remove(0, 1);
    }

    static const QRegularExpression re(
        QStringLiteral(R"(^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$)"));
    const auto match = re.match(text);
    if (!match.hasMatch()) return std::nullopt;

    bool ok1 = false;
    bool ok2 = false;
    bool ok3 = false;
    const auto major = match.captured(1).toULongLong(&ok1);
    const auto minor = match.captured(2).toULongLong(&ok2);
    const auto patch = match.captured(3).toULongLong(&ok3);
    if (!ok1 || !ok2 || !ok3) return std::nullopt;
    return StableVersion{major, minor, patch};
}

int compare_versions(const StableVersion& a, const StableVersion& b) {
    if (a.major != b.major) return a.major > b.major ? 1 : -1;
    if (a.minor != b.minor) return a.minor > b.minor ? 1 : -1;
    if (a.patch != b.patch) return a.patch > b.patch ? 1 : -1;
    return 0;
}

bool allowed_download_url(const QUrl& url) {
    if (url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0) {
        return false;
    }

    const auto host = url.host().toLower();
    return host == QStringLiteral("github.com") ||
           host == QStringLiteral("objects.githubusercontent.com") ||
           host == QStringLiteral("release-assets.githubusercontent.com") ||
           host.endsWith(QStringLiteral(".githubusercontent.com"));
}

QByteArray sha256_file(const QString& path, QString* error_message = nullptr) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error_message) {
            *error_message =
                QStringLiteral("无法读取下载文件：%1").arg(path);
        }
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!f.atEnd()) {
        const auto block = f.read(1024 * 1024);
        if (block.isEmpty() && f.error() != QFile::NoError) {
            if (error_message) {
                *error_message =
                    QStringLiteral("读取下载文件失败：%1").arg(path);
            }
            return {};
        }
        hash.addData(block);
    }
    return hash.result().toHex();
}

QString checksum_from_text(const QByteArray& bytes) {
    const auto text = QString::fromUtf8(bytes);
    static const QRegularExpression re(
        QStringLiteral(R"(^\s*([0-9a-fA-F]{64})(?:\s+\*?([^\r\n]+))?\s*$)"),
        QRegularExpression::MultilineOption);
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        const auto file = m.captured(2).trimmed();
        if (file.isEmpty() || file == QString::fromLatin1(kArchiveName)) {
            return m.captured(1).toLower();
        }
    }
    return {};
}

QString asset_url(const QJsonArray& assets, const QString& name) {
    for (const auto& value : assets) {
        const auto object = value.toObject();
        if (object.value(QStringLiteral("name")).toString() == name) {
            return object.value(QStringLiteral("browser_download_url")).toString();
        }
    }
    return {};
}

QString find_release_root() {
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 8; ++i) {
        if (QFileInfo(
                dir.filePath(QStringLiteral(".monitor-hub-release.json")))
                .isFile()) {
            return dir.absolutePath();
        }
        if (!dir.cdUp()) break;
    }
    return {};
}

QString find_git_root() {
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 12; ++i) {
        if (QFileInfo(dir.filePath(QStringLiteral(".git"))).exists()) {
            return dir.absolutePath();
        }
        if (!dir.cdUp()) break;
    }
    return {};
}

bool run_safe_extract(
    const QString& install_root,
    const QString& archive,
    const QString& destination,
    QString* error_message) {
    const auto script =
        QDir(install_root)
            .filePath(QStringLiteral("tools/safe_extract_update.ps1"));
    if (!QFileInfo(script).isFile()) {
        if (error_message) {
            *error_message =
                QStringLiteral("更新解压工具缺失：%1").arg(script);
        }
        return false;
    }

    QProcess process;
    process.start(
        QStringLiteral("powershell.exe"),
        {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-NonInteractive"),
            QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"),
            QStringLiteral("-File"),
            script,
            QStringLiteral("-Archive"),
            archive,
            QStringLiteral("-Destination"),
            destination,
        });

    if (!process.waitForStarted(10000) ||
        !process.waitForFinished(120000) ||
        process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        if (error_message) {
            auto detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
            if (detail.isEmpty()) {
                detail = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
            }
            *error_message = detail.isEmpty()
                ? QStringLiteral("安全解压更新包失败。")
                : QStringLiteral("安全解压更新包失败：%1").arg(detail);
        }
        return false;
    }
    return true;
}

bool verify_extracted_stage(
    const QString& helper,
    const QString& stage,
    const QString& expected_version,
    QString* error_message) {
    QProcess process;
    process.start(
        helper,
        {QStringLiteral("--verify-stage"), stage});

    if (!process.waitForStarted(10000) ||
        !process.waitForFinished(120000) ||
        process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        if (error_message) {
            auto detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
            *error_message = detail.isEmpty()
                ? QStringLiteral("更新包 manifest 校验失败。")
                : QStringLiteral("更新包 manifest 校验失败：%1").arg(detail);
        }
        return false;
    }

    const auto verified_version =
        QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    if (verified_version != expected_version) {
        if (error_message) {
            *error_message =
                QStringLiteral("Release 版本与更新包 manifest 不一致：%1 != %2")
                    .arg(expected_version, verified_version);
        }
        return false;
    }
    return true;
}

}  // namespace

QtUpdateService::QtUpdateService(QObject* parent)
    : QObject(parent),
      network_(new QNetworkAccessManager(this)) {}

QString QtUpdateService::install_root() const {
    const auto release = find_release_root();
    if (!release.isEmpty()) return release;

    const auto git = find_git_root();
    if (!git.isEmpty()) return git;

    return QDir(QCoreApplication::applicationDirPath()).absolutePath();
}

QString QtUpdateService::install_mode() const {
    if (!find_git_root().isEmpty()) return QStringLiteral("development");
    if (!find_release_root().isEmpty()) return QStringLiteral("release");
    return QStringLiteral("unmanaged");
}

QString QtUpdateService::update_root() const {
    auto root =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty()) {
        root = QDir::tempPath() + QStringLiteral("/MonitorHub");
    }
    return QDir(root).filePath(QStringLiteral("updates"));
}

void QtUpdateService::download_bytes(
    const QUrl& url,
    std::function<void(QByteArray data, QString error)> callback) {
    if (!allowed_download_url(url)) {
        callback({}, QStringLiteral("拒绝非 GitHub HTTPS 下载地址。"));
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral("MonitorHub-Updater/2"));

    auto* reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback)] {
        const auto error = reply->error();
        const auto detail = reply->errorString();
        const auto data = reply->readAll();
        reply->deleteLater();

        if (error != QNetworkReply::NoError) {
            callback(
                {},
                QStringLiteral("下载失败：%1").arg(detail));
            return;
        }
        callback(data, {});
    });
}

void QtUpdateService::download_file(
    const QUrl& url,
    const QString& path,
    std::function<void(QString error)> callback) {
    if (!allowed_download_url(url)) {
        callback(QStringLiteral("拒绝非 GitHub HTTPS 下载地址。"));
        return;
    }

    QFileInfo info(path);
    QDir().mkpath(info.absolutePath());

    auto file = std::make_shared<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        callback(QStringLiteral("无法创建更新下载文件：%1").arg(path));
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral("MonitorHub-Updater/2"));

    auto* reply = network_->get(request);
    auto write_failed = std::make_shared<bool>(false);

    connect(reply, &QIODevice::readyRead, this, [reply, file, write_failed] {
        const auto data = reply->readAll();
        if (!data.isEmpty() && file->write(data) != data.size()) {
            *write_failed = true;
            reply->abort();
        }
    });

    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [reply, file, write_failed, callback = std::move(callback)] {
            const auto error = reply->error();
            const auto detail = reply->errorString();
            const auto final_data = reply->readAll();
            if (!final_data.isEmpty() &&
                file->write(final_data) != final_data.size()) {
                *write_failed = true;
            }
            file->close();
            reply->deleteLater();

            if (*write_failed) {
                QFile::remove(file->fileName());
                callback(QStringLiteral("写入更新下载文件失败。"));
                return;
            }
            if (error != QNetworkReply::NoError) {
                QFile::remove(file->fileName());
                callback(QStringLiteral("下载失败：%1").arg(detail));
                return;
            }
            callback({});
        });
}

void QtUpdateService::check_latest(CheckCallback callback) {
    const auto current_text = QCoreApplication::applicationVersion();
    const auto current = parse_stable_version(current_text);

    QNetworkRequest request(
        QUrl(QStringLiteral("https://api.github.com/repos/%1/releases/latest")
                 .arg(QString::fromLatin1(kRepo))));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral("MonitorHub-Updater/2"));

    auto* reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, current_text, current, callback = std::move(callback)] {
        QtUpdateCheck result;
        result.current_version = current_text;
        result.install_mode = install_mode();

        const auto network_error = reply->error();
        const auto network_detail = reply->errorString();
        const auto bytes = reply->readAll();
        reply->deleteLater();

        if (network_error != QNetworkReply::NoError) {
            callback(
                std::move(result),
                QStringLiteral("检查 GitHub Release 失败：%1")
                    .arg(network_detail));
            return;
        }

        QJsonParseError parse_error;
        const auto doc = QJsonDocument::fromJson(bytes, &parse_error);
        if (parse_error.error != QJsonParseError::NoError ||
            !doc.isObject()) {
            callback(
                std::move(result),
                QStringLiteral("GitHub Release 响应格式无效。"));
            return;
        }

        const auto object = doc.object();
        if (object.value(QStringLiteral("draft")).toBool() ||
            object.value(QStringLiteral("prerelease")).toBool()) {
            callback(
                std::move(result),
                QStringLiteral("latest Release 不是稳定正式版本。"));
            return;
        }

        const auto tag = object.value(QStringLiteral("tag_name")).toString();
        const auto latest = parse_stable_version(tag);
        if (!latest || !current) {
            callback(
                std::move(result),
                QStringLiteral("Release 或当前版本不是稳定 SemVer。"));
            return;
        }

        QtUpdateRelease release;
        release.tag = tag;
        release.version =
            tag.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)
                ? tag.mid(1)
                : tag;
        release.name =
            object.value(QStringLiteral("name")).toString(release.tag);
        release.notes =
            object.value(QStringLiteral("body")).toString();
        release.page_url =
            QUrl(object.value(QStringLiteral("html_url")).toString());

        const auto assets =
            object.value(QStringLiteral("assets")).toArray();
        release.archive_url =
            QUrl(asset_url(assets, QString::fromLatin1(kArchiveName)));
        release.checksum_url =
            QUrl(asset_url(assets, QString::fromLatin1(kChecksumName)));

        result.available = compare_versions(*latest, *current) > 0;
        result.release = release;

        if (result.available &&
            (!release.archive_url.isValid() ||
             !release.checksum_url.isValid())) {
            callback(
                std::move(result),
                QStringLiteral("新版本缺少 updater 所需 ZIP 或 SHA-256 asset。"));
            return;
        }

        callback(std::move(result), {});
    });
}

void QtUpdateService::stage_release(
    const QtUpdateRelease& release,
    StageCallback callback) {
    if (install_mode() != QStringLiteral("release")) {
        callback({}, QStringLiteral("当前不是受管 Release 安装，禁止自动覆盖。"));
        return;
    }

    const auto base = update_root();
    QDir().mkpath(base);

    const auto work =
        QDir(base).filePath(
            QStringLiteral(".tmp-%1-%2")
                .arg(QCoreApplication::applicationPid())
                .arg(QDateTime::currentMSecsSinceEpoch()));
    const auto archive = QDir(work).filePath(QString::fromLatin1(kArchiveName));
    const auto extracted = QDir(work).filePath(QStringLiteral("extracted"));
    QDir().mkpath(work);

    download_bytes(
        release.checksum_url,
        [this, release, callback = std::move(callback), work, archive, extracted](
            QByteArray checksum_bytes,
            QString checksum_error) mutable {
            if (!checksum_error.isEmpty()) {
                QDir(work).removeRecursively();
                callback({}, checksum_error);
                return;
            }

            const auto expected = checksum_from_text(checksum_bytes);
            if (expected.isEmpty()) {
                QDir(work).removeRecursively();
                callback({}, QStringLiteral("Release SHA-256 文件格式无效。"));
                return;
            }

            download_file(
                release.archive_url,
                archive,
                [this, release, callback = std::move(callback), work, archive, extracted, expected](
                    QString download_error) mutable {
                    if (!download_error.isEmpty()) {
                        QDir(work).removeRecursively();
                        callback({}, download_error);
                        return;
                    }

                    QString hash_error;
                    const auto actual =
                        QString::fromLatin1(
                            sha256_file(archive, &hash_error));
                    if (!hash_error.isEmpty() ||
                        actual.compare(expected, Qt::CaseInsensitive) != 0) {
                        QDir(work).removeRecursively();
                        callback(
                            {},
                            hash_error.isEmpty()
                                ? QStringLiteral("Release ZIP SHA-256 校验失败。")
                                : hash_error);
                        return;
                    }

                    QString extract_error;
                    const auto root = install_root();
                    if (!run_safe_extract(
                            root,
                            archive,
                            extracted,
                            &extract_error)) {
                        QDir(work).removeRecursively();
                        callback({}, extract_error);
                        return;
                    }

                    const auto helper =
                        QDir(QCoreApplication::applicationDirPath())
                            .filePath(
                                QStringLiteral("monitor_hub_updater.exe"));
                    QString verify_error;
                    if (!verify_extracted_stage(
                            helper,
                            extracted,
                            release.version,
                            &verify_error)) {
                        QDir(work).removeRecursively();
                        callback({}, verify_error);
                        return;
                    }

                    const auto target =
                        QDir(update_root())
                            .filePath(QStringLiteral("v%1").arg(release.version));
                    QDir(target).removeRecursively();
                    if (!QDir().rename(extracted, target)) {
                        QDir(work).removeRecursively();
                        callback({}, QStringLiteral("无法固定已验证的更新暂存目录。"));
                        return;
                    }

                    QDir(work).removeRecursively();
                    callback(target, {});
                });
        });
}

bool QtUpdateService::launch_apply(
    const QtUpdateRelease& release,
    const QString& stage_dir,
    QString* error_message) {
    if (install_mode() != QStringLiteral("release")) {
        if (error_message) {
            *error_message =
                QStringLiteral("当前不是受管 Release 安装，禁止自动覆盖。");
        }
        return false;
    }

    const auto installed_helper =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("monitor_hub_updater.exe"));
    if (!QFileInfo(installed_helper).isFile()) {
        if (error_message) {
            *error_message =
                QStringLiteral("更新 helper 缺失：%1").arg(installed_helper);
        }
        return false;
    }

    const auto helper_root =
        QDir(update_root()).filePath(QStringLiteral("helpers"));
    const auto helper_run_dir =
        QDir(helper_root)
            .filePath(
                QStringLiteral("run-%1-%2")
                    .arg(release.version)
                    .arg(QDateTime::currentMSecsSinceEpoch()));
    if (!QDir().mkpath(helper_run_dir)) {
        if (error_message) {
            *error_message =
                QStringLiteral("无法创建独立更新 helper 目录。");
        }
        return false;
    }

    const auto detached_helper =
        QDir(helper_run_dir)
            .filePath(QStringLiteral("monitor_hub_updater.exe"));
    if (!QFile::copy(installed_helper, detached_helper)) {
        if (error_message) {
            *error_message =
                QStringLiteral("无法复制独立更新 helper。");
        }
        QDir(helper_run_dir).removeRecursively();
        return false;
    }

    // The detached helper must not load Qt/MSVC runtime DLLs from the live
    // install tree, otherwise Windows will keep those managed files locked
    // while the helper tries to replace them.
    const QDir app_bin(QCoreApplication::applicationDirPath());
    const QStringList dependency_filters{
        QStringLiteral("Qt6Core*.dll"),
        QStringLiteral("msvcp*.dll"),
        QStringLiteral("vcruntime*.dll"),
        QStringLiteral("concrt*.dll"),
    };
    for (const auto& dep :
         app_bin.entryInfoList(dependency_filters, QDir::Files)) {
        const auto destination =
            QDir(helper_run_dir).filePath(dep.fileName());
        if (!QFile::copy(dep.absoluteFilePath(), destination)) {
            if (error_message) {
                *error_message =
                    QStringLiteral("无法复制更新 helper 运行库：%1")
                        .arg(dep.fileName());
            }
            QDir(helper_run_dir).removeRecursively();
            return false;
        }
    }

    if (!QFileInfo(
            QDir(helper_run_dir).filePath(QStringLiteral("Qt6Core.dll")))
            .isFile()) {
        if (error_message) {
            *error_message =
                QStringLiteral("独立更新 helper 缺少 Qt6Core.dll。");
        }
        QDir(helper_run_dir).removeRecursively();
        return false;
    }

    const auto root = install_root();
    const auto backup_root =
        QDir(update_root()).filePath(QStringLiteral("backups"));
    QDir().mkpath(backup_root);

    const auto restart_exe = QCoreApplication::applicationFilePath();
    const QStringList args{
        QStringLiteral("--pid"),
        QString::number(QCoreApplication::applicationPid()),
        QStringLiteral("--stage"),
        stage_dir,
        QStringLiteral("--install-root"),
        root,
        QStringLiteral("--backup-root"),
        backup_root,
        QStringLiteral("--restart-exe"),
        restart_exe,
        QStringLiteral("--restart-arg"),
        QStringLiteral("--background"),
    };

    if (!QProcess::startDetached(detached_helper, args, helper_run_dir)) {
        if (error_message) {
            *error_message =
                QStringLiteral("无法启动独立更新 helper。");
        }
        return false;
    }

    return true;
}

}  // namespace monitor_hub
