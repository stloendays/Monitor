#pragma once

#include "monitor_hub/core.hpp"
#include "monitor_hub/setup_request.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace monitor_hub {

struct DispatchRecord {
    int schema_version = 0;
    std::string dispatch_id;
    std::string command_id;
    std::string command_type;
    std::string dispatched_at;
    std::string project_id;
    std::string task_id;
    std::string issue_id;
    std::string correlation_id;
    std::string authority;
    std::string policy_id;
    std::string policy_ref;
    std::string action_id;
    std::string dispatch_kind;
    std::string handler;
    std::string agent_profile;
    std::vector<std::string> constraints;
    std::vector<std::string> completion_criteria;
    std::vector<std::string> context_refs;
    json::object requested_command;
};

struct AgentRunPlan {
    std::string dispatch_id;
    std::string run_id;
    std::string job_name;
    fs::path run_dir;
    fs::path prompt_file;
    fs::path result_file;
    fs::path working_directory;
    SetupAgentRuntime runtime;
    std::string command;
    std::vector<std::string> arguments;
};

struct WorkerReceipt {
    std::string dispatch_id;
    std::string command_id;
    std::string project_id;
    std::string authority;
    std::string state;
    std::string reason;
    std::string updated_at;
    std::string run_id;
    fs::path result_file;
};

struct DispatchWorkerResult {
    fs::path receipt_path;
    std::vector<WorkerReceipt> receipts;
    std::vector<std::string> diagnostics;
    std::size_t l1_completed = 0;
    std::size_t l1_failed = 0;
    std::size_t l2_launched = 0;
    std::size_t l2_completed = 0;
    std::size_t l2_needs_user = 0;
    std::size_t l2_failed = 0;
    std::size_t active_l2 = 0;
    std::size_t already_terminal = 0;
    std::size_t malformed_dispatches = 0;
};

std::optional<DispatchRecord> load_dispatch_record(
    const fs::path& path,
    std::string& error);

std::optional<AgentRunPlan> prepare_l2_agent_run(
    const RuntimePaths& paths,
    const DispatchRecord& dispatch,
    std::string& error);

DispatchWorkerResult run_dispatch_workers(
    const RuntimePaths& paths,
    bool launch_agents = true);

json::object worker_receipt_to_json(const WorkerReceipt& receipt);
json::object dispatch_worker_result_to_json(
    const DispatchWorkerResult& result);

}  // namespace monitor_hub
