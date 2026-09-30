#include "monitor_hub/event_store.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

namespace {

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

monitor_hub::RuntimePaths paths_for(const fs::path& root) {
    monitor_hub::RuntimePaths paths;
    paths.hub_data = root / "hub";
    paths.registry = root / "registry.json";
    paths.job_root = root / "jobs";
    return paths;
}

const monitor_hub::IssueProjection& only_issue(
    const monitor_hub::ProjectEventProjection& projection) {
    CHECK(projection.issues.size() == 1);
    return projection.issues[0];
}

}  // namespace

int main() {
    using namespace monitor_hub;

    const auto root =
        fs::temp_directory_path() /
        "monitor-hub-recovery-projection-tests";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto paths = paths_for(root);
    const auto events = paths.hub_data / "events";

    write_text(
        events / "agent.jsonl",
        R"({"schema_version":1,"event_id":"a1","event_type":"issue.detected","occurred_at":"2026-09-30T10:00:00+08:00","project_id":"agent","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"monitor","id":"m1"},"severity":"warning","payload":{"summary":"problem"}})"
        "\n"
        R"({"schema_version":1,"event_id":"a2","event_type":"agent.started","occurred_at":"2026-09-30T10:01:00+08:00","project_id":"agent","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"child_agent","id":"bounded"},"severity":"info","payload":{"summary":"investigating"}})"
        "\n");
    {
        const auto projection =
            load_project_event_projection(paths, "agent", 100);
        const auto& issue = only_issue(projection);
        CHECK(issue.agent_active);
        CHECK(issue.authority == "L2");
        CHECK(issue_recovery_stage(issue) == "agent_handling");
        CHECK(issue_recovery_next_step(issue).find("Child Agent") !=
              std::string::npos);
    }

    write_text(
        events / "restart.jsonl",
        R"({"schema_version":1,"event_id":"r1","event_type":"issue.detected","occurred_at":"2026-09-30T10:00:00+08:00","project_id":"restart","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"monitor","id":"m1"},"severity":"warning","payload":{"summary":"problem"}})"
        "\n"
        R"({"schema_version":1,"event_id":"r2","event_type":"issue.action_applied","occurred_at":"2026-09-30T10:01:00+08:00","project_id":"restart","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"core","id":"monitor-hub-dispatch-worker"},"severity":"info","payload":{"summary":"policy restart launched"}})"
        "\n"
        R"({"schema_version":1,"event_id":"r3","event_type":"task.restarted","occurred_at":"2026-09-30T10:02:00+08:00","project_id":"restart","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"core","id":"monitor-hub-dispatch-worker"},"severity":"info","payload":{"summary":"replacement process started"}})"
        "\n");
    {
        const auto projection =
            load_project_event_projection(paths, "restart", 100);
        const auto& issue = only_issue(projection);
        CHECK(issue.action_applied);
        CHECK(issue.task_restarted);
        CHECK(issue.authority == "L1");
        CHECK(!issue.recovery_verified);
        CHECK(!issue.resolved);
        CHECK(issue_recovery_stage(issue) == "waiting_verification");
        CHECK(issue_recovery_next_step(issue).find("独立验证恢复") !=
              std::string::npos);
    }

    write_text(
        events / "verified.jsonl",
        R"({"schema_version":1,"event_id":"v1","event_type":"issue.detected","occurred_at":"2026-09-30T10:00:00+08:00","project_id":"verified","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"monitor","id":"m1"},"severity":"warning","payload":{"summary":"problem"}})"
        "\n"
        R"({"schema_version":1,"event_id":"v2","event_type":"issue.action_applied","occurred_at":"2026-09-30T10:01:00+08:00","project_id":"verified","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"core","id":"monitor-hub-dispatch-worker"},"severity":"info","payload":{"summary":"action"}})"
        "\n"
        R"({"schema_version":1,"event_id":"v3","event_type":"issue.recovery_started","occurred_at":"2026-09-30T10:02:00+08:00","project_id":"verified","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"monitor","id":"m1"},"severity":"info","payload":{"summary":"checking recovery"}})"
        "\n"
        R"({"schema_version":1,"event_id":"v4","event_type":"issue.recovery_verified","occurred_at":"2026-09-30T10:03:00+08:00","project_id":"verified","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"monitor","id":"m1"},"severity":"info","payload":{"summary":"recovery passed"}})"
        "\n");
    {
        const auto projection =
            load_project_event_projection(paths, "verified", 100);
        const auto& issue = only_issue(projection);
        CHECK(issue.recovery_started);
        CHECK(issue.recovery_verified);
        CHECK(!issue.resolved);
        CHECK(issue_recovery_stage(issue) == "recovery_verified");
        CHECK(issue_recovery_next_step(issue).find("issue.resolved") !=
              std::string::npos);
    }

    write_text(
        events / "resolved.jsonl",
        R"({"schema_version":1,"event_id":"z1","event_type":"issue.detected","occurred_at":"2026-09-30T10:00:00+08:00","project_id":"resolved","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"monitor","id":"m1"},"severity":"warning","payload":{"summary":"problem"}})"
        "\n"
        R"({"schema_version":1,"event_id":"z2","event_type":"issue.recovery_verified","occurred_at":"2026-09-30T10:01:00+08:00","project_id":"resolved","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"monitor","id":"m1"},"severity":"info","payload":{"summary":"verified"}})"
        "\n"
        R"({"schema_version":1,"event_id":"z3","event_type":"issue.resolved","occurred_at":"2026-09-30T10:02:00+08:00","project_id":"resolved","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"monitor","id":"m1"},"severity":"info","payload":{"summary":"done"}})"
        "\n");
    {
        const auto projection =
            load_project_event_projection(paths, "resolved", 100);
        const auto& issue = only_issue(projection);
        CHECK(issue.resolved);
        CHECK(issue_recovery_stage(issue) == "resolved");
        CHECK(issue_recovery_next_step(issue) == "无");
    }

    write_text(
        events / "decision.jsonl",
        R"({"schema_version":1,"event_id":"d1","event_type":"issue.detected","occurred_at":"2026-09-30T10:00:00+08:00","project_id":"decision","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"monitor","id":"m1"},"severity":"warning","payload":{"summary":"problem"}})"
        "\n"
        R"({"schema_version":1,"event_id":"d2","event_type":"issue.user_action_required","occurred_at":"2026-09-30T10:01:00+08:00","project_id":"decision","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"core","id":"dispatcher"},"severity":"warning","payload":{"summary":"choose method","authority":"L3"}})"
        "\n");
    {
        const auto projection =
            load_project_event_projection(paths, "decision", 100);
        const auto& issue = only_issue(projection);
        CHECK(issue.user_action_required);
        CHECK(issue.authority == "L3");
        CHECK(issue_recovery_stage(issue) == "needs_user");
    }

    write_text(
        events / "failed.jsonl",
        R"({"schema_version":1,"event_id":"f1","event_type":"issue.detected","occurred_at":"2026-09-30T10:00:00+08:00","project_id":"failed","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"monitor","id":"m1"},"severity":"warning","payload":{"summary":"problem"}})"
        "\n"
        R"({"schema_version":1,"event_id":"f2","event_type":"agent.started","occurred_at":"2026-09-30T10:01:00+08:00","project_id":"failed","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"child_agent","id":"bounded"},"severity":"info","payload":{"summary":"investigating"}})"
        "\n"
        R"({"schema_version":1,"event_id":"f3","event_type":"agent.failed","occurred_at":"2026-09-30T10:02:00+08:00","project_id":"failed","task_id":"t1","issue_id":"i1","correlation_id":"c1","source":{"kind":"child_agent","id":"bounded"},"severity":"error","payload":{"summary":"agent failed"}})"
        "\n");
    {
        const auto projection =
            load_project_event_projection(paths, "failed", 100);
        const auto& issue = only_issue(projection);
        CHECK(issue.agent_failed);
        CHECK(issue_recovery_stage(issue) == "failed");
    }

    fs::remove_all(root);
    std::cout << "recovery projection tests passed\n";
    return 0;
}
