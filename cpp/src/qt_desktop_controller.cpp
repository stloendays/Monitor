#include "monitor_hub/qt_desktop_controller.hpp"

#include "monitor_hub/qt_main_window.hpp"
#include "monitor_hub/notification_outbox.hpp"
#include "monitor_hub/qt_app_logger.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeySequence>
#include <QNetworkInformation>
#include <QLabel>
#include <QLineEdit>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QShortcut>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDebug>

#include <vector>

namespace monitor_hub {
namespace {

QString q(const std::string& value) {
    return QString::fromUtf8(
        value.data(),
        static_cast<qsizetype>(value.size()));
}

QString qpath(const fs::path& path) {
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.wstring());
#else
    return q(path.string());
#endif
}

bool needs_attention(const std::string& health) {
    return health == "attention" || health == "error" || health == "stale";
}

QString concise_summary(const std::string& summary) {
    auto text = q(summary).simplified();
    constexpr qsizetype kMax = 220;
    if (text.size() > kMax) {
        text = text.left(kMax - 1) + QStringLiteral("…");
    }
    return text;
}

QString orchestrator_file_name() {
#ifdef Q_OS_WIN
    return QStringLiteral("monitor_hub_orchestrator.exe");
#else
    return QStringLiteral("monitor_hub_orchestrator");
#endif
}

}  // namespace

QtDesktopController::QtDesktopController(QtMainWindow& window, QObject* parent)
    : QObject(parent),
      window_(&window),
      settings_(load_desktop_settings()) {}

QtDesktopController::~QtDesktopController() {
    if (control_process_ &&
        control_process_->state() != QProcess::NotRunning) {
        control_process_->terminate();
        if (!control_process_->waitForFinished(300)) {
            control_process_->kill();
            control_process_->waitForFinished(300);
        }
    }
    if (instance_server_ && instance_server_->isListening()) {
        instance_server_->close();
    }
}

QString QtDesktopController::single_instance_server_name() const {
    // Scope the endpoint to this user's local application-data root so two
    // simultaneously logged-in users do not suppress each other's UI.
    const auto scope =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const auto digest =
        QCryptographicHash::hash(scope.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(16);
    return QStringLiteral("monitor-hub.qt.%1")
        .arg(QString::fromLatin1(digest.constData(), digest.size()));
}

bool QtDesktopController::notify_existing_instance() const {
    QLocalSocket peer;
    peer.connectToServer(single_instance_server_name(), QIODevice::WriteOnly);
    if (!peer.waitForConnected(200)) return false;

    peer.write("activate\n");
    peer.flush();
    peer.waitForBytesWritten(200);
    peer.disconnectFromServer();
    return true;
}

InstanceStatus QtDesktopController::establish_single_instance(QString* error_message) {
    if (notify_existing_instance()) return InstanceStatus::ActivatedExisting;

    // A crashed process can leave a stale local-server endpoint behind.
    QLocalServer::removeServer(single_instance_server_name());

    instance_server_ = new QLocalServer(this);
    if (!instance_server_->listen(single_instance_server_name())) {
        // Handle a startup race where another instance won between the first
        // probe and listen().
        if (notify_existing_instance()) return InstanceStatus::ActivatedExisting;
        if (error_message) *error_message = instance_server_->errorString();
        return InstanceStatus::Error;
    }

    connect(instance_server_, &QLocalServer::newConnection, this, [this] {
        while (instance_server_->hasPendingConnections()) {
            auto* client = instance_server_->nextPendingConnection();
            show_main_window();
            if (client) {
                client->readAll();
                client->write("ok\n");
                client->flush();
                client->disconnectFromServer();
                client->deleteLater();
            }
        }
    });
    return InstanceStatus::Primary;
}

bool QtDesktopController::install_system_tray(const QIcon& icon) {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QApplication::setQuitOnLastWindowClosed(true);
        return false;
    }

    tray_ = new QSystemTrayIcon(icon, this);
    tray_->setToolTip(QStringLiteral("Monitor Hub · 正在初始化"));

    auto* menu = new QMenu(window_);
    auto* open_action = menu->addAction(QStringLiteral("打开 Monitor Hub"));
    connect(open_action, &QAction::triggered, this, [this] { show_main_window(); });

    status_action_ = menu->addAction(QStringLiteral("状态初始化中"));
    status_action_->setEnabled(false);

    control_action_ = menu->addAction(QStringLiteral("自动处理：初始化中"));
    control_action_->setEnabled(false);

    menu->addSeparator();
    auto* settings_action = menu->addAction(QStringLiteral("设置…"));
    connect(settings_action, &QAction::triggered, this, [this] {
        show_settings_dialog();
    });

    auto* diagnostics_action = menu->addAction(QStringLiteral("复制诊断信息"));
    connect(diagnostics_action, &QAction::triggered, this, [this] {
        copy_diagnostics();
    });

    auto* logs_action = menu->addAction(QStringLiteral("打开日志目录"));
    connect(logs_action, &QAction::triggered, this, [this] {
        open_logs();
    });

    auto* about_action = menu->addAction(QStringLiteral("关于 Monitor Hub…"));
    connect(about_action, &QAction::triggered, this, [this] {
        show_about_dialog();
    });

    menu->addSeparator();
    auto* quit_action = menu->addAction(QStringLiteral("退出"));
    connect(quit_action, &QAction::triggered, this, [this] { request_quit(); });

    tray_->setContextMenu(menu);
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick) {
                    show_main_window();
                }
            });
    connect(tray_, &QSystemTrayIcon::messageClicked, this, [this] {
        show_main_window();
    });

    window_->installEventFilter(this);
    QApplication::setQuitOnLastWindowClosed(false);
    tray_->show();
    return true;
}

void QtDesktopController::start(bool background_requested) {
    bool previous_session_clean = true;
    QString session_error;
    if (!begin_desktop_session(&previous_session_clean, &session_error)) {
        qWarning().noquote()
            << "desktop session marker could not be written:" << session_error;
    } else if (!previous_session_clean) {
        qWarning() << "previous Monitor Hub desktop session ended uncleanly";
    }

    window_->restore_desktop_ui_state(load_desktop_ui_state());
    install_shortcuts();
    configure_network_monitoring();

    retry_timer_ = new QTimer(this);
    retry_timer_->setSingleShot(true);
    connect(retry_timer_, &QTimer::timeout, this, [this] {
        start_control_tick();
    });

    state_save_timer_ = new QTimer(this);
    state_save_timer_->setInterval(15 * 1000);
    connect(state_save_timer_, &QTimer::timeout, this, [this] {
        save_ui_state();
    });
    state_save_timer_->start();

    connect(
        QCoreApplication::instance(),
        &QCoreApplication::aboutToQuit,
        this,
        [this] {
            save_ui_state();
            QString error;
            if (!end_desktop_session(&error) && !error.isEmpty()) {
                qWarning().noquote()
                    << "clean shutdown marker could not be written:" << error;
            }
        });

    poll_project_notifications(true);
    poll_durable_notifications(false);

    control_timer_ = new QTimer(this);
    control_timer_->setInterval(60 * 1000);
    connect(control_timer_, &QTimer::timeout, this, [this] {
        start_control_tick();
    });
    apply_control_timer_state(true);

    if (tray_available()) {
        notification_timer_ = new QTimer(this);
        notification_timer_->setInterval(65 * 1000);
        connect(notification_timer_, &QTimer::timeout, this, [this] {
            poll_project_notifications(false);
            poll_durable_notifications(false);
        });
        notification_timer_->start();

        if (!previous_session_clean &&
            settings_.notifications &&
            QSystemTrayIcon::supportsMessages()) {
            tray_->showMessage(
                QStringLiteral("Monitor Hub · 已从异常退出恢复"),
                QStringLiteral(
                    "上一次桌面会话没有正常结束。Monitor Hub 已重新读取实时状态，"
                    "不会因界面崩溃而重复执行恢复动作。"),
                QSystemTrayIcon::Warning,
                6500);
        }
    }

    if (background_requested && tray_available()) {
        window_->hide();
        return;
    }
    show_main_window();
}

void QtDesktopController::show_main_window() {
    if (!window_) return;
    if (window_->isMinimized()) window_->showNormal();
    else window_->show();
    window_->raise();
    window_->activateWindow();
}

void QtDesktopController::request_quit() {
    force_quit_ = true;
    save_ui_state();
    if (notification_timer_) notification_timer_->stop();
    if (control_timer_) control_timer_->stop();
    if (retry_timer_) retry_timer_->stop();
    if (state_save_timer_) state_save_timer_->stop();
    if (control_process_ &&
        control_process_->state() != QProcess::NotRunning) {
        control_process_->terminate();
    }
    if (tray_) tray_->hide();
    qInfo() << "Monitor Hub desktop explicit quit";
    QApplication::quit();
}

bool QtDesktopController::tray_available() const noexcept {
    return tray_ && tray_->isVisible();
}

void QtDesktopController::show_background_notice_once() {
    if (!tray_ || background_notice_shown_) return;
    background_notice_shown_ = true;
    tray_->showMessage(
        QStringLiteral("Monitor Hub 仍在后台运行"),
        QStringLiteral(
            "监控不会因关闭主窗口而停止。可点击托盘图标重新打开；"
            "选择“退出”才会结束程序。"),
        QSystemTrayIcon::Information,
        5000);
}

void QtDesktopController::show_settings_dialog() {
    const auto current = load_desktop_settings();
    const bool development_checkout = running_from_development_checkout();

    QDialog dialog(window_);
    dialog.setWindowTitle(QStringLiteral("Monitor Hub 设置"));
    dialog.setModal(true);

    auto* layout = new QVBoxLayout(&dialog);

    auto* close_to_tray =
        new QCheckBox(QStringLiteral("关闭主窗口时继续在后台运行"), &dialog);
    close_to_tray->setChecked(current.close_to_tray);
    close_to_tray->setToolTip(
        QStringLiteral("关闭窗口后保留托盘图标和监控进程；显式“退出”仍会结束程序。"));
    layout->addWidget(close_to_tray);

    auto* notifications =
        new QCheckBox(QStringLiteral("显示重要桌面通知"), &dialog);
    notifications->setChecked(current.notifications);
    notifications->setToolTip(
        QStringLiteral(
            "仅通知需要处理、监控错误、状态过期、恢复处理中和项目完成等重要变化。"));
    layout->addWidget(notifications);

    auto* automatic_control =
        new QCheckBox(QStringLiteral("自动处理已授权的恢复任务（L1/L2）"), &dialog);
    automatic_control->setChecked(current.automatic_control);
    automatic_control->setToolTip(
        QStringLiteral(
            "每分钟运行一次全局控制面。只消费项目 recovery policy 已授权的命令；"
            "L3 决策仍必须交给你或主 Agent。"));
    layout->addWidget(automatic_control);

    auto* runtime_label = new QLabel(
        QStringLiteral(
            "数据位置（修改后下次启动生效）\n"
            "环境变量 MONITOR_HUB_REGISTRY / MONITOR_HUB_DATA 的优先级高于这里。"),
        &dialog);
    runtime_label->setWordWrap(true);
    layout->addWidget(runtime_label);

    auto* registry_row = new QHBoxLayout();
    auto* registry_path = new QLineEdit(&dialog);
    registry_path->setPlaceholderText(QStringLiteral("monitor_hub_projects.json 路径"));
    registry_path->setText(
        current.registry_path.isEmpty()
            ? q(window_->runtime_paths().registry.string())
            : current.registry_path);
    auto* choose_registry = new QPushButton(QStringLiteral("选择登记表…"), &dialog);
    registry_row->addWidget(registry_path, 1);
    registry_row->addWidget(choose_registry);
    layout->addLayout(registry_row);
    connect(choose_registry, &QPushButton::clicked, &dialog, [&dialog, registry_path] {
        const auto chosen = QFileDialog::getOpenFileName(
            &dialog,
            QStringLiteral("选择 monitor_hub_projects.json"),
            QFileInfo(registry_path->text()).absolutePath(),
            QStringLiteral("JSON (*.json);;All files (*)"));
        if (!chosen.isEmpty()) registry_path->setText(chosen);
    });

    auto* hub_data_row = new QHBoxLayout();
    auto* hub_data_path = new QLineEdit(&dialog);
    hub_data_path->setPlaceholderText(QStringLiteral("%LOCALAPPDATA%\\Monitor Hub"));
    hub_data_path->setText(
        current.hub_data_path.isEmpty()
            ? q(window_->runtime_paths().hub_data.string())
            : current.hub_data_path);
    auto* choose_hub_data = new QPushButton(QStringLiteral("选择 Hub 数据目录…"), &dialog);
    hub_data_row->addWidget(hub_data_path, 1);
    hub_data_row->addWidget(choose_hub_data);
    layout->addLayout(hub_data_row);
    connect(choose_hub_data, &QPushButton::clicked, &dialog, [&dialog, hub_data_path] {
        const auto chosen = QFileDialog::getExistingDirectory(
            &dialog,
            QStringLiteral("选择 Monitor Hub 数据目录"),
            hub_data_path->text());
        if (!chosen.isEmpty()) hub_data_path->setText(chosen);
    });

    auto* launch_at_login =
        new QCheckBox(QStringLiteral("登录 Windows 时自动启动 Monitor Hub"), &dialog);
    launch_at_login->setChecked(current.launch_at_login);
    launch_at_login->setToolTip(
        QStringLiteral("登录后以 --background 模式启动当前已安装的 Monitor Hub。"));
    layout->addWidget(launch_at_login);

    if (development_checkout && !current.launch_at_login) {
        launch_at_login->setEnabled(false);
        launch_at_login->setToolTip(
            QStringLiteral(
                "当前程序位于 Git 开发工作区。为避免注册临时 build 路径，"
                "开发版禁止新增开机启动；正式安装版可启用。"));
        auto* note = new QLabel(
            QStringLiteral(
                "当前为开发工作区：开机启动不会指向临时 build 目录。"
                "安装正式 Release 后即可启用。"),
            &dialog);
        note->setWordWrap(true);
        layout->addWidget(note);
    }

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) return;

    QString error;
    const bool automatic_control_changed =
        automatic_control->isChecked() != current.automatic_control;

    if (!save_desktop_preferences(
            close_to_tray->isChecked(),
            notifications->isChecked(),
            automatic_control->isChecked(),
            &error)) {
        QMessageBox::warning(
            window_,
            QStringLiteral("Monitor Hub"),
            QStringLiteral("保存桌面设置失败：%1").arg(error));
    }

    QString runtime_error;
    if (!save_runtime_locations(
            registry_path->text(),
            hub_data_path->text(),
            &runtime_error)) {
        QMessageBox::warning(
            window_,
            QStringLiteral("Monitor Hub"),
            QStringLiteral("保存数据位置失败：%1").arg(runtime_error));
    }

    if (launch_at_login->isEnabled() &&
        launch_at_login->isChecked() != current.launch_at_login) {
        QString startup_error;
        if (!set_launch_at_login(
                launch_at_login->isChecked(),
                &startup_error)) {
            QMessageBox::warning(
                window_,
                QStringLiteral("Monitor Hub"),
                QStringLiteral("更新开机启动失败：%1").arg(startup_error));
        }
    }

    settings_ = load_desktop_settings();
    apply_control_timer_state(
        automatic_control_changed && settings_.automatic_control);
    if (settings_.notifications) poll_durable_notifications(false);
}

void QtDesktopController::show_about_dialog() {
    QMessageBox dialog(window_);
    dialog.setWindowTitle(QStringLiteral("关于 Monitor Hub"));
    dialog.setIconPixmap(
        QApplication::windowIcon().pixmap(64, 64));
    dialog.setText(
        QStringLiteral("<b>Monitor Hub %1</b><br/>Agent Operations Console")
            .arg(QCoreApplication::applicationVersion()));
    dialog.setInformativeText(
        QStringLiteral(
            "面向长任务与 Agent 工作流的本地监控、恢复与审计控制台。\n\n"
            "Qt %1\n"
            "快捷键：F5 刷新 · Ctrl+N 新建监控 · Ctrl+K 项目列表 · "
            "Ctrl+, 设置 · Ctrl+Shift+L 日志 · F1 关于 · Ctrl+Q 退出")
            .arg(QString::fromLatin1(qVersion())));
    dialog.setStandardButtons(QMessageBox::Ok);
    dialog.exec();
}

void QtDesktopController::open_logs() {
    QString error;
    if (open_desktop_log_directory(&error)) return;
    QMessageBox::warning(
        window_,
        QStringLiteral("Monitor Hub"),
        error.isEmpty()
            ? QStringLiteral("无法打开日志目录。")
            : error);
}

void QtDesktopController::install_shortcuts() {
    auto* settings_shortcut =
        new QShortcut(QKeySequence(QStringLiteral("Ctrl+,")), window_);
    connect(settings_shortcut, &QShortcut::activated, this, [this] {
        show_settings_dialog();
    });

    auto* logs_shortcut =
        new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+L")), window_);
    connect(logs_shortcut, &QShortcut::activated, this, [this] {
        open_logs();
    });

    auto* about_shortcut =
        new QShortcut(QKeySequence(QStringLiteral("F1")), window_);
    connect(about_shortcut, &QShortcut::activated, this, [this] {
        show_about_dialog();
    });

    auto* quit_shortcut =
        new QShortcut(QKeySequence::Quit, window_);
    connect(quit_shortcut, &QShortcut::activated, this, [this] {
        request_quit();
    });
}

void QtDesktopController::save_ui_state() {
    if (!window_) return;
    QString error;
    if (!save_desktop_ui_state(window_->desktop_ui_state(), &error) &&
        !error.isEmpty()) {
        qWarning().noquote()
            << "desktop UI state could not be persisted:" << error;
    }
}

void QtDesktopController::configure_network_monitoring() {
    if (!QNetworkInformation::loadDefaultBackend()) {
        qInfo() << "Qt network reachability backend unavailable;"
                   " control requests will rely on normal operation errors";
        return;
    }

    auto* network = QNetworkInformation::instance();
    if (!network) return;

    connect(
        network,
        &QNetworkInformation::reachabilityChanged,
        this,
        [this](QNetworkInformation::Reachability reachability) {
            update_network_state(
                reachability == QNetworkInformation::Reachability::Disconnected);
        });

    update_network_state(
        network->reachability() ==
        QNetworkInformation::Reachability::Disconnected);
}

void QtDesktopController::update_network_state(bool disconnected) {
    if (network_disconnected_ == disconnected) return;
    network_disconnected_ = disconnected;

    if (disconnected) {
        if (retry_timer_) retry_timer_->stop();
        set_control_status(QStringLiteral("自动处理：网络离线（本地继续）"));
        qWarning() << "network reachability changed to disconnected";
        if (settings_.notifications &&
            tray_available() &&
            QSystemTrayIcon::supportsMessages()) {
            tray_->showMessage(
                QStringLiteral("Monitor Hub · 网络连接中断"),
                QStringLiteral(
                    "联网相关动作失败时会退避重试；本地状态读取和已授权的确定性控制仍继续。"),
                QSystemTrayIcon::Warning,
                5500);
        }
        return;
    }

    qInfo() << "network reachability recovered";
    control_retry_attempt_ = 0;
    control_failure_active_ = false;
    if (settings_.automatic_control) {
        set_control_status(QStringLiteral("自动处理：网络已恢复，正在重新检查"));
        QTimer::singleShot(1000, this, [this] {
            if (window_) window_->trigger_refresh();
            start_control_tick();
        });
    }
    if (settings_.notifications &&
        tray_available() &&
        QSystemTrayIcon::supportsMessages()) {
        tray_->showMessage(
            QStringLiteral("Monitor Hub · 网络已恢复"),
            QStringLiteral("已恢复联网检查，并重新读取当前项目状态。"),
            QSystemTrayIcon::Information,
            4000);
    }
}

void QtDesktopController::schedule_control_retry() {
    if (!retry_timer_ ||
        retry_timer_->isActive() ||
        force_quit_ ||
        network_disconnected_ ||
        !settings_.automatic_control) {
        return;
    }

    constexpr int delays[] = {5, 15, 30, 60, 120};
    const int index =
        control_retry_attempt_ < 5 ? control_retry_attempt_ : 4;
    const int seconds = delays[index];
    if (control_retry_attempt_ < 5) ++control_retry_attempt_;
    retry_timer_->start(seconds * 1000);
    qInfo() << "scheduled control retry in" << seconds << "seconds";
}

void QtDesktopController::copy_diagnostics() {
    auto diagnostics = desktop_diagnostics_text();
    diagnostics += QStringLiteral("\ntray_available=%1")
                       .arg(tray_available() ? QStringLiteral("true")
                                             : QStringLiteral("false"));
    diagnostics += QStringLiteral("\ncontrol_tick_running=%1")
                       .arg(control_process_ ? QStringLiteral("true")
                                             : QStringLiteral("false"));
    diagnostics += QStringLiteral("\norchestrator=%1")
                       .arg(QDir::toNativeSeparators(orchestrator_program()));
    diagnostics += QStringLiteral("\nlog_file=%1")
                       .arg(QDir::toNativeSeparators(desktop_log_file()));
    diagnostics += QStringLiteral("\nnetwork_disconnected=%1")
                       .arg(network_disconnected_
                                ? QStringLiteral("true")
                                : QStringLiteral("false"));

    QApplication::clipboard()->setText(diagnostics);

    if (tray_available()) {
        tray_->showMessage(
            QStringLiteral("Monitor Hub"),
            QStringLiteral("诊断信息已复制到剪贴板。"),
            QSystemTrayIcon::Information,
            3000);
    } else {
        QMessageBox::information(
            window_,
            QStringLiteral("Monitor Hub"),
            QStringLiteral("诊断信息已复制到剪贴板。"));
    }
}

QString QtDesktopController::orchestrator_program() const {
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(orchestrator_file_name());
}

void QtDesktopController::set_control_status(const QString& text) {
    if (control_action_) control_action_->setText(text);
}

void QtDesktopController::report_control_failure(const QString& detail) {
    set_control_status(QStringLiteral("自动处理：异常"));
    if (control_failure_active_) return;
    control_failure_active_ = true;

    if (settings_.notifications &&
        tray_available() &&
        QSystemTrayIcon::supportsMessages()) {
        tray_->showMessage(
            QStringLiteral("Monitor Hub · 自动处理异常"),
            detail,
            QSystemTrayIcon::Warning,
            7000);
    }
}

void QtDesktopController::apply_control_timer_state(bool run_immediately) {
    if (!control_timer_) return;

    if (!settings_.automatic_control) {
        control_timer_->stop();
        set_control_status(
            control_process_
                ? QStringLiteral("自动处理：已暂停（当前轮完成后）")
                : QStringLiteral("自动处理：已暂停"));
        return;
    }

    if (!control_timer_->isActive()) control_timer_->start();
    if (!control_process_)
        set_control_status(QStringLiteral("自动处理：等待下一轮"));

    if (run_immediately) {
        QTimer::singleShot(1200, this, [this] {
            start_control_tick();
        });
    }
}

void QtDesktopController::start_control_tick() {
    if (force_quit_ ||
        !settings_.automatic_control ||
        control_process_) {
        return;
    }

    const auto program = orchestrator_program();
    const QFileInfo executable(program);
    if (!executable.exists() || !executable.isFile()) {
        report_control_failure(
            QStringLiteral(
                "找不到控制面程序：%1。安装版应包含 monitor_hub_orchestrator；"
                "开发版请构建 desktop targets。")
                .arg(QDir::toNativeSeparators(program)));
        return;
    }

    auto* process = new QProcess(this);
    control_process_ = process;

    const auto& paths = window_->runtime_paths();
    QStringList arguments;
    arguments
        << QStringLiteral("--tick")
        << QStringLiteral("--hub-data")
        << qpath(paths.hub_data)
        << QStringLiteral("--registry")
        << qpath(paths.registry)
        << QStringLiteral("--job-root")
        << qpath(paths.job_root);

    process->setProgram(program);
    process->setArguments(arguments);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    set_control_status(QStringLiteral("自动处理：处理中…"));

    connect(
        process,
        &QProcess::errorOccurred,
        this,
        [this, process, program](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart ||
                process != control_process_) {
                return;
            }

            const auto detail =
                QStringLiteral("无法启动 %1：%2")
                    .arg(
                        QDir::toNativeSeparators(program),
                        process->errorString());
            control_process_ = nullptr;
            report_control_failure(detail);
            schedule_control_retry();
            process->deleteLater();
        });

    connect(
        process,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this,
        [this, process](
            int exit_code,
            QProcess::ExitStatus exit_status) {
            if (process != control_process_) {
                process->deleteLater();
                return;
            }

            const auto stdout_text =
                QString::fromUtf8(
                    process->readAllStandardOutput()).trimmed();
            const auto stderr_text =
                QString::fromUtf8(
                    process->readAllStandardError()).trimmed();
            control_process_ = nullptr;

            if (exit_status != QProcess::NormalExit ||
                exit_code != 0) {
                report_control_failure(
                    QStringLiteral(
                        "控制面本轮失败（exit=%1）：%2")
                        .arg(exit_code)
                        .arg(
                            stderr_text.isEmpty()
                                ? stdout_text.right(1200)
                                : stderr_text.right(1200)));
                schedule_control_retry();
                process->deleteLater();
                return;
            }

            QJsonParseError parse_error;
            const auto document =
                QJsonDocument::fromJson(
                    stdout_text.toUtf8(),
                    &parse_error);
            if (parse_error.error !=
                    QJsonParseError::NoError ||
                !document.isObject()) {
                report_control_failure(
                    QStringLiteral(
                        "控制面返回了无效 JSON：%1")
                        .arg(parse_error.errorString()));
                schedule_control_retry();
                process->deleteLater();
                return;
            }

            const auto root = document.object();
            const bool needs_main_agent =
                root.value(QStringLiteral("needs_main_agent"))
                    .toBool(false);
            const auto outbox =
                root.value(QStringLiteral("outbox")).toObject();
            const int pending =
                outbox.value(QStringLiteral("pending_count"))
                    .toInt(0);

            control_failure_active_ = false;
            control_retry_attempt_ = 0;
            if (retry_timer_) retry_timer_->stop();
            if (needs_main_agent) {
                set_control_status(
                    settings_.automatic_control
                        ? QStringLiteral("自动处理：等待决策 %1")
                              .arg(pending)
                        : QStringLiteral("自动处理：已暂停 · 待决策 %1")
                              .arg(pending));
                if (!previous_needs_main_agent_ &&
                    settings_.notifications &&
                    tray_available() &&
                    QSystemTrayIcon::supportsMessages()) {
                    tray_->showMessage(
                        QStringLiteral(
                            "Monitor Hub · 需要你/主 Agent 处理"),
                        QStringLiteral(
                            "自动恢复控制面有 %1 个待确认事项。"
                            "打开 Monitor Hub 可查看对应 Issue。")
                            .arg(pending),
                        QSystemTrayIcon::Warning,
                        7000);
                }
            } else {
                set_control_status(
                    settings_.automatic_control
                        ? QStringLiteral("自动处理：运行正常")
                        : QStringLiteral("自动处理：已暂停"));
            }
            previous_needs_main_agent_ = needs_main_agent;

            process->deleteLater();
        });

    process->start();
}

void QtDesktopController::poll_durable_notifications(bool baseline_only) {
    if (!window_) return;

    const auto outbox = sync_notification_outbox(window_->runtime_paths());
    std::vector<const NotificationRecord*> fresh;
    for (const auto& item : outbox.items) {
        if (item.state != "pending") continue;
        if (!item.target.empty() && item.target != "main_agent") continue;
        if (notified_outbox_ids_.count(item.notification_id)) continue;
        fresh.push_back(&item);
    }

    if (baseline_only) {
        for (const auto* item : fresh)
            notified_outbox_ids_.insert(item->notification_id);
        return;
    }

    if (!settings_.notifications ||
        !tray_available() ||
        !QSystemTrayIcon::supportsMessages()) {
        return;
    }

    const std::size_t limit = fresh.size() < 3 ? fresh.size() : 3;
    for (std::size_t index = 0; index < limit; ++index) {
        const auto& item = *fresh[index];
        QString title = QStringLiteral("Monitor Hub · 待处理通知");
        if (item.reason == "decision_required")
            title = QStringLiteral("Monitor Hub · 需要决策");
        else if (item.reason == "project_completed")
            title = QStringLiteral("Monitor Hub · 项目已完成");

        auto body = concise_summary(item.summary);
        if (body.isEmpty()) {
            body = item.project_id.empty()
                ? QStringLiteral("有新的持久化通知等待处理。")
                : QStringLiteral("项目 %1 有新的持久化通知。")
                      .arg(q(item.project_id));
        }
        tray_->showMessage(
            title,
            body,
            item.reason == "decision_required"
                ? QSystemTrayIcon::Warning
                : QSystemTrayIcon::Information,
            6500);
        notified_outbox_ids_.insert(item.notification_id);
    }

    if (fresh.size() > limit) {
        tray_->showMessage(
            QStringLiteral("Monitor Hub · 还有待处理通知"),
            QStringLiteral("另有 %1 条持久化通知，可在 Monitor Hub 中查看。")
                .arg(static_cast<qulonglong>(fresh.size() - limit)),
            QSystemTrayIcon::Information,
            5000);
        for (std::size_t index = limit; index < fresh.size(); ++index)
            notified_outbox_ids_.insert(fresh[index]->notification_id);
    }

    for (const auto& diagnostic : outbox.diagnostics) {
        qWarning().noquote() << "notification outbox:" << q(diagnostic);
    }
}

void QtDesktopController::poll_project_notifications(bool baseline_only) {
    if (!window_) return;

    const auto states = window_->desktop_project_states();
    std::map<std::string, ProjectStateMemory> next;

    int attention_count = 0;
    int working_count = 0;
    int done_count = 0;

    for (const auto& state : states) {
        const auto key = state.id.empty() ? state.name : state.id;
        next[key] = {state.health, state.summary};

        if (needs_attention(state.health)) ++attention_count;
        else if (state.health == "working") ++working_count;
        else if (state.health == "done") ++done_count;

        if (baseline_only || !project_state_baselined_ ||
            !settings_.notifications || !tray_available() ||
            !QSystemTrayIcon::supportsMessages()) {
            continue;
        }

        const auto old_it = previous_project_states_.find(key);
        const bool has_old = old_it != previous_project_states_.end();
        const auto old_health =
            has_old ? old_it->second.health : std::string{};
        const auto old_summary =
            has_old ? old_it->second.summary : std::string{};

        const bool health_changed = old_health != state.health;
        const bool important_summary_changed =
            needs_attention(state.health) &&
            old_summary != state.summary;

        if (!health_changed && !important_summary_changed) continue;

        const auto project_name =
            state.name.empty() ? QStringLiteral("项目") : q(state.name);
        const auto summary = concise_summary(state.summary);
        const auto body = summary.isEmpty()
            ? project_name
            : project_name + QStringLiteral("：") + summary;

        if (state.health == "error") {
            tray_->showMessage(
                QStringLiteral("Monitor Hub · 监控出错"),
                body,
                QSystemTrayIcon::Critical,
                7000);
        } else if (state.health == "attention") {
            tray_->showMessage(
                QStringLiteral("Monitor Hub · 需要处理"),
                body,
                QSystemTrayIcon::Warning,
                7000);
        } else if (state.health == "stale") {
            tray_->showMessage(
                QStringLiteral("Monitor Hub · 状态过期"),
                body,
                QSystemTrayIcon::Warning,
                7000);
        } else if (state.health == "done" &&
                   (!has_old || old_health != "done")) {
            tray_->showMessage(
                QStringLiteral("Monitor Hub · 项目已完成"),
                body,
                QSystemTrayIcon::Information,
                6000);
        } else if (state.health == "working" &&
                   has_old &&
                   needs_attention(old_health)) {
            tray_->showMessage(
                QStringLiteral("Monitor Hub · 已进入后台处理"),
                body,
                QSystemTrayIcon::Information,
                5000);
        } else if (state.health == "ok" &&
                   has_old &&
                   needs_attention(old_health)) {
            tray_->showMessage(
                QStringLiteral("Monitor Hub · 状态已恢复"),
                body,
                QSystemTrayIcon::Information,
                5000);
        }
    }

    previous_project_states_ = std::move(next);
    project_state_baselined_ = true;

    if (!tray_available()) return;

    QString status;
    if (attention_count > 0) {
        status = QStringLiteral("需要处理：%1 · 后台处理中：%2")
                     .arg(attention_count)
                     .arg(working_count);
    } else if (working_count > 0) {
        status = QStringLiteral("运行正常 · 后台处理中：%1")
                     .arg(working_count);
    } else {
        status = QStringLiteral("运行正常");
    }

    if (done_count > 0) {
        status += QStringLiteral(" · 已完成：%1").arg(done_count);
    }

    if (status_action_) status_action_->setText(status);
    tray_->setToolTip(QStringLiteral("Monitor Hub · %1").arg(status));
}

bool QtDesktopController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == window_ &&
        event &&
        event->type() == QEvent::Close &&
        !force_quit_) {
        auto* close_event = static_cast<QCloseEvent*>(event);

        if (tray_available() && settings_.close_to_tray) {
            close_event->ignore();
            window_->hide();
            show_background_notice_once();
            return true;
        }

        close_event->accept();
        request_quit();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

}  // namespace monitor_hub
