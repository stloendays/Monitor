#include "monitor_hub/qt_main_window.hpp"
#include "monitor_hub/overview.hpp"
#include "monitor_hub/setup_request.hpp"
#include "monitor_hub/windows_probe.hpp"

#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QFontDatabase>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <limits>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QFile>
#include <QFileInfo>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>

#include <algorithm>
#include <exception>
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

QColor health_color(const std::string& health) {
    if (health == "attention" || health == "stale") return QColor(QStringLiteral("#9A641F"));
    if (health == "working") return QColor(QStringLiteral("#4A6F9C"));
    if (health == "done") return QColor(QStringLiteral("#58705D"));
    if (health == "error") return QColor(QStringLiteral("#A4483F"));
    if (health == "paused") return QColor(QStringLiteral("#77736C"));
    return QColor(QStringLiteral("#2E6B47"));
}

QColor agent_state_color(const std::string& state) {
    if (state == "running") return QColor(QStringLiteral("#4A6F9C"));
    if (state == "ok") return QColor(QStringLiteral("#58705D"));
    if (state == "failed") return QColor(QStringLiteral("#A4483F"));
    return QColor(QStringLiteral("#77736C"));
}

QColor recovery_stage_color(const std::string& stage) {
    if (stage == "resolved" || stage == "recovery_verified")
        return QColor(QStringLiteral("#3F7D5A"));
    if (stage == "needs_user")
        return QColor(QStringLiteral("#9A641F"));
    if (stage == "failed")
        return QColor(QStringLiteral("#A34747"));
    if (stage == "waiting_verification" ||
        stage == "recovery_verification" ||
        stage == "agent_handling" ||
        stage == "agent_completed" ||
        stage == "action_selected")
        return QColor(QStringLiteral("#4E6B8A"));
    return QColor(QStringLiteral("#77736C"));
}

QString recovery_flow_text(const IssueProjection& issue) {
    const auto stage = issue_recovery_stage(issue);
    if (stage == "needs_user") {
        return QStringLiteral(
            "✓ 问题  →  ⚠ 决策  →  ○ 动作  →  ○ 验证  →  ○ 解决");
    }

    const auto mark = [](bool done, bool active) {
        if (done) return QStringLiteral("✓");
        if (active) return QStringLiteral("●");
        return QStringLiteral("○");
    };

    const bool handling_done =
        issue.action_applied ||
        issue.task_restarted ||
        issue.recovery_started ||
        issue.recovery_verified ||
        issue.resolved;
    const bool handling_active =
        stage == "classified" ||
        stage == "assigned" ||
        stage == "investigating" ||
        stage == "agent_handling" ||
        stage == "agent_completed" ||
        stage == "action_selected";

    const bool action_done =
        issue.action_applied ||
        issue.task_restarted ||
        issue.recovery_started ||
        issue.recovery_verified ||
        issue.resolved;
    const bool action_active = stage == "action_selected";

    const bool verify_done =
        issue.recovery_verified || issue.resolved;
    const bool verify_active =
        stage == "waiting_verification" ||
        stage == "recovery_verification";

    const bool resolve_done = issue.resolved;
    const bool resolve_active = stage == "recovery_verified";

    return QStringLiteral(
               "%1 问题  →  %2 处理  →  %3 动作  →  %4 验证  →  %5 解决")
        .arg(mark(true, stage == "detected"))
        .arg(mark(handling_done, handling_active))
        .arg(mark(action_done, action_active))
        .arg(mark(verify_done, verify_active))
        .arg(mark(resolve_done, resolve_active));
}

void emphasize_item(QTableWidgetItem* item, const QColor& color) {
    if (!item) return;
    item->setForeground(QBrush(color));
    auto font = item->font();
    font.setWeight(QFont::DemiBold);
    item->setFont(font);
}

void configure_table(QTableWidget* table) {
    if (!table) return;
    table->setShowGrid(false);
    table->setAlternatingRowColors(false);
    table->setFocusPolicy(Qt::NoFocus);
    table->setWordWrap(false);
    table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->verticalHeader()->setDefaultSectionSize(42);
}

void set_health_badge(QLabel* label, const std::string& health) {
    if (!label) return;
    label->setProperty("health", q(health));
    label->setText(health_text(health));
    label->style()->unpolish(label);
    label->style()->polish(label);
    label->update();
}

QString usage_reset_text(const std::optional<double>& epoch) {
    if (!epoch) return QStringLiteral("重置时间未知");
    const auto seconds = static_cast<qint64>(*epoch);
    const auto when = QDateTime::fromSecsSinceEpoch(seconds).toLocalTime();
    const auto now = QDateTime::currentDateTime();

    QString absolute;
    if (when.date() == now.date())
        absolute = QStringLiteral("今天 %1").arg(when.toString(QStringLiteral("HH:mm")));
    else if (when.date() == now.date().addDays(1))
        absolute = QStringLiteral("明天 %1").arg(when.toString(QStringLiteral("HH:mm")));
    else
        absolute = when.toString(QStringLiteral("MM-dd HH:mm"));

    const auto remaining = now.secsTo(when);
    if (remaining <= 0)
        return absolute + QStringLiteral("（已到重置时间）");
    if (remaining < 3600)
        return absolute + QStringLiteral("（约 %1 分钟后）").arg((remaining + 59) / 60);
    if (remaining < 48 * 3600) {
        const auto hours = remaining / 3600;
        const auto minutes = (remaining % 3600) / 60;
        return absolute + QStringLiteral("（约 %1 小时 %2 分钟后）").arg(hours).arg(minutes);
    }
    return absolute;
}

QString observed_text(const std::optional<double>& epoch) {
    if (!epoch) return QStringLiteral("还没有收到 CLI 用量快照");
    const auto seconds = static_cast<qint64>(*epoch);
    const auto when = QDateTime::fromSecsSinceEpoch(seconds).toLocalTime();
    return QStringLiteral("数据更新 %1").arg(when.toString(QStringLiteral("MM-dd HH:mm:ss")));
}

QString activity_time_text(const std::optional<double>& epoch) {
    if (!epoch) return QStringLiteral("时间未知");
    const auto when = QDateTime::fromSecsSinceEpoch(
        static_cast<qint64>(*epoch)).toLocalTime();
    const auto now = QDateTime::currentDateTime();
    const auto age = when.secsTo(now);
    if (age >= 0 && age < 60)
        return QStringLiteral("刚刚");
    if (age >= 0 && age < 3600)
        return QStringLiteral("%1 分钟前").arg((age + 59) / 60);
    if (when.date() == now.date())
        return QStringLiteral("今天 %1").arg(when.toString(QStringLiteral("HH:mm")));
    return when.toString(QStringLiteral("MM-dd HH:mm"));
}

QString quoted_command(const QString& path) {
    return QStringLiteral("\"%1\"").arg(QFileInfo(path).absoluteFilePath());
}

QString claude_bridge_command() {
    const auto explicit_path = qEnvironmentVariable("MONITOR_HUB_CLAUDE_BRIDGE");
    if (!explicit_path.isEmpty() && QFileInfo::exists(explicit_path)) {
        if (explicit_path.endsWith(QStringLiteral(".py"), Qt::CaseInsensitive))
            return QStringLiteral("python %1").arg(quoted_command(explicit_path));
        return quoted_command(explicit_path);
    }

    const auto app_dir = QCoreApplication::applicationDirPath();
#ifdef _WIN32
    const auto native_cli = app_dir + QStringLiteral("/monitor_hub_cli.exe");
#else
    const auto native_cli = app_dir + QStringLiteral("/monitor_hub_cli");
#endif
    if (QFileInfo::exists(native_cli))
        return quoted_command(native_cli) + QStringLiteral(" --claude-statusline");

    const QStringList python_candidates = {
        QStringLiteral(R"(D:\Research\Monitor\hub\claude_statusline_bridge.py)"),
        app_dir + QStringLiteral("/../share/monitor_hub/claude_statusline_bridge.py"),
        app_dir + QStringLiteral("/../../hub/claude_statusline_bridge.py"),
        app_dir + QStringLiteral("/../../../hub/claude_statusline_bridge.py"),
    };
    for (const auto& path : python_candidates) {
        if (QFileInfo::exists(path))
            return QStringLiteral("python %1").arg(quoted_command(path));
    }

    return QStringLiteral(R"(python "D:\Research\Monitor\hub\claude_statusline_bridge.py")");
}

void set_usage_bar(QProgressBar* bar,
                   QLabel* label,
                   const QString& name,
                   const ClaudeUsageWindow& window) {
    if (!bar || !label) return;
    bar->setRange(0, 100);
    if (window.used_percentage) {
        const auto value = std::clamp(
            static_cast<int>(*window.used_percentage + 0.5), 0, 100);
        bar->setValue(value);
        bar->setFormat(QStringLiteral("%1%").arg(value));
        label->setText(
            QStringLiteral("%1 · 已用 %2% · 重置 %3")
                .arg(name)
                .arg(value)
                .arg(usage_reset_text(window.resets_at)));
    } else {
        bar->setValue(0);
        bar->setFormat(QStringLiteral("—"));
        label->setText(
            QStringLiteral("%1 · 用量未知 · 重置 %2")
                .arg(name)
                .arg(usage_reset_text(window.resets_at)));
    }
}

QPushButton* info_button(const QString& tooltip, QWidget* parent) {
    auto* button = new QPushButton(QStringLiteral("ⓘ"), parent);
    button->setObjectName(QStringLiteral("infoButton"));
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
    setWindowTitle(QStringLiteral("Monitor Hub"));
    refresh();

    timer_ = new QTimer(this);
    timer_->setInterval(60 * 1000);
    connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
    timer_->start();
}

void QtMainWindow::build_ui() {
    auto* central = new QWidget(this);
    central->setObjectName(QStringLiteral("appRoot"));
    auto* root = new QHBoxLayout(central);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(14);

    auto* sidebar = new QWidget(central);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setMinimumWidth(270);
    sidebar->setMaximumWidth(330);
    auto* side_layout = new QVBoxLayout(sidebar);
    side_layout->setContentsMargins(14, 14, 14, 14);
    side_layout->setSpacing(10);

    auto* brand_row = new QHBoxLayout();
    brand_row->setSpacing(10);
    auto* brand_icon = new QLabel(sidebar);
    const QPixmap app_icon(QStringLiteral(":/monitor_hub/icons/monitor_hub.png"));
    brand_icon->setPixmap(app_icon.scaled(
        30, 30, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    brand_icon->setFixedSize(32, 32);
    brand_row->addWidget(brand_icon);

    auto* brand_text = new QVBoxLayout();
    brand_text->setSpacing(0);
    auto* brand_title = new QLabel(QStringLiteral("Monitor Hub"), sidebar);
    brand_title->setObjectName(QStringLiteral("brandTitle"));
    auto* brand_subtitle = new QLabel(QStringLiteral("Agent Operations"), sidebar);
    brand_subtitle->setObjectName(QStringLiteral("brandSubtitle"));
    brand_text->addWidget(brand_title);
    brand_text->addWidget(brand_subtitle);
    brand_row->addLayout(brand_text, 1);
    side_layout->addLayout(brand_row);

    auto* side_head = new QHBoxLayout();
    auto* projects_label = new QLabel(QStringLiteral("项目"), sidebar);
    projects_label->setObjectName(QStringLiteral("sectionLabel"));
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
    project_list_->setObjectName(QStringLiteral("projectList"));
    project_list_->setAlternatingRowColors(false);
    project_list_->setSpacing(2);
    project_list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    side_layout->addWidget(project_list_, 1);
    connect(project_list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) return;
        auto* item = project_list_->item(row);
        if (!item) return;
        select_project(item->data(Qt::UserRole).toString().toUtf8().toStdString());
    });

    new_monitor_ = new QPushButton(QStringLiteral("＋ 新建监控任务"), sidebar);
    new_monitor_->setObjectName(QStringLiteral("newMonitorButton"));
    new_monitor_->setToolTip(
        QStringLiteral("把新的监控需求交给后台 Claude setup agent："
                       "它会按你填写的允许/禁止边界设置监控、登记到 Monitor Hub，"
                       "办理过程会出现在“新任务办理”项目里。"));
    side_layout->addWidget(new_monitor_);
    connect(new_monitor_, &QPushButton::clicked, this, [this] {
        open_new_monitor_dialog();
    });

    auto* claude_card = new QFrame(sidebar);
    claude_card->setObjectName(QStringLiteral("claudeCard"));
    claude_card->setFrameShape(QFrame::NoFrame);
    auto* claude_layout = new QVBoxLayout(claude_card);
    claude_layout->setContentsMargins(11, 10, 11, 10);
    claude_layout->setSpacing(6);

    auto* claude_head = new QHBoxLayout();
    auto* claude_title = new QLabel(QStringLiteral("Claude CLI"), claude_card);
    claude_title->setObjectName(QStringLiteral("sectionLabel"));
    claude_head->addWidget(claude_title);
    claude_cli_state_ = new QLabel(claude_card);
    claude_cli_state_->setObjectName(QStringLiteral("claudeState"));
    claude_head->addWidget(claude_cli_state_);
    claude_head->addStretch();
    claude_head->addWidget(info_button(
        QStringLiteral("用量优先来自 Claude Code statusLine 自带的 rate_limits："
                       "5 小时和 7 天 used_percentage + resets_at。"
                       "Monitor Hub 不读取 OAuth token，也不会为了查额度额外调用模型。"),
        claude_card));
    claude_layout->addLayout(claude_head);

    claude_cli_meta_ = new QLabel(claude_card);
    claude_cli_meta_->setObjectName(QStringLiteral("mutedText"));
    claude_cli_meta_->setWordWrap(true);
    claude_layout->addWidget(claude_cli_meta_);

    claude_session_ = new QLabel(claude_card);
    claude_session_->setObjectName(QStringLiteral("claudeSession"));
    claude_session_->setWordWrap(true);
    claude_layout->addWidget(claude_session_);

    claude_activity_ = new QLabel(claude_card);
    claude_activity_->setObjectName(QStringLiteral("claudeActivity"));
    claude_activity_->setWordWrap(true);
    claude_layout->addWidget(claude_activity_);

    claude_five_text_ = new QLabel(QStringLiteral("5 小时 · 等待数据"), claude_card);
    claude_five_text_->setObjectName(QStringLiteral("usageText"));
    claude_layout->addWidget(claude_five_text_);
    claude_five_bar_ = new QProgressBar(claude_card);
    claude_five_bar_->setObjectName(QStringLiteral("usageBar"));
    claude_five_bar_->setTextVisible(true);
    claude_layout->addWidget(claude_five_bar_);

    claude_seven_text_ = new QLabel(QStringLiteral("7 天 · 等待数据"), claude_card);
    claude_seven_text_->setObjectName(QStringLiteral("usageText"));
    claude_layout->addWidget(claude_seven_text_);
    claude_seven_bar_ = new QProgressBar(claude_card);
    claude_seven_bar_->setObjectName(QStringLiteral("usageBar"));
    claude_seven_bar_->setTextVisible(true);
    claude_layout->addWidget(claude_seven_bar_);

    claude_updated_ = new QLabel(claude_card);
    claude_updated_->setObjectName(QStringLiteral("brandSubtitle"));
    claude_updated_->setWordWrap(true);
    claude_layout->addWidget(claude_updated_);

    auto* claude_actions = new QHBoxLayout();
    claude_actions->setSpacing(5);
    claude_setup_ = new QPushButton(QStringLiteral("接入用量"), claude_card);
    claude_setup_->setProperty("role", QStringLiteral("quick"));
    claude_setup_->setToolTip(
        QStringLiteral("复制一条 /statusline 配置指令。粘贴到 Claude CLI 后，"
                       "Claude 自己的状态栏会把用量快照同步给 Monitor Hub。"));
    claude_config_ = new QPushButton(QStringLiteral("CLI 配置"), claude_card);
    claude_config_->setProperty("role", QStringLiteral("quick"));
    claude_config_->setToolTip(
        QStringLiteral("打开当前 CLAUDE_CONFIG_DIR（默认 ~/.claude）或 settings.json。"));
    claude_usage_ = new QPushButton(QStringLiteral("复制 /usage"), claude_card);
    claude_usage_->setProperty("role", QStringLiteral("quick"));
    claude_usage_->setToolTip(
        QStringLiteral("复制 Claude Code 官方 /usage 命令；可在终端里查看完整计划用量。"));
    claude_actions->addWidget(claude_setup_);
    claude_actions->addWidget(claude_config_);
    claude_actions->addWidget(claude_usage_);
    claude_actions->addStretch();
    claude_layout->addLayout(claude_actions);

    auto* claude_session_actions = new QHBoxLayout();
    claude_session_actions->setSpacing(5);
    claude_project_ = new QPushButton(QStringLiteral("关联项目"), claude_card);
    claude_project_->setProperty("role", QStringLiteral("quick"));
    claude_project_->setToolTip(
        QStringLiteral("根据 Claude project_dir/cwd 与 Monitor 项目路径自动匹配；匹配后跳到对应项目。"));
    claude_workspace_ = new QPushButton(QStringLiteral("工作目录"), claude_card);
    claude_workspace_->setProperty("role", QStringLiteral("quick"));
    claude_workspace_->setToolTip(
        QStringLiteral("打开当前 Claude 会话的 project_dir / cwd。"));
    claude_transcript_ = new QPushButton(QStringLiteral("会话记录"), claude_card);
    claude_transcript_->setProperty("role", QStringLiteral("quick"));
    claude_transcript_->setToolTip(
        QStringLiteral("打开 Claude Code 当前会话 transcript 文件。Monitor Hub 只读取尾部工具调用元数据，不展示对话正文。"));
    claude_session_actions->addWidget(claude_project_);
    claude_session_actions->addWidget(claude_workspace_);
    claude_session_actions->addWidget(claude_transcript_);
    claude_session_actions->addStretch();
    claude_layout->addLayout(claude_session_actions);

    connect(claude_setup_, &QPushButton::clicked, this, [this] {
        copy_claude_statusline_setup();
    });
    connect(claude_config_, &QPushButton::clicked, this, [this] {
        open_claude_config();
    });
    connect(claude_usage_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(QStringLiteral("/usage"));
        if (claude_updated_) claude_updated_->setText(QStringLiteral("已复制 /usage"));
        QTimer::singleShot(2200, this, [this] { render_claude_cli_status(); });
    });
    connect(claude_project_, &QPushButton::clicked, this, [this] {
        if (claude_linked_project_id_.empty()) return;
        select_project(claude_linked_project_id_);
        if (tabs_) tabs_->setCurrentIndex(1);
    });
    connect(claude_workspace_, &QPushButton::clicked, this, [this] {
        const auto status = load_claude_cli_status(system_, paths_);
        const auto path = !status.project_dir.empty() ? status.project_dir : status.cwd;
        if (!path.empty()) open_local(path);
    });
    connect(claude_transcript_, &QPushButton::clicked, this, [this] {
        const auto status = load_claude_cli_status(system_, paths_);
        if (!status.transcript_path.empty())
            open_local(status.transcript_path.string());
    });

    side_layout->addWidget(claude_card);

    auto* main = new QWidget(central);
    main->setObjectName(QStringLiteral("mainPane"));
    auto* main_layout = new QVBoxLayout(main);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(12);

    auto* header = new QWidget(main);
    header->setObjectName(QStringLiteral("contextHeader"));
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(18, 14, 18, 14);
    header_layout->setSpacing(8);

    auto* title_line = new QHBoxLayout();
    title_ = new QLabel(header);
    title_->setObjectName(QStringLiteral("pageTitle"));
    QFont title_font = title_->font();
    title_font.setBold(true);
    title_font.setPointSize(title_font.pointSize() + 5);
    title_->setFont(title_font);
    title_line->addWidget(title_);
    area_ = new QLabel(header);
    area_->setObjectName(QStringLiteral("pageMeta"));
    title_line->addWidget(area_);
    title_line->addStretch();
    header_layout->addLayout(title_line);

    auto* state_line = new QHBoxLayout();
    health_ = new QLabel(header);
    health_->setObjectName(QStringLiteral("healthBadge"));
    QFont health_font = health_->font();
    health_font.setBold(true);
    health_font.setPointSize(health_font.pointSize() + 2);
    health_->setFont(health_font);
    state_line->addWidget(health_);
    headline_ = new QLabel(header);
    headline_->setObjectName(QStringLiteral("headline"));
    headline_->setWordWrap(true);
    state_line->addWidget(headline_, 1);
    header_layout->addLayout(state_line);

    runner_ = new QLabel(header);
    runner_->setObjectName(QStringLiteral("mutedText"));
    runner_->setWordWrap(true);
    header_layout->addWidget(runner_);
    main_layout->addWidget(header);

    auto* quick_bar = new QFrame(main);
    quick_bar->setObjectName(QStringLiteral("quickBar"));
    quick_bar->setFrameShape(QFrame::NoFrame);
    auto* quick_layout = new QVBoxLayout(quick_bar);
    quick_layout->setContentsMargins(14, 10, 14, 10);
    quick_layout->setSpacing(8);

    auto* quick_head = new QHBoxLayout();
    auto* quick_title = new QLabel(QStringLiteral("快速调试"), quick_bar);
    quick_title->setObjectName(QStringLiteral("sectionLabel"));
    quick_head->addWidget(quick_title);
    quick_feedback_ = new QLabel(QStringLiteral("只读安全操作"), quick_bar);
    quick_feedback_->setObjectName(QStringLiteral("quickFeedback"));
    quick_head->addWidget(quick_feedback_);
    quick_head->addStretch();
    quick_head->addWidget(info_button(
        QStringLiteral("新手推荐顺序：先“刷新状态”，再看“状态文件”和“监控日志”；"
                       "如果 Monitor 已启动 Agent，再看“后台记录”；最后检查“结果”。"
                       "这一排按钮不会暂停、重启或修改计算参数。"),
        quick_bar));
    quick_layout->addLayout(quick_head);

    auto make_quick = [quick_bar](const QString& text, const QString& tip) {
        auto* button = new QPushButton(text, quick_bar);
        button->setProperty("role", QStringLiteral("quick"));
        button->setToolTip(tip);
        return button;
    };

    quick_refresh_ = make_quick(
        QStringLiteral("↻ 刷新状态"),
        QStringLiteral("重新读取 Task Scheduler、WMI 和状态文件。只读，不会启动或停止任务。"));
    quick_monitor_dir_ = make_quick(
        QStringLiteral("监控目录"),
        QStringLiteral("打开当前项目登记的 monitor 目录，适合检查脚本、状态文件和 takeover 记录。"));
    quick_status_ = make_quick(
        QStringLiteral("状态文件"),
        QStringLiteral("打开当前项目的 hub_status.json / legacy status Markdown。先看它最容易判断 Monitor 实际读到了什么。"));
    quick_log_ = make_quick(
        QStringLiteral("监控日志"),
        QStringLiteral("优先打开当前任务日志；没有任务日志时打开项目登记的监控日志。"));
    quick_task_dir_ = make_quick(
        QStringLiteral("任务目录"),
        QStringLiteral("打开当前选中任务的 open_path / path / workdir。没有结构化任务路径时会禁用。"));
    quick_result_ = make_quick(
        QStringLiteral("结果"),
        QStringLiteral("优先打开当前任务结果；没有任务结果时打开项目登记的第一个已生成结果。"));
    quick_takeovers_ = make_quick(
        QStringLiteral("后台记录"),
        QStringLiteral("切换到后台处理记录，查看 Monitor 启动的 Agent/takeover 历史。"));
    quick_registry_ = make_quick(
        QStringLiteral("登记表"),
        QStringLiteral("打开 monitor_hub_projects.json。项目没有出现在侧边栏时，先检查这里的 id、路径和 runner 配置。"));
    quick_hub_data_ = make_quick(
        QStringLiteral("Hub 数据"),
        QStringLiteral("打开 Monitor Hub 本地数据目录，用来检查 requests、events、outbox 等运行数据。"));
    quick_job_root_ = make_quick(
        QStringLiteral("作业目录"),
        QStringLiteral("打开 cdesktop-jobs 目录，用来检查 detached monitor / child-agent 的 pid、exitcode、output.log。"));
    quick_copy_command_ = make_quick(
        QStringLiteral("复制命令"),
        QStringLiteral("复制当前任务命令；没有任务命令时尝试复制项目 runner/start command。不会执行。"));
    quick_copy_debug_ = make_quick(
        QStringLiteral("复制诊断"),
        QStringLiteral("把当前项目、状态、runner、关键路径和选中任务信息复制到剪贴板，便于发给 Agent 排查。不会复制密码或 token。"));

    auto* project_actions = new QHBoxLayout();
    project_actions->setSpacing(7);
    for (auto* button : {
             quick_refresh_, quick_monitor_dir_, quick_status_, quick_log_,
             quick_task_dir_, quick_result_, quick_takeovers_}) {
        project_actions->addWidget(button);
    }
    project_actions->addStretch();
    quick_layout->addLayout(project_actions);

    auto* system_actions = new QHBoxLayout();
    system_actions->setSpacing(7);
    for (auto* button : {
             quick_registry_, quick_hub_data_, quick_job_root_,
             quick_copy_command_, quick_copy_debug_}) {
        system_actions->addWidget(button);
    }
    system_actions->addStretch();
    quick_layout->addLayout(system_actions);

    connect(quick_refresh_, &QPushButton::clicked, this, [this] {
        this->refresh();
        if (quick_feedback_) quick_feedback_->setText(QStringLiteral("已刷新"));
        QTimer::singleShot(2200, this, [this] {
            if (quick_feedback_) quick_feedback_->setText(QStringLiteral("只读安全操作"));
        });
    });
    connect(quick_monitor_dir_, &QPushButton::clicked, this, [this] {
        open_quick_target("monitor_dir");
    });
    connect(quick_status_, &QPushButton::clicked, this, [this] {
        open_quick_target("status");
    });
    connect(quick_log_, &QPushButton::clicked, this, [this] {
        open_quick_target("log");
    });
    connect(quick_task_dir_, &QPushButton::clicked, this, [this] {
        open_quick_target("task_dir");
    });
    connect(quick_result_, &QPushButton::clicked, this, [this] {
        open_quick_target("result");
    });
    connect(quick_takeovers_, &QPushButton::clicked, this, [this] {
        if (tabs_) tabs_->setCurrentIndex(3);
    });
    connect(quick_registry_, &QPushButton::clicked, this, [this] {
        open_quick_target("registry");
    });
    connect(quick_hub_data_, &QPushButton::clicked, this, [this] {
        open_quick_target("hub_data");
    });
    connect(quick_job_root_, &QPushButton::clicked, this, [this] {
        open_quick_target("job_root");
    });
    connect(quick_copy_command_, &QPushButton::clicked, this, [this] {
        const auto* meta = selected_task_meta();
        std::string command = meta ? s(meta->if_contains("command")) : std::string{};
        const auto* project = current_project();
        if (command.empty() && project) command = s(project->if_contains("action"));
        if (command.empty() && project) {
            if (const auto* runner = object(project->if_contains("runner")))
                command = s(runner->if_contains("start_cmd"));
        }
        if (!command.empty()) {
            QApplication::clipboard()->setText(q(command));
            if (quick_feedback_) quick_feedback_->setText(QStringLiteral("命令已复制"));
            QTimer::singleShot(2200, this, [this] {
                if (quick_feedback_) quick_feedback_->setText(QStringLiteral("只读安全操作"));
            });
        }
    });
    connect(quick_copy_debug_, &QPushButton::clicked, this, [this] {
        copy_debug_summary();
    });

    main_layout->addWidget(quick_bar);

    tabs_ = new QTabWidget(main);
    tabs_->setObjectName(QStringLiteral("mainTabs"));
    tabs_->setDocumentMode(true);
    tabs_->tabBar()->setExpanding(false);

    // Cross-project operations overview.
    auto* overview_tab = new QWidget(tabs_);
    auto* overview_layout = new QVBoxLayout(overview_tab);
    overview_layout->setContentsMargins(6, 6, 6, 6);
    overview_layout->setSpacing(8);

    auto* overview_head = new QHBoxLayout();
    auto* overview_title = new QLabel(QStringLiteral("总览"), overview_tab);
    overview_title->setObjectName(QStringLiteral("pageSectionTitle"));
    QFont overview_title_font = overview_title->font();
    overview_title_font.setBold(true);
    overview_title_font.setPointSize(overview_title_font.pointSize() + 2);
    overview_title->setFont(overview_title_font);
    overview_head->addWidget(overview_title);
    overview_counts_ = new QLabel(overview_tab);
    overview_counts_->setObjectName(QStringLiteral("summaryText"));
    overview_counts_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    overview_head->addWidget(overview_counts_, 1);
    overview_head->addWidget(info_button(
        QStringLiteral("总览汇总 normalized project snapshot、Protocol v1 事件和 takeover 记录；"
                       "不会从 raw log 自己推断业务状态。双击表格行可下钻到对应项目。"),
        overview_tab));
    overview_layout->addLayout(overview_head);

    auto* overview_projects_label = new QLabel(QStringLiteral("项目状态"), overview_tab);
    overview_projects_label->setObjectName(QStringLiteral("sectionLabel"));
    QFont overview_section_font = overview_projects_label->font();
    overview_section_font.setBold(true);
    overview_projects_label->setFont(overview_section_font);
    overview_layout->addWidget(overview_projects_label);

    overview_projects_ = new QTableWidget(overview_tab);
    configure_table(overview_projects_);
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
    overview_attention_label->setObjectName(QStringLiteral("sectionLabel"));
    overview_attention_label->setFont(overview_section_font);
    overview_attention_head->addWidget(overview_attention_label);
    overview_attention_head->addStretch();
    overview_attention_head->addWidget(info_button(
        QStringLiteral("这里只放需要用户/主 Agent 关注的项目级事项，以及监控错误或状态过期。"
                       "普通运行日志不会进入这里。"),
        overview_tab));
    overview_layout->addLayout(overview_attention_head);

    overview_attention_ = new QTableWidget(overview_tab);
    configure_table(overview_attention_);
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
    overview_agent_label->setObjectName(QStringLiteral("sectionLabel"));
    overview_agent_label->setFont(overview_section_font);
    overview_agent_head->addWidget(overview_agent_label);
    overview_agent_head->addStretch();
    overview_agent_head->addWidget(info_button(
        QStringLiteral("跨项目汇总最近的 child-agent/takeover 记录。"
                       "双击后进入对应项目的“后台处理记录”，再双击可打开原始证据文件。"),
        overview_tab));
    overview_layout->addLayout(overview_agent_head);

    overview_agents_ = new QTableWidget(overview_tab);
    configure_table(overview_agents_);
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
        const auto source = item->data(Qt::UserRole + 1).toString();
        if (id.empty()) return;
        select_project(id);
        tabs_->setCurrentIndex(source == QStringLiteral("event") ? 2 : 1);
    });
    connect(overview_agents_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        auto* item = overview_agents_->item(row, 1);
        if (!item) return;
        const auto id = item->data(Qt::UserRole).toString().toUtf8().toStdString();
        const auto source = item->data(Qt::UserRole + 1).toString();
        if (id.empty()) return;
        select_project(id);
        tabs_->setCurrentIndex(source == QStringLiteral("event") ? 2 : 3);
    });
    tabs_->addTab(overview_tab, QStringLiteral("总览"));

    // Progress + task detail.
    auto* progress_tab = new QWidget(tabs_);
    auto* progress_layout = new QVBoxLayout(progress_tab);
    progress_layout->setContentsMargins(6, 6, 6, 6);

    auto* progress_head = new QHBoxLayout();
    auto* progress_label = new QLabel(QStringLiteral("任务进度"), progress_tab);
    progress_label->setObjectName(QStringLiteral("sectionLabel"));
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
    configure_table(progress_);
    progress_->setSelectionBehavior(QAbstractItemView::SelectRows);
    progress_->setSelectionMode(QAbstractItemView::SingleSelection);
    progress_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    progress_->verticalHeader()->setVisible(false);
    progress_->horizontalHeader()->setStretchLastSection(true);
    progress_left_layout->addWidget(progress_, 1);
    notes_ = new QTextEdit(progress_left);
    notes_->setObjectName(QStringLiteral("notesPanel"));
    notes_->setReadOnly(true);
    notes_->setMaximumHeight(150);
    progress_left_layout->addWidget(notes_);

    auto* detail = new QFrame(split);
    detail->setObjectName(QStringLiteral("detailCard"));
    detail->setFrameShape(QFrame::NoFrame);
    detail->setMinimumWidth(360);
    auto* detail_layout = new QVBoxLayout(detail);
    detail_layout->setContentsMargins(14, 14, 14, 14);
    detail_layout->setSpacing(10);
    task_title_ = new QLabel(QStringLiteral("未选择任务"), detail);
    task_title_->setObjectName(QStringLiteral("cardTitle"));
    QFont task_font = task_title_->font();
    task_font.setBold(true);
    task_font.setPointSize(task_font.pointSize() + 2);
    task_title_->setFont(task_font);
    detail_layout->addWidget(task_title_);
    task_sub_ = new QLabel(detail);
    task_sub_->setObjectName(QStringLiteral("mutedText"));
    detail_layout->addWidget(task_sub_);
    task_paths_ = new QLabel(detail);
    task_paths_->setObjectName(QStringLiteral("codeBlock"));
    task_paths_->setWordWrap(true);
    task_paths_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    task_paths_->setFont(mono);
    detail_layout->addWidget(task_paths_);

    auto* task_buttons = new QHBoxLayout();
    open_task_ = new QPushButton(QStringLiteral("打开任务"), detail);
    open_task_->setProperty("role", QStringLiteral("primary"));
    open_log_ = new QPushButton(QStringLiteral("日志"), detail);
    open_log_->setProperty("role", QStringLiteral("secondary"));
    open_result_ = new QPushButton(QStringLiteral("结果"), detail);
    open_result_->setProperty("role", QStringLiteral("secondary"));
    copy_command_ = new QPushButton(QStringLiteral("复制命令"), detail);
    copy_command_->setProperty("role", QStringLiteral("secondary"));
    open_task_->setToolTip(QStringLiteral("打开 open_path / path / workdir；没有目录时再尝试日志或结果。"));
    open_log_->setToolTip(QStringLiteral("打开当前任务登记的主要日志文件。"));
    open_result_->setToolTip(QStringLiteral("打开当前任务登记的结果文件。"));
    copy_command_->setToolTip(QStringLiteral("只把登记的命令复制到剪贴板，不会执行。"));
    for (auto* button : {open_task_, open_log_, open_result_, copy_command_}) task_buttons->addWidget(button);
    detail_layout->addLayout(task_buttons);

    auto* params_label = new QLabel(QStringLiteral("参数"), detail);
    params_label->setObjectName(QStringLiteral("sectionLabel"));
    params_label->setFont(section_font);
    detail_layout->addWidget(params_label);
    params_ = new QTableWidget(detail);
    configure_table(params_);
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

    // First-class Issue / Agent Event timeline.
    auto* event_tab = new QWidget(tabs_);
    auto* event_layout = new QVBoxLayout(event_tab);
    event_layout->setContentsMargins(6, 6, 6, 6);
    event_layout->setSpacing(8);

    auto* event_head = new QHBoxLayout();
    auto* event_label = new QLabel(QStringLiteral("问题与 Agent"), event_tab);
    event_label->setObjectName(QStringLiteral("pageSectionTitle"));
    event_label->setFont(overview_title_font);
    event_head->addWidget(event_label);
    event_status_ = new QLabel(QStringLiteral("等待协议事件"), event_tab);
    event_status_->setObjectName(QStringLiteral("summaryText"));
    event_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    event_head->addWidget(event_status_, 1);
    event_head->addWidget(info_button(
        QStringLiteral("读取 MONITOR_HUB_DATA/events/<project_id>.jsonl 的 Protocol v1 事件。"
                       "Issue、Agent 动作和恢复验证按稳定 ID 关联；"
                       "Agent 动作完成不等于 Issue 已解决，必须有后续恢复验证/解决事件。"),
        event_tab));
    event_layout->addLayout(event_head);

    auto* recovery_label =
        new QLabel(QStringLiteral("自动恢复流程"), event_tab);
    recovery_label->setObjectName(QStringLiteral("sectionLabel"));
    recovery_label->setFont(section_font);
    event_layout->addWidget(recovery_label);

    recovery_flow_ = new QTableWidget(event_tab);
    configure_table(recovery_flow_);
    recovery_flow_->setColumnCount(6);
    recovery_flow_->setHorizontalHeaderLabels({
        QStringLiteral("任务"),
        QStringLiteral("Issue"),
        QStringLiteral("恢复链条"),
        QStringLiteral("当前阶段"),
        QStringLiteral("下一步"),
        QStringLiteral("权限"),
    });
    recovery_flow_->setSelectionBehavior(QAbstractItemView::SelectRows);
    recovery_flow_->setSelectionMode(QAbstractItemView::SingleSelection);
    recovery_flow_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    recovery_flow_->verticalHeader()->setVisible(false);
    recovery_flow_->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Stretch);
    recovery_flow_->horizontalHeader()->setSectionResizeMode(
        4, QHeaderView::Stretch);
    recovery_flow_->setMinimumHeight(120);
    recovery_flow_->setMaximumHeight(240);
    event_layout->addWidget(recovery_flow_);

    auto* issues_label = new QLabel(QStringLiteral("Issue 状态"), event_tab);
    issues_label->setObjectName(QStringLiteral("sectionLabel"));
    issues_label->setFont(section_font);
    event_layout->addWidget(issues_label);

    issues_ = new QTableWidget(event_tab);
    configure_table(issues_);
    issues_->setColumnCount(6);
    issues_->setHorizontalHeaderLabels({
        QStringLiteral("任务"),
        QStringLiteral("Issue"),
        QStringLiteral("阶段"),
        QStringLiteral("权限"),
        QStringLiteral("当前动作"),
        QStringLiteral("摘要"),
    });
    issues_->setSelectionBehavior(QAbstractItemView::SelectRows);
    issues_->setSelectionMode(QAbstractItemView::SingleSelection);
    issues_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    issues_->verticalHeader()->setVisible(false);
    issues_->horizontalHeader()->setStretchLastSection(true);
    event_layout->addWidget(issues_, 1);

    auto* timeline_label = new QLabel(QStringLiteral("事件时间线"), event_tab);
    timeline_label->setObjectName(QStringLiteral("sectionLabel"));
    timeline_label->setFont(section_font);
    event_layout->addWidget(timeline_label);

    event_timeline_ = new QTableWidget(event_tab);
    configure_table(event_timeline_);
    event_timeline_->setColumnCount(5);
    event_timeline_->setHorizontalHeaderLabels({
        QStringLiteral("时间"),
        QStringLiteral("事件"),
        QStringLiteral("任务"),
        QStringLiteral("来源"),
        QStringLiteral("摘要"),
    });
    event_timeline_->setSelectionBehavior(QAbstractItemView::SelectRows);
    event_timeline_->setSelectionMode(QAbstractItemView::SingleSelection);
    event_timeline_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    event_timeline_->verticalHeader()->setVisible(false);
    event_timeline_->horizontalHeader()->setStretchLastSection(true);
    event_layout->addWidget(event_timeline_, 2);

    connect(
        recovery_flow_,
        &QTableWidget::cellDoubleClicked,
        this,
        [this](int row, int) {
            auto* item = recovery_flow_->item(row, 0);
            if (!item) return;
            const auto task_id =
                item->data(Qt::UserRole).toString().toUtf8().toStdString();
            if (task_id.empty()) return;
            selected_task_id_ = task_id;
            render_project();
            if (tabs_) tabs_->setCurrentIndex(1);
        });

    connect(issues_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        auto* item = issues_->item(row, 0);
        if (!item) return;
        const auto task_id = item->data(Qt::UserRole).toString().toUtf8().toStdString();
        if (task_id.empty()) return;
        selected_task_id_ = task_id;
        render_project();
        if (tabs_) tabs_->setCurrentIndex(1);
    });

    tabs_->addTab(event_tab, QStringLiteral("问题与 Agent"));

    auto* takeover_tab = new QWidget(tabs_);
    auto* takeover_layout = new QVBoxLayout(takeover_tab);
    auto* takeover_head = new QHBoxLayout();
    auto* takeover_label = new QLabel(QStringLiteral("后台处理记录"), takeover_tab);
    takeover_label->setObjectName(QStringLiteral("sectionLabel"));
    takeover_head->addWidget(takeover_label);
    takeover_head->addStretch();
    takeover_head->addWidget(info_button(
        QStringLiteral("这里显示 monitor 启动的 Claude takeover 历史。当前 Qt 阶段只读，不在这里启动或修改后台作业。"), takeover_tab));
    takeover_layout->addLayout(takeover_head);
    takeovers_ = new QTableWidget(takeover_tab);
    configure_table(takeovers_);
    takeovers_->setColumnCount(3);
    takeovers_->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("结果"), QStringLiteral("摘要")});
    takeovers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    takeovers_->verticalHeader()->setVisible(false);
    takeovers_->horizontalHeader()->setStretchLastSection(true);
    takeover_layout->addWidget(takeovers_, 1);

    auto* takeover_stream_label =
        new QLabel(QStringLiteral("处理过程（实时尾部）"), takeover_tab);
    takeover_stream_label->setObjectName(QStringLiteral("sectionLabel"));
    takeover_layout->addWidget(takeover_stream_label);
    takeover_stream_ = new QTextEdit(takeover_tab);
    takeover_stream_->setReadOnly(true);
    takeover_stream_->setMinimumHeight(150);
    takeover_stream_->setPlaceholderText(
        QStringLiteral("选择一条后台处理记录后，这里显示其最新读取、工具调用和结果。"
                       "运行中的记录会随 Monitor Hub 刷新自动更新。"));
    takeover_layout->addWidget(takeover_stream_, 1);

    connect(takeovers_, &QTableWidget::cellClicked, this, [this](int, int) {
        render_takeover_stream();
    });
    connect(takeovers_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        auto* item = takeovers_->item(row, 2);
        if (!item) return;
        open_local(item->data(Qt::UserRole).toString().toUtf8().toStdString());
    });
    tabs_->addTab(takeover_tab, QStringLiteral("后台处理记录"));

    auto* result_tab = new QWidget(tabs_);
    auto* result_layout = new QVBoxLayout(result_tab);
    auto* result_head = new QHBoxLayout();
    auto* result_label = new QLabel(QStringLiteral("最终结果"), result_tab);
    result_label->setObjectName(QStringLiteral("sectionLabel"));
    result_head->addWidget(result_label);
    result_head->addStretch();
    result_head->addWidget(info_button(QStringLiteral("双击已生成的结果文件可用系统默认程序打开。"), result_tab));
    result_layout->addLayout(result_head);
    results_ = new QTableWidget(result_tab);
    configure_table(results_);
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

    auto* qa_tab = new QWidget(tabs_);
    auto* qa_layout = new QVBoxLayout(qa_tab);
    auto* qa_head = new QHBoxLayout();
    auto* qa_label = new QLabel(QStringLiteral("项目提问"), qa_tab);
    qa_label->setObjectName(QStringLiteral("sectionLabel"));
    qa_head->addWidget(qa_label);
    qa_status_ = new QLabel(QStringLiteral("只读问答"), qa_tab);
    qa_status_->setObjectName(QStringLiteral("mutedText"));
    qa_head->addWidget(qa_status_);
    qa_head->addStretch();
    qa_head->addWidget(info_button(
        QStringLiteral(
            "提问使用项目登记的 qa_cwd / qa_sources / claude_config_dir。"
            "Claude 只获得 Read、Grep、Glob 工具；禁止 Bash、Edit、Write 和 NotebookEdit。"
            "如果项目登记了 live_query，会先读取实时状态再附到问题上下文。"),
        qa_tab));
    qa_layout->addLayout(qa_head);

    qa_history_ = new QTextEdit(qa_tab);
    qa_history_->setReadOnly(true);
    qa_history_->setPlaceholderText(
        QStringLiteral("这里显示当前项目的只读问答结果。切换项目后不会把答案带到其他项目。"));
    qa_layout->addWidget(qa_history_, 1);

    qa_input_ = new QTextEdit(qa_tab);
    qa_input_->setPlaceholderText(
        QStringLiteral("例如：现在跑到哪一步？哪个任务有问题？最近一次 takeover 做了什么？"));
    qa_input_->setMaximumHeight(110);
    qa_layout->addWidget(qa_input_);

    auto* qa_actions = new QHBoxLayout();
    qa_live_ = new QPushButton(QStringLiteral("刷新实时状态"), qa_tab);
    qa_send_ = new QPushButton(QStringLiteral("发送问题"), qa_tab);
    qa_actions->addWidget(qa_live_);
    qa_actions->addStretch();
    qa_actions->addWidget(qa_send_);
    qa_layout->addLayout(qa_actions);

    connect(qa_live_, &QPushButton::clicked, this, [this] {
        run_project_live_query(false);
    });
    connect(qa_send_, &QPushButton::clicked, this, [this] {
        ask_project();
    });
    tabs_->addTab(qa_tab, QStringLiteral("提问"));

    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
        render_context_header();
    });

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
    event_projections_.clear();
    for (const auto& project : projects_) {
        const auto id = s(project.if_contains("id"));
        snapshots_[id] = snapshot(project, system_, paths_);
        event_projections_[id] =
            load_project_event_projection(paths_, id, 500);
    }
    selected_project_ = keep_project;
    if (selected_project_.empty() || !snapshots_.count(selected_project_))
        selected_project_ = projects_.empty() ? std::string{} : s(projects_.front().if_contains("id"));
    render_overview();
    render_sidebar();
    render_project();
    render_context_header();
    render_claude_cli_status();
    refresh_quick_actions();
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

void QtMainWindow::render_context_header() {
    if (tabs_ && tabs_->currentIndex() == 0) {
        const auto model = build_overview_model(projects_, snapshots_, event_projections_, 24);
        const int needs_action =
            model.attention_projects + model.error_projects + model.stale_projects;

        title_->setText(QStringLiteral("Monitor Hub"));
        area_->setText(QStringLiteral("Agent Operations"));

        std::string aggregate_health = "ok";
        if (needs_action > 0) aggregate_health = "attention";
        else if (model.working_projects > 0) aggregate_health = "working";
        else if (model.total_projects > 0 &&
                 model.done_projects == model.total_projects)
            aggregate_health = "done";
        set_health_badge(health_, aggregate_health);

        headline_->setText(
            QStringLiteral("%1 个项目 · %2 个需要处理 · %3 个后台处理中 · %4 个已完成")
                .arg(model.total_projects)
                .arg(needs_action)
                .arg(model.working_projects)
                .arg(model.done_projects));
        runner_->setText(QStringLiteral("自动刷新间隔：60 秒"));
        return;
    }

    const auto* project = current_project();
    const auto* snap = current_snapshot();
    if (!project || !snap) {
        title_->setText(QStringLiteral("Monitor Hub"));
        area_->setText(QStringLiteral("Agent Operations"));
        set_health_badge(health_, "ok");
        headline_->setText(QStringLiteral("暂无可显示的监控项目"));
        runner_->clear();
        return;
    }

    title_->setText(q(s(project->if_contains("name"))));
    area_->setText(q(s(project->if_contains("area"))));
    const auto health = s(snap->if_contains("health"), "ok");
    set_health_badge(health_, health);

    auto headline = s(snap->if_contains("problem"));
    if (headline.empty()) headline = s(snap->if_contains("headline"));
    headline_->setText(q(headline));

    const auto* runner = object(snap->if_contains("runner"));
    runner_->setText(runner ? q(s(runner->if_contains("text"))) : QString{});
}

void QtMainWindow::render_claude_cli_status() {
    if (!claude_cli_state_ || !claude_cli_meta_ ||
        !claude_five_bar_ || !claude_seven_bar_) {
        return;
    }

    const auto status = load_claude_cli_status(system_, paths_);

    QString state;
    const auto now_epoch = QDateTime::currentSecsSinceEpoch();
    const auto observed_age = status.observed_at
        ? now_epoch - static_cast<qint64>(*status.observed_at)
        : std::numeric_limits<qint64>::max();
    if (!status.source.empty() && status.running_processes > 0 &&
        observed_age >= 0 && observed_age <= 120) {
        state = QStringLiteral("● 正在工作");
    } else if (!status.source.empty() && observed_age >= 0 && observed_age <= 600) {
        state = QStringLiteral("◐ 最近活动");
    } else if (status.running_processes > 0) {
        state = QStringLiteral("○ CLI 运行中");
    } else if (!status.source.empty()) {
        state = QStringLiteral("○ 已缓存");
    } else if (status.cli_found) {
        state = QStringLiteral("○ 待接用量");
    } else {
        state = QStringLiteral("○ 未发现");
    }
    claude_cli_state_->setText(state);

    QStringList meta;
    if (!status.model.empty()) meta << q(status.model);
    if (!status.version.empty()) meta << QStringLiteral("v") + q(status.version);
    if (status.running_processes > 0)
        meta << QStringLiteral("%1 个 CLI 进程").arg(status.running_processes);
    else if (status.cli_found)
        meta << QStringLiteral("CLI 已安装");
    if (meta.isEmpty())
        meta << QStringLiteral("等待 Claude CLI");
    claude_cli_meta_->setText(meta.join(QStringLiteral(" · ")));

    QString session_text;
    if (!status.session_name.empty()) {
        session_text = QStringLiteral("会话 · %1").arg(q(status.session_name));
    } else if (!status.session_id.empty()) {
        auto short_id = q(status.session_id);
        if (short_id.size() > 12) short_id = short_id.left(12) + QStringLiteral("…");
        session_text = QStringLiteral("会话 · %1").arg(short_id);
    } else {
        session_text = QStringLiteral("会话 · 等待 statusLine 元数据");
    }
    if (!status.agent_name.empty())
        session_text += QStringLiteral(" · Agent %1").arg(q(status.agent_name));
    else if (!status.agent_type.empty())
        session_text += QStringLiteral(" · Agent %1").arg(q(status.agent_type));
    claude_session_->setText(session_text);

    QStringList activity;
    const auto workspace = !status.project_dir.empty() ? status.project_dir : status.cwd;
    claude_linked_project_id_ =
        match_claude_workspace_project(status, projects_);
    QString linked_project_name;
    if (!claude_linked_project_id_.empty()) {
        for (const auto& project : projects_) {
            const auto id = s(project.if_contains("id"));
            if (id != claude_linked_project_id_) continue;
            linked_project_name = q(s(project.if_contains("name"), id));
            break;
        }
    }

    if (!workspace.empty())
        activity << QStringLiteral("目录 %1").arg(q(workspace));
    if (!linked_project_name.isEmpty())
        activity << QStringLiteral("关联 Monitor 项目 %1").arg(linked_project_name);
    if (!status.recent_tool.empty())
        activity << QStringLiteral("最近工具 %1 · %2")
                        .arg(q(status.recent_tool))
                        .arg(activity_time_text(status.recent_tool_at));
    if (!status.recent_agent.empty())
        activity << QStringLiteral("最近 Agent %1 · %2")
                        .arg(q(status.recent_agent))
                        .arg(activity_time_text(status.recent_agent_at));
    if (!status.git_worktree.empty())
        activity << QStringLiteral("worktree %1").arg(q(status.git_worktree));
    claude_activity_->setText(
        activity.isEmpty() ? QStringLiteral("尚无会话活动元数据")
                           : activity.join(QStringLiteral("\n")));

    set_usage_bar(
        claude_five_bar_,
        claude_five_text_,
        QStringLiteral("5 小时"),
        status.five_hour);
    set_usage_bar(
        claude_seven_bar_,
        claude_seven_text_,
        QStringLiteral("7 天"),
        status.seven_day);

    QString updated = observed_text(status.observed_at);
    if (!status.source.empty()) {
        updated += status.source == "claude_statusline"
            ? QStringLiteral(" · statusLine")
            : QStringLiteral(" · 后台 Claude 快照");
    }
    if (status.context_used_percentage)
        updated += QStringLiteral(" · ctx %1%")
                       .arg(static_cast<int>(*status.context_used_percentage + 0.5));
    claude_updated_->setText(updated);
    claude_updated_->setToolTip(
        QStringLiteral("用量缓存：%1\nClaude 配置：%2\nSession ID：%3\nTranscript：%4")
            .arg(q(status.status_file.string()))
            .arg(q(status.config_dir.string()))
            .arg(q(status.session_id))
            .arg(q(status.transcript_path.string())));

    claude_config_->setEnabled(!status.config_dir.empty());
    claude_project_->setEnabled(!claude_linked_project_id_.empty());
    claude_project_->setToolTip(
        claude_linked_project_id_.empty()
            ? QStringLiteral("当前 Claude workspace 没有匹配到已登记的 Monitor 项目。")
            : QStringLiteral("跳到已匹配的 Monitor 项目：%1").arg(linked_project_name));
    claude_workspace_->setEnabled(
        !status.project_dir.empty() || !status.cwd.empty());
    claude_transcript_->setEnabled(
        !status.transcript_path.empty() &&
        local_exists(status.transcript_path.string()));
}

void QtMainWindow::refresh_quick_actions() {
    const auto* project = current_project();
    const auto* snap = current_snapshot();
    const auto* meta = selected_task_meta();

    auto existing_value = [](const json::object* source,
                             std::initializer_list<const char*> keys) {
        if (!source) return std::string{};
        for (const auto* key : keys) {
            const auto value = s(source->if_contains(key));
            if (local_exists(value)) return value;
        }
        return std::string{};
    };

    auto monitor_dir = existing_value(project, {"dir", "qa_cwd"});
    if (monitor_dir.empty() && project) {
        if (const auto* runner = object(project->if_contains("runner")))
            monitor_dir = existing_value(runner, {"workdir"});
    }
    if (monitor_dir.empty()) {
        const auto status = existing_value(project, {"status_json", "status_md"});
        if (!status.empty()) {
            std::error_code ec;
            const auto parent = std::filesystem::path(status).parent_path();
            if (std::filesystem::exists(parent, ec) && !ec)
                monitor_dir = parent.string();
        }
    }

    const auto status_path = existing_value(project, {"status_json", "status_md"});
    auto log_path = existing_value(meta, {"log"});
    if (log_path.empty()) log_path = existing_value(project, {"log"});
    const auto task_dir = existing_value(meta, {"open_path", "path", "workdir"});

    auto result_path = existing_value(meta, {"result"});
    if (result_path.empty() && snap) {
        if (const auto* results = array(snap->if_contains("results_list"))) {
            for (const auto& value : *results) {
                const auto* pair = array(&value);
                if (!pair || pair->size() < 2) continue;
                const auto path = s(&(*pair)[1]);
                if (local_exists(path)) {
                    result_path = path;
                    break;
                }
            }
        }
    }

    std::string command = meta ? s(meta->if_contains("command")) : std::string{};
    if (command.empty() && project) command = s(project->if_contains("action"));
    if (command.empty() && project) {
        if (const auto* runner = object(project->if_contains("runner")))
            command = s(runner->if_contains("start_cmd"));
    }

    if (quick_refresh_) quick_refresh_->setEnabled(true);
    if (quick_monitor_dir_) quick_monitor_dir_->setEnabled(!monitor_dir.empty());
    if (quick_status_) quick_status_->setEnabled(!status_path.empty());
    if (quick_log_) quick_log_->setEnabled(!log_path.empty());
    if (quick_task_dir_) quick_task_dir_->setEnabled(!task_dir.empty());
    if (quick_result_) quick_result_->setEnabled(!result_path.empty());
    if (quick_takeovers_) quick_takeovers_->setEnabled(project != nullptr);
    if (quick_registry_) {
        std::error_code ec;
        const bool exists = std::filesystem::exists(paths_.registry, ec) && !ec;
        const auto parent = paths_.registry.parent_path();
        std::error_code parent_ec;
        const bool parent_exists =
            !parent.empty() && std::filesystem::exists(parent, parent_ec) && !parent_ec;
        quick_registry_->setEnabled(exists || parent_exists);
    }
    if (quick_hub_data_) quick_hub_data_->setEnabled(local_exists(paths_.hub_data.string()));
    if (quick_job_root_) quick_job_root_->setEnabled(local_exists(paths_.job_root.string()));
    if (quick_copy_command_) quick_copy_command_->setEnabled(!command.empty());
    if (quick_copy_debug_) quick_copy_debug_->setEnabled(project != nullptr);
}

void QtMainWindow::render_overview() {
    if (!overview_counts_ || !overview_projects_ ||
        !overview_attention_ || !overview_agents_) {
        return;
    }

    const auto model = build_overview_model(projects_, snapshots_, event_projections_, 24);
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
        auto* status = new QTableWidgetItem(health_text(project.health));
        emphasize_item(status, health_color(project.health));
        overview_projects_->setItem(row, 1, status);
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
        auto* kind = new QTableWidgetItem(q(item.kind));
        emphasize_item(kind, QColor(QStringLiteral("#9A641F")));
        overview_attention_->setItem(row, 0, kind);
        auto* project = new QTableWidgetItem(q(item.project_name));
        project->setData(Qt::UserRole, q(item.project_id));
        project->setData(Qt::UserRole + 1, q(item.source));
        project->setData(Qt::UserRole + 2, q(item.issue_id));
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
        project->setData(Qt::UserRole + 1, q(item.source));
        overview_agents_->setItem(row, 1, project);
        auto* state = new QTableWidgetItem(agent_state_text(item.state));
        emphasize_item(state, agent_state_color(item.state));
        overview_agents_->setItem(row, 2, state);
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
        item->setSizeHint(QSize(0, 42));
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
    qa_live_cache_.clear();
    if (qa_history_) qa_history_->clear();
    render_project();
    render_context_header();
    refresh_quick_actions();
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
    set_health_badge(health_, health);
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
    render_event_timeline();
    render_takeovers();
    render_results();
    render_project_qa();
    render_context_header();
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
    refresh_quick_actions();
}


void QtMainWindow::render_project_qa() {
    const auto* project = current_project();
    const auto has_project = project != nullptr;
    const auto* live = project ? object(project->if_contains("live_query")) : nullptr;
    const bool has_live = live && array(live->if_contains("cmd")) &&
                          !array(live->if_contains("cmd"))->empty();

    if (qa_live_) qa_live_->setEnabled(has_project && has_live && !qa_process_);
    if (qa_send_) qa_send_->setEnabled(has_project && !qa_process_);
    if (qa_input_) qa_input_->setEnabled(has_project && !qa_process_);
    if (!qa_status_) return;

    if (!has_project) {
        qa_status_->setText(QStringLiteral("未选择项目"));
        return;
    }
    if (qa_process_) {
        qa_status_->setText(QStringLiteral("正在查询…"));
        return;
    }
    const auto sources = array(project->if_contains("qa_sources"));
    qa_status_->setText(
        sources && !sources->empty()
            ? QStringLiteral("只读问答 · 已登记 %1 个来源").arg(sources->size())
            : QStringLiteral("只读问答 · 使用项目目录"));
}

void QtMainWindow::run_project_live_query(bool ask_after) {
    const auto* project = current_project();
    if (!project || qa_process_) return;
    const auto* live = object(project->if_contains("live_query"));
    const auto* cmd = live ? array(live->if_contains("cmd")) : nullptr;
    if (!cmd || cmd->empty()) {
        if (ask_after) start_project_question({});
        else if (qa_status_) qa_status_->setText(QStringLiteral("这个项目没有登记 live_query"));
        return;
    }

    QStringList parts;
    for (const auto& item : *cmd) parts << q(s(&item));
    if (parts.isEmpty() || parts.front().trimmed().isEmpty()) {
        if (ask_after) start_project_question({});
        return;
    }

    qa_process_ = new QProcess(this);
    qa_process_->setProcessChannelMode(QProcess::MergedChannels);
    qa_process_->setProgram(parts.takeFirst());
    qa_process_->setArguments(parts);
    const auto cwd = s(project->if_contains("qa_cwd"), s(project->if_contains("dir")));
    if (!cwd.empty()) qa_process_->setWorkingDirectory(q(cwd));
    if (qa_live_) qa_live_->setEnabled(false);
    if (qa_send_) qa_send_->setEnabled(false);
    if (qa_status_) qa_status_->setText(QStringLiteral("正在读取实时状态…"));

    auto* process = qa_process_;
    connect(process, &QProcess::errorOccurred, this,
        [this, process, ask_after](QProcess::ProcessError error) {
            if (process != qa_process_ || error != QProcess::FailedToStart) return;
            const auto message = QStringLiteral("实时查询启动失败：%1").arg(process->errorString());
            if (qa_history_) qa_history_->append(message);
            qa_process_ = nullptr;
            process->deleteLater();
            render_project_qa();
            if (ask_after) start_project_question(message);
        });
    connect(process, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this,
        [this, process, ask_after](int exit_code, QProcess::ExitStatus exit_status) {
            if (process != qa_process_) return;
            auto output = QString::fromUtf8(process->readAll()).trimmed();
            if (output.size() > 16000) output = output.right(16000);
            if (exit_status != QProcess::NormalExit || exit_code != 0) {
                output = QStringLiteral("实时查询失败（exit %1）：\n%2").arg(exit_code).arg(output);
            }
            qa_live_cache_ = output;
            if (qa_history_ && !output.isEmpty()) {
                qa_history_->append(QStringLiteral("[实时状态]\n%1").arg(output));
            }
            qa_process_ = nullptr;
            process->deleteLater();
            render_project_qa();
            if (ask_after) start_project_question(output);
        });
    process->start();
}

void QtMainWindow::ask_project() {
    if (!qa_input_ || qa_process_) return;
    const auto question = qa_input_->toPlainText().trimmed();
    if (question.isEmpty()) return;
    if (qa_history_) qa_history_->append(QStringLiteral("你：%1").arg(question));
    qa_input_->clear();

    const auto* project = current_project();
    const auto* live = project ? object(project->if_contains("live_query")) : nullptr;
    if (live && array(live->if_contains("cmd")) && !array(live->if_contains("cmd"))->empty())
        run_project_live_query(true);
    else
        start_project_question({});
}

void QtMainWindow::start_project_question(const QString& live_context) {
    const auto* project = current_project();
    const auto* snap = current_snapshot();
    if (!project || !snap || qa_process_ || !qa_history_) return;

    QString question;
    const auto blocks = qa_history_->toPlainText().split(QStringLiteral("\n"));
    for (auto it = blocks.crbegin(); it != blocks.crend(); ++it) {
        if (it->startsWith(QStringLiteral("你："))) {
            question = it->mid(2).trimmed();
            break;
        }
    }
    if (question.isEmpty()) return;

    const auto claude = load_claude_cli_status(system_, paths_);
    if (!claude.cli_found || claude.executable.empty()) {
        qa_history_->append(QStringLiteral("Monitor Hub：未找到 Claude CLI。"));
        if (qa_status_) qa_status_->setText(QStringLiteral("Claude CLI 不可用"));
        return;
    }

    QStringList source_items;
    if (const auto* sources = array(project->if_contains("qa_sources"))) {
        for (const auto& item : *sources) source_items << q(s(&item));
    }
    if (source_items.isEmpty()) {
        const auto dir = s(project->if_contains("dir"));
        if (!dir.empty()) source_items << q(dir);
    }

    const auto project_name = q(s(project->if_contains("name")));
    const auto system_prompt = QStringLiteral(
        "You answer the user's questions about the monitored project \"%1\" and its monitor takeovers. "
        "Answer in Chinese, lead with the answer, keep it short and concrete. "
        "You can only read files with Read, Grep and Glob. Do not modify files, start/stop jobs, submit, commit or push. "
        "If an action is needed, state exactly what should be done and let the user decide. Sources: %2")
        .arg(project_name, source_items.join(QStringLiteral("; ")));

    QStringList context;
    context << QStringLiteral("[Monitor Hub current view]")
            << QStringLiteral("状态：%1").arg(q(s(snap->if_contains("health"))))
            << QStringLiteral("摘要：%1").arg(q(s(snap->if_contains("summary"))))
            << QStringLiteral("问题：%1").arg(q(s(snap->if_contains("problem"), s(snap->if_contains("headline")))));
    if (!live_context.trimmed().isEmpty()) {
        context << QStringLiteral("")
                << QStringLiteral("[Live query]")
                << live_context;
    }
    context << QStringLiteral("")
            << QStringLiteral("[User question]")
            << question;

    QStringList args{
        QStringLiteral("-p"),
        QStringLiteral("--output-format"), QStringLiteral("stream-json"),
        QStringLiteral("--verbose"),
        QStringLiteral("--model"), QStringLiteral("opus"),
        QStringLiteral("--permission-mode"), QStringLiteral("default"),
        QStringLiteral("--setting-sources"), QStringLiteral("project"),
        QStringLiteral("--strict-mcp-config"),
        QStringLiteral("--tools"), QStringLiteral("Read,Grep,Glob"),
        QStringLiteral("--disallowedTools"), QStringLiteral("Bash,Edit,Write,NotebookEdit"),
        QStringLiteral("--append-system-prompt"), system_prompt,
        QStringLiteral("--allowedTools"),
        QStringLiteral("Read"), QStringLiteral("Grep"), QStringLiteral("Glob"),
        context.join(QStringLiteral("\n"))
    };

    qa_process_ = new QProcess(this);
    qa_process_->setProcessChannelMode(QProcess::MergedChannels);
    const auto claude_program = q(claude.executable.string());
#ifdef Q_OS_WIN
    const auto suffix = QFileInfo(claude_program).suffix().toLower();
    if (suffix == QStringLiteral("cmd") || suffix == QStringLiteral("bat")) {
        qa_process_->setProgram(
            QProcessEnvironment::systemEnvironment().value(
                QStringLiteral("COMSPEC"),
                QStringLiteral("cmd.exe")));
        args.prepend(claude_program);
        args.prepend(QStringLiteral("/c"));
    } else {
        qa_process_->setProgram(claude_program);
    }
#else
    qa_process_->setProgram(claude_program);
#endif
    qa_process_->setArguments(args);
    const auto cwd = s(project->if_contains("qa_cwd"), s(project->if_contains("dir")));
    if (!cwd.empty()) qa_process_->setWorkingDirectory(q(cwd));

    auto env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("CLAUDE_CONFIG_DIR"));
    const auto config = s(project->if_contains("claude_config_dir"));
    if (!config.empty()) env.insert(QStringLiteral("CLAUDE_CONFIG_DIR"), q(config));
    qa_process_->setProcessEnvironment(env);

    if (qa_live_) qa_live_->setEnabled(false);
    if (qa_send_) qa_send_->setEnabled(false);
    if (qa_input_) qa_input_->setEnabled(false);
    if (qa_status_) qa_status_->setText(QStringLiteral("Claude 正在只读分析…"));

    auto* process = qa_process_;
    connect(process, &QProcess::errorOccurred, this,
        [this, process](QProcess::ProcessError error) {
            if (process != qa_process_ || error != QProcess::FailedToStart) return;
            qa_history_->append(QStringLiteral("Monitor Hub：Claude 启动失败：%1").arg(process->errorString()));
            qa_process_ = nullptr;
            process->deleteLater();
            render_project_qa();
        });
    connect(process, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this,
        [this, process](int exit_code, QProcess::ExitStatus exit_status) {
            if (process != qa_process_) return;
            const auto raw = QString::fromUtf8(process->readAll());
            QString answer;
            const auto lines = raw.split(QLatin1Char('\n'));
            for (const auto& line : lines) {
                boost::system::error_code error;
                const auto parsed = json::parse(line.toUtf8().toStdString(), error);
                if (error || !parsed.is_object()) continue;
                const auto& obj = parsed.as_object();
                if (s(obj.if_contains("type")) != "result") continue;
                answer = q(s(obj.if_contains("result")));
            }
            if (answer.trimmed().isEmpty()) {
                answer = exit_status == QProcess::NormalExit && exit_code == 0
                    ? raw.trimmed().right(6000)
                    : QStringLiteral("Claude 问答失败（exit %1）：\n%2")
                          .arg(exit_code)
                          .arg(raw.trimmed().right(3000));
            }
            qa_history_->append(QStringLiteral("Claude：%1").arg(answer));
            qa_process_ = nullptr;
            process->deleteLater();
            render_project_qa();
        });
    process->start();
}

void QtMainWindow::open_new_monitor_dialog() {
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("新建监控任务"));
    dialog->resize(880, 720);

    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("新建监控任务"), dialog);
    title->setObjectName(QStringLiteral("pageTitle"));
    layout->addWidget(title);

    auto* help = new QLabel(
        QStringLiteral("至少填写“项目名称”和“项目目录”。后台 Claude 会先检查项目现状，"
                       "避免重复建立监控；允许/禁止操作会作为权限边界写入 setup prompt。"),
        dialog);
    help->setWordWrap(true);
    help->setObjectName(QStringLiteral("mutedText"));
    layout->addWidget(help);

    auto* editor = new QTextEdit(dialog);
    editor->setPlainText(q(setup_request_template()));
    editor->setAcceptRichText(false);
    layout->addWidget(editor, 1);

    auto* buttons = new QHBoxLayout();
    auto* example = new QPushButton(QStringLiteral("填入示例"), dialog);
    auto* copy = new QPushButton(QStringLiteral("复制"), dialog);
    auto* cancel = new QPushButton(QStringLiteral("取消"), dialog);
    auto* submit =
        new QPushButton(QStringLiteral("交给后台 Claude 办理"), dialog);
    submit->setDefault(true);
    buttons->addWidget(example);
    buttons->addWidget(copy);
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(submit);
    layout->addLayout(buttons);

    connect(example, &QPushButton::clicked, dialog, [editor] {
        editor->setPlainText(q(setup_request_example()));
    });
    connect(copy, &QPushButton::clicked, dialog, [editor] {
        QApplication::clipboard()->setText(editor->toPlainText());
    });
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);

    connect(submit, &QPushButton::clicked, dialog,
            [this, dialog, editor, submit] {
        const auto body_q = editor->toPlainText().trimmed();
        const auto body =
            body_q.toUtf8().toStdString();
        const auto fields = parse_setup_request_fields(body);
        if (!fields) {
            QMessageBox::warning(
                dialog,
                QStringLiteral("还差一点"),
                QStringLiteral("至少填写“项目名称”和“项目目录（本机路径）”。"));
            return;
        }

        const auto runtime = setup_agent_runtime_from_env();
        const auto runtime_errors = validate_setup_agent_runtime(runtime);
        if (!runtime_errors.empty()) {
            QStringList lines;
            for (const auto& error : runtime_errors)
                lines << QStringLiteral("• ") + q(error);
            QMessageBox::critical(
                dialog,
                QStringLiteral("后台 Agent 环境未就绪"),
                QStringLiteral("请求还没有提交。请先修好下面的本地依赖：\n\n") +
                    lines.join(QStringLiteral("\n")));
            return;
        }

        const auto answer = QMessageBox::question(
            dialog,
            QStringLiteral("交给后台 Claude"),
            QStringLiteral(
                "后台 Claude 会按这份说明设置监控、登记到总台并启动监控。\n\n"
                "它可以执行你在“允许监控自动做的操作”中授权的动作；"
                "超出范围的决定必须升级给你/主 Agent。\n\n确定提交吗？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) return;

        SetupRequestLaunch launch;
        try {
            launch = prepare_setup_request(
                paths_,
                body,
                fs::path(fields->workdir));
        } catch (const std::exception& error) {
            QMessageBox::critical(
                dialog,
                QStringLiteral("提交失败"),
                q(error.what()));
            return;
        }

        submit->setEnabled(false);
        if (new_monitor_) new_monitor_->setEnabled(false);
        if (quick_feedback_)
            quick_feedback_->setText(QStringLiteral("正在提交新监控任务…"));

        auto* process = new QProcess(this);
        process->setProcessChannelMode(QProcess::MergedChannels);
        process->setProgram(q(launch.runtime.powershell.string()));
        QStringList args;
        for (const auto& arg : launch.arguments) args << q(arg);
        process->setArguments(args);

        const auto job_name = q(launch.job_name);
        const auto request_file = q(launch.request_file.string());

        connect(
            process,
            &QProcess::errorOccurred,
            this,
            [this, process, request_file](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart) return;
                if (new_monitor_) new_monitor_->setEnabled(true);
                if (quick_feedback_)
                    quick_feedback_->setText(
                        QStringLiteral("后台 Agent 启动失败"));
                QMessageBox::critical(
                    this,
                    QStringLiteral("后台 Agent 启动失败"),
                    QStringLiteral(
                        "请求文件已保留，但 PowerShell/detach helper 没有启动。\n\n"
                        "请求：%1\n\n%2")
                        .arg(request_file)
                        .arg(process->errorString()));
                process->deleteLater();
            });

        connect(
            process,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this, process, job_name, request_file](
                int exit_code,
                QProcess::ExitStatus exit_status) {
                const auto output =
                    QString::fromUtf8(process->readAll()).trimmed();
                if (new_monitor_) new_monitor_->setEnabled(true);

                if (exit_status != QProcess::NormalExit || exit_code != 0) {
                    if (quick_feedback_)
                        quick_feedback_->setText(
                            QStringLiteral("新监控任务提交失败"));
                    QMessageBox::critical(
                        this,
                        QStringLiteral("提交失败"),
                        QStringLiteral(
                            "请求文件已保留，但 detached setup agent 没有正常启动。\n\n"
                            "请求：%1\n\n%2")
                            .arg(request_file)
                            .arg(output.right(1600)));
                    process->deleteLater();
                    return;
                }

                if (quick_feedback_)
                    quick_feedback_->setText(
                        QStringLiteral("已提交：") + job_name);
                refresh();
                select_project("hub-setup");
                if (tabs_) tabs_->setCurrentIndex(1);
                QMessageBox::information(
                    this,
                    QStringLiteral("已提交"),
                    QStringLiteral(
                        "已交给后台 Claude（%1）。\n"
                        "“新任务办理”会显示办理进度和需要你决定的事项。")
                        .arg(job_name));
                process->deleteLater();
            });

        process->start();
        dialog->accept();
    });

    dialog->open();
}

void QtMainWindow::copy_claude_statusline_setup() {
    const auto bridge_command = claude_bridge_command();
    const auto command =
        QStringLiteral("/statusline 请把下面命令接入 Claude Code 状态栏：%1。"
                       "如果我已经配置了 statusLine，请保留并合并原有状态栏行为，不要静默覆盖；"
                       "确保这个 bridge 的 stdout 仍作为可见状态栏输出。")
            .arg(bridge_command);
    QApplication::clipboard()->setText(command);
    if (claude_updated_)
        claude_updated_->setText(QStringLiteral("接入指令已复制，粘贴到 Claude CLI"));
    QTimer::singleShot(3200, this, [this] { render_claude_cli_status(); });
}

void QtMainWindow::open_claude_config() {
    const auto status = load_claude_cli_status(system_, paths_);
    if (status.config_dir.empty()) return;
    const auto settings = status.config_dir / "settings.json";
    if (local_exists(settings.string())) open_local(settings.string());
    else if (local_exists(status.config_dir.string())) open_local(status.config_dir.string());
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

void QtMainWindow::open_quick_target(const std::string& kind) {
    const auto* project = current_project();
    const auto* snap = current_snapshot();
    const auto* meta = selected_task_meta();

    auto existing_value = [](const json::object* source,
                             std::initializer_list<const char*> keys) {
        if (!source) return std::string{};
        for (const auto* key : keys) {
            const auto value = s(source->if_contains(key));
            if (local_exists(value)) return value;
        }
        return std::string{};
    };

    std::string target;
    if (kind == "monitor_dir") {
        target = existing_value(project, {"dir", "qa_cwd"});
        if (target.empty() && project) {
            if (const auto* runner = object(project->if_contains("runner")))
                target = existing_value(runner, {"workdir"});
        }
        if (target.empty()) {
            const auto status = existing_value(project, {"status_json", "status_md"});
            if (!status.empty()) {
                std::error_code ec;
                const auto parent = std::filesystem::path(status).parent_path();
                if (std::filesystem::exists(parent, ec) && !ec)
                    target = parent.string();
            }
        }
    } else if (kind == "status") {
        target = existing_value(project, {"status_json", "status_md"});
    } else if (kind == "log") {
        target = existing_value(meta, {"log"});
        if (target.empty()) target = existing_value(project, {"log"});
    } else if (kind == "task_dir") {
        target = existing_value(meta, {"open_path", "path", "workdir"});
    } else if (kind == "result") {
        target = existing_value(meta, {"result"});
        if (target.empty() && snap) {
            if (const auto* results = array(snap->if_contains("results_list"))) {
                for (const auto& value : *results) {
                    const auto* pair = array(&value);
                    if (!pair || pair->size() < 2) continue;
                    const auto path = s(&(*pair)[1]);
                    if (local_exists(path)) {
                        target = path;
                        break;
                    }
                }
            }
        }
    } else if (kind == "registry") {
        if (local_exists(paths_.registry.string())) {
            target = paths_.registry.string();
        } else {
            std::error_code ec;
            const auto parent = paths_.registry.parent_path();
            if (!parent.empty() && std::filesystem::exists(parent, ec) && !ec)
                target = parent.string();
        }
    } else if (kind == "hub_data") {
        if (local_exists(paths_.hub_data.string())) target = paths_.hub_data.string();
    } else if (kind == "job_root") {
        if (local_exists(paths_.job_root.string())) target = paths_.job_root.string();
    }

    if (!target.empty()) open_local(target);
}

void QtMainWindow::copy_task_command() {
    const auto* meta = selected_task_meta();
    if (!meta) return;
    const auto command = s(meta->if_contains("command"));
    if (!command.empty()) QApplication::clipboard()->setText(q(command));
}

void QtMainWindow::copy_debug_summary() {
    const auto* project = current_project();
    const auto* snap = current_snapshot();
    if (!project || !snap) return;

    QStringList lines;
    lines << QStringLiteral("Monitor Hub debug context");
    lines << QStringLiteral("project_id=%1").arg(q(s(project->if_contains("id"))));
    lines << QStringLiteral("project_name=%1").arg(q(s(project->if_contains("name"))));
    lines << QStringLiteral("area=%1").arg(q(s(project->if_contains("area"))));
    lines << QStringLiteral("adapter=%1").arg(q(s(project->if_contains("adapter"))));
    lines << QStringLiteral("health=%1").arg(q(s(snap->if_contains("health"))));

    auto headline = s(snap->if_contains("problem"));
    if (headline.empty()) headline = s(snap->if_contains("headline"));
    lines << QStringLiteral("headline=%1").arg(q(headline));

    if (const auto* runner = object(snap->if_contains("runner")))
        lines << QStringLiteral("runner=%1").arg(q(s(runner->if_contains("text"))));

    for (const auto& pair : std::vector<std::pair<const char*, QString>>{
             {"dir", QStringLiteral("monitor_dir")},
             {"status_json", QStringLiteral("status_json")},
             {"status_md", QStringLiteral("status_md")},
             {"log", QStringLiteral("project_log")},
             {"qa_cwd", QStringLiteral("qa_cwd")}}) {
        const auto value = s(project->if_contains(pair.first));
        if (!value.empty()) lines << pair.second + QStringLiteral("=") + q(value);
    }

    lines << QStringLiteral("registry=%1").arg(q(paths_.registry.string()));
    lines << QStringLiteral("hub_data=%1").arg(q(paths_.hub_data.string()));
    lines << QStringLiteral("job_root=%1").arg(q(paths_.job_root.string()));

    const auto claude = load_claude_cli_status(system_, paths_);
    lines << QStringLiteral("claude_cli_found=%1")
                 .arg(claude.cli_found ? QStringLiteral("true") : QStringLiteral("false"));
    lines << QStringLiteral("claude_cli_processes=%1").arg(claude.running_processes);
    if (!claude.source.empty())
        lines << QStringLiteral("claude_usage_source=%1").arg(q(claude.source));
    if (!claude.model.empty())
        lines << QStringLiteral("claude_model=%1").arg(q(claude.model));
    if (!claude.session_id.empty())
        lines << QStringLiteral("claude_session_id=%1").arg(q(claude.session_id));
    if (!claude.session_name.empty())
        lines << QStringLiteral("claude_session_name=%1").arg(q(claude.session_name));
    if (!claude.project_dir.empty())
        lines << QStringLiteral("claude_project_dir=%1").arg(q(claude.project_dir));
    else if (!claude.cwd.empty())
        lines << QStringLiteral("claude_cwd=%1").arg(q(claude.cwd));
    if (!claude.git_worktree.empty())
        lines << QStringLiteral("claude_git_worktree=%1").arg(q(claude.git_worktree));
    if (!claude.agent_name.empty())
        lines << QStringLiteral("claude_agent_name=%1").arg(q(claude.agent_name));
    if (!claude.agent_type.empty())
        lines << QStringLiteral("claude_agent_type=%1").arg(q(claude.agent_type));
    if (!claude.recent_tool.empty())
        lines << QStringLiteral("claude_recent_tool=%1").arg(q(claude.recent_tool));
    if (!claude.recent_agent.empty())
        lines << QStringLiteral("claude_recent_agent=%1").arg(q(claude.recent_agent));
    if (claude.five_hour.used_percentage)
        lines << QStringLiteral("claude_5h_used_percent=%1")
                     .arg(*claude.five_hour.used_percentage, 0, 'f', 1);
    if (claude.five_hour.resets_at)
        lines << QStringLiteral("claude_5h_resets_at=%1")
                     .arg(static_cast<qint64>(*claude.five_hour.resets_at));
    if (claude.seven_day.used_percentage)
        lines << QStringLiteral("claude_7d_used_percent=%1")
                     .arg(*claude.seven_day.used_percentage, 0, 'f', 1);
    if (claude.seven_day.resets_at)
        lines << QStringLiteral("claude_7d_resets_at=%1")
                     .arg(static_cast<qint64>(*claude.seven_day.resets_at));

    if (const auto* meta = selected_task_meta()) {
        const auto task_id = s(meta->if_contains("task_id"));
        if (!task_id.empty()) lines << QStringLiteral("task_id=%1").arg(q(task_id));
        for (const auto& pair : std::vector<std::pair<const char*, QString>>{
                 {"job_id", QStringLiteral("job_id")},
                 {"host", QStringLiteral("host")},
                 {"open_path", QStringLiteral("task_open_path")},
                 {"path", QStringLiteral("task_path")},
                 {"workdir", QStringLiteral("task_workdir")},
                 {"log", QStringLiteral("task_log")},
                 {"result", QStringLiteral("task_result")}}) {
            const auto value = s(meta->if_contains(pair.first));
            if (!value.empty()) lines << pair.second + QStringLiteral("=") + q(value);
        }
    }

    QApplication::clipboard()->setText(lines.join(QStringLiteral("\n")));
    if (quick_feedback_) quick_feedback_->setText(QStringLiteral("诊断上下文已复制"));
    QTimer::singleShot(2600, this, [this] {
        if (quick_feedback_) quick_feedback_->setText(QStringLiteral("只读安全操作"));
    });
}

void QtMainWindow::render_event_timeline() {
    if (!event_status_ || !recovery_flow_ || !issues_ || !event_timeline_)
        return;

    recovery_flow_->setRowCount(0);
    issues_->setRowCount(0);
    event_timeline_->setRowCount(0);

    const auto found = event_projections_.find(selected_project_);
    if (found == event_projections_.end()) {
        event_status_->setText(QStringLiteral("当前项目没有事件投影"));
        event_status_->setToolTip(QString{});
        return;
    }

    const auto& projection = found->second;
    std::size_t unresolved = 0;
    std::size_t waiting_verification = 0;
    std::size_t waiting_decision = 0;
    for (const auto& issue : projection.issues) {
        if (!issue.resolved) ++unresolved;
        const auto stage = issue_recovery_stage(issue);
        if (stage == "waiting_verification" ||
            stage == "recovery_verification")
            ++waiting_verification;
        if (stage == "needs_user") ++waiting_decision;
    }

    event_status_->setText(
        QStringLiteral(
            "未解决 %1 · 等待恢复验证 %2 · 等待决策 %3 · 协议事件 %4")
            .arg(static_cast<qulonglong>(unresolved))
            .arg(static_cast<qulonglong>(waiting_verification))
            .arg(static_cast<qulonglong>(waiting_decision))
            .arg(static_cast<qulonglong>(projection.events.size())));

    QStringList diagnostics;
    diagnostics
        << QStringLiteral("事件文件：%1")
               .arg(q(projection.source_path.string()))
        << QStringLiteral("重复事件：%1")
               .arg(static_cast<qulonglong>(projection.duplicate_events))
        << QStringLiteral("无效事件：%1")
               .arg(static_cast<qulonglong>(projection.malformed_lines));
    for (const auto& item : projection.diagnostics)
        diagnostics << QStringLiteral("• ") + q(item);
    event_status_->setToolTip(diagnostics.join(QStringLiteral("\n")));

    std::vector<const IssueProjection*> issue_rows;
    issue_rows.reserve(projection.issues.size());
    for (const auto& issue : projection.issues)
        issue_rows.push_back(&issue);
    std::stable_sort(
        issue_rows.begin(),
        issue_rows.end(),
        [](const IssueProjection* lhs, const IssueProjection* rhs) {
            if (lhs->resolved != rhs->resolved) return !lhs->resolved;
            return lhs->last_event_at > rhs->last_event_at;
        });

    recovery_flow_->setRowCount(
        static_cast<int>(issue_rows.size()));
    for (int row = 0;
         row < static_cast<int>(issue_rows.size());
         ++row) {
        const auto& issue =
            *issue_rows[static_cast<std::size_t>(row)];
        const auto stage = issue_recovery_stage(issue);

        auto* task = new QTableWidgetItem(
            issue.task_id.empty()
                ? QStringLiteral("—")
                : q(issue.task_id));
        task->setData(Qt::UserRole, q(issue.task_id));
        recovery_flow_->setItem(row, 0, task);
        recovery_flow_->setItem(
            row,
            1,
            new QTableWidgetItem(q(issue.issue_id)));

        auto* flow =
            new QTableWidgetItem(recovery_flow_text(issue));
        flow->setToolTip(
            QStringLiteral(
                "动作完成不等于恢复完成；只有 Monitor 独立验证恢复后，"
                "流程才会进入“恢复已验证”。"));
        recovery_flow_->setItem(row, 2, flow);

        auto* stage_item = new QTableWidgetItem(
            q(issue_recovery_stage_display_name(stage)));
        emphasize_item(
            stage_item,
            recovery_stage_color(stage));
        recovery_flow_->setItem(row, 3, stage_item);

        auto* next = new QTableWidgetItem(
            q(issue_recovery_next_step(issue)));
        QStringList detail;
        if (!issue.current_action.empty())
            detail << QStringLiteral("当前动作：") +
                          q(issue.current_action);
        if (!issue.summary.empty())
            detail << QStringLiteral("Issue：") +
                          q(issue.summary);
        if (!detail.isEmpty())
            next->setToolTip(detail.join(QStringLiteral("\n")));
        recovery_flow_->setItem(row, 4, next);

        auto* authority = new QTableWidgetItem(
            issue.authority.empty()
                ? QStringLiteral("—")
                : q(issue.authority));
        if (issue.authority == "L3")
            emphasize_item(
                authority,
                QColor(QStringLiteral("#9A641F")));
        else if (issue.authority == "L1" ||
                 issue.authority == "L2")
            emphasize_item(
                authority,
                QColor(QStringLiteral("#4E6B8A")));
        recovery_flow_->setItem(row, 5, authority);
    }
    recovery_flow_->resizeColumnsToContents();

    issues_->setRowCount(static_cast<int>(issue_rows.size()));
    for (int row = 0; row < static_cast<int>(issue_rows.size()); ++row) {
        const auto& issue = *issue_rows[static_cast<std::size_t>(row)];

        auto* task = new QTableWidgetItem(
            issue.task_id.empty() ? QStringLiteral("—") : q(issue.task_id));
        task->setData(Qt::UserRole, q(issue.task_id));
        issues_->setItem(row, 0, task);
        issues_->setItem(row, 1, new QTableWidgetItem(q(issue.issue_id)));

        auto* state = new QTableWidgetItem(
            q(issue_state_display_name(issue.state)));
        if (issue.resolved)
            emphasize_item(state, QColor(QStringLiteral("#3F7D5A")));
        else if (issue.user_action_required)
            emphasize_item(state, QColor(QStringLiteral("#9A641F")));
        else
            emphasize_item(state, QColor(QStringLiteral("#4E6B8A")));
        issues_->setItem(row, 2, state);

        auto* authority = new QTableWidgetItem(
            issue.authority.empty() ? QStringLiteral("—") : q(issue.authority));
        if (issue.authority == "L3")
            emphasize_item(authority, QColor(QStringLiteral("#9A641F")));
        issues_->setItem(row, 3, authority);

        issues_->setItem(
            row,
            4,
            new QTableWidgetItem(
                issue.current_action.empty()
                    ? QStringLiteral("—")
                    : q(issue.current_action)));
        issues_->setItem(
            row,
            5,
            new QTableWidgetItem(
                issue.summary.empty()
                    ? QStringLiteral("—")
                    : q(issue.summary)));
    }
    issues_->resizeColumnsToContents();

    event_timeline_->setRowCount(static_cast<int>(projection.events.size()));
    for (int row = 0; row < static_cast<int>(projection.events.size()); ++row) {
        const auto& event =
            projection.events[projection.events.size() - 1 -
                              static_cast<std::size_t>(row)];

        event_timeline_->setItem(
            row, 0, new QTableWidgetItem(q(short_time(event.occurred_at))));

        auto* type = new QTableWidgetItem(q(event_display_name(event.event_type)));
        if (event.event_type == "issue.user_action_required" ||
            event.event_type == "issue.escalated")
            emphasize_item(type, QColor(QStringLiteral("#9A641F")));
        else if (event.event_type == "issue.recovery_verified" ||
                 event.event_type == "issue.resolved")
            emphasize_item(type, QColor(QStringLiteral("#3F7D5A")));
        else if (event.severity == "error" || event.severity == "critical" ||
                 event.event_type == "agent.failed")
            emphasize_item(type, QColor(QStringLiteral("#A34747")));
        event_timeline_->setItem(row, 1, type);

        event_timeline_->setItem(
            row,
            2,
            new QTableWidgetItem(
                event.task_id.empty() ? QStringLiteral("—") : q(event.task_id)));

        QString source = q(event.source_kind);
        if (!event.source_id.empty())
            source += QStringLiteral(" · ") + q(event.source_id);
        event_timeline_->setItem(row, 3, new QTableWidgetItem(source));

        auto* summary = new QTableWidgetItem(
            event.summary.empty()
                ? q(event_display_name(event.event_type))
                : q(event.summary));
        if (!event.evidence_refs.empty()) {
            QStringList evidence;
            evidence << QStringLiteral("Evidence:");
            for (const auto& ref : event.evidence_refs)
                evidence << QStringLiteral("• ") + q(ref);
            summary->setToolTip(evidence.join(QStringLiteral("\n")));
        }
        event_timeline_->setItem(row, 4, summary);
    }
    event_timeline_->resizeColumnsToContents();
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
        const auto state_value = s(item->if_contains("state"));
        auto* state = new QTableWidgetItem(agent_state_text(state_value));
        emphasize_item(state, agent_state_color(state_value));
        takeovers_->setItem(row, 1, state);
        auto* summary = new QTableWidgetItem(q(s(item->if_contains("summary"))));
        summary->setData(Qt::UserRole, q(s(item->if_contains("path"))));
        if (!s(item->if_contains("path")).empty())
            summary->setToolTip(QStringLiteral("双击打开这次处理的原始记录。"));
        takeovers_->setItem(row, 2, summary);
    }
    takeovers_->resizeColumnsToContents();
    if (takeovers_->rowCount() > 0 && takeovers_->currentRow() < 0)
        takeovers_->selectRow(0);
    render_takeover_stream();
}

void QtMainWindow::render_takeover_stream() {
    if (!takeover_stream_ || !takeovers_) return;
    const auto row = takeovers_->currentRow();
    if (row < 0) {
        takeover_stream_->clear();
        return;
    }
    auto* item = takeovers_->item(row, 2);
    if (!item) {
        takeover_stream_->clear();
        return;
    }
    const auto path = item->data(Qt::UserRole).toString();
    if (path.isEmpty()) {
        takeover_stream_->setPlainText(QStringLiteral("这条记录没有可读取的原始文件。"));
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        takeover_stream_->setPlainText(
            QStringLiteral("无法读取：%1").arg(path));
        return;
    }

    constexpr qint64 kTailBytes = 180000;
    if (file.size() > kTailBytes)
        file.seek(file.size() - kTailBytes);
    auto raw = QString::fromUtf8(file.readAll());
    if (file.pos() > kTailBytes) {
        const auto first_newline = raw.indexOf(QLatin1Char('\n'));
        if (first_newline >= 0) raw.remove(0, first_newline + 1);
    }

    if (!path.endsWith(QStringLiteral(".jsonl"), Qt::CaseInsensitive)) {
        takeover_stream_->setPlainText(raw.trimmed());
        return;
    }

    QStringList readable;
    const auto lines = raw.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const auto& line : lines) {
        boost::system::error_code error;
        const auto parsed = json::parse(line.toUtf8().toStdString(), error);
        if (error || !parsed.is_object()) continue;
        const auto& event = parsed.as_object();
        const auto type = s(event.if_contains("type"));

        if (type == "result") {
            const auto result = s(event.if_contains("result"));
            if (!result.empty())
                readable << QStringLiteral("✓ 结果：%1").arg(q(result));
            continue;
        }

        const auto* message = object(event.if_contains("message"));
        const auto* content = message ? array(message->if_contains("content"))
                                      : array(event.if_contains("content"));
        if (!content) continue;
        for (const auto& block_value : *content) {
            const auto* block = object(&block_value);
            if (!block) continue;
            const auto block_type = s(block->if_contains("type"));
            if (block_type == "text") {
                const auto text = s(block->if_contains("text"));
                if (!text.empty()) readable << QStringLiteral("Claude：%1").arg(q(text));
            } else if (block_type == "tool_use") {
                const auto name = s(block->if_contains("name"));
                QString detail;
                if (const auto* input = object(block->if_contains("input"))) {
                    for (const auto* key : {"file_path", "path", "pattern", "command"}) {
                        const auto value = s(input->if_contains(key));
                        if (!value.empty()) {
                            detail = q(value);
                            break;
                        }
                    }
                }
                readable << (detail.isEmpty()
                    ? QStringLiteral("→ 工具：%1").arg(q(name))
                    : QStringLiteral("→ %1：%2").arg(q(name), detail));
            }
        }
    }

    if (readable.isEmpty()) {
        takeover_stream_->setPlainText(raw.trimmed().right(12000));
    } else {
        takeover_stream_->setPlainText(readable.join(QStringLiteral("\n\n")));
    }
    auto cursor = takeover_stream_->textCursor();
    cursor.movePosition(QTextCursor::End);
    takeover_stream_->setTextCursor(cursor);
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
