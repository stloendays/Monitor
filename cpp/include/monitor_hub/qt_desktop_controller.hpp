#pragma once

#include "monitor_hub/qt_desktop_settings.hpp"
#include "monitor_hub/qt_update_service.hpp"

#include <QIcon>
#include <QObject>
#include <QString>

#include <map>
#include <optional>
#include <string>

class QAction;
class QEvent;
class QLocalServer;
class QSystemTrayIcon;
class QTimer;

namespace monitor_hub {

class QtMainWindow;

enum class InstanceStatus {
    Primary,
    ActivatedExisting,
    Error,
};

class QtDesktopController final : public QObject {
public:
    explicit QtDesktopController(QtMainWindow& window, QObject* parent = nullptr);
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
    struct ProjectStateMemory {
        std::string health;
        std::string summary;
    };

    QString single_instance_server_name() const;
    bool notify_existing_instance() const;
    void show_background_notice_once();
    void show_settings_dialog();
    void copy_diagnostics();
    void poll_project_notifications(bool baseline_only = false);

    void check_for_updates(bool interactive);
    void begin_update_install(const QtUpdateRelease& release);
    void open_pending_release_page();

    QtMainWindow* window_ = nullptr;
    QLocalServer* instance_server_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    QAction* status_action_ = nullptr;
    QAction* update_action_ = nullptr;
    QTimer* notification_timer_ = nullptr;
    QTimer* update_timer_ = nullptr;
    QtUpdateService* updates_ = nullptr;

    DesktopSettings settings_;
    std::map<std::string, ProjectStateMemory> previous_project_states_;
    std::optional<QtUpdateRelease> pending_update_;
    bool project_state_baselined_ = false;
    bool update_busy_ = false;
    bool force_quit_ = false;
    bool background_notice_shown_ = false;
};

}  // namespace monitor_hub
