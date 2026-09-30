#pragma once

#include "monitor_hub/dispatch_worker.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace monitor_hub {

struct DeterministicHandlerResult {
    bool supported = false;
    bool success = false;
    bool action_applied = false;
    std::string handler;
    std::string summary;
    std::string error;
    int exit_code = -1;
    std::uint64_t process_id = 0;
    std::vector<std::string> evidence_refs;
    json::object metadata;
};

bool is_registered_deterministic_handler(
    const std::string& handler);

std::vector<std::string> registered_deterministic_handlers();

DeterministicHandlerResult execute_deterministic_handler(
    const DispatchRecord& dispatch);

}  // namespace monitor_hub
