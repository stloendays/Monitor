#include "monitor_hub/control_orchestrator.hpp"

namespace monitor_hub {

OrchestratorTickResult run_control_tick(
    const RuntimePaths& paths,
    const OrchestratorOptions& options) {

    OrchestratorTickResult result;
    result.started_at = iso_now_local();

    // Ordering is intentional:
    // 1. convert durable commands into validated L1/L2/L3 outcomes;
    // 2. execute/reconcile already-validated L1/L2 work;
    // 3. project any decision/failure/completion notifications into the
    //    durable main-Agent outbox in the same tick.
    result.command_control =
        dispatch_pending_commands(paths);
    result.worker =
        run_dispatch_workers(
            paths,
            options.launch_agents);
    result.outbox =
        sync_notification_outbox(paths);

    result.needs_main_agent =
        result.outbox.pending_count() > 0;
    result.completed_at = iso_now_local();
    return result;
}

json::object orchestrator_tick_to_json(
    const OrchestratorTickResult& result) {

    json::object out;
    out["schema_version"] = 1;
    out["started_at"] = result.started_at;
    out["completed_at"] = result.completed_at;
    out["needs_main_agent"] = result.needs_main_agent;
    out["command_control"] =
        command_control_result_to_json(
            result.command_control);
    out["worker"] =
        dispatch_worker_result_to_json(result.worker);
    out["outbox"] =
        notification_outbox_to_json(
            result.outbox,
            false);
    return out;
}

}  // namespace monitor_hub
