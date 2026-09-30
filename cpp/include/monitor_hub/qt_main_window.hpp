#pragma once

#include "monitor_hub/core.hpp"

#include <QMainWindow>

#include <map>
#include <string>
#include <vector>

class QLabel;
class QListWidget;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTextEdit;
class QTimer;

namespace monitor_hub {

struct QtDesktopProjectState {
    std::string id;
    std::string name;
    std::string health;
    std::string summary;
};

class QtMainWindow final : public QMainWindow {
public:
    explicit QtMainWindow(RuntimePaths paths, QWidget* parent = nullptr);

    std::vector<QtDesktopProjectState> desktop_project_states() const;

private:
    void build_ui();
    void refresh();
    void render_context_header();
    void refresh_quick_actions();
    void render_overview();
    void render_sidebar();
    void select_project(const std::string& id);
    void render_project();
    void render_task_detail();
    void render_takeovers();
    void render_results();

    const json::object* current_project() const;
    const json::object* current_snapshot() const;
    const json::object* selected_task_meta() const;

    void open_task_target(const std::string& key = {});
    void open_quick_target(const std::string& kind);
    void copy_task_command();
    void copy_debug_summary();

    RuntimePaths paths_;
    SystemInfo system_;
    std::vector<json::object> projects_;
    std::map<std::string, json::object> snapshots_;
    std::string selected_project_;
    std::string selected_task_id_;

    QListWidget* project_list_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* area_ = nullptr;
    QLabel* health_ = nullptr;
    QLabel* headline_ = nullptr;
    QLabel* runner_ = nullptr;
    QLabel* quick_feedback_ = nullptr;
    QPushButton* quick_refresh_ = nullptr;
    QPushButton* quick_monitor_dir_ = nullptr;
    QPushButton* quick_status_ = nullptr;
    QPushButton* quick_log_ = nullptr;
    QPushButton* quick_task_dir_ = nullptr;
    QPushButton* quick_result_ = nullptr;
    QPushButton* quick_takeovers_ = nullptr;
    QPushButton* quick_copy_command_ = nullptr;
    QPushButton* quick_copy_debug_ = nullptr;
    QTabWidget* tabs_ = nullptr;

    QLabel* overview_counts_ = nullptr;
    QTableWidget* overview_projects_ = nullptr;
    QTableWidget* overview_attention_ = nullptr;
    QTableWidget* overview_agents_ = nullptr;

    QTableWidget* progress_ = nullptr;
    QTextEdit* notes_ = nullptr;
    QLabel* task_title_ = nullptr;
    QLabel* task_sub_ = nullptr;
    QLabel* task_paths_ = nullptr;
    QTableWidget* params_ = nullptr;
    QPushButton* open_task_ = nullptr;
    QPushButton* open_log_ = nullptr;
    QPushButton* open_result_ = nullptr;
    QPushButton* copy_command_ = nullptr;

    QTableWidget* takeovers_ = nullptr;
    QTableWidget* results_ = nullptr;
    QTimer* timer_ = nullptr;
};

}  // namespace monitor_hub
