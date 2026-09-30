#include "monitor_hub/setup_request.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace monitor_hub {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::optional<std::string> field_value(
    const std::string& body,
    const std::string& prefix) {

    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) != 0) continue;
        return trim(line.substr(prefix.size()));
    }
    return std::nullopt;
}

std::string powershell_literal(const fs::path& path) {
    auto value = path.string();
    std::string escaped;
    escaped.reserve(value.size() + 4);
    for (const auto ch : value) {
        if (ch == '\'') escaped += "''";
        else escaped += ch;
    }
    return "'" + escaped + "'";
}

fs::path env_path(const char* name, const fs::path& fallback) {
    if (const auto* value = std::getenv(name); value && *value)
        return fs::path(value);
    return fallback;
}

std::string format_stamp(std::time_t value) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &value);
#else
    localtime_r(&value, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%d-%H%M%S");
    return out.str();
}

std::string local_time_text(std::time_t value) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &value);
#else
    localtime_r(&value, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M");
    return out.str();
}

void write_utf8(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) throw std::runtime_error("failed writing " + path.string());
}

std::string setup_prompt(
    const RuntimePaths& paths,
    const fs::path& report_file,
    const fs::path& request_file,
    const std::string& body,
    const std::string& submitted_at) {

    std::ostringstream prompt;
    prompt
        << "You are the setup agent for Monitor Hub. Configure this monitoring request end to end.\n\n"
        << "Control-plane rules:\n"
        << "1. Read the target project's AGENTS.md / CLAUDE.md / handoff files before changing anything.\n"
        << "2. Verify live state first. If an existing monitor already covers the requested jobs, reuse/register it instead of creating a duplicate.\n"
        << "3. Treat the user's allowed actions and forbidden actions below as hard authority boundaries. Anything outside them is L3 and must be reported, not silently performed.\n"
        << "4. Register the finished monitor in: " << paths.registry.string() << "\n"
        << "5. Keep runtime data under: " << paths.hub_data.string() << "\n"
        << "6. Emit the existing normalized hub_status.json contract. When practical, also emit Agent/Event Protocol v1 facts under MONITOR_HUB_DATA/events/<project_id>.jsonl.\n"
        << "7. Reuse headless scheduling and detached-agent conventions already present in this Monitor repository.\n"
        << "8. Never delete project outputs or change scientific/business method unless the request explicitly authorizes it.\n"
        << "9. On completion write a short Chinese report to: " << report_file.string() << "\n"
        << "10. The final report must end with exactly one line: NEEDS_USER: <reason> or NEEDS_USER: none.\n\n"
        << "Request file: " << request_file.string() << "\n"
        << "Submitted: " << submitted_at << "\n\n"
        << "Request:\n"
        << body << "\n";
    return prompt.str();
}

}  // namespace

std::string setup_request_template() {
    return
        "【监控任务】\n"
        "项目名称：\n"
        "项目目录（本机路径）：\n"
        "计算在哪里跑：本机 / Vanda PBS / 其他服务器（写清楚）\n"
        "要监控的作业：作业名、PBS 作业号或启动命令、检查点 / 输出文件在哪里\n"
        "完成标准：什么情况算全部完成\n"
        "检查间隔：例如 15 分钟、5 小时\n"
        "允许监控自动做的操作：例如进程停了原地续算、SCF 不收敛按规程重投、跑完汇总结果\n"
        "禁止的操作：例如不改计算参数、不删数据、不提交新体系\n"
        "需要通知我的情况：\n"
        "最终交付：例如结果表、RESULTS.md、推送到 GitHub 分支\n"
        "备注：\n";
}

std::string setup_request_example() {
    return
        "【监控任务】\n"
        "项目名称：示例 · 表面吸附能计算（第 2 批）\n"
        "项目目录（本机路径）：D:\\Research\\Example\\adsorption_batch2\n"
        "计算在哪里跑：HPC 集群 PBS\n"
        "要监控的作业：8 个体系，作业号见 SUBMISSION.txt\n"
        "完成标准：8 个体系最终单点完成，能量和磁矩写入结果表\n"
        "检查间隔：5 小时\n"
        "允许监控自动做的操作：墙时或 NELM 停机时从已有检查点原地续算\n"
        "禁止的操作：不改泛函、U、ENCUT；不提交新体系；不删除输出\n"
        "需要通知我的情况：同一体系第 2 次 NELM 用满；需要改变科学方法\n"
        "最终交付：results/batch2_table.md\n"
        "备注：和其他批次共用集群配额\n";
}

std::optional<SetupRequestFields> parse_setup_request_fields(
    const std::string& body) {

    const auto name = field_value(body, "项目名称：");
    const auto workdir = field_value(body, "项目目录（本机路径）：");
    if (!name || name->empty() || !workdir || workdir->empty())
        return std::nullopt;
    return SetupRequestFields{*name, *workdir};
}

SetupAgentRuntime setup_agent_runtime_from_env() {
#ifdef _WIN32
    const auto user = std::getenv("USERPROFILE")
        ? fs::path(std::getenv("USERPROFILE"))
        : fs::path("C:\\Users\\ASUS");
    const fs::path default_detach =
        user / ".claude" / "tools" / "cdesktop-detach.ps1";
    const fs::path preferred_pwsh =
        "D:\\Tools\\PowerShell\\7.6.3\\pwsh.exe";
    const fs::path default_claude =
        "D:\\Download\\npm-global\\node_modules\\@anthropic-ai\\claude-code\\bin\\claude.exe";
    const auto powershell = env_path(
        "MONITOR_HUB_PWSH",
        fs::exists(preferred_pwsh) ? preferred_pwsh : fs::path("pwsh"));
#else
    const auto home = std::getenv("HOME")
        ? fs::path(std::getenv("HOME"))
        : fs::path();
    const fs::path default_detach =
        home / ".claude" / "tools" / "cdesktop-detach.ps1";
    const fs::path default_claude = "claude";
    const auto powershell = env_path("MONITOR_HUB_PWSH", fs::path("pwsh"));
#endif
    return {
        powershell,
        env_path("MONITOR_HUB_DETACH", default_detach),
        env_path("MONITOR_HUB_CLAUDE_EXE", default_claude),
    };
}

SetupRequestLaunch prepare_setup_request(
    const RuntimePaths& paths,
    const std::string& body,
    const fs::path& requested_workdir,
    const std::optional<std::string>& stamp_override) {

    const auto fields = parse_setup_request_fields(body);
    if (!fields)
        throw std::invalid_argument(
            "monitor request requires project name and local project directory");

    const auto request_dir = paths.hub_data / "requests";
    std::error_code ec;
    fs::create_directories(request_dir, ec);
    if (ec)
        throw std::runtime_error(
            "cannot create request directory: " + ec.message());

    const auto now = std::chrono::system_clock::now();
    auto time_value = std::chrono::system_clock::to_time_t(now);

    std::string stamp = stamp_override.value_or(format_stamp(time_value));
    if (!stamp_override) {
        while (fs::exists(request_dir / (stamp + "_request.md"))) {
            ++time_value;
            stamp = format_stamp(time_value);
        }
    }

    SetupRequestLaunch launch;
    launch.stamp = stamp;
    launch.job_name = "hub-setup-" + stamp;
    launch.request_file = request_dir / (stamp + "_request.md");
    launch.report_file = request_dir / (stamp + "_report.md");
    launch.prompt_file = request_dir / (stamp + "_prompt.txt");
    launch.runtime = setup_agent_runtime_from_env();

    std::error_code dir_ec;
    launch.working_directory =
        fs::is_directory(requested_workdir, dir_ec) && !dir_ec
        ? requested_workdir
        : paths.hub_data;

    const auto request_tmp =
        fs::path(launch.request_file.string() + ".tmp");
    const auto prompt_tmp =
        fs::path(launch.prompt_file.string() + ".tmp");

    const auto submitted_at = local_time_text(time_value);
    const auto prompt = setup_prompt(
        paths,
        launch.report_file,
        launch.request_file,
        body,
        submitted_at);

    try {
        write_utf8(request_tmp, body + "\n");
        write_utf8(prompt_tmp, prompt);
        fs::rename(request_tmp, launch.request_file);
        fs::rename(prompt_tmp, launch.prompt_file);
    } catch (...) {
        std::error_code cleanup;
        fs::remove(request_tmp, cleanup);
        fs::remove(prompt_tmp, cleanup);
        throw;
    }

    launch.command =
        "[Console]::OutputEncoding = [Text.Encoding]::UTF8; "
        "$OutputEncoding = [Text.Encoding]::UTF8; "
        "Remove-Item Env:CLAUDE_CONFIG_DIR -ErrorAction SilentlyContinue; "
        "Get-Content -Raw -Encoding utf8 -LiteralPath " +
        powershell_literal(launch.prompt_file) +
        " | & " +
        powershell_literal(launch.runtime.claude_executable) +
        " -p --dangerously-skip-permissions "
        "--output-format stream-json --verbose --model opus";

    launch.arguments = {
        "-NoProfile",
        "-NonInteractive",
        "-File",
        launch.runtime.detach_script.string(),
        "-Name",
        launch.job_name,
        "-Command",
        launch.command,
        "-WorkDir",
        launch.working_directory.string(),
    };
    return launch;
}

}  // namespace monitor_hub
