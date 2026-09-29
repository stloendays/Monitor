#include "monitor_hub/core.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <set>
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

struct ContractIssues {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

std::string join(const std::vector<std::string>& xs, const std::string& sep, std::size_t limit = static_cast<std::size_t>(-1)) {
    std::string out;
    for (std::size_t i = 0; i < xs.size() && i < limit; ++i) {
        if (!out.empty()) out += sep;
        out += xs[i];
    }
    return out;
}

ContractIssues validate_status_contract(const object& st) {
    ContractIssues out;
    if (const auto* schema = st.if_contains("schema_version")) {
        const bool ok = (schema->is_int64() && schema->as_int64() >= 1) ||
                        (schema->is_uint64() && schema->as_uint64() >= 1);
        if (!ok) out.errors.push_back("schema_version 必须是 >= 1 的整数");
    }
    const auto* updated = st.if_contains("updated");
    if (!updated || !updated->is_string() || str(updated).empty() || !parse_iso_local_seconds(str(updated)))
        out.errors.push_back("updated 必须是有效 ISO 8601 字符串");
    const auto* headline = st.if_contains("headline");
    if (!headline || !headline->is_string() || str(headline).empty())
        out.errors.push_back("headline 必须是非空字符串");
    if (const auto* done = st.if_contains("done"); done && !done->is_bool())
        out.errors.push_back("done 必须是 boolean");

    const auto* table = obj(st.if_contains("table"));
    if (!table) {
        out.errors.push_back("table 必须是 object");
        return out;
    }
    const auto* cols = arr(table->if_contains("cols"));
    const auto* rows = arr(table->if_contains("rows"));
    if (!cols) out.errors.push_back("table.cols 必须是字符串数组");
    else for (const auto& x : *cols) if (!x.is_string()) { out.errors.push_back("table.cols 必须是字符串数组"); break; }
    if (!rows) out.errors.push_back("table.rows 必须是二维数组");
    if (rows) {
        for (std::size_t i = 0; i < rows->size(); ++i) {
            const auto* row = arr(&(*rows)[i]);
            if (!row) {
                out.errors.push_back("table.rows[" + std::to_string(i) + "] 必须是数组");
                continue;
            }
            if (cols && row->size() != cols->size())
                out.errors.push_back("table.rows[" + std::to_string(i) + "] 列数与 cols 不一致");
            for (std::size_t j = 0; j < row->size(); ++j) {
                const auto& cell = (*row)[j];
                if (!(cell.is_string() || cell.is_int64() || cell.is_uint64() || cell.is_double()))
                    out.errors.push_back("table.rows[" + std::to_string(i) + "][" + std::to_string(j) + "] 只能是字符串或数字");
            }
        }
    }

    if (const auto* tags_v = table->if_contains("tags")) {
        if (!tags_v->is_array()) out.errors.push_back("table.tags 必须是数组");
        else {
            const auto& tags = tags_v->as_array();
            if (rows && tags.size() != rows->size()) out.warnings.push_back("table.tags 数量与 rows 不一致；总台会自动补齐/截断");
            static const std::set<std::string> allowed{"done","run","queue","bad","other",""};
            for (std::size_t i = 0; i < tags.size(); ++i) {
                if (!tags[i].is_string() || !allowed.count(str(&tags[i])))
                    out.errors.push_back("table.tags[" + std::to_string(i) + "] 不在允许集合中");
            }
        }
    }

    if (const auto* meta_v = table->if_contains("row_meta")) {
        if (!meta_v->is_array()) out.errors.push_back("table.row_meta 必须是数组");
        else {
            const auto& metas = meta_v->as_array();
            if (rows && metas.size() != rows->size()) out.warnings.push_back("table.row_meta 数量与 rows 不一致；总台会自动补齐/截断");
            std::set<std::string> ids;
            for (std::size_t i = 0; i < metas.size(); ++i) {
                const auto* m = obj(&metas[i]);
                if (!m) { out.errors.push_back("table.row_meta[" + std::to_string(i) + "] 必须是 object"); continue; }
                if (const auto* tid = m->if_contains("task_id")) {
                    if (!tid->is_string() || str(tid).empty()) out.errors.push_back("task_id 必须是非空字符串");
                    else if (!ids.insert(str(tid)).second) out.warnings.push_back("task_id " + str(tid) + " 重复");
                } else if (!m->empty()) {
                    out.warnings.push_back("非空 row_meta 建议提供稳定 task_id");
                }
                if (const auto* params = m->if_contains("params"); params && !params->is_object())
                    out.errors.push_back("row_meta.params 必须是 object");
            }
        }
    }
    return out;
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
    const auto issues = validate_status_contract(st);
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
    array extras;
    for (const auto& w : issues.warnings) extras.emplace_back("状态协议提示：" + w);
    snap["extras"] = std::move(extras);
    snap["table"] = std::move(table);
    snap["attention"] = json_strings(strings(st.if_contains("attention")));
    const auto working = str(st.if_contains("working"));
    snap["working"] = working.empty() ? value(nullptr) : value(working);
    snap["done"] = boolean(st.if_contains("done")) && issues.errors.empty();
    snap["results"] = st.if_contains("results") && st.at("results").is_array() ? st.at("results") : value(array{});
    const auto next = str(st.if_contains("next"));
    if (runner.next) snap["next"] = short_time(*runner.next);
    else if (!next.empty() && next.find('T') != std::string::npos) snap["next"] = short_time(next);
    else snap["next"] = next.empty() ? value(nullptr) : value(next);
    const auto monitor_error = str(st.if_contains("error"));
    const auto schema_error = issues.errors.empty() ? std::string{} : "状态文件格式错误：" + join(issues.errors, "；", 4);
    std::string error = monitor_error;
    if (!schema_error.empty()) {
        if (!error.empty()) error += "；";
        error += schema_error;
    }
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
    if(const auto* procs=arr(root.if_contains("procs"))) for(const auto& x:*procs){ if(!x.is_object())continue; const auto& o=x.as_object(); ProcessInfo p; p.pid=integer(o.if_contains("pid")).value_or(0); p.name=str(o.if_contains("name")); p.cmd=str(o.if_contains("cmd")); out.procs.push_back(std::move(p)); }
    out.error=str(root.if_contains("error")); return out;
}

std::optional<std::string> detach_state(const std::string& name, const SystemInfo& system, const RuntimePaths& paths) {
    const auto dir = paths.job_root / name;
    std::error_code ec;
    if(!fs::is_directory(dir, ec) || ec) return std::nullopt;
    const auto exit_file = dir / "exitcode";
    if(fs::exists(exit_file, ec) && !ec) return "exit:" + trim(read_text(exit_file));
    int pid = 0;
    try { pid = std::stoi(trim(read_text(dir / "pid"))); } catch(...) { return std::string("gone"); }
    const auto needle = lower(dir.string());
    for(const auto& p : system.procs) {
        if(p.pid == pid && lower(p.cmd).find(needle) != std::string::npos) return std::string("running");
    }
    return std::string("gone");
}

bool pid_alive(std::int64_t pid, const SystemInfo& system) {
    return std::any_of(system.procs.begin(), system.procs.end(), [&](const ProcessInfo& p){ return p.pid == pid; });
}

std::string read_tail(const fs::path& path, std::size_t max_bytes = 200000) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if(!in) return {};
    const auto end = in.tellg();
    if(end <= 0) return {};
    const auto size = static_cast<std::uint64_t>(end);
    const auto start = size > max_bytes ? size - max_bytes : 0;
    in.seekg(static_cast<std::streamoff>(start), std::ios::beg);
    std::string data(static_cast<std::size_t>(size - start), '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    return data;
}

std::optional<object> stream_result(const fs::path& path) {
    const auto text = read_tail(path);
    std::size_t end = text.size();
    while(end > 0) {
        auto begin = text.rfind('\n', end - 1);
        begin = begin == std::string::npos ? 0 : begin + 1;
        auto line = trim(text.substr(begin, end - begin));
        if(!line.empty() && line.front() == '{') {
            boost::system::error_code ec;
            auto value = json::parse(line, ec);
            if(!ec && value.is_object() && str(value.as_object().if_contains("type")) == "result")
                return value.as_object();
        }
        if(begin == 0) break;
        end = begin - 1;
    }
    return std::nullopt;
}

std::string first_line(std::string text) {
    const auto p = text.find_first_of("\r\n");
    if(p != std::string::npos) text.resize(p);
    return trim(text);
}

std::string friendly_error(std::string text) {
    const auto l = lower(text);
    if(l.find("session limit") != std::string::npos || l.find("usage limit") != std::string::npos || l.find("hit your limit") != std::string::npos)
        return "Claude 额度用完";
    if(l.find("not logged in") != std::string::npos || l.find("please run /login") != std::string::npos || l.find("invalid api key") != std::string::npos)
        return "Claude 没有登录";
    if(l.find("api error") != std::string::npos || l.find("internal server error") != std::string::npos || l.find("overloaded") != std::string::npos)
        return "Claude 服务出错（" + text.substr(0, std::min<std::size_t>(80, text.size())) + "）";
    return text;
}

array detach_takeovers(const std::string& prefix, const SystemInfo& system, const RuntimePaths& paths) {
    array out;
    std::error_code ec;
    if(!fs::is_directory(paths.job_root, ec) || ec) return out;
    const std::regex stamp_re(R"((\d{8})-(\d{4}))");
    for(const auto& entry : fs::directory_iterator(paths.job_root, ec)) {
        if(ec) break;
        if(!entry.is_directory(ec) || ec) continue;
        const auto name = entry.path().filename().string();
        if(name.rfind(prefix, 0) != 0) continue;
        const auto st = detach_state(name, system, paths);
        const auto log = entry.path() / "output.log";
        const auto result = stream_result(log);
        const bool result_error = result && boolean(result->if_contains("is_error"));
        const auto result_text = result ? str(result->if_contains("result")) : std::string{};
        const std::string state = st && *st == "running" ? "running" :
                                  (result && !result_error && st && *st == "exit:0") ? "ok" : "failed";
        std::string error;
        if(state == "failed") error = result_error ? friendly_error(result_text) :
            "进程退出（" + (st ? *st : std::string("不存在")) + "），没有结果";
        const auto summary = strip_md(first_line(error.empty() ? result_text : error));

        double when = mtime_seconds(entry.path() / "started").value_or(mtime_seconds(entry.path()).value_or(0.0));
        std::string label = name;
        std::smatch m;
        if(std::regex_search(name, m, stamp_re))
            label = m[1].str().substr(4,2) + "-" + m[1].str().substr(6,2) + " " + m[2].str().substr(0,2) + ":" + m[2].str().substr(2,2);

        object item;
        item["key"] = name; item["time"] = when; item["state"] = state;
        item["error"] = error.substr(0, std::min<std::size_t>(160, error.size()));
        item["summary"] = summary.substr(0, std::min<std::size_t>(200, summary.size()));
        item["path"] = log.string(); item["kind"] = "jsonl"; item["label"] = label;
        out.emplace_back(std::move(item));
    }
    return out;
}

double stamp_epoch(const std::string& stamp) {
    if(stamp.size() != 13) return 0.0;
    std::tm tm{};
    try {
        tm.tm_year = std::stoi(stamp.substr(0,4)) - 1900;
        tm.tm_mon = std::stoi(stamp.substr(4,2)) - 1;
        tm.tm_mday = std::stoi(stamp.substr(6,2));
        tm.tm_hour = std::stoi(stamp.substr(9,2));
        tm.tm_min = std::stoi(stamp.substr(11,2));
    } catch(...) { return 0.0; }
    tm.tm_isdst = -1;
    return static_cast<double>(std::mktime(&tm));
}

array glob_takeovers(const object& config, const SystemInfo& system) {
    array out;
    const fs::path pattern = str(config.if_contains("pattern"));
    if(pattern.empty()) return out;
    const auto folder = pattern.parent_path();
    std::error_code ec;
    if(!fs::is_directory(folder, ec) || ec) return out;

    struct Pair { fs::path jsonl; fs::path md; };
    std::map<std::string, Pair> stamps;
    const std::regex file_re(R"(^claude_takeover_(\d{8}_\d{4})\.(jsonl|md)$)", std::regex::icase);
    for(const auto& entry : fs::directory_iterator(folder, ec)) {
        if(ec) break;
        if(!entry.is_regular_file(ec) || ec) continue;
        std::smatch m;
        const auto name = entry.path().filename().string();
        if(!std::regex_match(name, m, file_re)) continue;
        if(lower(m[2].str()) == "jsonl") stamps[m[1].str()].jsonl = entry.path();
        else stamps[m[1].str()].md = entry.path();
    }

    std::optional<std::int64_t> lock_pid;
    const fs::path lock = str(config.if_contains("lock"));
    if(!lock.empty() && fs::exists(lock, ec) && !ec) {
        try { lock_pid = std::stoll(trim(read_text(lock))); } catch(...) {}
    }
    const auto newest = stamps.empty() ? std::string{} : stamps.rbegin()->first;

    for(const auto& [stamp, files] : stamps) {
        const auto md = trim(read_text(files.md));
        const auto result = files.jsonl.empty() ? std::optional<object>{} : stream_result(files.jsonl);
        std::string state;
        if(stamp == newest && lock_pid && pid_alive(*lock_pid, system)) state = "running";
        else if(result) state = boolean(result->if_contains("is_error")) ? "failed" : "ok";
        else state = md.empty() ? "failed" : "ok";

        std::string summary = md.empty() ? (result ? first_line(str(result->if_contains("result"))) : std::string{}) : first_line(md);
        std::string error = state == "failed" && result ? friendly_error(str(result->if_contains("result"))) : std::string{};
        if(!error.empty()) summary = error;
        const auto when = stamp_epoch(stamp);

        object item;
        item["key"] = stamp; item["time"] = when; item["state"] = state; item["error"] = error;
        item["summary"] = strip_md(summary).substr(0, std::min<std::size_t>(200, strip_md(summary).size()));
        item["path"] = (!files.jsonl.empty() ? files.jsonl : files.md).string();
        item["kind"] = files.jsonl.empty() ? "md" : "jsonl";
        item["label"] = stamp.substr(4,2) + "-" + stamp.substr(6,2) + " " + stamp.substr(9,2) + ":" + stamp.substr(11,2);
        out.emplace_back(std::move(item));
    }
    return out;
}

RunnerInfo runner_info(const object& p, const SystemInfo& system, const RuntimePaths& paths) {
    RunnerInfo x; const auto* r=obj(p.if_contains("runner")); if(!r)return x; x.kind=str(r->if_contains("kind")); x.name=str(r->if_contains("name")); x.interval_min=integer(r->if_contains("interval_min"));
    if(x.kind=="none"){x.exists=true;x.text=str(p.if_contains("runner_text"));return x;}
    if(x.kind=="schtask"){
        const auto it=system.tasks.find(x.name); if(it==system.tasks.end()){x.error="找不到定时任务 "+x.name;x.text="Windows 定时任务 "+x.name+"（不存在）";return x;}
        const auto& t=it->second; x.exists=true; if(auto m=iso_minutes(t.interval))x.interval_min=m; x.paused=t.state=="Disabled"; x.running=t.state=="Running"; if(!t.last.empty())x.last=t.last;if(!x.paused&&!t.next.empty())x.next=t.next;
        std::string rs=t.result==0?"成功":t.result==267009?"正在运行":t.result==267011?"还没运行过":"出错"; if(rs=="出错"){std::ostringstream os;os<<"定时任务上次运行出错（代码 0x"<<std::uppercase<<std::hex<<t.result<<"）";x.error=os.str();}
        x.text="Windows 定时任务 "+x.name+"，"+(every(x.interval_min).empty()?"按计划":every(x.interval_min))+" · 上次运行 "+short_time(t.last)+"（"+rs+"）· "+(x.paused?"已停用":"下次 "+short_time(t.next)); return x;
    }
    if(x.kind=="detach"){
        const auto st=detach_state(x.name,system,paths);
        x.exists=st.has_value();
        x.running=st&&*st=="running";
        x.paused=st&&(*st=="exit:stopped"||*st=="exit:0");
        if(!st)x.error="找不到后台作业 "+x.name;
        else if(*st!="running"&&*st!="exit:stopped"&&*st!="exit:0")x.error="监控进程意外退出（"+*st+"）";
        const auto state_text=!st?"不存在":*st=="running"?"运行中":"没在运行（"+*st+"）";
        x.text="后台作业 "+x.name+"，"+every(x.interval_min)+" · "+state_text;
        return x;
    }
    x.error="C++ 尚未接入 runner.kind="+x.kind; return x;
}

json::array takeovers(const object& p, const SystemInfo& system, const RuntimePaths& paths) {
    const auto* config = obj(p.if_contains("takeovers"));
    if(!config) return {};
    array out;
    const auto kind = str(config->if_contains("kind"));
    if(kind == "detach") out = detach_takeovers(str(config->if_contains("prefix")), system, paths);
    else if(kind == "glob") out = glob_takeovers(*config, system);
    std::sort(out.begin(), out.end(), [](const value& a, const value& b) {
        const auto* ao = obj(&a); const auto* bo = obj(&b);
        const double at = ao && ao->if_contains("time") && ao->at("time").is_double() ? ao->at("time").as_double() : 0.0;
        const double bt = bo && bo->if_contains("time") && bo->at("time").is_double() ? bo->at("time").as_double() : 0.0;
        return at > bt;
    });
    return out;
}

json::object snapshot(const object& p, const SystemInfo& system, const RuntimePaths& paths) {
    const auto runner=runner_info(p,system,paths); const auto adapter=str(p.if_contains("adapter"),"runner_only"); object s;
    if(adapter=="generic")s=generic_adapter(p,runner); else if(adapter=="setup")s=setup_adapter(paths); else if(adapter=="runner_only")s=runner_only_adapter(runner); else {s=runner_only_adapter(runner);s["error"]="C++ phase 1 尚未迁移 adapter："+adapter;}
    s["runner"]=runner_json(runner); s["results_list"]=result_list(p,s); s["takeovers"]=takeovers(p,system,paths);
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
    std::error_code ec;
    if(fs::is_directory(paths.job_root,ec)&&!ec) for(const auto& entry:fs::directory_iterator(paths.job_root,ec)){
        if(ec)break;if(!entry.is_directory(ec)||ec)continue;const auto name=entry.path().filename().string();if(lower(name).find("monitor")==std::string::npos)continue;
        bool known=false;for(const auto& p:out)if(const auto* pr=obj(p.if_contains("runner"));pr&&str(pr->if_contains("kind"))=="detach"&&str(pr->if_contains("name"))==name)known=true;if(known)continue;
        object p;p["id"]="job:"+name;p["name"]=name;p["area"]="其他监控 · 后台作业";p["adapter"]="runner_only";object rr;rr["kind"]="detach";rr["name"]=name;p["runner"]=rr;
        p["action"]=read_tail(entry.path()/"run.ps1",1200);p["unregistered"]=true;out.push_back(std::move(p));
    }
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
