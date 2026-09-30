#include "monitor_hub/dispatch_worker.hpp"
#include "monitor_hub/event_store.hpp"
#include "monitor_hub/notification_outbox.hpp"

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

void write_text(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

void write_json(const fs::path& path, const json::object& value) {
    write_text(path, json::serialize(value) + "\n");
}

void append_json(const fs::path& path, const json::object& value) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out << json::serialize(value) << "\n";
}

monitor_hub::RuntimePaths make_paths(
    const fs::path& root,
    const std::string& name) {
    monitor_hub::RuntimePaths paths;
    paths.hub_data = root / name / "hub";
    paths.registry = root / name / "registry.json";
    paths.job_root = root / name / "jobs";
    return paths;
}

json::object dispatch(
    const std::string& dispatch_id,
    const std::string& authority,
    const std::string& dispatch_kind,
    const std::string& project_id,
    const std::string& handler,
    const std::string& agent_profile,
    const std::vector<std::string>& refs) {

    json::array context_refs;
    for (const auto& ref : refs) context_refs.emplace_back(ref);

    json::object requested;
    requested["schema_version"] = 1;
    requested["command_id"] = "cmd-" + dispatch_id;
    requested["payload"] = json::object{};

    json::object out;
    out["schema_version"] = 1;
    out["dispatch_id"] = dispatch_id;
    out["command_id"] = "cmd-" + dispatch_id;
    out["command_type"] =
        authority == "L1"
        ? "monitor.probe.requested"
        : "agent.troubleshoot.request";
    out["dispatched_at"] = "2026-09-30T13:00:00+08:00";
    out["project_id"] = project_id;
    out["task_id"] = "task-1";
    out["issue_id"] = "issue-1";
    out["correlation_id"] = "corr-1";
    out["authority"] = authority;
    out["policy_id"] = "demo-policy-v1";
    out["policy_ref"] = "policies/demo";
    out["action_id"] =
        authority == "L1"
        ? "probe"
        : "troubleshoot";
    out["dispatch_kind"] = dispatch_kind;
    if (!handler.empty()) out["handler"] = handler;
    if (!agent_profile.empty())
        out["agent_profile"] = agent_profile;
    out["constraints"] =
        json::array{"do not change scientific method"};
    out["completion_criteria"] =
        json::array{"record evidence"};
    out["context_refs"] = std::move(context_refs);
    out["requested_command"] = std::move(requested);
    return out;
}

json::object launched_receipt(
    const monitor_hub::DispatchRecord& dispatch,
    const monitor_hub::AgentRunPlan& plan) {

    json::object out;
    out["schema_version"] = 1;
    out["dispatch_id"] = dispatch.dispatch_id;
    out["command_id"] = dispatch.command_id;
    out["project_id"] = dispatch.project_id;
    out["authority"] = dispatch.authority;
    out["state"] = "launched";
    out["reason"] = "test fixture";
    out["updated_at"] = "2026-09-30T13:01:00+08:00";
    out["run_id"] = plan.run_id;
    out["result_file"] = plan.result_file.string();
    return out;
}

bool has_event(
    const monitor_hub::ProjectEventProjection& projection,
    const std::string& event_type) {
    for (const auto& event : projection.events)
        if (event.event_type == event_type) return true;
    return false;
}

}  // namespace

int main() {
    using namespace monitor_hub;

    const auto root =
        fs::temp_directory_path() /
        "monitor-hub-dispatch-worker-tests";
    fs::remove_all(root);
    fs::create_directories(root);

    // L1 built-in read_only_probe is genuinely read-only and completes
    // without claiming that an Issue is resolved.
    {
        const auto paths = make_paths(root, "l1-probe");
        const auto evidence =
            root / "l1-probe" / "project" / "OUTCAR";
        write_text(evidence, "probe evidence\n");

        write_json(
            paths.hub_data / "dispatch" / "l1" / "probe.json",
            dispatch(
                "dsp-l1-probe",
                "L1",
                "deterministic",
                "demo",
                "read_only_probe",
                "",
                {evidence.string()}));

        const auto result =
            run_dispatch_workers(paths, false);
        CHECK(result.l1_completed == 1);
        CHECK(result.l1_failed == 0);
        CHECK(result.receipts.size() == 1);
        CHECK(result.receipts[0].state == "completed");

        const auto projection =
            load_project_event_projection(paths, "demo", 100);
        CHECK(has_event(projection, "monitor.check_completed"));
        CHECK(!has_event(projection, "issue.resolved"));

        const auto receipt_bytes =
            read_text(paths.hub_data /
                      "dispatch" / "worker-receipts.jsonl");
        const auto repeated =
            run_dispatch_workers(paths, false);
        CHECK(repeated.already_terminal == 1);
        CHECK(
            receipt_bytes ==
            read_text(paths.hub_data /
                      "dispatch" / "worker-receipts.jsonl"));
    }

    // Unknown L1 handlers fail closed and become a durable escalation.
    {
        const auto paths = make_paths(root, "l1-unsupported");
        write_json(
            paths.hub_data / "dispatch" / "l1" / "bad.json",
            dispatch(
                "dsp-l1-unsupported",
                "L1",
                "deterministic",
                "demo",
                "restart_same_parameters",
                "",
                {}));

        const auto result =
            run_dispatch_workers(paths, false);
        CHECK(result.l1_failed == 1);
        CHECK(result.receipts[0].state == "failed");

        const auto projection =
            load_project_event_projection(paths, "demo", 100);
        CHECK(has_event(
            projection,
            "issue.user_action_required"));
        CHECK(has_event(
            projection,
            "notification.requested"));

        const auto outbox =
            sync_notification_outbox(paths);
        CHECK(outbox.pending_count() == 1);
        CHECK(outbox.items[0].reason ==
              "unsupported_l1_handler");
    }

    // L2 plan materialization is testable without launching Claude.
    // A completed result produces Agent facts only; recovery remains for the
    // monitor to verify.
    {
        const auto paths = make_paths(root, "l2-complete");
        const auto evidence =
            root / "l2-complete" / "project" / "OUTCAR";
        write_text(evidence, "agent evidence\n");

        const auto dispatch_path =
            paths.hub_data / "dispatch" / "l2" / "agent.json";
        write_json(
            dispatch_path,
            dispatch(
                "dsp-l2-complete",
                "L2",
                "child_agent",
                "demo",
                "",
                "bounded-troubleshooter",
                {evidence.string()}));

        std::string error;
        const auto record =
            load_dispatch_record(dispatch_path, error);
        CHECK(record);
        const auto plan =
            prepare_l2_agent_run(paths, *record, error);
        CHECK(plan);
        CHECK(fs::exists(plan->prompt_file));
        CHECK(plan->command.find(
                  "--dangerously-skip-permissions") !=
              std::string::npos);
        CHECK(plan->working_directory ==
              evidence.parent_path());

        append_json(
            paths.hub_data /
                "dispatch" / "worker-receipts.jsonl",
            launched_receipt(*record, *plan));

        json::object agent_result;
        agent_result["schema_version"] = 1;
        agent_result["dispatch_id"] = record->dispatch_id;
        agent_result["success"] = true;
        agent_result["summary"] =
            "Applied bounded recovery action";
        agent_result["action_type"] =
            "restart_same_parameters";
        agent_result["action_applied"] = true;
        agent_result["evidence_refs"] =
            json::array{evidence.string()};
        agent_result["uncertainty"] = "";
        agent_result["needs_user"] = "none";
        write_json(plan->result_file, agent_result);

        const auto result =
            run_dispatch_workers(paths, false);
        CHECK(result.l2_completed == 1);
        CHECK(result.l2_needs_user == 0);

        const auto projection =
            load_project_event_projection(paths, "demo", 100);
        CHECK(has_event(
            projection,
            "agent.evidence_recorded"));
        CHECK(has_event(
            projection,
            "agent.action_finished"));
        CHECK(has_event(
            projection,
            "agent.completed"));
        CHECK(!has_event(
            projection,
            "issue.recovery_verified"));
        CHECK(!has_event(
            projection,
            "issue.resolved"));
    }

    // L2 uncertainty is escalated instead of allowing the child Agent to
    // self-authorize an L3 decision.
    {
        const auto paths = make_paths(root, "l2-needs-user");
        const auto evidence =
            root / "l2-needs-user" / "project" / "OUTCAR";
        write_text(evidence, "uncertain evidence\n");

        const auto dispatch_path =
            paths.hub_data / "dispatch" / "l2" / "agent.json";
        write_json(
            dispatch_path,
            dispatch(
                "dsp-l2-needs-user",
                "L2",
                "child_agent",
                "demo",
                "",
                "bounded-troubleshooter",
                {evidence.string()}));

        std::string error;
        const auto record =
            load_dispatch_record(dispatch_path, error);
        CHECK(record);
        const auto plan =
            prepare_l2_agent_run(paths, *record, error);
        CHECK(plan);

        append_json(
            paths.hub_data /
                "dispatch" / "worker-receipts.jsonl",
            launched_receipt(*record, *plan));

        json::object agent_result;
        agent_result["schema_version"] = 1;
        agent_result["dispatch_id"] = record->dispatch_id;
        agent_result["success"] = true;
        agent_result["summary"] =
            "Two scientifically different recovery options remain";
        agent_result["action_type"] = "";
        agent_result["action_applied"] = false;
        agent_result["evidence_refs"] =
            json::array{evidence.string()};
        agent_result["uncertainty"] =
            "method choice changes interpretation";
        agent_result["needs_user"] =
            "Choose between scientifically different recovery methods";
        write_json(plan->result_file, agent_result);

        const auto result =
            run_dispatch_workers(paths, false);
        CHECK(result.l2_needs_user == 1);

        const auto projection =
            load_project_event_projection(paths, "demo", 100);
        CHECK(has_event(
            projection,
            "agent.analysis_recorded"));
        CHECK(has_event(
            projection,
            "issue.user_action_required"));
        CHECK(has_event(
            projection,
            "notification.requested"));

        const auto outbox =
            sync_notification_outbox(paths);
        CHECK(outbox.pending_count() == 1);
        CHECK(outbox.items[0].reason ==
              "child_agent_needs_user");
    }

    // Invalid dispatch records are isolated.
    {
        const auto paths = make_paths(root, "malformed");
        write_text(
            paths.hub_data /
                "dispatch" / "l1" / "bad.json",
            "{bad json}\n");
        const auto result =
            run_dispatch_workers(paths, false);
        CHECK(result.malformed_dispatches == 1);
        CHECK(result.receipts.empty());
    }

    fs::remove_all(root);
    std::cout << "dispatch worker tests passed\n";
    return 0;
}
