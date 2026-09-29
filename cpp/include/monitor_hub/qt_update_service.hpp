#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>
#include <optional>

class QNetworkAccessManager;

namespace monitor_hub {

struct QtUpdateRelease {
    QString version;
    QString tag;
    QString name;
    QString notes;
    QUrl page_url;
    QUrl archive_url;
    QUrl checksum_url;
};

struct QtUpdateCheck {
    QString current_version;
    QString install_mode;
    std::optional<QtUpdateRelease> release;
    bool available = false;
};

class QtUpdateService final : public QObject {
public:
    using CheckCallback =
        std::function<void(QtUpdateCheck result, QString error_message)>;
    using StageCallback =
        std::function<void(QString stage_dir, QString error_message)>;

    explicit QtUpdateService(QObject* parent = nullptr);

    void check_latest(CheckCallback callback);
    void stage_release(const QtUpdateRelease& release, StageCallback callback);

    bool launch_apply(
        const QtUpdateRelease& release,
        const QString& stage_dir,
        QString* error_message = nullptr);

    QString install_mode() const;
    QString install_root() const;
    QString update_root() const;

private:
    void download_bytes(
        const QUrl& url,
        std::function<void(QByteArray data, QString error)> callback);
    void download_file(
        const QUrl& url,
        const QString& path,
        std::function<void(QString error)> callback);

    QNetworkAccessManager* network_ = nullptr;
};

}  // namespace monitor_hub
