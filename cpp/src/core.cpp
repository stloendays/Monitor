#include "monitor_hub/core.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

namespace monitor_hub {
namespace {
using json::array;
using json::object;
using json::value;

std::string str(const value* v, std::string fallback = {}) {
    if (!v) return fallback;
    if (v->is_string()) return std::string(v->as_string());
    if (v->is_int64()) return std::to_string(v->as_int64());
    if (v->is_uint64()) return std::to_string(v->as_uint64());
    if (v->is_double()) { std::ostringstream os; os << v->as_double(); return os.str(); }
    if (v->is_bool()) return v->as_bool() ? "true" : "false";
    return fallback;
}

bool boolean(const value* v, bool fallback = false) {
    return v && v->is_bool() ? v->as_bool() : fallback;
}

std::optional<int> integer(const value* v) {
    if (!v) return std::nullopt;
    if (v->is_int64()) return static_cast<int>(v->as_int64());
    if (v->is_uint64()) return static_cast<int>(v->as_uint64());
    if (v->is_double()) return static_cast<int>(v->as_double());
    return std::nullopt;
}

const object* obj(const value* v) { return v && v->is_object() ? &v->as_object() : nullptr; }
const array* arr(const value* v) { return v && v->is_array() ? &v->as_array() : nullptr; }

std::vector<std::string> strings(const value* v) {
    std::vector<std::string> out;
    if (const auto* a = arr(v)) for (const auto& x : *a) out.push_back(str(&x));
    return out;
}

array json_strings(const std::vector<std::string>& xs) {
    array out; for (const auto& x : xs) out.emplace_back(x); return out;
}

std::string trim(std::string s) {
    auto keep = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), keep));
    s.erase(std::find_if(s.rbegin(), s.rend(), keep).base(), s.end());
    return s;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

object runner_json(const RunnerInfo& r) {
    object o;
    o["kind"] = r.kind; o["name"] = r.name;
    o["interval"] = r.interval_min ? value(*r.interval_min) : value(nullptr);
    o["exists"] = r.exists; o["running"] = r.running; o["paused"] = r.paused;
    o["error"] = r.error ? value(*r.error) : value(nullptr);
    o["text"] = r.text;
    o["last"] = r.last ? value(*r.last) : value(nullptr);
    o["next"] = r.next ? value(*r.next) : value(nullptr);
    return o;
}

array result_list(const object& p, const object& snap) {
    array out;
    auto append = [&](const value* v) {
        if (const auto* a = arr(v)) for (const auto& x : *a) {
            const auto* o = obj(&x); if (!o) continue;
            auto path = str(o->if_contains("path")); if (path.empty()) continue;
            auto label = str(o->if_contains("label")); if (label.empty()) label = fs::path(path).filename().string();
            array pair; pair.emplace_back(label); pair.emplace_back(path); out.emplace_back(std::move(pair));
        }
    };
    append(p.if_contains("results"));
    append(snap.if_contains("results"));
    return out;
}

object generic_adapter(const object& p, const RunnerInfo& runner) {
    const fs::path path = str(p.if_contains("status_json"));
    object st;
    if (auto v = read_json(path); v && v->is_object()) st = v->as_object();
    auto updated = parse_iso_local_seconds(str(st.if_contains("updated")));
    if (!updated) updated = mtime_seconds(path);

    array cols, rows;
    std::vector<std::string> tags;
    if (const auto* table = obj(st.if_contains("table"))) {
        if (const auto* a = arr(table->if_contains("cols"))) for (const auto& x : *a) cols.emplace_back(str(&x));
        if (const auto* a = arr(table->if_contains("rows"))) for (const auto& row_v : *a) {
            array row; if (const auto* row_a = arr(&row_v)) for (const auto& cell : *row_a) row.emplace_back(str(&cell));
            rows.emplace_back(std::move(row));
        }
        tags = strings(table->if_contains("tags"));
    }
    tags.resize(rows.size());

    array row_meta;
    if (const auto* src = obj(st.if_contains("table"))) {
        if (const auto* a = arr(src->if_contains("row_meta"))) for (const auto& x : *a) row_meta.emplace_back(x.is_object() ? value(x.as_object()) : value(object{}));
    }
    while (row_meta.size() < rows.size()) row_meta.emplace_back(object{});
    if (row_meta.size() > rows.size()) row_meta.resize(rows.size());
    object table;
    table["cols"] = std::move(cols); table["rows"] = std::move(rows); table["tags"] = json_strings(tags); table["row_meta"] = std::move(row_meta);
    object snap;
    snap["updated"] = updated ? value(*updated) : value(nullptr);
    snap["headline"] = str(st.if_contains("headline"));
    const auto summary = str(st.if_contains("summary"));
    snap["summary"] = summary.empty() ? count_summary(tags) : summary;
    snap["notes"] = json_strings(strings(st.if_contains("notes")));
    snap["extras"] = array{};
    snap["table"] = std::move(table);
    snap["attention"] = json_strings(strings(st.if_contains("attention")));
    const auto working = str(st.if_contains("working"));
    snap["working"] = working.empty() ? value(nullptr) : value(working);
    snap["done"] = boolean(st.if_contains("done"));
    snap["results"] = st.if_contains("results") && st.at("results").is_array() ? st.at("results") : value(array{});
    const auto next = str(st.if_contains("next"));
    if (runner.next) snap["next"] = short_time(*runner.next);
    else if (!next.empty() && next.find('T') != std::string::npos) snap["next"] = short_time(next);
    else snap["next"] = next.empty() ? value(nullptr) : value(next);
    const auto error = str(st.if_contains("error"));
    snap["error"] = error.empty() ? value(nullptr) : value(error);
    snap["takeovers"] = array{};
    return snap;
}

object setup_adapter(const RuntimePaths& paths) {
    const auto dir = paths.hub_data / "requests";
    double updated = 0.0; std::size_t count = 0;
    if (fs::is_directory(dir)) for (const auto& e : fs::directory_iterator(dir)) {
        updated = std::max(updated, mtime_seconds(e.path()).value_or(0.0));
        if (e.is_regular_file() && e.path().filename().string().ends_with("_request.md")) ++count;
    }
    object tb; tb["cols"] = array{"提交时间", "项目", "状态", "说明"}; tb["rows"] = array{}; tb["tags"] = array{};
    object s;
    s["updated"] = updated > 0 ? value(updated) : value(nullptr);
    s["headline"] = count ? "交给后台 Claude 设置的监控任务。办好后，新项目会出现在左侧“项目”里。" :
                            "还没有提交过新任务。点左下角“新建监控任务”，按格式填写后交给后台 Claude 办理。";
    s["summary"] = count ? std::to_string(count) + " 个请求" : "";
    s["notes"] = array{}; s["extras"] = array{}; s["table"] = std::move(tb); s["attention"] = array{};
    s["working"] = nullptr; s["done"] = false; s["takeovers"] = array{}; s["next"] = nullptr;
    return s;
}

object runner_only_adapter(const RunnerInfo& r) {
    object tb; tb["cols"] = array{}; tb["rows"] = array{}; tb["tags"] = array{};
    object s;
    s["updated"] = nullptr;
    s["headline"] = "这个监控还没登记到监控总台，这里只能看到它的运行情况。登记后可以看到进度和后台处理记录。";
    s["notes"] = array{}; s["extras"] = array{}; s["table"] = std::move(tb); s["summary"] = r.text;
    s["attention"] = array{}; s["takeovers"] = array{}; s["next"] = nullptr; s["done"] = false;
    return s;
}

} // namespace

std::string read_text(const fs::path& path, std::optional<std::size_t> limit) {
    if (path.empty()) return {};
    std::ifstream in(path, std::ios::binary); if (!in) return {};
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (limit && data.size() > *limit) data.resize(*limit);
    if (data.rfind("\xEF\xBB\xBF", 0) == 0) data.erase(0, 3);
    return data;
}

std::optional<json::value> read_json(const fs::path& path) {
    const auto text = read_text(path); if (text.empty()) return std::nullopt;
    boost::system::error_code ec; auto v = json::parse(text, ec); if (ec) return std::nullopt; return v;
}

std::optional<double> mtime_seconds(const fs::path& path) {
    std::error_code ec; const auto ft = fs::last_write_time(path, ec); if (ec) return std::nullopt;
    const auto sys = std::chrono::system_clock::now() +
        std::chrono::duration_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now());
    return std::chrono::duration<double>(sys.time_since_epoch()).count();
}

std::optional<double> parse_iso_local_seconds(const std::string& text) {
    if (text.size() < 19) return std::nullopt;
    std::tm tm{}; std::istringstream in(text.substr(0, 19)); in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (in.fail()) return std::nullopt; tm.tm_isdst = -1; const auto t = std::mktime(&tm);
    return t == -1 ? std::nullopt : std::optional<double>(static_cast<double>(t));
}

std::string iso_now_local() {
    const auto now = std::chrono::system_clock::now(); const auto t = std::chrono::system_clock::to_time_t(now); std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os; os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S"); return os.str();
}

std::string short_time(const std::string& text) {
    if (text.empty()) return "—"; auto s = text; std::replace(s.begin(), s.end(), 'T', ' '); return s.size() >= 16 ? s.substr(5, 11) : s;
}

std::string ago(double epoch_seconds) {
    const auto now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto m = (now - epoch_seconds) / 60.0;
    if (m < 1) return "刚刚";
    if (m < 60) return std::to_string(static_cast<int>(m)) + " 分钟前";
    if (m < 2880) { std::ostringstream os; os << std::fixed << std::setprecision(1) << m / 60 << " 小时前"; return os.str(); }
    return std::to_string(static_cast<int>(m / 1440)) + " 天前";
}

std::string every(std::optional<int> minutes) {
    if (!minutes || *minutes <= 0) return {};
    if (*minutes < 60) return "每 " + std::to_string(*minutes) + " 分钟";
    if (*minutes % 60 == 0) return "每 " + std::to_string(*minutes / 60) + " 小时";
    std::ostringstream os; os << "每 " << static_cast<double>(*minutes) / 60 << " 小时"; return os.str();
}

std::optional<int> iso_minutes(const std::string& duration) {
    const std::regex re(R"(^P(?:(\d+)D)?T?(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?$)"); std::smatch m;
    if (!std::regex_match(duration, m, re)) return std::nullopt;
    const int d = m[1].matched ? std::stoi(m[1]) : 0, h = m[2].matched ? std::stoi(m[2]) : 0, mi = m[3].matched ? std::stoi(m[3]) : 0;
    return d * 1440 + h * 60 + mi;
}

std::string strip_md(std::string text) {
    text = std::regex_replace(text, std::regex(R"(\*\*(.*?)\*\*)"), "$1"); text.erase(std::remove(text.begin(), text.end(), '`'), text.end()); return text;
}

std::string classify_row(const std::string& status, const std::string& progress) {
    const auto s = trim(status), l = lower(s);
    if (progress.find("非监控") != std::string::npos) return "other";
    if (s.find("完成") != std::string::npos || l.rfind("complete", 0) == 0) return "done";
    if (s.rfind("失败", 0) == 0 || l.rfind("fail", 0) == 0 || l.rfind("dead", 0) == 0 || s.rfind("停", 0) == 0) return "bad";
    if (s.rfind("R", 0) == 0 || s.find("运行") != std::string::npos) return "run";
    if (s.rfind("Q", 0) == 0 || s.rfind("H", 0) == 0 || s.find("排队") != std::string::npos) return "queue";
    return {};
}

std::string count_summary(const std::vector<std::string>& tags) {
    int d=0,r=0,q=0,b=0,o=0; for (const auto& t:tags) { if(t=="done")++d; else if(t=="run")++r; else if(t=="queue")++q; else if(t=="bad")++b; else if(t=="other")++o; }
    std::vector<std::string> p; if(d)p.push_back("完成 "+std::to_string(d)); if(r)p.push_back("运行 "+std::to_string(r)); if(q)p.push_back("排队 "+std::to_string(q)); if(b)p.push_back("异常 "+std::to_string(b));
    std::string s; for(std::size_t i=0;i<p.size();++i){if(i)s+="，";s+=p[i];} if(o)s+="；另有 "+std::to_string(o)+" 个作业不归这个监控管"; return s;
}

SystemInfo load_system_info_fixture(const fs::path& path) {
    SystemInfo out; auto v=read_json(path); if(!v||!v->is_object())return out; const auto& root=v->as_object();
    if(const auto* tasks=obj(root.if_contains("tasks"))) for(const auto& kv:*tasks){ if(!kv.value().is_object())continue; const auto& o=kv.value().as_object(); TaskInfo t; t.name=std::string(kv.key()); t.state=str(o.if_contains("state")); t.last=str(o.if_contains("last")); t.result=integer(o.if_contains("result")).value_or(0); t.next=str(o.if_contains("next")); t.interval=str(o.if_contains("interval")); t.action=str(o.if_contains("action")); out.tasks[t.name]=std::move(t); }
    out.error=str(root.if_contains("error")); return out;
}

RunnerInfo runner_info(const object& p, const SystemInfo& system, const RuntimePaths&) {
    RunnerInfo x; const auto* r=obj(p.if_contains("runner")); if(!r)return x; x.kind=str(r->if_contains("kind")); x.name=str(r->if_contains("name")); x.interval_min=integer(r->if_contains("interval_min"));
    if(x.kind=="none"){x.exists=true;x.text=str(p.if_contains("runner_text"));return x;}
    if(x.kind=="schtask"){
        const auto it=system.tasks.find(x.name); if(it==system.tasks.end()){x.error="找不到定时任务 "+x.name;x.text="Windows 定时任务 "+x.name+"（不存在）";return x;}
        const auto& t=it->second; x.exists=true; if(auto m=iso_minutes(t.interval))x.interval_min=m; x.paused=t.state=="Disabled"; x.running=t.state=="Running"; if(!t.last.empty())x.last=t.last;if(!x.paused&&!t.next.empty())x.next=t.next;
        std::string rs=t.result==0?"成功":t.result==267009?"正在运行":t.result==267011?"还没运行过":"出错"; if(rs=="出错")x.error="定时任务上次运行出错";
        x.text="Windows 定时任务 "+x.name+"，"+(every(x.interval_min).empty()?"按计划":every(x.interval_min))+" · 上次运行 "+short_time(t.last)+"（"+rs+"）· "+(x.paused?"已停用":"下次 "+short_time(t.next)); return x;
    }
    x.error="C++ phase 1 尚未接入 runner.kind="+x.kind; return x;
}

json::array takeovers(const object&, const SystemInfo&, const RuntimePaths&) { return {}; }

json::object snapshot(const object& p, const SystemInfo& system, const RuntimePaths& paths) {
    const auto runner=runner_info(p,system,paths); const auto adapter=str(p.if_contains("adapter"),"runner_only"); object s;
    if(adapter=="generic")s=generic_adapter(p,runner); else if(adapter=="setup")s=setup_adapter(paths); else if(adapter=="runner_only")s=runner_only_adapter(runner); else {s=runner_only_adapter(runner);s["error"]="C++ phase 1 尚未迁移 adapter："+adapter;}
    s["runner"]=runner_json(runner); s["results_list"]=result_list(p,s);
    std::optional<double> updated; if(const auto* v=s.if_contains("updated");v&&v->is_double())updated=v->as_double();
    const bool stale=updated&&runner.interval_min&&!runner.paused&&adapter!="setup"&&(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()-*updated>(2**runner.interval_min+30)*60.0);
    const bool done=boolean(s.if_contains("done")), has_error=!str(s.if_contains("error")).empty()||runner.error.has_value(); const auto attention=strings(s.if_contains("attention")); const bool working=!str(s.if_contains("working")).empty(); std::string health;
    if(done)health="done"; else if(has_error)health="error"; else if(runner.paused){health="paused";s["history"]=s.if_contains("attention")?s.at("attention"):value(array{});s["attention"]=array{};} else if(!attention.empty())health="attention"; else if(stale)health="stale"; else if(working)health="working"; else if(adapter=="runner_only")health=runner.exists?"ok":"error"; else health="ok";
    s["health"]=health; const auto err=str(s.if_contains("error")); s["problem"]=!err.empty()?err:(runner.error?*runner.error:(stale?"状态文件 "+ago(*updated)+" 没更新（应"+every(runner.interval_min)+"更新一次）":"")); return s;
}

std::vector<object> load_projects(const SystemInfo& system, const RuntimePaths& paths) {
    std::vector<object> out; auto reg=read_json(paths.registry); if(reg&&reg->is_object())if(const auto* a=arr(reg->as_object().if_contains("projects")))for(const auto& x:*a)if(x.is_object()&&!str(x.as_object().if_contains("id")).empty())out.push_back(x.as_object());
    object setup; setup["id"]="hub-setup";setup["name"]="新任务办理";setup["area"]="交给后台 Claude 设置的新监控";setup["adapter"]="setup";object r;r["kind"]="none";setup["runner"]=r;setup["runner_text"]="点左下角“新建监控任务”提交；每个请求由一个后台 Claude 办理";setup["dir"]=(paths.hub_data/"requests").string();setup["builtin"]=true;out.push_back(std::move(setup));
    if(!paths.discovery)return out;
    for(const auto& [name,t]:system.tasks){bool known=false;for(const auto& p:out)if(const auto* pr=obj(p.if_contains("runner"));pr&&str(pr->if_contains("kind"))=="schtask"&&str(pr->if_contains("name"))==name)known=true;if(known)continue;object p;p["id"]="task:"+name;p["name"]=name;p["area"]="其他监控 · 定时任务";p["adapter"]="runner_only";object rr;rr["kind"]="schtask";rr["name"]=name;p["runner"]=rr;p["action"]=t.action;p["unregistered"]=true;out.push_back(std::move(p));}
    return out;
}

json::object dump_all(const SystemInfo& system, const RuntimePaths& paths) { object ps; for(const auto& p:load_projects(system,paths))ps[str(p.if_contains("id"))]=snapshot(p,system,paths); object out;out["generated"]=iso_now_local();out["registry"]=paths.registry.string();out["projects"]=std::move(ps);return out; }

RuntimePaths runtime_paths_from_env(const fs::path& exe) {
    RuntimePaths p; const auto base=exe.empty()?fs::current_path():exe.parent_path(); const auto def=(base/".."/"hub"/"monitor_hub_projects.json").lexically_normal().string();
    p.registry=std::getenv("MONITOR_HUB_REGISTRY")?std::getenv("MONITOR_HUB_REGISTRY"):def; p.hub_data=std::getenv("MONITOR_HUB_DATA")?std::getenv("MONITOR_HUB_DATA"):"D:\\Research\\monitor-hub";
#ifdef _WIN32
    const auto local=std::getenv("LOCALAPPDATA")?std::getenv("LOCALAPPDATA"):"C:\\Users\\ASUS\\AppData\\Local";
#else
    const auto local=std::getenv("LOCALAPPDATA")?std::getenv("LOCALAPPDATA"):"/tmp";
#endif
    p.job_root=fs::path(local)/"cdesktop-jobs"; p.discovery=std::getenv("MONITOR_HUB_NO_DISCOVERY")==nullptr; return p;
}

} // namespace monitor_hub
