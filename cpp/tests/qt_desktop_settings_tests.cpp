#include "monitor_hub/qt_desktop_settings.hpp"
#include "monitor_hub/qt_search.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << "\n";
    return false;
}

bool create_file(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write("test\n") > 0;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MonitorHubTests"));
    QCoreApplication::setApplicationName(QStringLiteral("DesktopSettings"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QStandardPaths::setTestModeEnabled(true);

    QTemporaryDir temp;
    if (!temp.isValid()) {
        std::cerr << "temporary directory unavailable\n";
        return 1;
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());

    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }
    QDir(monitor_hub::desktop_config_backup_directory()).removeRecursively();

    monitor_hub::DesktopUiState expected;
    expected.window_geometry = QByteArray("geometry-v1");
    expected.window_state = QByteArray("window-state-v1");
    expected.project_id = QStringLiteral("project-alpha");
    expected.tab_index = 4;

    QString error;
    if (!expect(
            monitor_hub::save_desktop_ui_state(expected, &error),
            "save_desktop_ui_state failed")) {
        std::cerr << error.toStdString() << "\n";
        return 1;
    }

    const auto actual = monitor_hub::load_desktop_ui_state();
    if (!expect(
            actual.window_geometry == expected.window_geometry,
            "geometry did not round-trip") ||
        !expect(
            actual.window_state == expected.window_state,
            "window state did not round-trip") ||
        !expect(
            actual.project_id == expected.project_id,
            "project id did not round-trip") ||
        !expect(
            actual.tab_index == expected.tab_index,
            "tab index did not round-trip")) {
        return 1;
    }

    bool previous_clean = false;
    if (!expect(
            monitor_hub::begin_desktop_session(&previous_clean, &error),
            "begin_desktop_session failed") ||
        !expect(previous_clean, "first session should start from clean state")) {
        return 1;
    }

    previous_clean = true;
    if (!expect(
            monitor_hub::begin_desktop_session(&previous_clean, &error),
            "second begin_desktop_session failed") ||
        !expect(
            !previous_clean,
            "second session marker should detect an unclean predecessor")) {
        return 1;
    }

    if (!expect(
            monitor_hub::end_desktop_session(&error),
            "end_desktop_session failed")) {
        return 1;
    }

    previous_clean = false;
    if (!expect(
            monitor_hub::begin_desktop_session(&previous_clean, &error),
            "third begin_desktop_session failed") ||
        !expect(previous_clean, "clean shutdown marker was not restored")) {
        return 1;
    }

    if (!expect(
            monitor_hub::end_desktop_session(&error),
            "final end_desktop_session failed")) {
        return 1;
    }

    const auto recent_a = temp.filePath(QStringLiteral("OUTCAR"));
    const auto recent_b = temp.filePath(QStringLiteral("RESULTS.md"));
    if (!expect(create_file(recent_a), "failed to create recent file A") ||
        !expect(create_file(recent_b), "failed to create recent file B")) {
        return 1;
    }

    if (!expect(
            monitor_hub::remember_recent_path(recent_a, &error),
            "remember recent A failed") ||
        !expect(
            monitor_hub::remember_recent_path(recent_b, &error),
            "remember recent B failed") ||
        !expect(
            monitor_hub::remember_recent_path(recent_a, &error),
            "remember recent A second time failed")) {
        std::cerr << error.toStdString() << "\n";
        return 1;
    }

    auto recent = monitor_hub::load_recent_paths();
    if (!expect(recent.size() == 2, "recent path deduplication failed") ||
        !expect(
            QDir::cleanPath(recent.front()) == QDir::cleanPath(recent_a),
            "most-recent path was not promoted")) {
        return 1;
    }

    if (!expect(
            monitor_hub::save_desktop_preferences(
                false, false, false, &error),
            "saving backup source preferences failed") ||
        !expect(
            monitor_hub::save_runtime_locations(
                QStringLiteral("registry-a.json"),
                QStringLiteral("hub-a"),
                &error),
            "saving backup source runtime paths failed")) {
        return 1;
    }

    const auto backup =
        monitor_hub::create_desktop_config_backup(&error);
    if (!expect(!backup.isEmpty(), "config backup was not created") ||
        !expect(QFileInfo::exists(backup), "config backup file missing")) {
        std::cerr << error.toStdString() << "\n";
        return 1;
    }

    if (!expect(
            monitor_hub::save_desktop_preferences(
                true, true, true, &error),
            "mutating preferences failed") ||
        !expect(
            monitor_hub::save_runtime_locations(
                QStringLiteral("registry-b.json"),
                QStringLiteral("hub-b"),
                &error),
            "mutating runtime paths failed") ||
        !expect(
            monitor_hub::clear_recent_paths(&error),
            "clearing recent paths failed")) {
        return 1;
    }

    if (!expect(
            monitor_hub::restore_desktop_config_backup(backup, &error),
            "config restore failed")) {
        std::cerr << error.toStdString() << "\n";
        return 1;
    }

    const auto restored = monitor_hub::load_desktop_settings();
    recent = monitor_hub::load_recent_paths();
    if (!expect(!restored.close_to_tray, "close-to-tray restore failed") ||
        !expect(!restored.notifications, "notification restore failed") ||
        !expect(!restored.automatic_control, "automatic-control restore failed") ||
        !expect(
            restored.registry_path == QStringLiteral("registry-a.json"),
            "registry path restore failed") ||
        !expect(
            restored.hub_data_path == QStringLiteral("hub-a"),
            "hub data path restore failed") ||
        !expect(
            recent.size() == 2 &&
                QDir::cleanPath(recent.front()) == QDir::cleanPath(recent_a),
            "recent paths were not restored")) {
        return 1;
    }

    const auto backups = monitor_hub::list_desktop_config_backups();
    if (!expect(!backups.empty(), "config backup listing is empty") ||
        !expect(
            QDir::cleanPath(backups.front().path) ==
                QDir::cleanPath(backup),
            "latest config backup was not listed first")) {
        return 1;
    }

    std::vector<monitor_hub::WorkspaceSearchDocument> documents;
    documents.push_back({
        QStringLiteral("项目"),
        QStringLiteral("CeOx/Rh"),
        QStringLiteral("surface catalyst"),
        QStringLiteral("CeOx/Rh"),
        QStringLiteral("ceox"),
        {},
        {},
        {},
        {},
        1,
        10,
    });
    documents.push_back({
        QStringLiteral("Issue"),
        QStringLiteral("SCF non-convergence"),
        QStringLiteral("oscillatory SCF requires recovery verification"),
        QStringLiteral("CeOx/Rh"),
        QStringLiteral("ceox"),
        QStringLiteral("task-4"),
        QStringLiteral("issue-7"),
        {},
        {},
        2,
        20,
    });
    documents.push_back({
        QStringLiteral("最近文件"),
        QStringLiteral("OUTCAR"),
        recent_a,
        {},
        {},
        {},
        {},
        {},
        recent_a,
        -1,
        30,
    });

    const auto issue_search =
        monitor_hub::search_workspace_documents(
            documents,
            QStringLiteral("SCF CeOx"),
            10);
    if (!expect(
            issue_search.size() == 1 &&
                issue_search.front().issue_id ==
                    QStringLiteral("issue-7"),
            "multi-token workspace search failed")) {
        return 1;
    }

    const auto project_search =
        monitor_hub::search_workspace_documents(
            documents,
            QStringLiteral("CeOx"),
            10);
    if (!expect(
            !project_search.empty() &&
                project_search.front().project_id ==
                    QStringLiteral("ceox") &&
                project_search.front().kind ==
                    QStringLiteral("项目"),
            "workspace search ranking failed")) {
        return 1;
    }

    QDir(monitor_hub::desktop_config_backup_directory()).removeRecursively();
    return 0;
}
