#include "monitor_hub/control_orchestrator.hpp"
#include "monitor_hub/event_store.hpp"

#include <boost/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
namespace json = boost::json;

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

void write_json(const fs::path& path, const json::object& value) {
    write_text(path, json::serialize(value) + "\n");
}

monitor_hub::RuntimePaths paths_for(
    const fs::path& root,
    const std::string& name) {
    monitor_hub::RuntimePaths paths;
    paths.hub_data = root / name / "hub";
    paths.registry = root / name / "registry.json";
    paths.job_root = root / name / "jobs";
    return paths;
}

json::object requested_by() {
    json::object out;
    out["kind"] = "monitor";
    out["id"] = "orchestrator-test-monitor";
    return out;
}

json::object l1_command(
    const fs::path& evidence) {
    json::object payload;
    payload["action_id"] = "probe_context";

    json::object out;
    out["schema_version"] = 1;
    out["command_id"] = "cmd-orchestrator-l1";
    out["command_type"] = "monitor.probe.requested";
    out["requested_at"] = "2026-09-30T15:00:00+08:00";
    out["project_id"] = "demo";
    out["task_id"] = "task-1";
    out["issue_id"] = "issue-1";
    out["correlation_id"] = "corr-l1";
    out["requested_by"] = requested_by();
    out["authority"] = "L1";
    out["policy_ref"] = "policies/demo";
    out["constraints"] = json::array{};
    out["completion_criteria"] =
        json::array{"metadata recorded"};
    out["context_refs"] =
        json::array{evidence.string()};
    out["payload"] = std::move(payload);
    return out;
}

json::object l3_command() {
    json::object out;
    out["schema_version"] = 1;
    out["command_id"] = "cmd-orchestrator-l3";
    out["command_type"] =
        "project.method_change.requested";
    out["requested_at"] = "2026-09-30T15:10:00+08:00";
    out["project_id"] = "demo";
    out["task_id"] = "task-2";
    out["issue_id"] = "issue-2";
    out["correlation_id"] = "corr-l3";
    out["requested_by"] = requested_by();
    out["authority"] = "L3";
    out["constraints"] =
        json::array{"do not choose method automatically"};
    out["completion_criteria"] =
        json::array{"main Agent records decision"};
    out["context_refs"] = json::array{};
    out["payload"] = json::object{};
    return out;
}

json::object read_only_policy() {
    json::object action;
    action["action_id"] = "probe_context";
    action["authority"] = "L1";
    action["dispatch_kind"] = "deterministic";
    action["handler"] = "read_only_probe";
    action["allowed_command_types"] =
        json::array{"monitor.probe.requested"};
    action["constraints"] =
        json::array{"read-only"};
    action["completion_criteria"] =
        json::array{"metadata recorded"};
    action["enabled"] = true;

    json::array actions;
    actions.emplace_back(std::move(action));

    json::object out;
    out["schema_version"] = 1;
    out["policy_id"] = "demo-policy-v1";
    out["project_id"] = "demo";
    out["actions"] = std::move(actions);
    return out;
}

bool has_event(
    const monitor_hub::ProjectEventProjection& projection,
    const std::string& type) {
    for (const auto& event : projection.events)
        if (event.event_type == type) return true;
    return false;
}

}  // namespace

int main() {
    using namespace monitor_hub;

    const auto root =
        fs::temp_directory_path() /
        "monitor-hub-control-orchestrator-tests";
    fs::remove_all(root);
    fs::create_directories(root);

    // One tick converts an L1 command into a validated dispatch and executes
    // the built-in handler before returning.
    {
        const auto paths = paths_for(root, "l1");
        const auto evidence =
            root / "l1" / "project" / "OUTCAR";
        write_text(evidence, "evidence\n");
        write_json(
            paths.hub_data / "policies" / "demo.json",
            read_only_policy());
        CHECK(submit_command(paths, l1_command(evidence)).accepted);

        OrchestratorOptions options;
        options.launch_agents = false;
        const auto tick =
            run_control_tick(paths, options);

        CHECK(tick.command_control.queued_l1 == 1);
        CHECK(tick.worker.l1_completed == 1);
        CHECK(tick.outbox.pending_count() == 0);
        CHECK(!tick.needs_main_agent);

        const auto projection =
            load_project_event_projection(paths, "demo", 100);
        CHECK(has_event(
            projection,
            "monitor.check_completed"));

        const auto second =
            run_control_tick(paths, options);
        CHECK(second.command_control.already_processed == 1);
        CHECK(second.worker.already_terminal == 1);
        CHECK(second.outbox.pending_count() == 0);
    }

    // L3 escalation reaches the durable main-Agent outbox in the same tick.
    {
        const auto paths = paths_for(root, "l3");
        CHECK(submit_command(paths, l3_command()).accepted);

        OrchestratorOptions options;
        options.launch_agents = false;
        auto tick =
            run_control_tick(paths, options);

        CHECK(tick.command_control.escalated_l3 == 1);
        CHECK(tick.outbox.pending_count() == 1);
        CHECK(tick.needs_main_agent);
        CHECK(
            tick.outbox.items[0].notification_id ==
            "ntf:cmd:cmd-orchestrator-l3");

        CHECK(acknowledge_notification(
            paths,
            "ntf:cmd:cmd-orchestrator-l3",
            "orchestrator-test-main-agent"));

        tick = run_control_tick(paths, options);
        CHECK(tick.outbox.pending_count() == 0);
        CHECK(!tick.needs_main_agent);
    }

    fs::remove_all(root);
    std::cout << "control orchestrator tests passed\n";
    return 0;
}
