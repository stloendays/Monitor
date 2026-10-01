#pragma once

#include "monitor_hub/qt_desktop_settings.hpp"

#include <QIcon>
#include <QObject>
#include <QString>

#include <map>
#include <set>
#include <string>

class QAction;
class QEvent;
class QLocalServer;
class QProcess;
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
    void show_about_dialog();
    void copy_diagnostics();
    void open_logs();
    void install_shortcuts();
    void save_ui_state();
    void configure_network_monitoring();
    void update_network_state(bool disconnected);
    void schedule_control_retry();
    void poll_project_notifications(bool baseline_only = false);
    void poll_durable_notifications(bool baseline_only = false);
    void apply_control_timer_state(bool run_immediately = false);
    void start_control_tick();
    void set_control_status(const QString& text);
    void report_control_failure(const QString& detail);
    QString orchestrator_program() const;

    QtMainWindow* window_ = nullptr;
    QLocalServer* instance_server_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    QAction* status_action_ = nullptr;
    QAction* control_action_ = nullptr;
    QTimer* notification_timer_ = nullptr;
    QTimer* control_timer_ = nullptr;
    QTimer* retry_timer_ = nullptr;
    QTimer* state_save_timer_ = nullptr;
    QProcess* control_process_ = nullptr;

    DesktopSettings settings_;
    std::map<std::string, ProjectStateMemory> previous_project_states_;
    std::set<std::string> notified_outbox_ids_;
    bool project_state_baselined_ = false;
    bool force_quit_ = false;
    bool background_notice_shown_ = false;
    bool control_failure_active_ = false;
    bool previous_needs_main_agent_ = false;
    bool network_disconnected_ = false;
    int control_retry_attempt_ = 0;
};

}  // namespace monitor_hub
