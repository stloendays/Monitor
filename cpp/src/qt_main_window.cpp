#include "monitor_hub/qt_main_window.hpp"
#include "monitor_hub/overview.hpp"
#include "monitor_hub/windows_probe.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace monitor_hub {
namespace {

QString q(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

std::string s(const json::value* v, std::string fallback = {}) {
    if (!v) return fallback;
    if (v->is_string()) return std::string(v->as_string());
    if (v->is_int64()) return std::to_string(v->as_int64());
    if (v->is_uint64()) return std::to_string(v->as_uint64());
    if (v->is_double()) {
        std::ostringstream os;
        os << v->as_double();
        return os.str();
    }
    if (v->is_bool()) return v->as_bool() ? "true" : "false";
    return fallback;
}

const json::object* object(const json::value* v) {
    return v && v->is_object() ? &v->as_object() : nullptr;
}

const json::array* array(const json::value* v) {
    return v && v->is_array() ? &v->as_array() : nullptr;
}

QString health_text(const std::string& health) {
    if (health == "attention") return QStringLiteral("⚠ 需要处理");
    if (health == "working") return QStringLiteral("⟳ 后台处理中");
    if (health == "done") return QStringLiteral("✔ 已完成");
    if (health == "paused") return QStringLiteral("⏸ 已暂停");
    if (health == "stale") return QStringLiteral("⚠ 状态过期");
    if (health == "error") return QStringLiteral("✖ 监控出错");
    return QStringLiteral("● 正常");
}

QString health_prefix(const std::string& health) {
    if (health == "attention" || health == "stale") return QStringLiteral("⚠ ");
    if (health == "working") return QStringLiteral("⟳ ");
    if (health == "done") return QStringLiteral("✔ ");
    if (health == "paused") return QStringLiteral("⏸ ");
    if (health == "error") return QStringLiteral("✖ ");
    return QStringLiteral("● ");
}

QString agent_state_text(const std::string& state) {
    if (state == "running") return QStringLiteral("处理中");
    if (state == "ok") return QStringLiteral("已完成");
    if (state == "failed") return QStringLiteral("失败");
    return state.empty() ? QStringLiteral("—") : q(state);
}

bool local_exists(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec) && !ec;
}

void open_local(const std::string& path) {
    if (!local_exists(path)) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(q(path)));
}

QPushButton* info_button(const QString& tooltip, QWidget* parent) {
    auto* button = new QPushButton(QStringLiteral("ⓘ"), parent);
    button->setFlat(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(tooltip);
    button->setMaximumWidth(32);
    return button;
}

}  // namespace

QtMainWindow::QtMainWindow(RuntimePaths paths, QWidget* parent)
    : QMainWindow(parent), paths_(std::move(paths)) {
    build_ui();
    resize(1420, 900);
    setWindowTitle(QStringLiteral("Monitor Hub · C++ / Qt"));
    refresh();

    timer_ = new QTimer(this);
    timer_->setInterval(60 * 1000);
    connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
    timer_->start();
}

void QtMainWindow::build_ui() {
    auto* central = new QWidget(this);
    auto* root = new QHBoxLayout(central);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    auto* sidebar = new QWidget(central);
    sidebar->setMinimumWidth(280);
    sidebar->setMaximumWidth(360);
    auto* side_layout = new QVBoxLayout(sidebar);
    side_layout->setContentsMargins(0, 0, 0, 0);

    auto* side_head = new QHBoxLayout();
    auto* projects_label = new QLabel(QStringLiteral("项目"), sidebar);
    QFont side_font = projects_label->font();
    side_font.setBold(true);
    side_font.setPointSize(side_font.pointSize() + 2);
    projects_label->setFont(side_font);
    side_head->addWidget(projects_label);
    side_head->addStretch();
    side_head->addWidget(info_button(
        QStringLiteral("项目列表来自登记表、Windows Task Scheduler 和 detached monitor 自动发现。"
                       "状态图标是当前运行状态，不是帮助提示。"), sidebar));
    side_layout->addLayout(side_head);

    project_list_ = new QListWidget(sidebar);
    project_list_->setAlternatingRowColors(true);
    side_layout->addWidget(project_list_, 1);
    connect(project_list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) return;
        auto* item = project_list_->item(row);
        if (!item) return;
        select_project(item->data(Qt::UserRole).toString().toUtf8().toStdString());
    });

    auto* main = new QWidget(central);
    auto* main_layout = new QVBoxLayout(main);
    main_layout->setContentsMargins(0, 0, 0, 0);

    auto* header = new QWidget(main);
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(10, 6, 10, 6);

    auto* title_line = new QHBoxLayout();
    title_ = new QLabel(header);
    QFont title_font = title_->font();
    title_font.setBold(true);
    title_font.setPointSize(title_font.pointSize() + 5);
    title_->setFont(title_font);
    title_line->addWidget(title_);
    area_ = new QLabel(header);
    title_line->addWidget(area_);
    title_line->addStretch();
    auto* refresh_button = new QPushButton(QStringLiteral("立即刷新"), header);
    refresh_button->setToolTip(QStringLiteral("重新读取 Task Scheduler、WMI 进程和状态文件；不会启动、停止或修改计算任务。"));
    title_line->addWidget(refresh_button);
    connect(refresh_button, &QPushButton::clicked, this, [this] { this->refresh(); });
    header_layout->addLayout(title_line);

    auto* state_line = new QHBoxLayout();
    health_ = new QLabel(header);
    QFont health_font = health_->font();
    health_font.setBold(true);
    health_font.setPointSize(health_font.pointSize() + 2);
    health_->setFont(health_font);
    state_line->addWidget(health_);
    headline_ = new QLabel(header);
    headline_->setWordWrap(true);
    state_line->addWidget(headline_, 1);
    header_layout->addLayout(state_line);

    runner_ = new QLabel(header);
    runner_->setWordWrap(true);
    header_layout->addWidget(runner_);
    main_layout->addWidget(header);

    tabs_ = new QTabWidget(main);

    // Cross-project operations overview.
    auto* overview_tab = new QWidget(tabs_);
    auto* overview_layout = new QVBoxLayout(overview_tab);
    overview_layout->setContentsMargins(6, 6, 6, 6);
    overview_layout->setSpacing(8);

    auto* overview_head = new QHBoxLayout();
    auto* overview_title = new QLabel(QStringLiteral("总览 · 控制塔"), overview_tab);
    QFont overview_title_font = overview_title->font();
    overview_title_font.setBold(true);
    overview_title_font.setPointSize(overview_title_font.pointSize() + 2);
    overview_title->setFont(overview_title_font);
    overview_head->addWidget(overview_title);
    overview_counts_ = new QLabel(overview_tab);
    overview_counts_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    overview_head->addWidget(overview_counts_, 1);
    overview_head->addWidget(info_button(
        QStringLiteral("总览只汇总 normalized project snapshot 和 takeover 记录；"
                       "不会从 raw log 自己推断业务状态。双击表格行可跳到对应项目。"),
        overview_tab));
    overview_layout->addLayout(overview_head);

    auto* overview_projects_label = new QLabel(QStringLiteral("项目状态"), overview_tab);
    QFont overview_section_font = overview_projects_label->font();
    overview_section_font.setBold(true);
    overview_projects_label->setFont(overview_section_font);
    overview_layout->addWidget(overview_projects_label);

    overview_projects_ = new QTableWidget(overview_tab);
    overview_projects_->setColumnCount(4);
    overview_projects_->setHorizontalHeaderLabels({
        QStringLiteral("项目"),
        QStringLiteral("状态"),
        QStringLiteral("进度"),
        QStringLiteral("当前情况"),
    });
    overview_projects_->setSelectionBehavior(QAbstractItemView::SelectRows);
    overview_projects_->setSelectionMode(QAbstractItemView::SingleSelection);
    overview_projects_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    overview_projects_->verticalHeader()->setVisible(false);
    overview_projects_->horizontalHeader()->setStretchLastSection(true);
    overview_layout->addWidget(overview_projects_, 2);

    auto* overview_attention_head = new QHBoxLayout();
    auto* overview_attention_label = new QLabel(QStringLiteral("需要处理"), overview_tab);
    overview_attention_label->setFont(overview_section_font);
    overview_attention_head->addWidget(overview_attention_label);
    overview_attention_head->addStretch();
    overview_attention_head->addWidget(info_button(
        QStringLiteral("这里只放需要用户/主 Agent 关注的项目级事项，以及监控错误或状态过期。"
                       "普通运行日志不会进入这里。"),
        overview_tab));
    overview_layout->addLayout(overview_attention_head);

    overview_attention_ = new QTableWidget(overview_tab);
    overview_attention_->setColumnCount(3);
    overview_attention_->setHorizontalHeaderLabels({
        QStringLiteral("类型"),
        QStringLiteral("项目"),
        QStringLiteral("说明"),
    });
    overview_attention_->setSelectionBehavior(QAbstractItemView::SelectRows);
    overview_attention_->setSelectionMode(QAbstractItemView::SingleSelection);
    overview_attention_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    overview_attention_->verticalHeader()->setVisible(false);
    overview_attention_->horizontalHeader()->setStretchLastSection(true);
    overview_layout->addWidget(overview_attention_, 1);

    auto* overview_agent_head = new QHBoxLayout();
    auto* overview_agent_label = new QLabel(QStringLiteral("最近 Agent Activity"), overview_tab);
    overview_agent_label->setFont(overview_section_font);
    overview_agent_head->addWidget(overview_agent_label);
    overview_agent_head->addStretch();
    overview_agent_head->addWidget(info_button(
        QStringLiteral("跨项目汇总最近的 child-agent/takeover 记录。"
                       "双击后进入对应项目的“后台处理记录”，再双击可打开原始证据文件。"),
        overview_tab));
    overview_layout->addLayout(overview_agent_head);

    overview_agents_ = new QTableWidget(overview_tab);
    overview_agents_->setColumnCount(4);
    overview_agents_->setHorizontalHeaderLabels({
        QStringLiteral("时间"),
        QStringLiteral("项目"),
        QStringLiteral("状态"),
        QStringLiteral("摘要"),
    });
    overview_agents_->setSelectionBehavior(QAbstractItemView::SelectRows);
    overview_agents_->setSelectionMode(QAbstractItemView::SingleSelection);
    overview_agents_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    overview_agents_->verticalHeader()->setVisible(false);
    overview_agents_->horizontalHeader()->setStretchLastSection(true);
    overview_layout->addWidget(overview_agents_, 2);

    connect(overview_projects_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        auto* item = overview_projects_->item(row, 0);
        if (!item) return;
        const auto id = item->data(Qt::UserRole).toString().toUtf8().toStdString();
        if (id.empty()) return;
        select_project(id);
        tabs_->setCurrentIndex(1);
    });
    connect(overview_attention_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        auto* item = overview_attention_->item(row, 1);
        if (!item) return;
        const auto id = item->data(Qt::UserRole).toString().toUtf8().toStdString();
        if (id.empty()) return;
        select_project(id);
        tabs_->setCurrentIndex(1);
    });
    connect(overview_agents_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        auto* item = overview_agents_->item(row, 1);
        if (!item) return;
        const auto id = item->data(Qt::UserRole).toString().toUtf8().toStdString();
        if (id.empty()) return;
        select_project(id);
        tabs_->setCurrentIndex(2);
    });
    tabs_->addTab(overview_tab, QStringLiteral("总览"));

    // Progress + task detail.
    auto* progress_tab = new QWidget(tabs_);
    auto* progress_layout = new QVBoxLayout(progress_tab);
    progress_layout->setContentsMargins(6, 6, 6, 6);

    auto* progress_head = new QHBoxLayout();
    auto* progress_label = new QLabel(QStringLiteral("任务进度"), progress_tab);
    QFont section_font = progress_label->font();
    section_font.setBold(true);
    progress_label->setFont(section_font);
    progress_head->addWidget(progress_label);
    progress_head->addStretch();
    progress_head->addWidget(info_button(
        QStringLiteral("选中任务后，右侧显示 job/host、路径、脚本、命令和结构化参数。"
                       "双击任务行等价于“打开任务”。帮助文字只在鼠标悬停时显示。"), progress_tab));
    progress_layout->addLayout(progress_head);

    auto* split = new QSplitter(Qt::Horizontal, progress_tab);
    auto* progress_left = new QWidget(split);
    auto* progress_left_layout = new QVBoxLayout(progress_left);
    progress_left_layout->setContentsMargins(0, 0, 0, 0);
    progress_ = new QTableWidget(progress_left);
    progress_->setSelectionBehavior(QAbstractItemView::SelectRows);
    progress_->setSelectionMode(QAbstractItemView::SingleSelection);
    progress_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    progress_->verticalHeader()->setVisible(false);
    progress_->horizontalHeader()->setStretchLastSection(true);
    progress_left_layout->addWidget(progress_, 1);
    notes_ = new QTextEdit(progress_left);
    notes_->setReadOnly(true);
    notes_->setMaximumHeight(150);
    progress_left_layout->addWidget(notes_);

    auto* detail = new QFrame(split);
    detail->setFrameShape(QFrame::StyledPanel);
    detail->setMinimumWidth(360);
    auto* detail_layout = new QVBoxLayout(detail);
    task_title_ = new QLabel(QStringLiteral("未选择任务"), detail);
    QFont task_font = task_title_->font();
    task_font.setBold(true);
    task_font.setPointSize(task_font.pointSize() + 2);
    task_title_->setFont(task_font);
    detail_layout->addWidget(task_title_);
    task_sub_ = new QLabel(detail);
    detail_layout->addWidget(task_sub_);
    task_paths_ = new QLabel(detail);
    task_paths_->setWordWrap(true);
    task_paths_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    task_paths_->setFont(mono);
    detail_layout->addWidget(task_paths_);

    auto* task_buttons = new QHBoxLayout();
    open_task_ = new QPushButton(QStringLiteral("打开任务"), detail);
    open_log_ = new QPushButton(QStringLiteral("日志"), detail);
    open_result_ = new QPushButton(QStringLiteral("结果"), detail);
    copy_command_ = new QPushButton(QStringLiteral("复制命令"), detail);
    open_task_->setToolTip(QStringLiteral("打开 open_path / path / workdir；没有目录时再尝试日志或结果。"));
    open_log_->setToolTip(QStringLiteral("打开当前任务登记的主要日志文件。"));
    open_result_->setToolTip(QStringLiteral("打开当前任务登记的结果文件。"));
    copy_command_->setToolTip(QStringLiteral("只把登记的命令复制到剪贴板，不会执行。"));
    for (auto* button : {open_task_, open_log_, open_result_, copy_command_}) task_buttons->addWidget(button);
    detail_layout->addLayout(task_buttons);

    auto* params_label = new QLabel(QStringLiteral("参数"), detail);
    params_label->setFont(section_font);
    detail_layout->addWidget(params_label);
    params_ = new QTableWidget(detail);
    params_->setColumnCount(2);
    params_->setHorizontalHeaderLabels({QStringLiteral("参数"), QStringLiteral("值")});
    params_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    params_->verticalHeader()->setVisible(false);
    params_->horizontalHeader()->setStretchLastSection(true);
    detail_layout->addWidget(params_, 1);

    split->addWidget(progress_left);
    split->addWidget(detail);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    progress_layout->addWidget(split, 1);

    connect(progress_, &QTableWidget::itemSelectionChanged, this, [this] { render_task_detail(); });
    connect(progress_, &QTableWidget::cellDoubleClicked, this, [this](int, int) { open_task_target(); });
    connect(open_task_, &QPushButton::clicked, this, [this] { open_task_target(); });
    connect(open_log_, &QPushButton::clicked, this, [this] { open_task_target("log"); });
    connect(open_result_, &QPushButton::clicked, this, [this] { open_task_target("result"); });
    connect(copy_command_, &QPushButton::clicked, this, [this] { copy_task_command(); });
    tabs_->addTab(progress_tab, QStringLiteral("进度"));

    auto* takeover_tab = new QWidget(tabs_);
    auto* takeover_layout = new QVBoxLayout(takeover_tab);
    auto* takeover_head = new QHBoxLayout();
    takeover_head->addWidget(new QLabel(QStringLiteral("后台处理记录"), takeover_tab));
    takeover_head->addStretch();
    takeover_head->addWidget(info_button(
        QStringLiteral("这里显示 monitor 启动的 Claude takeover 历史。当前 Qt 阶段只读，不在这里启动或修改后台作业。"), takeover_tab));
    takeover_layout->addLayout(takeover_head);
    takeovers_ = new QTableWidget(takeover_tab);
    takeovers_->setColumnCount(3);
    takeovers_->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("结果"), QStringLiteral("摘要")});
    takeovers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    takeovers_->verticalHeader()->setVisible(false);
    takeovers_->horizontalHeader()->setStretchLastSection(true);
    takeover_layout->addWidget(takeovers_);
    connect(takeovers_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        auto* item = takeovers_->item(row, 2);
        if (!item) return;
        open_local(item->data(Qt::UserRole).toString().toUtf8().toStdString());
    });
    tabs_->addTab(takeover_tab, QStringLiteral("后台处理记录"));

    auto* result_tab = new QWidget(tabs_);
    auto* result_layout = new QVBoxLayout(result_tab);
    auto* result_head = new QHBoxLayout();
    result_head->addWidget(new QLabel(QStringLiteral("最终结果"), result_tab));
    result_head->addStretch();
    result_head->addWidget(info_button(QStringLiteral("双击已生成的结果文件可用系统默认程序打开。"), result_tab));
    result_layout->addLayout(result_head);
    results_ = new QTableWidget(result_tab);
    results_->setColumnCount(2);
    results_->setHorizontalHeaderLabels({QStringLiteral("文件"), QStringLiteral("状态")});
    results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results_->verticalHeader()->setVisible(false);
    results_->horizontalHeader()->setStretchLastSection(true);
    result_layout->addWidget(results_);
    connect(results_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        auto* item = results_->item(row, 0);
        if (item) open_local(item->data(Qt::UserRole).toString().toUtf8().toStdString());
    });
    tabs_->addTab(result_tab, QStringLiteral("最终结果"));

    main_layout->addWidget(tabs_, 1);
    root->addWidget(sidebar);
    root->addWidget(main, 1);
    setCentralWidget(central);
}

void QtMainWindow::refresh() {
    const auto keep_project = selected_project_;
    system_ = probe_system_info();
    projects_ = load_projects(system_, paths_);
    snapshots_.clear();
    for (const auto& project : projects_) {
        const auto id = s(project.if_contains("id"));
        snapshots_[id] = snapshot(project, system_, paths_);
    }
    selected_project_ = keep_project;
    if (selected_project_.empty() || !snapshots_.count(selected_project_))
        selected_project_ = projects_.empty() ? std::string{} : s(projects_.front().if_contains("id"));
    render_overview();
    render_sidebar();
    render_project();
}

std::vector<QtDesktopProjectState> QtMainWindow::desktop_project_states() const {
    std::vector<QtDesktopProjectState> out;
    out.reserve(projects_.size());

    for (const auto& project : projects_) {
        const auto id = s(project.if_contains("id"));
        const auto name = s(project.if_contains("name"), id);
        const auto it = snapshots_.find(id);
        if (it == snapshots_.end()) {
            out.push_back({id, name, "error", "monitor snapshot unavailable"});
            continue;
        }

        const auto health = s(it->second.if_contains("health"), "ok");
        auto summary = s(it->second.if_contains("problem"));
        if (summary.empty()) summary = s(it->second.if_contains("headline"));
        out.push_back({id, name, health, summary});
    }

    return out;
}

void QtMainWindow::render_overview() {
    if (!overview_counts_ || !overview_projects_ ||
        !overview_attention_ || !overview_agents_) {
        return;
    }

    const auto model = build_overview_model(projects_, snapshots_, 24);
    const int needs_action =
        model.attention_projects + model.error_projects + model.stale_projects;

    QStringList counters;
    counters << QStringLiteral("监控项目 %1").arg(model.total_projects);
    if (needs_action > 0)
        counters << QStringLiteral("需要处理 %1").arg(needs_action);
    if (model.working_projects > 0)
        counters << QStringLiteral("后台处理中 %1").arg(model.working_projects);
    if (model.ok_projects > 0)
        counters << QStringLiteral("正常 %1").arg(model.ok_projects);
    if (model.paused_projects > 0)
        counters << QStringLiteral("已暂停 %1").arg(model.paused_projects);
    if (model.done_projects > 0)
        counters << QStringLiteral("已完成 %1").arg(model.done_projects);
    overview_counts_->setText(counters.join(QStringLiteral("  ·  ")));
    overview_counts_->setToolTip(
        QStringLiteral("“需要处理”包含明确 Attention、监控错误和状态过期；"
                       "Agent 正在处理的项目单独计入“后台处理中”。"));

    overview_projects_->setRowCount(static_cast<int>(model.projects.size()));
    for (int row = 0; row < static_cast<int>(model.projects.size()); ++row) {
        const auto& project = model.projects[static_cast<std::size_t>(row)];
        auto* name = new QTableWidgetItem(q(project.project_name));
        name->setData(Qt::UserRole, q(project.project_id));
        overview_projects_->setItem(row, 0, name);
        overview_projects_->setItem(row, 1, new QTableWidgetItem(health_text(project.health)));
        overview_projects_->setItem(
            row,
            2,
            new QTableWidgetItem(
                project.progress.empty() ? QStringLiteral("—") : q(project.progress)));
        overview_projects_->setItem(
            row,
            3,
            new QTableWidgetItem(
                project.headline.empty() ? QStringLiteral("—") : q(project.headline)));
    }
    overview_projects_->resizeColumnsToContents();

    overview_attention_->setRowCount(static_cast<int>(model.attention.size()));
    for (int row = 0; row < static_cast<int>(model.attention.size()); ++row) {
        const auto& item = model.attention[static_cast<std::size_t>(row)];
        overview_attention_->setItem(row, 0, new QTableWidgetItem(q(item.kind)));
        auto* project = new QTableWidgetItem(q(item.project_name));
        project->setData(Qt::UserRole, q(item.project_id));
        overview_attention_->setItem(row, 1, project);
        overview_attention_->setItem(row, 2, new QTableWidgetItem(q(item.summary)));
    }
    overview_attention_->resizeColumnsToContents();

    overview_agents_->setRowCount(static_cast<int>(model.activity.size()));
    for (int row = 0; row < static_cast<int>(model.activity.size()); ++row) {
        const auto& item = model.activity[static_cast<std::size_t>(row)];
        overview_agents_->setItem(
            row,
            0,
            new QTableWidgetItem(
                item.label.empty() ? QStringLiteral("—") : q(item.label)));
        auto* project = new QTableWidgetItem(q(item.project_name));
        project->setData(Qt::UserRole, q(item.project_id));
        overview_agents_->setItem(row, 1, project);
        overview_agents_->setItem(row, 2, new QTableWidgetItem(agent_state_text(item.state)));
        auto* summary = new QTableWidgetItem(
            item.summary.empty() ? QStringLiteral("—") : q(item.summary));
        summary->setToolTip(
            item.path.empty()
                ? QStringLiteral("双击进入对应项目的后台处理记录。")
                : QStringLiteral("双击进入对应项目；原始记录：%1").arg(q(item.path)));
        overview_agents_->setItem(row, 3, summary);
    }
    overview_agents_->resizeColumnsToContents();
}

void QtMainWindow::render_sidebar() {
    project_list_->blockSignals(true);
    project_list_->clear();
    int selected_row = -1;
    for (int i = 0; i < static_cast<int>(projects_.size()); ++i) {
        const auto& project = projects_[static_cast<std::size_t>(i)];
        const auto id = s(project.if_contains("id"));
        const auto name = s(project.if_contains("name"), id);
        const auto it = snapshots_.find(id);
        const auto health = it == snapshots_.end() ? std::string("error") : s(it->second.if_contains("health"), "ok");
        auto* item = new QListWidgetItem(health_prefix(health) + q(name), project_list_);
        item->setData(Qt::UserRole, q(id));
        const auto area = s(project.if_contains("area"));
        if (!area.empty()) item->setToolTip(q(area));
        if (id == selected_project_) selected_row = i;
    }
    if (selected_row >= 0) project_list_->setCurrentRow(selected_row);
    project_list_->blockSignals(false);
}

const json::object* QtMainWindow::current_project() const {
    const auto it = std::find_if(projects_.begin(), projects_.end(), [&](const json::object& p) {
        return s(p.if_contains("id")) == selected_project_;
    });
    return it == projects_.end() ? nullptr : &*it;
}

const json::object* QtMainWindow::current_snapshot() const {
    const auto it = snapshots_.find(selected_project_);
    return it == snapshots_.end() ? nullptr : &it->second;
}

void QtMainWindow::select_project(const std::string& id) {
    if (id == selected_project_) return;
    selected_project_ = id;
    selected_task_id_.clear();
    render_project();
}

void QtMainWindow::render_project() {
    const auto* project = current_project();
    const auto* snap = current_snapshot();
    if (!project || !snap) {
        title_->setText(QStringLiteral("没有项目"));
        area_->clear();
        health_->clear();
        headline_->clear();
        runner_->clear();
        progress_->setRowCount(0);
        progress_->setColumnCount(0);
        render_task_detail();
        return;
    }

    title_->setText(q(s(project->if_contains("name"))));
    area_->setText(q(s(project->if_contains("area"))));
    const auto health = s(snap->if_contains("health"), "ok");
    health_->setText(health_text(health));
    auto headline = s(snap->if_contains("problem"));
    if (headline.empty()) headline = s(snap->if_contains("headline"));
    headline_->setText(q(headline));
    const auto* runner = object(snap->if_contains("runner"));
    runner_->setText(runner ? q(s(runner->if_contains("text"))) : QString{});

    const auto* table = object(snap->if_contains("table"));
    const auto* cols = table ? array(table->if_contains("cols")) : nullptr;
    const auto* rows = table ? array(table->if_contains("rows")) : nullptr;
    const auto* metas = table ? array(table->if_contains("row_meta")) : nullptr;

    progress_->blockSignals(true);
    progress_->clear();
    progress_->setRowCount(rows ? static_cast<int>(rows->size()) : 0);
    progress_->setColumnCount(cols ? static_cast<int>(cols->size()) : 0);
    QStringList headers;
    if (cols) for (const auto& col : *cols) headers << q(s(&col));
    progress_->setHorizontalHeaderLabels(headers);

    int select_row = -1;
    if (rows) {
        for (int r = 0; r < static_cast<int>(rows->size()); ++r) {
            const auto* row = array(&(*rows)[static_cast<std::size_t>(r)]);
            if (!row) continue;
            for (int col = 0; col < static_cast<int>(row->size()); ++col)
                progress_->setItem(r, col, new QTableWidgetItem(q(s(&(*row)[static_cast<std::size_t>(col)]))));
            if (metas && static_cast<std::size_t>(r) < metas->size()) {
                if (const auto* meta = object(&(*metas)[static_cast<std::size_t>(r)])) {
                    const auto task_id = s(meta->if_contains("task_id"));
                    if (!task_id.empty() && task_id == selected_task_id_) select_row = r;
                }
            }
        }
    }
    progress_->resizeColumnsToContents();
    if (select_row < 0 && progress_->rowCount() > 0) select_row = 0;
    if (select_row >= 0) progress_->selectRow(select_row);
    progress_->blockSignals(false);

    QStringList note_lines;
    if (const auto* extras = array(snap->if_contains("extras")))
        for (const auto& item : *extras) note_lines << q(s(&item));
    if (const auto* notes = array(snap->if_contains("notes")))
        for (const auto& item : *notes) note_lines << QStringLiteral("• ") + q(s(&item));
    notes_->setPlainText(note_lines.join("\n"));

    render_task_detail();
    render_takeovers();
    render_results();
}

const json::object* QtMainWindow::selected_task_meta() const {
    const auto* snap = current_snapshot();
    if (!snap || progress_->currentRow() < 0) return nullptr;
    const auto* table = object(snap->if_contains("table"));
    const auto* metas = table ? array(table->if_contains("row_meta")) : nullptr;
    const auto row = static_cast<std::size_t>(progress_->currentRow());
    if (!metas || row >= metas->size()) return nullptr;
    return object(&(*metas)[row]);
}

void QtMainWindow::render_task_detail() {
    const auto row = progress_->currentRow();
    const auto* meta = selected_task_meta();
    std::string task;
    if (meta) task = s(meta->if_contains("task_id"));
    if (task.empty() && row >= 0 && progress_->item(row, 0)) task = progress_->item(row, 0)->text().toUtf8().toStdString();
    selected_task_id_ = task;

    task_title_->setText(task.empty() ? QStringLiteral("未选择任务") : q(task));
    QStringList sub;
    if (meta) {
        const auto job = s(meta->if_contains("job_id"));
        const auto host = s(meta->if_contains("host"));
        if (!job.empty()) sub << QStringLiteral("Job ") + q(job);
        if (!host.empty()) sub << q(host);
    }
    task_sub_->setText(sub.join(QStringLiteral("  ·  ")));

    QStringList paths;
    if (meta) {
        for (const auto& pair : std::vector<std::pair<const char*, QString>>{
                 {"open_path", QStringLiteral("路径")}, {"script", QStringLiteral("脚本")},
                 {"command", QStringLiteral("命令")}, {"log", QStringLiteral("日志")},
                 {"result", QStringLiteral("结果")}}) {
            const auto value = s(meta->if_contains(pair.first));
            if (!value.empty()) paths << pair.second + QStringLiteral("：") + q(value);
        }
    }
    task_paths_->setText(paths.join("\n"));

    params_->setRowCount(0);
    const auto* param_obj = meta ? object(meta->if_contains("params")) : nullptr;
    if (param_obj) {
        params_->setRowCount(static_cast<int>(param_obj->size()));
        int row_index = 0;
        for (const auto& kv : *param_obj) {
            params_->setItem(row_index, 0, new QTableWidgetItem(q(std::string(kv.key().data(), kv.key().size()))));
            params_->setItem(row_index, 1, new QTableWidgetItem(q(s(&kv.value(), json::serialize(kv.value())))));
            ++row_index;
        }
        params_->resizeColumnsToContents();
    }

    auto first_existing = [&](std::initializer_list<const char*> keys) {
        if (!meta) return std::string{};
        for (const auto* key : keys) {
            const auto value = s(meta->if_contains(key));
            if (local_exists(value)) return value;
        }
        return std::string{};
    };
    open_task_->setEnabled(!first_existing({"open_path", "path", "workdir", "log", "result"}).empty());
    open_log_->setEnabled(meta && local_exists(s(meta->if_contains("log"))));
    open_result_->setEnabled(meta && local_exists(s(meta->if_contains("result"))));
    copy_command_->setEnabled(meta && !s(meta->if_contains("command")).empty());
}

void QtMainWindow::open_task_target(const std::string& key) {
    const auto* meta = selected_task_meta();
    if (!meta) return;
    if (!key.empty()) {
        open_local(s(meta->if_contains(key)));
        return;
    }
    for (const auto* candidate : {"open_path", "path", "workdir", "log", "result"}) {
        const auto value = s(meta->if_contains(candidate));
        if (local_exists(value)) {
            open_local(value);
            return;
        }
    }
}

void QtMainWindow::copy_task_command() {
    const auto* meta = selected_task_meta();
    if (!meta) return;
    const auto command = s(meta->if_contains("command"));
    if (!command.empty()) QApplication::clipboard()->setText(q(command));
}

void QtMainWindow::render_takeovers() {
    const auto* snap = current_snapshot();
    const auto* items = snap ? array(snap->if_contains("takeovers")) : nullptr;
    takeovers_->setRowCount(items ? static_cast<int>(items->size()) : 0);
    if (!items) return;
    for (int row = 0; row < static_cast<int>(items->size()); ++row) {
        const auto* item = object(&(*items)[static_cast<std::size_t>(row)]);
        if (!item) continue;
        takeovers_->setItem(row, 0, new QTableWidgetItem(q(s(item->if_contains("label")))));
        takeovers_->setItem(row, 1, new QTableWidgetItem(agent_state_text(s(item->if_contains("state")))));
        auto* summary = new QTableWidgetItem(q(s(item->if_contains("summary"))));
        summary->setData(Qt::UserRole, q(s(item->if_contains("path"))));
        if (!s(item->if_contains("path")).empty())
            summary->setToolTip(QStringLiteral("双击打开这次处理的原始记录。"));
        takeovers_->setItem(row, 2, summary);
    }
    takeovers_->resizeColumnsToContents();
}

void QtMainWindow::render_results() {
    const auto* snap = current_snapshot();
    const auto* items = snap ? array(snap->if_contains("results_list")) : nullptr;
    results_->setRowCount(items ? static_cast<int>(items->size()) : 0);
    if (!items) return;
    for (int row = 0; row < static_cast<int>(items->size()); ++row) {
        const auto* pair = array(&(*items)[static_cast<std::size_t>(row)]);
        if (!pair || pair->size() < 2) continue;
        const auto label = s(&(*pair)[0]);
        const auto path = s(&(*pair)[1]);
        auto* file = new QTableWidgetItem(q(label));
        file->setData(Qt::UserRole, q(path));
        results_->setItem(row, 0, file);
        results_->setItem(row, 1, new QTableWidgetItem(local_exists(path) ? QStringLiteral("已生成") : QStringLiteral("尚未生成")));
    }
    results_->resizeColumnsToContents();
}

}  // namespace monitor_hub
