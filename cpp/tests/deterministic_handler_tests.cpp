#include "monitor_hub/command_control.hpp"
#include "monitor_hub/dispatch_worker.hpp"
#include "monitor_hub/event_store.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

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

json::object command(
    const std::string& id,
    const std::string& policy_ref,
    const std::string& action_id) {

    json::object requested_by;
    requested_by["kind"] = "monitor";
    requested_by["id"] = "handler-test-monitor";

    json::object payload;
    payload["action_id"] = action_id;
    // These values are deliberately malicious/incorrect. They must remain
    // inert data because executable/script authority comes from policy.
    payload["program"] = "C:/command-payload-must-not-run.exe";
    payload["script_path"] = "/command/payload/must/not/run.pbs";
    payload["arguments"] = json::array{"--payload-override"};

    json::object out;
    out["schema_version"] = 1;
    out["command_id"] = id;
    out["command_type"] = "task.restart.requested";
    out["requested_at"] = "2026-09-30T14:00:00+08:00";
    out["project_id"] = "demo";
    out["task_id"] = "task-1";
    out["issue_id"] = "issue-1";
    out["correlation_id"] = "corr-" + id;
    out["requested_by"] = std::move(requested_by);
    out["authority"] = "L1";
    out["policy_ref"] = policy_ref;
    out["constraints"] = json::array{"same parameters only"};
    out["completion_criteria"] =
        json::array{"replacement execution is observed"};
    out["context_refs"] = json::array{};
    out["payload"] = std::move(payload);
    return out;
}

json::object policy_action(
    const std::string& action_id,
    const std::string& handler,
    json::object config) {

    json::object action;
    action["action_id"] = action_id;
    action["authority"] = "L1";
    action["dispatch_kind"] = "deterministic";
    action["handler"] = handler;
    action["handler_config"] = std::move(config);
    action["allowed_command_types"] =
        json::array{"task.restart.requested"};
    action["constraints"] =
        json::array{"do not change scientific parameters"};
    action["completion_criteria"] =
        json::array{"restart request is accepted"};
    action["enabled"] = true;
    return action;
}

json::object policy(json::object action) {
    json::object out;
    out["schema_version"] = 1;
    out["policy_id"] = "handler-policy-v1";
    out["project_id"] = "demo";
    json::array actions;
    actions.emplace_back(std::move(action));
    out["actions"] = std::move(actions);
    return out;
}

bool wait_for_file(const fs::path& path) {
    for (int i = 0; i < 100; ++i) {
        if (fs::exists(path)) return true;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(20));
    }
    return fs::exists(path);
}

bool has_event(
    const monitor_hub::ProjectEventProjection& projection,
    const std::string& event_type) {
    for (const auto& event : projection.events)
        if (event.event_type == event_type) return true;
    return false;
}

void set_qsub(const fs::path& executable) {
#ifdef _WIN32
    _putenv_s("MONITOR_HUB_QSUB", executable.string().c_str());
#else
    setenv("MONITOR_HUB_QSUB", executable.string().c_str(), 1);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    using namespace monitor_hub;

    CHECK(argc >= 2);
    const fs::path fixture =
        fs::absolute(fs::path(argv[1]));
    CHECK(fs::is_regular_file(fixture));

    const auto root =
        fs::temp_directory_path() /
        "monitor-hub-deterministic-handler-tests";
    fs::remove_all(root);
    fs::create_directories(root);

    // Local restart: program + argv are frozen in policy. Command payload
    // attempts to override them but must not affect the dispatch/worker.
    {
        const auto paths = paths_for(root, "local");
        const auto project_dir =
            root / "local" / "project";
        fs::create_directories(project_dir);
        const auto marker =
            project_dir / "local-restart.marker";

        json::object config;
        config["program"] = fixture.string();
        config["arguments"] =
            json::array{"--record", marker.string(), "policy-local"};
        config["working_directory"] =
            project_dir.string();

        write_json(
            paths.hub_data / "policies" / "local.json",
            policy(policy_action(
                "local_restart",
                "local_process_restart_v1",
                std::move(config))));

        CHECK(submit_command(
                  paths,
                  command(
                      "cmd-local",
                      "policies/local",
                      "local_restart"))
                  .accepted);

        const auto control =
            dispatch_pending_commands(paths);
        CHECK(control.queued_l1 == 1);
        CHECK(control.receipts.size() == 1);

        const auto dispatch_value =
            read_json(control.receipts[0].dispatch_path);
        CHECK(dispatch_value && dispatch_value->is_object());
        const auto& dispatch_object =
            dispatch_value->as_object();
        CHECK(
            dispatch_object.at("handler").as_string() ==
            "local_process_restart_v1");
        const auto& frozen =
            dispatch_object.at("handler_config").as_object();
        CHECK(
            frozen.at("program").as_string() ==
            fixture.string());
        CHECK(
            frozen.at("arguments").as_array()[2].as_string() ==
            "policy-local");

        const auto worker =
            run_dispatch_workers(paths, false);
        CHECK(worker.l1_completed == 1);
        CHECK(worker.l1_failed == 0);
        CHECK(wait_for_file(marker));
        CHECK(read_text(marker).find("policy-local") !=
              std::string::npos);

        const auto projection =
            load_project_event_projection(paths, "demo", 100);
        CHECK(has_event(projection, "issue.action_applied"));
        CHECK(has_event(projection, "task.restarted"));
        CHECK(!has_event(projection, "issue.recovery_verified"));
        CHECK(!has_event(projection, "issue.resolved"));
    }

    // PBS restart: qsub binary is deployment configuration; policy freezes the
    // exact PBS script. The worker waits for qsub acceptance, then emits
    // action_applied/task.restarted but still not recovery_verified.
    {
        const auto paths = paths_for(root, "pbs");
        const auto project_dir =
            root / "pbs" / "project";
        fs::create_directories(project_dir);
        const auto script = project_dir / "restart.pbs";
        write_text(script, "# fixture PBS script\n");
        set_qsub(fixture);

        json::object config;
        config["script_path"] = script.string();
        config["working_directory"] =
            project_dir.string();

        write_json(
            paths.hub_data / "policies" / "pbs.json",
            policy(policy_action(
                "pbs_restart",
                "pbs_qsub_restart_v1",
                std::move(config))));

        CHECK(submit_command(
                  paths,
                  command(
                      "cmd-pbs",
                      "policies/pbs",
                      "pbs_restart"))
                  .accepted);
        const auto control =
            dispatch_pending_commands(paths);
        CHECK(control.queued_l1 == 1);

        const auto worker =
            run_dispatch_workers(paths, false);
        CHECK(worker.l1_completed == 1);
        CHECK(worker.l1_failed == 0);
        CHECK(fs::exists(
            fs::path(script.string() + ".submitted")));

        const auto projection =
            load_project_event_projection(paths, "demo", 100);
        CHECK(has_event(projection, "issue.action_applied"));
        CHECK(has_event(projection, "task.restarted"));
        CHECK(!has_event(projection, "issue.resolved"));
    }

    // Handler config syntax is validated at policy load time.
    {
        const auto paths = paths_for(root, "invalid");
        json::object config;
        config["program"] = "relative-program.exe";
        write_json(
            paths.hub_data / "policies" / "bad.json",
            policy(policy_action(
                "bad_local",
                "local_process_restart_v1",
                std::move(config))));

        std::string error;
        const auto loaded =
            load_recovery_policy(
                paths,
                "policies/bad",
                error);
        CHECK(!loaded);
        CHECK(error.find("absolute path") !=
              std::string::npos);
    }

    fs::remove_all(root);
    std::cout << "deterministic handler tests passed\n";
    return 0;
}
