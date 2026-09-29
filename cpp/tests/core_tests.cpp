#include "monitor_hub/core.hpp"

#include <boost/json.hpp>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
namespace json = boost::json;

static void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << s;
}

int main() {
    using namespace monitor_hub;

    assert(classify_row("完成") == "done");
    assert(classify_row("R 02:14") == "run");
    assert(classify_row("Q") == "queue");
    assert(classify_row("停止") == "bad");
    assert(classify_row("NELM 停").empty());
    assert(classify_row("R", "非监控") == "other");
    assert(count_summary({"done", "run", "queue", "bad", "other"}) == "完成 1，运行 1，排队 1，异常 1；另有 1 个作业不归这个监控管");
    assert(iso_minutes("PT5H") == 300);
    assert(iso_minutes("PT1H30M") == 90);
    assert(short_time("2026-09-28T21:45:00") == "09-28 21:45");

    const auto root = fs::temp_directory_path() / "monitor-hub-cpp-core-test";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto status = root / "hub_status.json";
    write_file(status,
        R"({"updated":"2026-09-28T21:45:00","headline":"正常","summary":"","done":false,"attention":[],"working":"","table":{"cols":["作业","状态"],"rows":[["a","完成"],["b","运行中"]],"tags":["done","run"],"row_meta":[{"task_id":"a","open_path":"C:/demo/a"}]},"notes":[],"next":"2026-09-28T22:00:00","error":""})");

    json::object p;
    p["id"] = "x";
    p["name"] = "x";
    p["adapter"] = "generic";
    p["status_json"] = status.string();
    json::object r;
    r["kind"] = "none";
    r["interval_min"] = 15;
    p["runner"] = r;
    p["runner_text"] = "test";

    RuntimePaths paths;
    paths.registry = root / "registry.json";
    paths.hub_data = root / "hubdata";
    paths.job_root = root / "jobs";
    paths.discovery = false;
    SystemInfo sys;
    const auto s = snapshot(p, sys, paths);
    assert(s.at("health").as_string() == "ok" || s.at("health").as_string() == "stale");
    assert(s.at("summary").as_string() == "完成 1，运行 1");
    assert(s.at("results_list").as_array().empty());
    const auto& meta = s.at("table").as_object().at("row_meta").as_array();
    assert(meta.size() == 2);
    assert(meta[0].as_object().at("task_id").as_string() == "a");

    write_file(status,
        R"({"updated":"2026-09-28T21:45:00","headline":"done","done":true,"attention":["x"],"working":"x","table":{"cols":[],"rows":[],"tags":[]},"error":"boom"})");
    const auto done = snapshot(p, sys, paths);
    assert(done.at("health").as_string() == "done");

    write_file(status,
        R"({"updated":"bad-time","headline":"","done":false,"table":{"cols":["任务","状态"],"rows":[["a"]],"tags":["run"],"row_meta":[{"task_id":"a"},{"task_id":"a"}]},"attention":[],"notes":[],"error":""})");
    const auto invalid = snapshot(p, sys, paths);
    assert(invalid.at("health").as_string() == "error");
    assert(std::string(invalid.at("problem").as_string()).find("状态文件格式错误") != std::string::npos);
    assert(!invalid.at("extras").as_array().empty());

    // Detached runners are resolved from pid/exitcode files plus the WMI-style process list.
    const auto job = paths.job_root / "demo__monitor__local__15m";
    write_file(job / "pid", "4242\n");
    json::object dp;
    dp["id"] = "detached";
    json::object dr;
    dr["kind"] = "detach";
    dr["name"] = "demo__monitor__local__15m";
    dr["interval_min"] = 15;
    dp["runner"] = dr;
    SystemInfo detached_sys;
    detached_sys.procs.push_back(ProcessInfo{4242, "python.exe", "python monitor.py --job-root \"" + job.string() + "\""});
    const auto live = runner_info(dp, detached_sys, paths);
    assert(live.exists && live.running && !live.paused && !live.error);
    write_file(job / "exitcode", "0\n");
    const auto stopped = runner_info(dp, detached_sys, paths);
    assert(stopped.exists && !stopped.running && stopped.paused && !stopped.error);

    // Detached takeover history is indexed from cdesktop job folders.
    const auto tk = paths.job_root / "demo-takeover-20260929-1315";
    write_file(tk / "exitcode", "0\n");
    write_file(tk / "output.log",
        "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"result\":\"fixed and resumed\",\"duration_ms\":1000,\"total_cost_usd\":0.01}\n");
    json::object tp;
    tp["id"] = "tk";
    json::object tr;
    tr["kind"] = "none";
    tp["runner"] = tr;
    json::object tcfg;
    tcfg["kind"] = "detach";
    tcfg["prefix"] = "demo-takeover-";
    tp["takeovers"] = tcfg;
    const auto indexed = takeovers(tp, detached_sys, paths);
    assert(indexed.size() == 1);
    assert(indexed[0].as_object().at("state").as_string() == "ok");
    assert(indexed[0].as_object().at("summary").as_string() == "fixed and resumed");

    // Discovery includes unregistered detached monitor folders.
    const auto unregistered = paths.job_root / "extra__monitor__local__15m";
    write_file(unregistered / "exitcode", "0\n");
    write_file(unregistered / "run.ps1", "python monitor__extra__local__15m.py\n");
    write_file(paths.registry, "{\"projects\":[]}");
    paths.discovery = true;
    const auto projects = load_projects(detached_sys, paths);
    bool found_detached = false;
    for(const auto& item : projects)
        if(item.if_contains("id") && item.at("id").is_string() && item.at("id").as_string() == "job:extra__monitor__local__15m")
            found_detached = true;
    assert(found_detached);

    fs::remove_all(root);
    std::cout << "core tests passed\n";
    return 0;
}
