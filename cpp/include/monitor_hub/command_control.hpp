#pragma once

#include "monitor_hub/core.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace monitor_hub {

struct CommandEnvelope {
    int schema_version = 0;
    std::string command_id;
    std::string command_type;
    std::string requested_at;
    std::string project_id;
    std::string task_id;
    std::string issue_id;
    std::string correlation_id;
    std::string requested_by_kind;
    std::string requested_by_id;
    std::string authority;
    std::string policy_ref;
    std::vector<std::string> constraints;
    std::vector<std::string> completion_criteria;
    std::vector<std::string> context_refs;
    json::object payload;
};

struct PolicyAction {
    std::string action_id;
    std::string authority;
    std::string dispatch_kind;
    std::string handler;
    json::object handler_config;
    std::string agent_profile;
    std::vector<std::string> allowed_command_types;
    std::vector<std::string> constraints;
    std::vector<std::string> completion_criteria;
    bool enabled = true;
};

struct RecoveryPolicy {
    int schema_version = 0;
    std::string policy_id;
    std::string project_id;
    fs::path source_path;
    std::vector<PolicyAction> actions;
};

struct CommandReceipt {
    std::string command_id;
    std::string command_type;
    std::string project_id;
    std::string state;
    std::string reason;
    std::string processed_at;
    std::string policy_id;
    std::string action_id;
    std::string dispatch_kind;
    fs::path dispatch_path;
};

struct CommandControlResult {
    fs::path inbox_path;
    fs::path receipt_path;
    std::vector<CommandReceipt> receipts;
    std::vector<std::string> diagnostics;
    std::size_t malformed_commands = 0;
    std::size_t duplicate_commands = 0;
    std::size_t queued_l1 = 0;
    std::size_t queued_l2 = 0;
    std::size_t escalated_l3 = 0;
    std::size_t rejected = 0;
    std::size_t already_processed = 0;
};

struct CommandSubmitResult {
    bool accepted = false;
    bool duplicate = false;
    std::string command_id;
    std::string reason;
    fs::path inbox_path;
};

std::optional<CommandEnvelope> parse_command_envelope(
    const json::object& root,
    std::string& error);

std::optional<RecoveryPolicy> load_recovery_policy(
    const RuntimePaths& paths,
    const std::string& policy_ref,
    std::string& error);

const PolicyAction* find_policy_action(
    const RecoveryPolicy& policy,
    const std::string& action_id);

CommandSubmitResult submit_command(
    const RuntimePaths& paths,
    const json::object& command);

CommandSubmitResult submit_command_file(
    const RuntimePaths& paths,
    const fs::path& command_file);

CommandControlResult dispatch_pending_commands(
    const RuntimePaths& paths);

json::object command_receipt_to_json(const CommandReceipt& receipt);
json::object command_control_result_to_json(
    const CommandControlResult& result);
json::object command_submit_result_to_json(
    const CommandSubmitResult& result);

std::string command_policy_example();

}  // namespace monitor_hub
