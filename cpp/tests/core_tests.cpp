#include "monitor_hub/core.hpp"

#include <boost/json.hpp>
#include <algorithm>
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

    SystemInfo fixture_sys;
    TaskInfo fixture_task;
    fixture_task.name = "demo__monitor__local__15m";
    fixture_task.state = "Ready";
    fixture_task.result = 0;
    fixture_task.interval = "PT15M";
    fixture_sys.tasks[fixture_task.name] = fixture_task;
    fixture_sys.procs.push_back(ProcessInfo{1234, "python.exe", "python monitor__demo__local__15m.py"});
    const auto fixture_json = system_info_json(fixture_sys);
    assert(fixture_json.at("tasks").as_object().contains("demo__monitor__local__15m"));
    assert(fixture_json.at("procs").as_array().size() == 1);
    assert(fixture_json.at("error").as_string().empty());

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

    // Detached monitor state: pid + command line must both identify the job.
    paths.job_root = root / "jobs";
    const auto job_name = std::string("demo__monitor__local__15m");
    const auto job_dir = paths.job_root / job_name;
    fs::create_directories(job_dir);
    write_file(job_dir / "pid", "4321\n");
    SystemInfo detsys;
    detsys.procs.push_back(ProcessInfo{4321, "python.exe", "python " + (job_dir / "worker.py").string()});

    json::object dp;
    dp["id"] = "detach-demo";
    dp["name"] = "detach-demo";
    dp["adapter"] = "runner_only";
    json::object dr;
    dr["kind"] = "detach";
    dr["name"] = job_name;
    dr["interval_min"] = 15;
    dp["runner"] = dr;

    auto dri = runner_info(dp, detsys, paths);
    assert(dri.exists && dri.running && !dri.paused && !dri.error);

    detsys.procs.clear();
    dri = runner_info(dp, detsys, paths);
    assert(dri.exists && !dri.running && dri.error);
    assert(dri.error->find("gone") != std::string::npos);

    write_file(job_dir / "exitcode", "stopped\n");
    dri = runner_info(dp, detsys, paths);
    assert(dri.exists && !dri.running && dri.paused && !dri.error);

    write_file(job_dir / "exitcode", "0\n");
    dri = runner_info(dp, detsys, paths);
    assert(dri.paused && !dri.error);

    fs::remove(job_dir / "exitcode");
    detsys.procs.push_back(ProcessInfo{4321, "python.exe", "python C:/other/job.py"});
    dri = runner_info(dp, detsys, paths);
    assert(dri.error && dri.error->find("gone") != std::string::npos);

    // Discovery should infer display name/scope/interval from canonical detached-job names.
    write_file(paths.registry, R"({"projects":[]})");
    paths.discovery = true;
    const auto discovered = load_projects(detsys, paths);
    const auto it = std::find_if(discovered.begin(), discovered.end(), [&](const json::object& x) {
        return x.at("id").as_string() == "job:demo__monitor__local__15m";
    });
    assert(it != discovered.end());
    assert(it->at("name").as_string() == "demo · local monitor");
    assert(it->at("area").as_string() == "其他监控 · 后台作业 · local");
    assert(it->at("runner").as_object().at("interval_min").as_int64() == 15);

    // Legacy Markdown adapter: first table, row classification, notes, DONE and next-check text.
    const auto md_status = root / "status_latest.md";
    const auto md_done = root / "DONE";
    write_file(md_status,
        "# Vanda monitor\n"
        "计算正常，没有需要处理的事。\n\n"
        "| 作业 | 状态 | 进度 | 能量 |\n"
        "|---|---|---|---|\n"
        "| slab_clean | 完成 | 最终单点 | -247.1 |\n"
        "| slab_CO | R 02:14 | 离子步 31 | -262.8 |\n"
        "| outside | R | 非监控 | — |\n\n"
        "下次检查：09-29 18:00\n\n"
        "**备注**\n"
        "- scratch 正常\n");
    write_file(md_done, "DONE\n");
    json::object mp;
    mp["id"] = "markdown-demo";
    mp["name"] = "markdown-demo";
    mp["adapter"] = "markdown";
    mp["status_md"] = md_status.string();
    mp["done_file"] = md_done.string();
    json::object mr;
    mr["kind"] = "none";
    mp["runner"] = mr;
    mp["runner_text"] = "fixture";
    const auto ms = snapshot(mp, SystemInfo{}, paths);
    assert(ms.at("health").as_string() == "done");
    assert(ms.at("summary").as_string() == "完成 1，运行 1；另有 1 个作业不归这个监控管");
    const auto& mt = ms.at("table").as_object();
    assert(mt.at("tags").as_array()[0].as_string() == "done");
    assert(mt.at("tags").as_array()[1].as_string() == "run");
    assert(mt.at("tags").as_array()[2].as_string() == "other");
    assert(ms.at("notes").as_array()[0].as_string() == "scratch 正常");
    assert(ms.at("next").as_string() == "09-29 18:00");

    // Detached takeover: successful result + exit:0 should be indexed as ok.
    const auto tk_name = std::string("claude-monitor-20260929-1234");
    const auto tk_dir = paths.job_root / tk_name;
    fs::create_directories(tk_dir);
    write_file(tk_dir / "output.log", "{\"type\":\"result\",\"is_error\":false,\"result\":\"finished\\nmore\"}\n");
    write_file(tk_dir / "exitcode", "0\n");
    write_file(tk_dir / "started", "started\n");
    json::object tp;
    json::object tcfg;
    tcfg["kind"] = "detach";
    tcfg["prefix"] = "claude-monitor-";
    tp["takeovers"] = tcfg;
    auto tks = takeovers(tp, SystemInfo{}, paths);
    assert(tks.size() == 1);
    assert(tks[0].as_object().at("state").as_string() == "ok");
    assert(tks[0].as_object().at("summary").as_string() == "finished");
    assert(tks[0].as_object().at("label").as_string() == "09-29 12:34");

    // Glob takeover: parse the final stream-json result and friendly quota error.
    const auto glob_dir = root / "takeovers";
    fs::create_directories(glob_dir);
    const auto glob_log = glob_dir / "claude_takeover_20260929_1250.jsonl";
    write_file(glob_log, "{\"type\":\"system\",\"subtype\":\"init\"}\n"
                         "{\"type\":\"result\",\"is_error\":true,\"result\":\"usage limit · resets 15:00\"}\n");
    json::object gp;
    json::object gcfg;
    gcfg["kind"] = "glob";
    gcfg["pattern"] = (glob_dir / "claude_takeover_*").string();
    gp["takeovers"] = gcfg;
    tks = takeovers(gp, SystemInfo{}, paths);
    assert(tks.size() == 1);
    assert(tks[0].as_object().at("state").as_string() == "failed");
    assert(std::string(tks[0].as_object().at("summary").as_string()).find("Claude 额度用完") != std::string::npos);
    assert(tks[0].as_object().at("kind").as_string() == "jsonl");
    assert(tks[0].as_object().at("label").as_string() == "09-29 12:50");

    // QoI compatibility adapter: top-level live data overrides stale row checkpoint counts.
    const auto qoi_status = root / "qoi_status.json";
    write_file(qoi_status,
        R"({"updated":"2026-09-29T10:00:00","rows":[)"
        R"({"job":"fold1","status":"COMPLETE","checkpoints":1200,"failures":0,"total":1200,"rate_per_h":142.5,"branch_pushed":true,"params":{"fold":1}},)"
        R"({"job":"fold2","status":"RUNNING","checkpoints":610,"failures":1,"total":1200,"last_checkpoint_min_ago":9,"rate_per_h":138.2,"eta_h":4.3,"branch_pushed":false,"workdir":"C:/qoi/fold2","params":{"fold":2,"seed":43}})"
        R"(],"live":{"fold2":{"checkpoints":642,"failures":1,"newest_checkpoint":"2026-09-29T09:58:00"}},"notify":[],"attention":[]})");
    json::object qp;
    qp["id"] = "qoi-demo";
    qp["name"] = "qoi-demo";
    qp["adapter"] = "qoi";
    qp["status_json"] = qoi_status.string();
    json::object qr;
    qr["kind"] = "none";
    qr["interval_min"] = 15;
    qp["runner"] = qr;
    qp["runner_text"] = "fixture";
    const auto qs = snapshot(qp, SystemInfo{}, paths);
    const auto& qt = qs.at("table").as_object();
    assert(qt.at("rows").as_array().size() == 2);
    assert(qt.at("rows").as_array()[1].as_array()[2].as_string() == "642 / 1200（53.5%）");
    assert(qt.at("tags").as_array()[0].as_string() == "done");
    assert(qt.at("tags").as_array()[1].as_string() == "run");
    assert(qt.at("row_meta").as_array()[1].as_object().at("task_id").as_string() == "fold2");
    assert(qt.at("row_meta").as_array()[1].as_object().at("open_path").as_string() == "C:/qoi/fold2");
    assert(qt.at("row_meta").as_array()[1].as_object().at("params").as_object().at("seed").as_int64() == 43);
    assert(qs.at("summary").as_string() == "1/2 个作业已完成；fold2 53.5%，约 4.3 小时 进行中");
    assert(!qs.at("done").as_bool());

    // Setup adapter: request/report/takeover/NEEDS_USER must become a real table row and attention item.
    const auto req_dir = paths.hub_data / "requests";
    fs::create_directories(req_dir);
    const auto stamp = std::string("20260929-130000");
    write_file(req_dir / (stamp + "_request.md"),
               "【监控任务】\n项目名称：CeOx Phase B\n项目目录（本机路径）：C:/Research/CeOx\n");
    write_file(req_dir / (stamp + "_report.md"),
               "# 办理报告\n已创建监控脚本并完成第一轮检查。\nNEEDS_USER: 请选择 walltime 24h 或 48h\n");
    const auto setup_job = paths.job_root / ("hub-setup-" + stamp);
    fs::create_directories(setup_job);
    write_file(setup_job / "output.log",
               "{\"type\":\"result\",\"is_error\":false,\"result\":\"setup finished\"}\n");
    write_file(setup_job / "exitcode", "0\n");
    write_file(setup_job / "started", "started\n");

    json::object sp;
    sp["id"] = "hub-setup";
    sp["name"] = "新任务办理";
    sp["adapter"] = "setup";
    json::object sr;
    sr["kind"] = "none";
    sp["runner"] = sr;
    sp["runner_text"] = "fixture";
    const auto ss = snapshot(sp, SystemInfo{}, paths);
    assert(ss.at("health").as_string() == "attention");
    assert(ss.at("summary").as_string() == "1 个请求");
    assert(ss.at("table").as_object().at("rows").as_array().size() == 1);
    assert(ss.at("table").as_object().at("rows").as_array()[0].as_array()[1].as_string() == "CeOx Phase B");
    assert(ss.at("table").as_object().at("rows").as_array()[0].as_array()[2].as_string() == "办好了，有事要你定");
    assert(ss.at("table").as_object().at("tags").as_array()[0].as_string() == "bad");
    assert(ss.at("attention").as_array().size() == 1);
    assert(std::string(ss.at("attention").as_array()[0].as_string()).find("walltime") != std::string::npos);
    assert(ss.at("takeovers").as_array().size() == 1);
    assert(ss.at("table").as_object().at("row_meta").as_array()[0].as_object().at("result").as_string() ==
           (req_dir / (stamp + "_report.md")).string());

    // results_glob: matched files should appear alongside explicitly registered results.
    const auto results_dir = root / "results";
    fs::create_directories(results_dir);
    write_file(results_dir / "a.md", "# A\n");
    write_file(results_dir / "b.md", "# B\n");
    write_file(results_dir / "ignore.txt", "x\n");
    json::object rp;
    rp["id"] = "results-demo";
    rp["name"] = "results-demo";
    rp["adapter"] = "runner_only";
    json::object rr2;
    rr2["kind"] = "none";
    rp["runner"] = rr2;
    rp["runner_text"] = "fixture";
    json::array registered;
    json::object explicit_result;
    explicit_result["label"] = "显式结果";
    explicit_result["path"] = (root / "explicit.md").string();
    registered.emplace_back(std::move(explicit_result));
    rp["results"] = std::move(registered);
    json::array globs;
    globs.emplace_back((results_dir / "*.md").string());
    rp["results_glob"] = std::move(globs);
    const auto rs = snapshot(rp, SystemInfo{}, paths);
    assert(rs.at("results_list").as_array().size() == 3);
    assert(rs.at("results_list").as_array()[1].as_array()[0].as_string() == "a.md");
    assert(rs.at("results_list").as_array()[2].as_array()[0].as_string() == "b.md");

    fs::remove_all(root);
    std::cout << "core tests passed\n";
    return 0;
}
