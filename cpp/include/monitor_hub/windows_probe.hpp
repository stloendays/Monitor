#pragma once

#include "monitor_hub/core.hpp"

namespace monitor_hub {

// Reads Windows Task Scheduler and Win32_Process through COM/WMI.
// On non-Windows platforms this returns SystemInfo with an explanatory error.
SystemInfo probe_system_info();

}  // namespace monitor_hub
