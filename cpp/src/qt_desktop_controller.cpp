#include "monitor_hub/qt_desktop_controller.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMainWindow>
#include <QMenu>
#include <QSystemTrayIcon>

namespace monitor_hub {

QtDesktopController::QtDesktopController(QMainWindow& window, QObject* parent)
    : QObject(parent), window_(&window) {}

QtDesktopController::~QtDesktopController() {
    if (instance_server_ && instance_server_->isListening()) {
        instance_server_->close();
    }
}

QString QtDesktopController::single_instance_server_name() const {
    return QStringLiteral("monitor-hub.qt.single-instance.v1");
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
    tray_->setToolTip(QStringLiteral("Monitor Hub · 后台监控"));

    auto* menu = new QMenu(window_);
    auto* open_action = menu->addAction(QStringLiteral("打开 Monitor Hub"));
    connect(open_action, &QAction::triggered, this, [this] { show_main_window(); });

    auto* status_action = menu->addAction(QStringLiteral("后台监控运行中"));
    status_action->setEnabled(false);

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

    window_->installEventFilter(this);
    QApplication::setQuitOnLastWindowClosed(false);
    tray_->show();
    return true;
}

void QtDesktopController::start(bool background_requested) {
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
        QStringLiteral("监控不会因关闭主窗口而停止。可点击托盘图标重新打开；选择“退出”才会结束程序。"),
        QSystemTrayIcon::Information,
        5000);
}

bool QtDesktopController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == window_ &&
        event &&
        event->type() == QEvent::Close &&
        tray_available() &&
        !force_quit_) {
        static_cast<QCloseEvent*>(event)->ignore();
        window_->hide();
        show_background_notice_once();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

}  // namespace monitor_hub
