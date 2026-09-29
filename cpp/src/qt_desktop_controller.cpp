#include "monitor_hub/qt_desktop_controller.hpp"

#include "monitor_hub/qt_main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QEvent>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMessageBox>
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

}  // namespace

QtDesktopController::QtDesktopController(QtMainWindow& window, QObject* parent)
    : QObject(parent),
      window_(&window),
      updates_(new QtUpdateService(this)),
      settings_(load_desktop_settings()) {}

QtDesktopController::~QtDesktopController() {
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

    menu->addSeparator();
    update_action_ = menu->addAction(QStringLiteral("检查更新…"));
    connect(update_action_, &QAction::triggered, this, [this] {
        if (pending_update_ &&
            updates_->install_mode() == QStringLiteral("release")) {
            begin_update_install(*pending_update_);
        } else if (pending_update_) {
            open_pending_release_page();
        } else {
            check_for_updates(true);
        }
    });

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

    if (tray_available()) {
        notification_timer_ = new QTimer(this);
        notification_timer_->setInterval(65 * 1000);
        connect(notification_timer_, &QTimer::timeout, this, [this] {
            poll_project_notifications(false);
        });
        notification_timer_->start();

        update_timer_ = new QTimer(this);
        update_timer_->setInterval(6 * 60 * 60 * 1000);
        connect(update_timer_, &QTimer::timeout, this, [this] {
            check_for_updates(false);
        });
        update_timer_->start();

        QTimer::singleShot(7000, this, [this] {
            check_for_updates(false);
        });
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
    if (update_timer_) update_timer_->stop();
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

    auto* check_updates =
        new QCheckBox(QStringLiteral("自动检查稳定更新"), &dialog);
    check_updates->setChecked(current.check_updates);
    check_updates->setToolTip(
        QStringLiteral(
            "启动后及约每 6 小时检查一次 GitHub stable Release；"
            "不会自动覆盖 Git 开发工作区。"));
    layout->addWidget(check_updates);

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
    if (!save_desktop_preferences(
            close_to_tray->isChecked(),
            notifications->isChecked(),
            check_updates->isChecked(),
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
}

void QtDesktopController::copy_diagnostics() {
    auto diagnostics = desktop_diagnostics_text();
    diagnostics += QStringLiteral("\ntray_available=%1")
                       .arg(tray_available() ? QStringLiteral("true")
                                             : QStringLiteral("false"));
    diagnostics += QStringLiteral("\nupdate_install_mode=%1")
                       .arg(updates_->install_mode());
    diagnostics += QStringLiteral("\nupdate_install_root=%1")
                       .arg(updates_->install_root());

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


void QtDesktopController::open_pending_release_page() {
    if (!pending_update_ || !pending_update_->page_url.isValid()) {
        check_for_updates(true);
        return;
    }
    QDesktopServices::openUrl(pending_update_->page_url);
}

void QtDesktopController::check_for_updates(bool interactive) {
    if (!interactive && !settings_.check_updates) return;

    if (!updates_ || update_busy_) {
        if (interactive && update_busy_) {
            QMessageBox::information(
                window_,
                QStringLiteral("Monitor Hub"),
                QStringLiteral("更新操作正在进行中。"));
        }
        return;
    }

    if (update_action_) {
        update_action_->setText(QStringLiteral("正在检查更新…"));
        update_action_->setEnabled(false);
    }

    updates_->check_latest(
        [this, interactive](QtUpdateCheck result, QString error) {
            if (update_action_) {
                update_action_->setEnabled(true);
                update_action_->setText(QStringLiteral("检查更新…"));
            }

            if (!error.isEmpty()) {
                if (interactive) {
                    QMessageBox::warning(
                        window_,
                        QStringLiteral("Monitor Hub"),
                        error);
                }
                return;
            }

            if (!result.release || !result.available) {
                pending_update_.reset();
                if (interactive) {
                    QMessageBox::information(
                        window_,
                        QStringLiteral("Monitor Hub"),
                        QStringLiteral("当前已是最新稳定版本（%1）。")
                            .arg(result.current_version));
                }
                return;
            }

            pending_update_ = result.release;
            const auto& release = *pending_update_;

            if (result.install_mode == QStringLiteral("release")) {
                if (update_action_) {
                    update_action_->setText(
                        QStringLiteral("安装更新 v%1…")
                            .arg(release.version));
                }

                if (interactive) {
                    const auto answer = QMessageBox::question(
                        window_,
                        QStringLiteral("Monitor Hub 更新"),
                        QStringLiteral(
                            "发现稳定版本 v%1。\n\n"
                            "是否下载、校验并安装？安装时 Monitor Hub "
                            "会退出，更新 helper 完成替换后会自动后台重启。")
                            .arg(release.version),
                        QMessageBox::Yes | QMessageBox::No,
                        QMessageBox::Yes);
                    if (answer == QMessageBox::Yes) {
                        begin_update_install(release);
                    }
                } else if (tray_available() &&
                           QSystemTrayIcon::supportsMessages()) {
                    tray_->showMessage(
                        QStringLiteral("Monitor Hub · 新版本可用"),
                        QStringLiteral(
                            "v%1 已发布。可从托盘菜单选择“安装更新 v%1…”。")
                            .arg(release.version),
                        QSystemTrayIcon::Information,
                        6000);
                }
                return;
            }

            if (update_action_) {
                update_action_->setText(
                    QStringLiteral("新版本 v%1（打开 Release）")
                        .arg(release.version));
            }

            if (interactive) {
                QMessageBox box(window_);
                box.setWindowTitle(QStringLiteral("Monitor Hub 更新"));
                box.setIcon(QMessageBox::Information);
                box.setText(
                    QStringLiteral("发现稳定版本 v%1。").arg(release.version));
                box.setInformativeText(
                    result.install_mode == QStringLiteral("development")
                        ? QStringLiteral(
                              "当前是 Git 开发工作区。为避免覆盖正在进行的 "
                              "PR/本地修改，自动 apply 已禁用。")
                        : QStringLiteral(
                              "当前目录不是受管 Release 安装，自动 apply 已禁用。"));
                auto* open_button =
                    box.addButton(QStringLiteral("打开 Release 页面"),
                                  QMessageBox::ActionRole);
                box.addButton(QMessageBox::Close);
                box.exec();
                if (box.clickedButton() == open_button) {
                    open_pending_release_page();
                }
            } else if (tray_available() &&
                       QSystemTrayIcon::supportsMessages()) {
                tray_->showMessage(
                    QStringLiteral("Monitor Hub · 新版本可用"),
                    QStringLiteral(
                        "v%1 已发布；当前是%2模式，不会自动覆盖。")
                        .arg(
                            release.version,
                            result.install_mode == QStringLiteral("development")
                                ? QStringLiteral("开发")
                                : QStringLiteral("非受管")),
                    QSystemTrayIcon::Information,
                    6000);
            }
        });
}

void QtDesktopController::begin_update_install(
    const QtUpdateRelease& release) {
    if (!updates_ || update_busy_) return;

    if (updates_->install_mode() != QStringLiteral("release")) {
        open_pending_release_page();
        return;
    }

    update_busy_ = true;
    if (update_action_) {
        update_action_->setEnabled(false);
        update_action_->setText(
            QStringLiteral("正在下载 v%1…").arg(release.version));
    }

    if (tray_available() && QSystemTrayIcon::supportsMessages()) {
        tray_->showMessage(
            QStringLiteral("Monitor Hub 更新"),
            QStringLiteral("正在下载并校验 v%1…").arg(release.version),
            QSystemTrayIcon::Information,
            4000);
    }

    updates_->stage_release(
        release,
        [this, release](QString stage_dir, QString error) {
            if (!error.isEmpty()) {
                update_busy_ = false;
                if (update_action_) {
                    update_action_->setEnabled(true);
                    update_action_->setText(
                        QStringLiteral("安装更新 v%1…")
                            .arg(release.version));
                }
                QMessageBox::warning(
                    window_,
                    QStringLiteral("Monitor Hub 更新"),
                    error);
                return;
            }

            if (update_action_) {
                update_action_->setText(QStringLiteral("正在启动更新 helper…"));
            }

            QString launch_error;
            if (!updates_->launch_apply(
                    release,
                    stage_dir,
                    &launch_error)) {
                update_busy_ = false;
                if (update_action_) {
                    update_action_->setEnabled(true);
                    update_action_->setText(
                        QStringLiteral("安装更新 v%1…")
                            .arg(release.version));
                }
                QMessageBox::warning(
                    window_,
                    QStringLiteral("Monitor Hub 更新"),
                    launch_error);
                return;
            }

            request_quit();
        });
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
