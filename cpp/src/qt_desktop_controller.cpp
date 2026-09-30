#include "monitor_hub/qt_desktop_controller.hpp"

#include "monitor_hub/qt_main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

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
    poll_project_notifications(true);

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
        });
        notification_timer_->start();
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
    if (notification_timer_) notification_timer_->stop();
    if (control_timer_) control_timer_->stop();
    if (control_process_ &&
        control_process_->state() != QProcess::NotRunning) {
        control_process_->terminate();
    }
    if (tray_) tray_->hide();
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
