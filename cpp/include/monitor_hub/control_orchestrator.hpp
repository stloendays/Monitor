#pragma once

#include "monitor_hub/command_control.hpp"
#include "monitor_hub/dispatch_worker.hpp"
#include "monitor_hub/notification_outbox.hpp"

#include <string>

namespace monitor_hub {

struct OrchestratorOptions {
    bool launch_agents = true;
};

struct OrchestratorTickResult {
    std::string started_at;
    std::string completed_at;
    CommandControlResult command_control;
    DispatchWorkerResult worker;
    NotificationOutbox outbox;
    bool needs_main_agent = false;
};

OrchestratorTickResult run_control_tick(
    const RuntimePaths& paths,
    const OrchestratorOptions& options = {});

json::object orchestrator_tick_to_json(
    const OrchestratorTickResult& result);

}  // namespace monitor_hub
