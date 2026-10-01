#include "monitor_hub/qt_desktop_settings.hpp"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << "\n";
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MonitorHubTests"));
    QCoreApplication::setApplicationName(QStringLiteral("DesktopSettings"));
    QSettings::setDefaultFormat(QSettings::IniFormat);

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

    return 0;
}
