#include "monitor_hub/command_control.hpp"
#include "monitor_hub/core.hpp"
#include "monitor_hub/deterministic_handlers.hpp"
#include "monitor_hub/setup_request.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "CHECK failed: " #expr \
                      << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::exit(1); \
        } \
    } while (false)

int main() {
    using namespace monitor_hub;

    const auto root =
        fs::temp_directory_path() /
        "monitor-hub-setup-policy-bootstrap-tests";
    fs::remove_all(root);
    fs::create_directories(root);

    RuntimePaths paths;
    paths.hub_data = root / "hub";
    paths.registry = root / "registry.json";
    paths.job_root = root / "jobs";

    const auto project = root / "project";
    fs::create_directories(project);

    const auto template_text = setup_request_template();
    CHECK(
        template_text.find("可复用的重启入口") !=
        std::string::npos);

    const auto example_text = setup_request_example();
    CHECK(
        example_text.find("restart.pbs") !=
        std::string::npos);

    const std::string body =
        "【监控任务】\n"
        "项目名称：Policy bootstrap test\n"
        "项目目录（本机路径）：" + project.string() + "\n"
        "计算在哪里跑：HPC 集群 PBS\n"
        "要监控的作业：task-1\n"
        "可复用的重启入口：已有 restart.pbs\n"
        "完成标准：任务完成\n"
        "检查间隔：15 分钟\n"
        "允许监控自动做的操作：原样 qsub restart.pbs\n"
        "禁止的操作：不改科学参数，不删除输出\n"
        "需要通知我的情况：需要改变方法\n"
        "最终交付：RESULTS.md\n"
        "备注：\n";

    const auto launch =
        prepare_setup_request(
            paths,
            body,
            project,
            std::string("20310102-030405"));

    CHECK(fs::exists(launch.prompt_file));
    const auto prompt = read_text(launch.prompt_file);

    CHECK(
        prompt.find("monitor_hub_control --registered-handlers") !=
        std::string::npos);
    CHECK(
        prompt.find("monitor_hub_control --validate-policy-ref") !=
        std::string::npos);
    CHECK(
        prompt.find("local_process_restart_v1") !=
        std::string::npos);
    CHECK(
        prompt.find("pbs_qsub_restart_v1") !=
        std::string::npos);
    CHECK(
        prompt.find("handler_config") !=
        std::string::npos);
    CHECK(
        prompt.find("Never copy executable/script/argv authority") !=
        std::string::npos);
    CHECK(
        prompt.find("do not create a generic shell action") !=
        std::string::npos);
    CHECK(
        prompt.find("POLICY_REF:") !=
        std::string::npos);
    CHECK(
        prompt.find("AUTO_ACTIONS:") !=
        std::string::npos);
    CHECK(
        prompt.find("NEEDS_USER:") !=
        std::string::npos);
    CHECK(
        prompt.find("PROJECT_ID:") !=
        std::string::npos);
    CHECK(
        prompt.find("monitor_hub_cli --agent-bind") !=
        std::string::npos);
    CHECK(
        prompt.find("monitor_hub_cli --agent-post") !=
        std::string::npos);
    CHECK(
        prompt.find("Do not create a new one-off Q&A Agent/session") !=
        std::string::npos);
    CHECK(
        prompt.find("monitor_hub_control --submit-command") !=
        std::string::npos);
    CHECK(
        prompt.find("stable command_id") !=
        std::string::npos);
    CHECK(
        prompt.find("same issue_id and correlation_id") !=
        std::string::npos);
    CHECK(
        prompt.find("must not directly execute L1/L2 recovery") !=
        std::string::npos);
    CHECK(
        prompt.find("global Monitor Hub control orchestrator") !=
        std::string::npos);
    CHECK(
        prompt.find("issue.recovery_started") !=
        std::string::npos);
    CHECK(
        prompt.find("issue.recovery_verified") !=
        std::string::npos);
    CHECK(
        prompt.find("issue.resolved only when") !=
        std::string::npos);
    CHECK(
        prompt.find(paths.hub_data.string()) !=
        std::string::npos);
    CHECK(
        prompt.find(body) !=
        std::string::npos);

    const auto handlers =
        registered_deterministic_handlers();
    CHECK(
        std::find(
            handlers.begin(),
            handlers.end(),
            "read_only_probe") != handlers.end());
    CHECK(
        std::find(
            handlers.begin(),
            handlers.end(),
            "local_process_restart_v1") != handlers.end());
    CHECK(
        std::find(
            handlers.begin(),
            handlers.end(),
            "pbs_qsub_restart_v1") != handlers.end());

    const auto portable_example =
        command_policy_example();
    CHECK(
        portable_example.find("read_only_probe") !=
        std::string::npos);
    CHECK(
        portable_example.find("restart_same_parameters") ==
        std::string::npos);

    fs::remove_all(root);
    std::cout << "setup policy bootstrap tests passed\n";
    return 0;
}
