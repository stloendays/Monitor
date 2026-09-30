#pragma once

#include <QObject>
#include <QIcon>
#include <QString>

class QEvent;
class QLocalServer;
class QMainWindow;
class QSystemTrayIcon;

namespace monitor_hub {

enum class InstanceStatus {
    Primary,
    ActivatedExisting,
    Error,
};

class QtDesktopController final : public QObject {
public:
    explicit QtDesktopController(QMainWindow& window, QObject* parent = nullptr);
    ~QtDesktopController() override;

    InstanceStatus establish_single_instance(QString* error_message = nullptr);
    bool install_system_tray(const QIcon& icon);
    void start(bool background_requested);
    void show_main_window();
    void request_quit();

    bool tray_available() const noexcept;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QString single_instance_server_name() const;
    bool notify_existing_instance() const;
    void show_background_notice_once();

    QMainWindow* window_ = nullptr;
    QLocalServer* instance_server_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    bool force_quit_ = false;
    bool background_notice_shown_ = false;
};

}  // namespace monitor_hub
