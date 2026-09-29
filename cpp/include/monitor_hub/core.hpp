#pragma once

#include <boost/json.hpp>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace monitor_hub {

namespace json = boost::json;
namespace fs = std::filesystem;

struct TaskInfo {
    std::string name;
    std::string state;
    std::string last;
    std::uint32_t result = 0;
    std::string next;
    std::string interval;
    std::string action;
};

struct ProcessInfo {
    std::int64_t pid = 0;
    std::string name;
    std::string cmd;
};

struct SystemInfo {
    std::map<std::string, TaskInfo> tasks;
    std::vector<ProcessInfo> procs;
    std::string error;
};

struct RunnerInfo {
    std::string kind;
    std::string name;
    std::optional<int> interval_min;
    bool exists = false;
    bool running = false;
    bool paused = false;
    std::optional<std::string> error;
    std::string text;
    std::optional<std::string> last;
    std::optional<std::string> next;
};

struct RuntimePaths {
    fs::path registry;
    fs::path hub_data;
    fs::path job_root;
    bool discovery = true;
};

std::string read_text(const fs::path& path, std::optional<std::size_t> limit = std::nullopt);
std::optional<json::value> read_json(const fs::path& path);
std::optional<double> mtime_seconds(const fs::path& path);
std::optional<double> parse_iso_local_seconds(const std::string& text);
std::string iso_now_local();
std::string short_time(const std::string& text);
std::string ago(double epoch_seconds);
std::string every(std::optional<int> minutes);
std::optional<int> iso_minutes(const std::string& duration);

std::string strip_md(std::string text);
std::string classify_row(const std::string& status, const std::string& progress = {});
std::string count_summary(const std::vector<std::string>& tags);

SystemInfo load_system_info_fixture(const fs::path& path);
SystemInfo probe_system_info();
json::object system_info_json(const SystemInfo& system);
RunnerInfo runner_info(const json::object& project, const SystemInfo& system, const RuntimePaths& paths);
json::array takeovers(const json::object& project, const SystemInfo& system, const RuntimePaths& paths);
json::object snapshot(const json::object& project, const SystemInfo& system, const RuntimePaths& paths);
std::vector<json::object> load_projects(const SystemInfo& system, const RuntimePaths& paths);
json::object dump_all(const SystemInfo& system, const RuntimePaths& paths);

RuntimePaths runtime_paths_from_env(const fs::path& executable_path = {});

}  // namespace monitor_hub
