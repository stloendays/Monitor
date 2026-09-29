#pragma once

#include "monitor_hub/core.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace monitor_hub {

struct OverviewProject {
    std::string project_id;
    std::string project_name;
    std::string health;
    std::string progress;
    std::string headline;
};

struct OverviewAttentionItem {
    std::string project_id;
    std::string project_name;
    std::string kind;
    std::string summary;
};

struct OverviewAgentActivity {
    std::string project_id;
    std::string project_name;
    std::string label;
    std::string state;
    std::string summary;
    std::string path;
    double time = 0.0;
};

struct OverviewModel {
    int total_projects = 0;
    int ok_projects = 0;
    int working_projects = 0;
    int attention_projects = 0;
    int error_projects = 0;
    int stale_projects = 0;
    int paused_projects = 0;
    int done_projects = 0;

    std::vector<OverviewProject> projects;
    std::vector<OverviewAttentionItem> attention;
    std::vector<OverviewAgentActivity> activity;
};

OverviewModel build_overview_model(
    const std::vector<json::object>& projects,
    const std::map<std::string, json::object>& snapshots,
    std::size_t activity_limit = 24);

}  // namespace monitor_hub
