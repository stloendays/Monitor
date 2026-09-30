#pragma once

#include "monitor_hub/core.hpp"

namespace monitor_hub {

// Reads Windows Task Scheduler and Win32_Process through COM/WMI.
// On non-Windows platforms this returns SystemInfo with an explanatory error.
SystemInfo probe_system_info();

// Stable machine-readable projection used by --probe-system.
// This is a raw read-only platform probe, not the normalized project snapshot.
json::object system_info_json(const SystemInfo& system);

}  // namespace monitor_hub
