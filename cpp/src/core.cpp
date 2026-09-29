#include "monitor_hub/core.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
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

std::string pathish(std::string s) {
    s = lower(std::move(s));
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.find("//") != std::string::npos) s = std::regex_replace(s, std::regex("//+"), "/");
    return s;
}

struct MonitorIdentity {
    bool canonical = false;
    std::string project;
    std::string scope;
    std::optional<int> interval_min;
    std::string display_name;
};

std::optional<int> interval_token_minutes(const std::string& token) {
    std::smatch m;
    if (!std::regex_match(token, m, std::regex(R"(^([0-9]+)([mhd])$)", std::regex::icase))) return std::nullopt;
    const int n = std::stoi(m[1].str());
    const auto unit = static_cast<char>(std::tolower(static_cast<unsigned char>(m[2].str()[0])));
    return n * (unit == 'm' ? 1 : unit == 'h' ? 60 : 1440);
}

MonitorIdentity monitor_identity(const std::string& name, const std::string& action = {}) {
    static const std::regex runner_re(
        R"(^([a-z0-9][a-z0-9-]*)__monitor__([a-z0-9][a-z0-9-]*)__([0-9]+[mhd])$)",
        std::regex::icase);
    static const std::regex script_re(
        R"(monitor__([a-z0-9][a-z0-9-]*)__([a-z0-9][a-z0-9-]*)__([0-9]+[mhd])\.py)",
        std::regex::icase);
    std::smatch m;
    if (!std::regex_match(name, m, runner_re) && !std::regex_search(action, m, script_re)) return {};
    MonitorIdentity out;
    out.canonical = true;
    out.project = lower(m[1].str());
    out.scope = lower(m[2].str());
    out.interval_min = interval_token_minutes(lower(m[3].str()));
    out.display_name = out.project + " · " + out.scope + " monitor";
    return out;
}

std::optional<std::string> detach_state(const std::string& name, const SystemInfo& system, const RuntimePaths& paths) {
    const fs::path dir = paths.job_root / name;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return std::nullopt;

    const auto exit_text = trim(read_text(dir / "exitcode"));
    if (!exit_text.empty()) return "exit:" + exit_text;

    std::int64_t pid = 0;
    try {
        const auto pid_text = trim(read_text(dir / "pid"));
        if (pid_text.empty()) return std::string("gone");
        pid = std::stoll(pid_text);
    } catch (const std::exception&) {
        return std::string("gone");
    }

    const auto needle = pathish(dir.lexically_normal().string());
    for (const auto& p : system.procs) {
        if (p.pid == pid && pathish(p.cmd).find(needle) != std::string::npos) return std::string("running");
    }
    return std::string("gone");
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

std::string wildcard_regex_text(std::string pattern){
    std::replace(pattern.begin(),pattern.end(),'\\','/');
    std::string out="^";
    for(std::size_t i=0;i<pattern.size();++i){
        const char ch=pattern[i];
        if(ch=='*'){
            if(i+1<pattern.size()&&pattern[i+1]=='*'){out+=".*";++i;}
            else out+="[^/]*";
        }else if(ch=='?')out+="[^/]";
        else{
            if(std::string(".^$|()[]{}+\\").find(ch)!=std::string::npos)out+='\\';
            out+=ch;
        }
    }
    out+="$";return out;
}

std::vector<fs::path> glob_files(const std::string& raw_pattern){
    std::vector<fs::path> out;
    if(raw_pattern.empty())return out;
    std::string pattern=raw_pattern;std::replace(pattern.begin(),pattern.end(),'\\','/');
    const auto wild=pattern.find_first_of("*?");
    if(wild==std::string::npos){
        std::error_code ec;if(fs::is_regular_file(fs::path(raw_pattern),ec))out.push_back(fs::path(raw_pattern));
        return out;
    }
    const auto slash=pattern.rfind('/',wild);
    const std::string base_text=slash==std::string::npos?".":pattern.substr(0,slash);
    const std::string rel_pattern=slash==std::string::npos?pattern:pattern.substr(slash+1);
    const bool recursive=rel_pattern.find("**")!=std::string::npos||rel_pattern.find('/')!=std::string::npos;
    const std::regex re(wildcard_regex_text(rel_pattern),std::regex::icase);
    const fs::path base=base_text.empty()?fs::path("/"):fs::path(base_text);
    std::error_code ec;
    if(!fs::is_directory(base,ec))return out;
    auto consider=[&](const fs::directory_entry& e){
        std::error_code rec;if(!e.is_regular_file(rec))return;
        const auto rel=fs::relative(e.path(),base,rec);
        if(rec)return;
        auto text=rel.generic_string();
        if(std::regex_match(text,re))out.push_back(e.path());
    };
    if(recursive){
        for(fs::recursive_directory_iterator it(base,fs::directory_options::skip_permission_denied,ec),end;it!=end&&!ec;it.increment(ec))consider(*it);
    }else{
        for(fs::directory_iterator it(base,fs::directory_options::skip_permission_denied,ec),end;it!=end&&!ec;it.increment(ec))consider(*it);
    }
    std::sort(out.begin(),out.end(),[](const fs::path& a,const fs::path& b){return a.generic_string()<b.generic_string();});
    return out;
}

array result_list(const object& p, const object& snap) {
    array out;
    std::set<std::string> seen;
    auto add=[&](const std::string& label,const std::string& path){
        if(path.empty()||!seen.insert(path).second)return;
        array pair;pair.emplace_back(label.empty()?fs::path(path).filename().string():label);pair.emplace_back(path);out.emplace_back(std::move(pair));
    };
    auto append = [&](const value* v) {
        if (const auto* a = arr(v)) for (const auto& x : *a) {
            const auto* o = obj(&x); if (!o) continue;
            const auto path = str(o->if_contains("path")); if (path.empty()) continue;
            add(str(o->if_contains("label")),path);
        }
    };
    append(p.if_contains("results"));
    append(snap.if_contains("results"));
    if(const auto* globs=arr(p.if_contains("results_glob")))for(const auto& x:*globs){
        for(const auto& path:glob_files(str(&x)))add(path.filename().string(),path.string());
    }
    return out;
}

std::string tail_text_local(const fs::path& path, std::size_t nbytes = 200000) {
    std::ifstream in(path, std::ios::binary);
    if(!in)return {};
    in.seekg(0,std::ios::end);
    const auto pos=in.tellg();
    if(pos==std::streampos(-1))return {};
    const auto size=static_cast<std::streamoff>(pos);
    const std::streamoff start=size>static_cast<std::streamoff>(nbytes)?size-static_cast<std::streamoff>(nbytes):0;
    in.seekg(start,std::ios::beg);
    std::string data((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    if(start>0){
        const auto nl=data.find('\n');
        if(nl!=std::string::npos)data.erase(0,nl+1);
    }
    if(data.rfind("\xEF\xBB\xBF",0)==0)data.erase(0,3);
    return data;
}

std::string first_line(const std::string& text) {
    auto s=trim(text);
    const auto p=s.find_first_of("\r\n");
    return p==std::string::npos?s:s.substr(0,p);
}

struct ClaudeResult {
    bool found=false;
    bool is_error=false;
    std::string result;
};

ClaudeResult parse_claude_result(const std::string& text) {
    ClaudeResult out;
    std::istringstream in(text);
    std::string line;
    while(std::getline(in,line)){
        line=trim(line);
        if(line.empty()||line.front()!='{')continue;
        boost::system::error_code ec;
        auto v=json::parse(line,ec);
        if(ec||!v.is_object())continue;
        const auto& o=v.as_object();
        if(str(o.if_contains("type"))!="result")continue;
        out.found=true;
        out.is_error=boolean(o.if_contains("is_error"));
        out.result=str(o.if_contains("result"));
    }
    return out;
}

std::string friendly_error(std::string text) {
    if(std::regex_search(text,std::regex(R"(session limit|usage limit|hit your limit)",std::regex::icase))){
        const auto p=text.rfind("·");
        return p==std::string::npos?"Claude 额度用完":"Claude 额度用完（"+trim(text.substr(p+std::string("·").size()))+"）";
    }
    if(std::regex_search(text,std::regex(R"(not logged in|please run /login|invalid api key)",std::regex::icase)))return "Claude 没有登录";
    if(std::regex_search(text,std::regex(R"(api error|internal server error|overloaded)",std::regex::icase)))
        return "Claude 服务出错（"+text.substr(0,std::min<std::size_t>(80,text.size()))+"）";
    return text;
}

bool pid_alive(std::int64_t pid,const SystemInfo& system){
    return std::any_of(system.procs.begin(),system.procs.end(),[&](const ProcessInfo& p){return p.pid==pid;});
}

std::optional<double> parse_stamp_time(const std::string& stamp){
    if(stamp.size()!=13)return std::nullopt;
    std::tm tm{};
    std::istringstream in(stamp);
    in>>std::get_time(&tm,"%Y%m%d_%H%M");
    if(in.fail())return std::nullopt;
    tm.tm_isdst=-1;
    const auto t=std::mktime(&tm);
    return t==-1?std::nullopt:std::optional<double>(static_cast<double>(t));
}

std::string stamp_label(const std::string& stamp){
    if(stamp.size()!=13)return stamp;
    return stamp.substr(4,2)+"-"+stamp.substr(6,2)+" "+stamp.substr(9,2)+":"+stamp.substr(11,2);
}

array detach_takeovers(const std::string& prefix,const SystemInfo& system,const RuntimePaths& paths){
    struct Row{double time=0;object value;};
    std::vector<Row> rows;
    std::error_code ec;
    if(!fs::is_directory(paths.job_root,ec))return {};
    for(fs::directory_iterator it(paths.job_root,ec),end;it!=end&&!ec;it.increment(ec)){
        std::error_code dec;if(!it->is_directory(dec))continue;
        const auto name=it->path().filename().string();
        if(name.rfind(prefix,0)!=0)continue;
        const auto log=it->path()/"output.log";
        const auto st=detach_state(name,system,paths);
        const auto res=parse_claude_result(tail_text_local(log));
        std::string state;
        if(st&&*st=="running")state="running";
        else if(res.found&&!res.is_error&&st&&*st=="exit:0")state="ok";
        else state="failed";
        auto summary=res.found?first_line(res.result):std::string{};
        std::string error;
        if(state=="failed"){
            error=res.found&&res.is_error?friendly_error(trim(res.result)):
                "进程退出（"+(st?*st:"不存在")+"），没有结果";
        }
        std::smatch m;
        const bool has_stamp=std::regex_search(name,m,std::regex(R"(([0-9]{8})-([0-9]{4}))"));
        object o;
        o["key"]=name;
        const auto ts=mtime_seconds(it->path()/"started").value_or(mtime_seconds(it->path()).value_or(0.0));
        o["time"]=ts;
        o["state"]=state;
        o["error"]=error.substr(0,std::min<std::size_t>(160,error.size()));
        o["summary"]=strip_md(error.empty()?summary:error).substr(0,200);
        o["path"]=log.string();
        o["kind"]="jsonl";
        o["label"]=has_stamp?m[1].str().substr(4,2)+"-"+m[1].str().substr(6,2)+" "+m[2].str().substr(0,2)+":"+m[2].str().substr(2,2):name;
        rows.push_back({ts,std::move(o)});
    }
    std::sort(rows.begin(),rows.end(),[](const Row& a,const Row& b){return a.time>b.time;});
    array out;for(auto& r:rows)out.emplace_back(std::move(r.value));return out;
}

array glob_takeovers(const object& cfg,const SystemInfo& system){
    const fs::path pattern=str(cfg.if_contains("pattern"));
    const auto folder=pattern.parent_path();
    struct Files{fs::path jsonl;fs::path md;};
    std::map<std::string,Files> stamps;
    std::error_code ec;
    if(fs::is_directory(folder,ec)){
        for(fs::directory_iterator it(folder,ec),end;it!=end&&!ec;it.increment(ec)){
            if(!it->is_regular_file())continue;
            std::smatch m;
            const auto name=it->path().filename().string();
            if(!std::regex_match(name,m,std::regex(R"(^claude_takeover_([0-9]{8}_[0-9]{4})\.(jsonl|md)$)")))continue;
            auto& files=stamps[m[1].str()];
            if(m[2].str()=="jsonl")files.jsonl=it->path();else files.md=it->path();
        }
    }
    object last;
    if(auto v=read_json(str(cfg.if_contains("last")));v&&v->is_object())last=v->as_object();
    std::optional<std::int64_t> lock_pid;
    const auto lock_path=fs::path(str(cfg.if_contains("lock")));
    if(!lock_path.empty()&&fs::exists(lock_path)){
        try{
            std::istringstream in(read_text(lock_path));std::int64_t p=0;if(in>>p)lock_pid=p;
        }catch(const std::exception&){}
    }
    const auto newest=stamps.empty()?std::string{}:stamps.rbegin()->first;
    struct Row{double time=0;object value;};std::vector<Row> rows;
    for(const auto& [stamp,files]:stamps){
        const auto md=trim(read_text(files.md));
        const auto res=files.jsonl.empty()?ClaudeResult{}:parse_claude_result(tail_text_local(files.jsonl));
        std::string state;
        if(stamp==newest&&lock_pid&&pid_alive(*lock_pid,system))state="running";
        else if(res.found)state=res.is_error?"failed":"ok";
        else if(str(last.if_contains("report"))=="claude_takeover_"+stamp+".md")state=boolean(last.if_contains("ok"))?"ok":"failed";
        else state=md.empty()?"failed":"ok";
        const auto summary=!md.empty()?first_line(md):(res.found?first_line(res.result):std::string{});
        const auto error=state=="failed"&&res.found?friendly_error(trim(res.result)):std::string{};
        const auto ts=parse_stamp_time(stamp).value_or(0.0);
        object o;o["key"]=stamp;o["time"]=ts;o["state"]=state;o["error"]=error;
        o["summary"]=strip_md(error.empty()?summary:error).substr(0,200);
        const auto chosen=!files.jsonl.empty()?files.jsonl:files.md;
        o["path"]=chosen.string();o["kind"]=files.jsonl.empty()?"md":"jsonl";o["label"]=stamp_label(stamp);
        rows.push_back({ts,std::move(o)});
    }
    std::sort(rows.begin(),rows.end(),[](const Row& a,const Row& b){return a.time>b.time;});
    array out;for(auto& r:rows)out.emplace_back(std::move(r.value));return out;
}

struct MarkdownTable {std::vector<std::string> cols;std::vector<std::vector<std::string>> rows;};
struct MarkdownStatus {std::string title;std::string headline;std::vector<MarkdownTable> tables;std::vector<std::string> extras;std::vector<std::string> notes;};

std::vector<std::string> split_table_cells(const std::string& line){
    auto s=trim(line);
    if(!s.empty()&&s.front()=='|')s.erase(s.begin());
    if(!s.empty()&&s.back()=='|')s.pop_back();
    std::vector<std::string> out;std::stringstream ss(s);std::string cell;
    while(std::getline(ss,cell,'|'))out.push_back(trim(cell));
    return out;
}

bool separator_row(const std::vector<std::string>& cells){
    bool had=false;
    const std::regex re(R"(^:?-{2,}:?$)");
    for(const auto& c:cells){
        if(c.empty())continue;
        had=true;
        if(!std::regex_match(c,re))return false;
    }
    return had;
}

MarkdownStatus parse_status_markdown(const std::string& text){
    MarkdownStatus out;MarkdownTable* cur=nullptr;bool in_notes=false;
    std::istringstream in(text);std::string line;
    while(std::getline(in,line)){
        auto s=trim(line);
        if(!s.empty()&&s.front()=='|'){
            auto cells=split_table_cells(s);
            if(!cur){out.tables.push_back({cells,{}});cur=&out.tables.back();}
            else if(!separator_row(cells)){
                cells.resize(cur->cols.size());
                cur->rows.push_back(std::move(cells));
            }
            continue;
        }
        cur=nullptr;
        if(s.empty())continue;
        if(s.rfind("# ",0)==0&&out.title.empty())out.title=trim(s.substr(2));
        else if(s.rfind("**",0)==0&&s.find("备注")!=std::string::npos)in_notes=true;
        else if(in_notes&&s.rfind("- ",0)==0)out.notes.push_back(strip_md(s.substr(2)));
        else if(out.headline.empty())out.headline=strip_md(s);
        else out.extras.push_back(strip_md(s));
    }
    return out;
}

object markdown_adapter(const object& p,const SystemInfo& system,const RunnerInfo& runner,const RuntimePaths& paths){
    const fs::path path=str(p.if_contains("status_md"));
    const auto text=read_text(path);
    const auto ts=mtime_seconds(path);
    const auto md=parse_status_markdown(text);
    object snap;
    snap["updated"]=ts?value(*ts):value(nullptr);
    snap["headline"]=md.headline;
    snap["notes"]=json_strings(md.notes);
    snap["attention"]=array{};
    object table;table["cols"]=array{};table["rows"]=array{};table["tags"]=array{};table["row_meta"]=array{};
    std::vector<std::string> tags;
    if(!md.tables.empty()){
        const auto& tb=md.tables.front();
        array cols;for(const auto& x:tb.cols)cols.emplace_back(x);
        std::optional<std::size_t> si,pi;
        for(std::size_t i=0;i<tb.cols.size();++i){if(tb.cols[i]=="状态")si=i;if(tb.cols[i]=="进度")pi=i;}
        array rows;
        for(const auto& r:tb.rows){
            const auto status=si&&*si<r.size()?r[*si]:std::string{};
            const auto progress=pi&&*pi<r.size()?r[*pi]:std::string{};
            tags.push_back(classify_row(status,progress));
            array row;for(const auto& cell:r)row.emplace_back(strip_md(cell));rows.emplace_back(std::move(row));
        }
        table["cols"]=std::move(cols);table["rows"]=std::move(rows);table["tags"]=json_strings(tags);
        array meta;for(std::size_t i=0;i<tb.rows.size();++i)meta.emplace_back(object{});table["row_meta"]=std::move(meta);
    }
    snap["table"]=std::move(table);
    snap["summary"]=count_summary(tags);
    std::string md_next;
    const std::regex next_re(R"(下次检查[:：]\s*(.+))");
    array extras;
    for(const auto& e:md.extras){
        std::smatch m;if(md_next.empty()&&std::regex_search(e,m,next_re))md_next=trim(m[1].str());
        if(e.rfind("下次检查",0)!=0)extras.emplace_back(e);
    }
    snap["next"]=runner.next?value(short_time(*runner.next)):(md_next.empty()?value(nullptr):value(md_next));
    snap["extras"]=std::move(extras);
    const auto tks=takeovers(p,system,paths);
    const auto att_path=fs::path(str(p.if_contains("attention")));
    const auto att_t=att_path.empty()?std::optional<double>{}:mtime_seconds(att_path);
    const bool fresh=att_t&&ts&&*att_t>=*ts-120.0;
    if(fresh){
        std::vector<std::string> items;std::istringstream ain(read_text(att_path));std::string line;
        while(std::getline(ain,line)){line=trim(line);if(!line.empty())items.push_back(line);}
        const object* last_tk=!tks.empty()&&tks.front().is_object()?&tks.front().as_object():nullptr;
        const double last_time=last_tk&&last_tk->if_contains("time")&&last_tk->at("time").is_double()?last_tk->at("time").as_double():0.0;
        const bool handled=last_tk&&last_time>=*att_t-60.0;
        bool running=false;for(const auto& x:tks)if(x.is_object()&&str(x.as_object().if_contains("state"))=="running")running=true;
        const auto joined=join(items,"；");
        if(running)snap["working"]="后台 Claude 正在处理："+joined;
        else if(handled&&str(last_tk->if_contains("state"))=="ok"){
            array a;a.emplace_back("后台 Claude 已处理过这些问题，结果需要你看一下（见“后台处理记录”）：");
            for(const auto& x:items)a.emplace_back(x);snap["attention"]=std::move(a);
        }else if(handled){
            array a;a.emplace_back("后台 Claude 处理失败，需要你处理：");for(const auto& x:items)a.emplace_back(x);snap["attention"]=std::move(a);
        }else snap["working"]="监控发现问题，等待后台处理："+joined;
    }
    if(text.find("ssh/remote monitor FAILED")!=std::string::npos){
        snap["error"]="上次检查连不上服务器（ssh 失败）";
        snap["headline"]="上次检查时连不上服务器，监控没有取到作业状态。";
    }
    const auto done_text=lower(trim(read_text(str(p.if_contains("done_file")))));
    snap["done"]=done_text.rfind("done",0)==0;
    snap["takeovers"]=tks;
    return snap;
}

std::string qoi_status_label(const std::string& status){
    static const std::vector<std::pair<std::string,std::string>> labels={
        {"COMPLETE","已完成并推送"},
        {"RESULTS_WRITTEN_NOT_PUSHED","结果已写，待推送"},
        {"TAKEOVER_ACTIVE","后台处理中"},
        {"CHECKPOINTS_DONE_NO_RESULTS","计算完成，待汇总"},
        {"RETRY_FAILED_ITEMS","重试失败项"},
        {"DEAD","进程停止"},
        {"RUNNING","运行中"},
    };
    for(const auto& [prefix,label]:labels)if(status.rfind(prefix,0)==0)return label;
    return status;
}

std::string compact_decimal(double value,int precision=1){
    std::ostringstream os;os<<std::fixed<<std::setprecision(precision)<<value;
    auto s=os.str();
    if(auto p=s.find('.');p!=std::string::npos){
        while(!s.empty()&&s.back()=='0')s.pop_back();
        if(!s.empty()&&s.back()=='.')s.pop_back();
    }
    return s;
}

std::string epoch_mmdd_hm(double epoch){
    const auto t=static_cast<std::time_t>(epoch);std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm,&t);
#else
    localtime_r(&t,&tm);
#endif
    std::ostringstream os;os<<std::put_time(&tm,"%m-%d %H:%M");return os.str();
}

std::optional<double> qoi_live_time(const value* v){
    if(!v)return std::nullopt;
    if(v->is_double())return v->as_double();
    if(v->is_int64())return static_cast<double>(v->as_int64());
    if(v->is_uint64())return static_cast<double>(v->as_uint64());
    if(v->is_string())return parse_iso_local_seconds(str(v));
    return std::nullopt;
}

object qoi_adapter(const object& p,const SystemInfo& system,const RunnerInfo& runner,const RuntimePaths& paths){
    const fs::path path=str(p.if_contains("status_json"));
    object st;
    if(auto v=read_json(path);v&&v->is_object())st=v->as_object();
    auto updated=parse_iso_local_seconds(str(st.if_contains("updated")));
    if(!updated)updated=mtime_seconds(path);

    const auto* rows=arr(st.if_contains("rows"));
    const auto* live=obj(st.if_contains("live"));
    array out_rows,tags,row_meta;
    int done_n=0;
    std::vector<std::string> running_summary;

    if(rows)for(const auto& rv:*rows){
        if(!rv.is_object())continue;
        const auto& r=rv.as_object();
        const auto job=str(r.if_contains("job"));
        const auto status=str(r.if_contains("status"));
        const object* lr=nullptr;
        if(live){
            if(const auto* lv=live->if_contains(job);lv&&lv->is_object())lr=&lv->as_object();
        }

        const int checkpoints=lr?integer(lr->if_contains("checkpoints")).value_or(integer(r.if_contains("checkpoints")).value_or(0)):
                                 integer(r.if_contains("checkpoints")).value_or(0);
        const int failures=lr?integer(lr->if_contains("failures")).value_or(integer(r.if_contains("failures")).value_or(0)):
                              integer(r.if_contains("failures")).value_or(0);
        const int total=integer(r.if_contains("total")).value_or(0);
        std::string pct="0";
        if(total){
            if(checkpoints>=total)pct="100";
            else pct=compact_decimal(100.0*checkpoints/total);
        }

        std::optional<double> newest;
        if(lr){
            newest=qoi_live_time(lr->if_contains("newest_checkpoint"));
            if(!newest)newest=qoi_live_time(lr->if_contains("newest_checkpoint_epoch"));
        }
        std::string last="—";
        if(newest)last=ago(*newest);
        else if(const auto min=integer(r.if_contains("last_checkpoint_min_ago")))last=std::to_string(*min)+" 分钟前";

        std::string rate="—";
        if(const auto* v=r.if_contains("rate_per_h")){
            if(v->is_double()&&v->as_double()!=0)rate=compact_decimal(v->as_double(),3);
            else if(v->is_int64()&&v->as_int64()!=0)rate=std::to_string(v->as_int64());
            else if(v->is_uint64()&&v->as_uint64()!=0)rate=std::to_string(v->as_uint64());
            else if(v->is_string()&&!str(v).empty())rate=str(v);
        }

        std::string eta="—";
        if(const auto* v=r.if_contains("eta_h")){
            double n=0;bool has=false;
            if(v->is_double()){n=v->as_double();has=n!=0;}
            else if(v->is_int64()){n=static_cast<double>(v->as_int64());has=n!=0;}
            else if(v->is_uint64()){n=static_cast<double>(v->as_uint64());has=n!=0;}
            if(has){std::ostringstream os;os<<std::fixed<<std::setprecision(1)<<n<<" 小时";eta=os.str();}
        }

        array row;
        row.emplace_back(job);
        row.emplace_back(qoi_status_label(status));
        row.emplace_back(std::to_string(checkpoints)+" / "+std::to_string(total)+"（"+pct+"%）");
        row.emplace_back(std::to_string(failures));
        row.emplace_back(rate);
        row.emplace_back(eta);
        row.emplace_back(last);
        row.emplace_back(boolean(r.if_contains("branch_pushed"))?"是":"否");
        out_rows.emplace_back(std::move(row));

        std::string tag;
        if(status=="COMPLETE"){tag="done";++done_n;}
        else if(status.rfind("DEAD",0)==0)tag="bad";
        else if(status.rfind("RUNNING",0)==0)tag="run";
        tags.emplace_back(tag);

        object meta;
        meta["task_id"]=job;
        if(const auto* params=r.if_contains("params");params&&params->is_object())meta["params"]=params->as_object();
        if(const auto* workdir=r.if_contains("workdir");workdir&&workdir->is_string())meta["open_path"]=str(workdir);
        if(const auto* log=r.if_contains("log");log&&log->is_string())meta["log"]=str(log);
        if(const auto* result=r.if_contains("result");result&&result->is_string())meta["result"]=str(result);
        row_meta.emplace_back(std::move(meta));

        if(status.rfind("RUNNING",0)==0){
            std::string part=job+" "+pct+"%";
            if(eta!="—")part+="，约 "+eta;
            running_summary.push_back(std::move(part));
        }
    }

    const auto total_jobs=rows?static_cast<int>(rows->size()):0;
    object table;
    table["cols"]=array{"作业","状态","进度（实时）","失败","速率（个/小时）","预计剩余","最近检查点","已推送"};
    table["rows"]=std::move(out_rows);
    table["tags"]=std::move(tags);
    table["row_meta"]=std::move(row_meta);

    object snap;
    snap["updated"]=updated?value(*updated):value(nullptr);
    snap["notes"]=array{};
    snap["extras"]=array{};
    snap["table"]=std::move(table);
    std::string summary=std::to_string(done_n)+"/"+std::to_string(total_jobs)+" 个作业已完成";
    if(!running_summary.empty())summary+="；"+join(running_summary,"；")+" 进行中";
    snap["summary"]=summary;
    snap["headline"]=total_jobs&&done_n==total_jobs?
        "所有作业都已完成并推送。":
        "计算在跑，监控"+every(runner.interval_min.value_or(15))+"检查一次，出问题会自动重启或请后台 Claude 处理。";

    array attention;
    if(const auto* notify=arr(st.if_contains("notify")))for(const auto& x:*notify)attention.emplace_back("[需要你处理] "+str(&x));
    snap["attention"]=std::move(attention);

    std::vector<std::string> items;
    if(const auto* pending=arr(st.if_contains("attention")))for(const auto& x:*pending){
        if(x.is_array()){
            const auto& a=x.as_array();
            if(a.size()>=3)items.push_back(str(&a[0])+" "+str(&a[1])+"："+str(&a[2]));
        }else if(x.is_string())items.push_back(str(&x));
    }
    if(const auto* tk=obj(st.if_contains("takeover"))){
        snap["working"]="后台 Claude 正在处理 "+str(tk->if_contains("key"));
    }else if(!items.empty()){
        snap["working"]="监控发现问题，将请后台 Claude 处理："+join(items,"；");
    }else snap["working"]=nullptr;

    if(updated&&runner.running)snap["next"]=epoch_mmdd_hm(*updated+runner.interval_min.value_or(15)*60.0);
    else snap["next"]=nullptr;
    snap["done"]=total_jobs>0&&done_n==total_jobs;
    snap["takeovers"]=takeovers(p,system,paths);
    return snap;
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

object setup_adapter(const SystemInfo& system,const RuntimePaths& paths) {
    const auto dir=paths.hub_data/"requests";
    const auto tks=detach_takeovers("hub-setup-",system,paths);
    std::map<std::string,const object*> takeover_by_key;
    for(const auto& x:tks)if(x.is_object())takeover_by_key[str(x.as_object().if_contains("key"))]=&x.as_object();

    struct RequestRow{std::string stamp;fs::path request;};
    std::vector<RequestRow> requests;
    double updated=0.0;
    std::error_code ec;
    if(fs::is_directory(dir,ec)){
        for(fs::directory_iterator it(dir,fs::directory_options::skip_permission_denied,ec),end;it!=end&&!ec;it.increment(ec)){
            std::error_code rec;if(!it->is_regular_file(rec))continue;
            updated=std::max(updated,mtime_seconds(it->path()).value_or(0.0));
            const auto name=it->path().filename().string();
            const std::string suffix="_request.md";
            if(name.size()>suffix.size()&&name.compare(name.size()-suffix.size(),suffix.size(),suffix)==0)
                requests.push_back({name.substr(0,name.size()-suffix.size()),it->path()});
        }
    }
    std::sort(requests.begin(),requests.end(),[](const RequestRow& a,const RequestRow& b){return a.stamp>b.stamp;});

    array rows,tags,attention,row_meta;
    bool any_running=false;
    for(const auto& req:requests){
        const auto request_text=read_text(req.request);
        std::string project_name="（未写项目名称）";
        std::istringstream rin(request_text);std::string line;
        while(std::getline(rin,line)){
            line=trim(line);
            const std::string prefix="项目名称：";
            if(line.rfind(prefix,0)==0){auto name=trim(line.substr(prefix.size()));if(!name.empty())project_name=name;break;}
        }

        const auto report_path=dir/(req.stamp+"_report.md");
        const auto report=read_text(report_path);
        std::vector<std::string> needs;
        std::istringstream repin(report);
        const std::regex need_re(R"(^\s*NEEDS_USER:\s*(.*)$)",std::regex::icase);
        while(std::getline(repin,line)){
            std::smatch m;if(!std::regex_match(line,m,need_re))continue;
            auto v=trim(m[1].str());
            if(v.empty())continue;
            if(std::regex_match(v,std::regex(R"(^(none|no\b|n/?a|nothing|无|不需要).*$)",std::regex::icase)))continue;
            needs.push_back(v);
        }

        const auto key="hub-setup-"+req.stamp;
        const object* tk=nullptr;
        if(auto it=takeover_by_key.find(key);it!=takeover_by_key.end())tk=it->second;
        const auto state=tk?str(tk->if_contains("state")):std::string("failed");
        if(state=="running")any_running=true;

        std::string label;
        if(state=="running")label="办理中";
        else if(state=="ok")label=needs.empty()?"已办好":"办好了，有事要你定";
        else label="中断："+(tk&&!str(tk->if_contains("error")).empty()?str(tk->if_contains("error")):"后台作业不存在");

        std::string summary;
        std::istringstream sin(report);
        while(std::getline(sin,line)){
            line=trim(line);if(line.empty()||line.rfind("#",0)==0)continue;summary=line;break;
        }
        if(state=="failed"){
            summary=summary.empty()?"没有写出办理报告":"报告已写出："+summary+"；但任务没有正常结束，请核对报告";
        }else if(summary.empty()&&tk)summary=str(tk->if_contains("summary"));

        std::string display=req.stamp;
        if(req.stamp.size()>=13&&req.stamp[8]=='-')display=req.stamp.substr(4,2)+"-"+req.stamp.substr(6,2)+" "+req.stamp.substr(9,2)+":"+req.stamp.substr(11,2);

        array row;row.emplace_back(display);row.emplace_back(project_name);row.emplace_back(label);row.emplace_back(strip_md(summary).substr(0,120));rows.emplace_back(std::move(row));
        tags.emplace_back(state=="running"?"run":state=="ok"&&needs.empty()?"done":"bad");
        for(const auto& need:needs)attention.emplace_back(project_name+"："+need);
        if(state=="failed")attention.emplace_back(project_name+"：后台办理中断（"+(tk&&!str(tk->if_contains("error")).empty()?str(tk->if_contains("error")):"后台作业不存在")+"），见“后台处理记录”；额度恢复后可以重新提交");

        object meta;meta["task_id"]=req.stamp;meta["open_path"]=(fs::exists(report_path)?report_path:req.request).string();
        meta["result"]=fs::exists(report_path)?report_path.string():std::string{};
        if(tk)meta["log"]=str(tk->if_contains("path"));
        row_meta.emplace_back(std::move(meta));
    }

    object table;table["cols"]=array{"提交时间","项目","状态","说明"};table["rows"]=std::move(rows);table["tags"]=std::move(tags);table["row_meta"]=std::move(row_meta);
    object s;
    s["updated"]=updated>0?value(updated):value(nullptr);
    s["headline"]=requests.empty()?"还没有提交过新任务。点左下角“新建监控任务”，按格式填写后交给后台 Claude 办理。":"交给后台 Claude 设置的监控任务。办好后，新项目会出现在左侧“项目”里。";
    s["summary"]=requests.empty()?"":std::to_string(requests.size())+" 个请求";
    s["notes"]=array{};s["extras"]=array{};s["table"]=std::move(table);s["attention"]=std::move(attention);
    s["working"]=any_running?value("后台 Claude 正在设置新监控"):value(nullptr);
    s["done"]=false;s["takeovers"]=tks;s["next"]=nullptr;
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
    if(const auto* procs=arr(root.if_contains("procs"))) for(const auto& x:*procs){ if(!x.is_object())continue; const auto& o=x.as_object(); ProcessInfo p; p.pid=integer(o.if_contains("pid")).value_or(0); p.name=str(o.if_contains("name")); p.cmd=str(o.if_contains("cmd")); if(p.pid>0)out.procs.push_back(std::move(p)); }
    out.error=str(root.if_contains("error")); return out;
}

json::object system_info_json(const SystemInfo& system) {
    object tasks;
    for (const auto& [name, t] : system.tasks) {
        object row;
        row["name"] = t.name;
        row["state"] = t.state;
        row["last"] = t.last;
        row["result"] = t.result;
        row["next"] = t.next;
        row["interval"] = t.interval;
        row["action"] = t.action;
        tasks[name] = std::move(row);
    }
    array procs;
    for (const auto& p : system.procs) {
        object row;
        row["pid"] = p.pid;
        row["name"] = p.name;
        row["cmd"] = p.cmd;
        procs.emplace_back(std::move(row));
    }
    object out;
    out["tasks"] = std::move(tasks);
    out["procs"] = std::move(procs);
    out["error"] = system.error;
    return out;
}

RunnerInfo runner_info(const object& p, const SystemInfo& system, const RuntimePaths& paths) {
    RunnerInfo x; const auto* r=obj(p.if_contains("runner")); if(!r)return x; x.kind=str(r->if_contains("kind")); x.name=str(r->if_contains("name")); x.interval_min=integer(r->if_contains("interval_min"));
    if(x.kind=="none"){x.exists=true;x.text=str(p.if_contains("runner_text"));return x;}
    if(x.kind=="schtask"){
        const auto it=system.tasks.find(x.name); if(it==system.tasks.end()){x.error="找不到定时任务 "+x.name;x.text="Windows 定时任务 "+x.name+"（不存在）";return x;}
        const auto& t=it->second; x.exists=true; if(auto m=iso_minutes(t.interval))x.interval_min=m; x.paused=t.state=="Disabled"; x.running=t.state=="Running"; if(!t.last.empty())x.last=t.last;if(!x.paused&&!t.next.empty())x.next=t.next;
        std::string rs=t.result==0?"成功":t.result==267009?"正在运行":t.result==267011?"还没运行过":"出错"; if(rs=="出错")x.error="定时任务上次运行出错";
        x.text="Windows 定时任务 "+x.name+"，"+(every(x.interval_min).empty()?"按计划":every(x.interval_min))+" · 上次运行 "+short_time(t.last)+"（"+rs+"）· "+(x.paused?"已停用":"下次 "+short_time(t.next)); return x;
    }
    if(x.kind=="detach"){
        const auto st=detach_state(x.name,system,paths);
        x.exists=st.has_value();
        x.running=st&&*st=="running";
        x.paused=st&&(*st=="exit:stopped"||*st=="exit:0");
        if(!st)x.error="找不到后台作业 "+x.name;
        else if(*st!="running"&&*st!="exit:stopped"&&*st!="exit:0")x.error="监控进程意外退出（"+*st+"）";
        std::string state_text;
        if(!st)state_text="不存在";
        else if(*st=="running")state_text="运行中";
        else state_text="没在运行（"+*st+"）";
        x.text="后台作业 "+x.name+"，"+every(x.interval_min)+" · "+state_text;
        return x;
    }
    x.error="C++ 尚未接入 runner.kind="+x.kind; return x;
}

json::array takeovers(const object& p, const SystemInfo& system, const RuntimePaths& paths) {
    const auto* cfg=obj(p.if_contains("takeovers"));
    if(!cfg)return {};
    const auto kind=str(cfg->if_contains("kind"));
    if(kind=="detach")return detach_takeovers(str(cfg->if_contains("prefix")),system,paths);
    if(kind=="glob")return glob_takeovers(*cfg,system);
    return {};
}

json::object snapshot(const object& p, const SystemInfo& system, const RuntimePaths& paths) {
    const auto runner=runner_info(p,system,paths); const auto adapter=str(p.if_contains("adapter"),"runner_only"); object s;
    if(adapter=="generic")s=generic_adapter(p,runner); else if(adapter=="markdown")s=markdown_adapter(p,system,runner,paths); else if(adapter=="qoi")s=qoi_adapter(p,system,runner,paths); else if(adapter=="setup")s=setup_adapter(system,paths); else if(adapter=="runner_only")s=runner_only_adapter(runner); else {s=runner_only_adapter(runner);s["error"]="C++ 尚未迁移 adapter："+adapter;}
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

    for(const auto& [name,t]:system.tasks){
        bool known=false;for(const auto& p:out)if(const auto* pr=obj(p.if_contains("runner"));pr&&str(pr->if_contains("kind"))=="schtask"&&str(pr->if_contains("name"))==name)known=true;
        if(known)continue;
        const auto id=monitor_identity(name,t.action);
        object p;p["id"]="task:"+name;p["name"]=id.canonical?id.display_name:name;p["area"]="其他监控 · 定时任务"+(id.scope.empty()?"":" · "+id.scope);p["adapter"]="runner_only";
        object rr;rr["kind"]="schtask";rr["name"]=name;if(id.interval_min)rr["interval_min"]=*id.interval_min;p["runner"]=rr;p["action"]=t.action;p["unregistered"]=true;out.push_back(std::move(p));
    }

    std::set<std::string> known_jobs;
    for(const auto& p:out)if(const auto* pr=obj(p.if_contains("runner"));pr&&str(pr->if_contains("kind"))=="detach")known_jobs.insert(str(pr->if_contains("name")));
    std::error_code ec;
    if(fs::is_directory(paths.job_root,ec)){
        for(fs::directory_iterator it(paths.job_root,ec),end;it!=end&&!ec;it.increment(ec)){
            std::error_code dec;if(!it->is_directory(dec))continue;
            const auto name=it->path().filename().string();
            if(lower(name).find("monitor")==std::string::npos||known_jobs.count(name))continue;
            auto action=read_text(it->path()/"run.ps1");if(action.size()>1200)action=action.substr(action.size()-1200);
            const auto id=monitor_identity(name,action);
            object p;p["id"]="job:"+name;p["name"]=id.canonical?id.display_name:name;p["area"]="其他监控 · 后台作业"+(id.scope.empty()?"":" · "+id.scope);p["adapter"]="runner_only";
            object rr;rr["kind"]="detach";rr["name"]=name;if(id.interval_min)rr["interval_min"]=*id.interval_min;p["runner"]=rr;p["action"]=action;p["unregistered"]=true;out.push_back(std::move(p));
        }
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
