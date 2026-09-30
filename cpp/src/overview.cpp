#include "monitor_hub/overview.hpp"

#include <algorithm>
#include <sstream>

namespace monitor_hub {
namespace {

std::string str(const json::value* value, std::string fallback = {}) {
    if (!value) return fallback;
    if (value->is_string()) return std::string(value->as_string());
    if (value->is_int64()) return std::to_string(value->as_int64());
    if (value->is_uint64()) return std::to_string(value->as_uint64());
    if (value->is_double()) {
        std::ostringstream out;
        out << value->as_double();
        return out.str();
    }
    if (value->is_bool()) return value->as_bool() ? "true" : "false";
    return fallback;
}

bool boolean(const json::value* value, bool fallback = false) {
    return value && value->is_bool() ? value->as_bool() : fallback;
}

const json::array* array(const json::value* value) {
    return value && value->is_array() ? &value->as_array() : nullptr;
}

const json::object* object(const json::value* value) {
    return value && value->is_object() ? &value->as_object() : nullptr;
}

double number(const json::value* value) {
    if (!value) return 0.0;
    if (value->is_double()) return value->as_double();
    if (value->is_int64()) return static_cast<double>(value->as_int64());
    if (value->is_uint64()) return static_cast<double>(value->as_uint64());
    return 0.0;
}

bool builtin_project(const json::object& project) {
    return boolean(project.if_contains("builtin"));
}

std::string progress_summary(const json::object& snapshot) {
    auto summary = str(snapshot.if_contains("summary"));
    if (!summary.empty()) return summary;

    const auto* table = object(snapshot.if_contains("table"));
    const auto* tags = table ? array(table->if_contains("tags")) : nullptr;
    if (!tags) return {};

    std::vector<std::string> values;
    values.reserve(tags->size());
    for (const auto& tag : *tags) values.push_back(str(&tag));
    return count_summary(values);
}

std::string current_summary(const json::object& snapshot) {
    auto text = str(snapshot.if_contains("problem"));
    if (text.empty()) text = str(snapshot.if_contains("headline"));
    return text;
}

void count_health(OverviewModel& model, const std::string& health) {
    if (health == "working") ++model.working_projects;
    else if (health == "attention") ++model.attention_projects;
    else if (health == "error") ++model.error_projects;
    else if (health == "stale") ++model.stale_projects;
    else if (health == "paused") ++model.paused_projects;
    else if (health == "done") ++model.done_projects;
    else ++model.ok_projects;
}

}  // namespace

OverviewModel build_overview_model(
    const std::vector<json::object>& projects,
    const std::map<std::string, json::object>& snapshots,
    std::size_t activity_limit) {

    OverviewModel model;

    for (const auto& project : projects) {
        if (builtin_project(project)) continue;

        const auto project_id = str(project.if_contains("id"));
        if (project_id.empty()) continue;

        const auto project_name = str(project.if_contains("name"), project_id);
        const auto it = snapshots.find(project_id);

        ++model.total_projects;

        if (it == snapshots.end()) {
            ++model.error_projects;
            model.projects.push_back({
                project_id,
                project_name,
                "error",
                {},
                "monitor snapshot unavailable",
            });
            model.attention.push_back({
                project_id,
                project_name,
                "监控错误",
                "monitor snapshot unavailable",
            });
            continue;
        }

        const auto& snapshot = it->second;
        const auto health = str(snapshot.if_contains("health"), "ok");
        const auto headline = current_summary(snapshot);

        count_health(model, health);
        model.projects.push_back({
            project_id,
            project_name,
            health,
            progress_summary(snapshot),
            headline,
        });

        const auto* attention = array(snapshot.if_contains("attention"));
        if (health == "attention") {
            bool emitted = false;
            if (attention) {
                for (const auto& item : *attention) {
                    const auto summary = str(&item);
                    if (summary.empty()) continue;
                    model.attention.push_back({
                        project_id,
                        project_name,
                        "需要决策",
                        summary,
                    });
                    emitted = true;
                }
            }
            if (!emitted) {
                model.attention.push_back({
                    project_id,
                    project_name,
                    "需要处理",
                    headline,
                });
            }
        } else if (health == "error") {
            model.attention.push_back({
                project_id,
                project_name,
                "监控错误",
                headline,
            });
        } else if (health == "stale") {
            model.attention.push_back({
                project_id,
                project_name,
                "状态过期",
                headline,
            });
        }

        const auto* takeovers = array(snapshot.if_contains("takeovers"));
        if (!takeovers) continue;
        for (const auto& value : *takeovers) {
            const auto* item = object(&value);
            if (!item) continue;
            model.activity.push_back({
                project_id,
                project_name,
                str(item->if_contains("label")),
                str(item->if_contains("state")),
                str(item->if_contains("summary")),
                str(item->if_contains("path")),
                number(item->if_contains("time")),
            });
        }
    }

    std::stable_sort(
        model.activity.begin(),
        model.activity.end(),
        [](const OverviewAgentActivity& lhs, const OverviewAgentActivity& rhs) {
            return lhs.time > rhs.time;
        });

    if (activity_limit > 0 && model.activity.size() > activity_limit)
        model.activity.resize(activity_limit);

    return model;
}


OverviewModel build_overview_model(
    const std::vector<json::object>& projects,
    const std::map<std::string, json::object>& snapshots,
    const std::map<std::string, ProjectEventProjection>& event_projections,
    std::size_t activity_limit) {

    auto model = build_overview_model(projects, snapshots, 0);

    auto project_name = [&](const std::string& id) {
        const auto found = std::find_if(
            model.projects.begin(),
            model.projects.end(),
            [&](const OverviewProject& item) { return item.project_id == id; });
        return found == model.projects.end() ? id : found->project_name;
    };

    auto decrement_health = [&](const std::string& health) {
        int* counter = nullptr;
        if (health == "working") counter = &model.working_projects;
        else if (health == "attention") counter = &model.attention_projects;
        else if (health == "error") counter = &model.error_projects;
        else if (health == "stale") counter = &model.stale_projects;
        else if (health == "paused") counter = &model.paused_projects;
        else if (health == "done") counter = &model.done_projects;
        else counter = &model.ok_projects;
        if (*counter > 0) --*counter;
    };

    for (const auto& [project_id, projection] : event_projections) {
        for (const auto& issue : projection.issues) {
            if (issue.resolved || !issue.user_action_required) continue;
            const auto summary =
                issue.summary.empty() ? issue.issue_id : issue.summary;

            auto existing = std::find_if(
                model.attention.begin(),
                model.attention.end(),
                [&](const OverviewAttentionItem& item) {
                    return item.project_id == project_id &&
                           item.summary == summary;
                });
            if (existing != model.attention.end()) {
                existing->kind = "需要决策";
                existing->source = "event";
                existing->issue_id = issue.issue_id;
            } else {
                model.attention.push_back({
                    project_id,
                    project_name(project_id),
                    "需要决策",
                    summary,
                    "event",
                    issue.issue_id,
                });
            }

            auto project = std::find_if(
                model.projects.begin(),
                model.projects.end(),
                [&](const OverviewProject& item) {
                    return item.project_id == project_id;
                });
            if (project != model.projects.end() &&
                project->health != "attention" &&
                project->health != "error" &&
                project->health != "stale") {
                decrement_health(project->health);
                ++model.attention_projects;
                project->health = "attention";
                project->headline = summary;
            }
        }

        for (const auto& event : projection.events) {
            if (event.event_type.rfind("agent.", 0) != 0) continue;

            std::string state = "running";
            if (event.event_type == "agent.failed") state = "failed";
            else if (event.event_type == "agent.completed" ||
                     event.event_type == "agent.action_finished")
                state = "ok";

            model.activity.push_back({
                project_id,
                project_name(project_id),
                short_time(event.occurred_at),
                state,
                event.summary.empty()
                    ? event_display_name(event.event_type)
                    : event.summary,
                projection.source_path.string(),
                parse_iso_local_seconds(event.occurred_at).value_or(0.0),
                "event",
            });
        }
    }

    std::stable_sort(
        model.activity.begin(),
        model.activity.end(),
        [](const OverviewAgentActivity& lhs, const OverviewAgentActivity& rhs) {
            return lhs.time > rhs.time;
        });
    if (activity_limit > 0 && model.activity.size() > activity_limit)
        model.activity.resize(activity_limit);

    return model;
}

}  // namespace monitor_hub
