#include "monitor_hub/command_control.hpp"
#include "monitor_hub/event_store.hpp"
#include "monitor_hub/notification_outbox.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cassert>
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
                      << " at " << __FILE__ << ":" << __LINE__ << "\\n"; \
            std::exit(1); \
        } \
    } while (false)

namespace {

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
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

json::object requested_by() {
    json::object out;
    out["kind"] = "monitor";
    out["id"] = "test-monitor";
    return out;
}

json::object command(
    const std::string& command_id,
    const std::string& command_type,
    const std::string& authority,
    const std::string& project_id,
    const std::string& action_id = {},
    const std::string& policy_ref = {}) {

    json::object payload;
    if (!action_id.empty()) payload["action_id"] = action_id;

    json::object out;
    out["schema_version"] = 1;
    out["command_id"] = command_id;
    out["command_type"] = command_type;
    out["requested_at"] = "2026-09-30T12:00:00+08:00";
    out["project_id"] = project_id;
    out["task_id"] = "task-1";
    out["issue_id"] = "issue-1";
    out["correlation_id"] = "corr-" + command_id;
    out["requested_by"] = requested_by();
    out["authority"] = authority;
    if (!policy_ref.empty()) out["policy_ref"] = policy_ref;
    out["constraints"] =
        json::array{"command-specific constraint"};
    out["completion_criteria"] =
        json::array{"command-specific completion"};
    out["context_refs"] =
        json::array{"OUTCAR", "OSZICAR"};
    out["payload"] = std::move(payload);
    return out;
}

std::string policy_json(const std::string& project_id) {
    return std::string(R"({
  "schema_version": 1,
  "policy_id": "demo-policy-v1",
  "project_id": ")") + project_id + R"(",
  "actions": [
    {
      "action_id": "restart_same_parameters",
      "authority": "L1",
      "dispatch_kind": "deterministic",
      "handler": "restart_same_parameters",
      "allowed_command_types": ["task.restart.requested"],
      "constraints": ["reuse validated checkpoint"],
      "completion_criteria": ["replacement job is observed"],
      "enabled": true
    },
    {
      "action_id": "troubleshoot_known_failure",
      "authority": "L2",
      "dispatch_kind": "child_agent",
      "agent_profile": "bounded-troubleshooter",
      "allowed_command_types": ["agent.troubleshoot.request"],
      "constraints": ["do not change scientific method"],
      "completion_criteria": ["evidence is recorded"],
      "enabled": true
    }
  ]
})";
}

json::object read_json_object(const fs::path& path) {
    const auto value = monitor_hub::read_json(path);
    CHECK(value && value->is_object());
    return value->as_object();
}

}  // namespace

int main() {
    using namespace monitor_hub;

    const auto root =
        fs::temp_directory_path() /
        "monitor-hub-command-control-tests";
    fs::remove_all(root);
    fs::create_directories(root);

    // L1: a policy-approved deterministic action is routed to the durable
    // deterministic lane. The dispatcher never executes arbitrary shell text.
    {
        const auto paths = make_paths(root, "l1");
        write_file(
            paths.hub_data / "policies" / "demo.json",
            policy_json("demo"));

        auto l1 = command(
            "cmd-l1",
            "task.restart.requested",
            "L1",
            "demo",
            "restart_same_parameters",
            "policies/demo");
        l1["payload"].as_object()["untrusted_shell"] =
            "rm -rf / should never execute";

        const auto submit = submit_command(paths, l1);
        CHECK(submit.accepted);
        CHECK(!submit.duplicate);

        const auto duplicate = submit_command(paths, l1);
        CHECK(!duplicate.accepted);
        CHECK(duplicate.duplicate);

        const auto dispatched =
            dispatch_pending_commands(paths);
        CHECK(dispatched.queued_l1 == 1);
        CHECK(dispatched.queued_l2 == 0);
        CHECK(dispatched.rejected == 0);
        CHECK(dispatched.receipts.size() == 1);
        CHECK(dispatched.receipts[0].state == "queued_l1");
        CHECK(dispatched.receipts[0].policy_id == "demo-policy-v1");
        CHECK(dispatched.receipts[0].action_id ==
               "restart_same_parameters");
        CHECK(dispatched.receipts[0].dispatch_kind ==
               "deterministic");
        CHECK(fs::exists(dispatched.receipts[0].dispatch_path));

        const auto dispatch =
            read_json_object(dispatched.receipts[0].dispatch_path);
        CHECK(dispatch.at("handler").as_string() ==
               "restart_same_parameters");
        CHECK(dispatch.at("dispatch_kind").as_string() ==
               "deterministic");
        const auto constraints =
            dispatch.at("constraints").as_array();
        CHECK(constraints.size() == 2);

        const auto receipts_before =
            read_text(paths.hub_data /
                      "commands" / "receipts.jsonl");
        const auto repeated =
            dispatch_pending_commands(paths);
        CHECK(repeated.receipts.empty());
        CHECK(repeated.already_processed == 1);
        CHECK(
            receipts_before ==
            read_text(paths.hub_data /
                      "commands" / "receipts.jsonl"));
    }

    // L2: the same control plane can only route to an Agent profile explicitly
    // authorized by policy. It does not let the command choose an arbitrary
    // local executable or expand into L3 scope.
    {
        const auto paths = make_paths(root, "l2");
        write_file(
            paths.hub_data / "policies" / "demo.json",
            policy_json("demo"));

        const auto l2 = command(
            "cmd-l2",
            "agent.troubleshoot.request",
            "L2",
            "demo",
            "troubleshoot_known_failure",
            "policies/demo.json");
        CHECK(submit_command(paths, l2).accepted);

        const auto dispatched =
            dispatch_pending_commands(paths);
        CHECK(dispatched.queued_l2 == 1);
        CHECK(dispatched.receipts.size() == 1);
        const auto dispatch =
            read_json_object(dispatched.receipts[0].dispatch_path);
        CHECK(dispatch.at("agent_profile").as_string() ==
               "bounded-troubleshooter");
        CHECK(dispatch.at("dispatch_kind").as_string() ==
               "child_agent");
    }

    // L3: never route to an executor. Convert the command into an explicit
    // Issue + durable main-Agent notification instead.
    {
        const auto paths = make_paths(root, "l3");
        const auto l3 = command(
            "cmd-l3",
            "project.method_change.requested",
            "L3",
            "demo");
        CHECK(submit_command(paths, l3).accepted);

        const auto dispatched =
            dispatch_pending_commands(paths);
        CHECK(dispatched.escalated_l3 == 1);
        CHECK(dispatched.queued_l1 == 0);
        CHECK(dispatched.queued_l2 == 0);
        CHECK(dispatched.receipts[0].state == "needs_user");

        const auto projection =
            load_project_event_projection(paths, "demo", 100);
        CHECK(projection.events.size() == 2);
        CHECK(std::any_of(
            projection.events.begin(),
            projection.events.end(),
            [](const EventRecord& event) {
                return event.event_type ==
                       "issue.user_action_required";
            }));
        CHECK(std::any_of(
            projection.events.begin(),
            projection.events.end(),
            [](const EventRecord& event) {
                return event.event_type ==
                       "notification.requested";
            }));

        const auto outbox =
            sync_notification_outbox(paths);
        const auto notification = std::find_if(
            outbox.items.begin(),
            outbox.items.end(),
            [](const NotificationRecord& item) {
                return item.notification_id ==
                       "ntf:cmd:cmd-l3";
            });
        CHECK(notification != outbox.items.end());
        CHECK(notification->reason == "decision_required");
        CHECK(notification->state == "pending");
    }

    // A policy cannot authorize a different authority or command type.
    {
        const auto paths = make_paths(root, "mismatch");
        write_file(
            paths.hub_data / "policies" / "demo.json",
            policy_json("demo"));

        const auto mismatch = command(
            "cmd-mismatch",
            "task.restart.requested",
            "L2",
            "demo",
            "restart_same_parameters",
            "policies/demo");
        CHECK(submit_command(paths, mismatch).accepted);
        const auto result =
            dispatch_pending_commands(paths);
        CHECK(result.rejected == 1);
        CHECK(result.receipts[0].state == "rejected");
        CHECK(result.receipts[0].reason.find(
                   "authority") != std::string::npos);
    }

    // policy_ref is a capability boundary, not a generic filesystem path.
    {
        const auto paths = make_paths(root, "path");
        const auto traversal = command(
            "cmd-traversal",
            "task.restart.requested",
            "L1",
            "demo",
            "restart_same_parameters",
            "../secret-policy");
        CHECK(submit_command(paths, traversal).accepted);
        const auto result =
            dispatch_pending_commands(paths);
        CHECK(result.rejected == 1);
        CHECK(result.receipts[0].reason.find(
                   "HUB_DATA/policies") != std::string::npos);

        auto bad_project = command(
            "cmd-bad-project",
            "task.restart.requested",
            "L1",
            "../escape",
            "restart_same_parameters",
            "policies/demo");
        const auto bad_submit =
            submit_command(paths, bad_project);
        CHECK(!bad_submit.accepted);
        CHECK(bad_submit.reason.find(
                   "filesystem-safe") != std::string::npos);
    }

    // Malformed/duplicate policy definitions fail closed.
    {
        const auto paths = make_paths(root, "policy");
        write_file(
            paths.hub_data / "policies" / "bad.json",
            R"({
              "schema_version": 1,
              "policy_id": "bad",
              "project_id": "demo",
              "actions": [
                {
                  "action_id": "x",
                  "authority": "L1",
                  "dispatch_kind": "deterministic",
                  "handler": "x",
                  "allowed_command_types": ["x"]
                },
                {
                  "action_id": "x",
                  "authority": "L1",
                  "dispatch_kind": "deterministic",
                  "handler": "x",
                  "allowed_command_types": ["x"]
                }
              ]
            })");
        std::string error;
        const auto policy =
            load_recovery_policy(
                paths,
                "policies/bad",
                error);
        CHECK(!policy);
        CHECK(error.find("duplicate") != std::string::npos);
    }

    // One bad inbox line cannot poison valid command processing.
    {
        const auto paths = make_paths(root, "malformed");
        write_file(
            paths.hub_data / "commands" / "inbox.jsonl",
            "{bad json}\n");
        const auto result =
            dispatch_pending_commands(paths);
        CHECK(result.malformed_commands == 1);
        CHECK(result.receipts.empty());
    }

    fs::remove_all(root);
    std::cout << "command control tests passed\n";
    return 0;
}
