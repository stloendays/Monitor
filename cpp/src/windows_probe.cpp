#include "monitor_hub/core.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>

#ifdef _WIN32
#define _WIN32_DCOM
#include <windows.h>
#include <oleauto.h>
#include <taskschd.h>
#include <wbemidl.h>
#include <wrl/client.h>

namespace monitor_hub {
namespace {

using Microsoft::WRL::ComPtr;

std::string utf8(const wchar_t* text) {
    if (!text) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr)) return {};
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::string take_bstr(BSTR text) {
    std::string out = utf8(text);
    if (text) SysFreeString(text);
    return out;
}

std::string hr_text(HRESULT hr) {
    wchar_t* buf = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD n = FormatMessageW(flags, nullptr, static_cast<DWORD>(hr), 0,
                                   reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    if (n && buf) {
        std::wstring msg(buf, n);
        LocalFree(buf);
        while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ')) msg.pop_back();
        os << " " << utf8(msg.c_str());
    }
    return os.str();
}

void append_error(SystemInfo& out, const std::string& text) {
    if (!out.error.empty()) out.error += " ";
    out.error += text;
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string task_state_text(TASK_STATE state) {
    switch (state) {
        case TASK_STATE_DISABLED: return "Disabled";
        case TASK_STATE_QUEUED: return "Queued";
        case TASK_STATE_READY: return "Ready";
        case TASK_STATE_RUNNING: return "Running";
        default: return "Unknown";
    }
}

std::string date_iso(DATE value) {
    if (value <= 0) return {};
    SYSTEMTIME st{};
    if (!VariantTimeToSystemTime(value, &st) || st.wYear < 2000) return {};
    std::ostringstream os;
    os << std::setfill('0') << std::setw(4) << st.wYear << "-"
       << std::setw(2) << st.wMonth << "-" << std::setw(2) << st.wDay << "T"
       << std::setw(2) << st.wHour << ":" << std::setw(2) << st.wMinute << ":"
       << std::setw(2) << st.wSecond;
    return os.str();
}

VARIANT index_variant(LONG i) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = i;
    return v;
}

std::string task_action(ITaskDefinition* def) {
    if (!def) return {};
    ComPtr<IActionCollection> actions;
    if (FAILED(def->get_Actions(actions.GetAddressOf())) || !actions) return {};
    LONG count = 0;
    actions->get_Count(&count);
    std::string out;
    for (LONG i = 1; i <= count; ++i) {
        ComPtr<IAction> action;
        if (FAILED(actions->get_Item(i, action.GetAddressOf())) || !action) continue;
        TASK_ACTION_TYPE type = TASK_ACTION_EXEC;
        if (FAILED(action->get_Type(&type)) || type != TASK_ACTION_EXEC) continue;
        ComPtr<IExecAction> exec;
        if (FAILED(action.As(&exec)) || !exec) continue;
        BSTR path = nullptr, args = nullptr;
        exec->get_Path(&path);
        exec->get_Arguments(&args);
        auto p = take_bstr(path);
        auto a = take_bstr(args);
        std::string one = p + (a.empty() ? "" : " " + a);
        if (!one.empty()) {
            if (!out.empty()) out += " | ";
            out += one;
        }
    }
    return out;
}

std::string task_interval(ITaskDefinition* def) {
    if (!def) return {};
    ComPtr<ITriggerCollection> triggers;
    if (FAILED(def->get_Triggers(triggers.GetAddressOf())) || !triggers) return {};
    LONG count = 0;
    triggers->get_Count(&count);
    for (LONG i = 1; i <= count; ++i) {
        ComPtr<ITrigger> trigger;
        if (FAILED(triggers->get_Item(i, trigger.GetAddressOf())) || !trigger) continue;
        ComPtr<IRepetitionPattern> rep;
        if (FAILED(trigger->get_Repetition(rep.GetAddressOf())) || !rep) continue;
        BSTR interval = nullptr;
        if (SUCCEEDED(rep->get_Interval(&interval))) {
            auto s = take_bstr(interval);
            if (!s.empty()) return s;
        }
    }
    return {};
}

void read_task(IRegisteredTask* task, SystemInfo& out) {
    if (!task) return;
    BSTR name_b = nullptr;
    if (FAILED(task->get_Name(&name_b))) return;
    const auto name = take_bstr(name_b);

    ComPtr<ITaskDefinition> def;
    task->get_Definition(def.GetAddressOf());
    const auto action = task_action(def.Get());
    if (lower_ascii(name + " " + action).find("monitor") == std::string::npos) return;

    TaskInfo info;
    info.name = name;
    info.action = action;
    info.interval = task_interval(def.Get());

    TASK_STATE state = TASK_STATE_UNKNOWN;
    if (SUCCEEDED(task->get_State(&state))) info.state = task_state_text(state);

    DATE last = 0, next = 0;
    if (SUCCEEDED(task->get_LastRunTime(&last))) info.last = date_iso(last);
    LONG result = 0;
    if (SUCCEEDED(task->get_LastTaskResult(&result))) info.result = static_cast<std::uint32_t>(result);

    VARIANT_BOOL enabled = VARIANT_TRUE;
    task->get_Enabled(&enabled);
    if (enabled == VARIANT_TRUE && SUCCEEDED(task->get_NextRunTime(&next))) info.next = date_iso(next);

    out.tasks[info.name] = std::move(info);
}

void walk_folder(ITaskFolder* folder, SystemInfo& out) {
    if (!folder) return;
    ComPtr<IRegisteredTaskCollection> tasks;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, tasks.GetAddressOf())) && tasks) {
        LONG count = 0;
        tasks->get_Count(&count);
        for (LONG i = 1; i <= count; ++i) {
            VARIANT idx = index_variant(i);
            ComPtr<IRegisteredTask> task;
            if (SUCCEEDED(tasks->get_Item(idx, task.GetAddressOf()))) read_task(task.Get(), out);
            VariantClear(&idx);
        }
    }

    ComPtr<ITaskFolderCollection> folders;
    if (FAILED(folder->GetFolders(0, folders.GetAddressOf())) || !folders) return;
    LONG count = 0;
    folders->get_Count(&count);
    for (LONG i = 1; i <= count; ++i) {
        VARIANT idx = index_variant(i);
        ComPtr<ITaskFolder> child;
        const HRESULT hr = folders->get_Item(idx, child.GetAddressOf());
        VariantClear(&idx);
        if (FAILED(hr) || !child) continue;
        BSTR path_b = nullptr;
        child->get_Path(&path_b);
        const auto path = lower_ascii(take_bstr(path_b));
        if (path.rfind("\\microsoft", 0) == 0) continue;
        walk_folder(child.Get(), out);
    }
}

void probe_tasks(SystemInfo& out) {
    ComPtr<ITaskService> service;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(service.GetAddressOf()));
    if (FAILED(hr)) {
        append_error(out, "读取定时任务失败：" + hr_text(hr));
        return;
    }
    VARIANT empty;
    VariantInit(&empty);
    hr = service->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) {
        append_error(out, "读取定时任务失败：" + hr_text(hr));
        return;
    }
    BSTR root_path = SysAllocString(L"\\");
    ComPtr<ITaskFolder> root;
    hr = service->GetFolder(root_path, root.GetAddressOf());
    SysFreeString(root_path);
    if (FAILED(hr) || !root) {
        append_error(out, "读取定时任务失败：" + hr_text(hr));
        return;
    }
    walk_folder(root.Get(), out);
}

std::int64_t variant_i64(const VARIANT& v) {
    if (v.vt == VT_I1) return v.cVal;
    if (v.vt == VT_UI1) return v.bVal;
    if (v.vt == VT_I2) return v.iVal;
    if (v.vt == VT_UI2) return v.uiVal;
    if (v.vt == VT_I4 || v.vt == VT_INT) return v.lVal;
    if (v.vt == VT_UI4 || v.vt == VT_UINT) return v.ulVal;
    if (v.vt == VT_I8) return v.llVal;
    if (v.vt == VT_UI8) return static_cast<std::int64_t>(v.ullVal);
    return 0;
}

std::string variant_string(const VARIANT& v) {
    return v.vt == VT_BSTR && v.bstrVal ? utf8(v.bstrVal) : std::string{};
}

void probe_processes(SystemInfo& out) {
    ComPtr<IWbemLocator> locator;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(locator.GetAddressOf()));
    if (FAILED(hr)) {
        append_error(out, "读取进程失败：" + hr_text(hr));
        return;
    }

    BSTR ns = SysAllocString(L"ROOT\\CIMV2");
    ComPtr<IWbemServices> services;
    hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, services.GetAddressOf());
    SysFreeString(ns);
    if (FAILED(hr) || !services) {
        append_error(out, "读取进程失败：" + hr_text(hr));
        return;
    }

    hr = CoSetProxyBlanket(services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                           RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) {
        append_error(out, "读取进程失败：" + hr_text(hr));
        return;
    }

    BSTR lang = SysAllocString(L"WQL");
    BSTR query = SysAllocString(
        L"SELECT ProcessId, Name, CommandLine FROM Win32_Process WHERE "
        L"Name='pwsh.exe' OR Name='powershell.exe' OR Name='python.exe' OR Name='claude.exe'");
    ComPtr<IEnumWbemClassObject> rows;
    hr = services->ExecQuery(lang, query,
                             WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                             nullptr, rows.GetAddressOf());
    SysFreeString(lang);
    SysFreeString(query);
    if (FAILED(hr) || !rows) {
        append_error(out, "读取进程失败：" + hr_text(hr));
        return;
    }

    for (;;) {
        ComPtr<IWbemClassObject> row;
        ULONG got = 0;
        hr = rows->Next(WBEM_INFINITE, 1, row.GetAddressOf(), &got);
        if (FAILED(hr) || got == 0 || !row) break;

        VARIANT pid_v, name_v, cmd_v;
        VariantInit(&pid_v); VariantInit(&name_v); VariantInit(&cmd_v);
        row->Get(L"ProcessId", 0, &pid_v, nullptr, nullptr);
        row->Get(L"Name", 0, &name_v, nullptr, nullptr);
        row->Get(L"CommandLine", 0, &cmd_v, nullptr, nullptr);

        ProcessInfo p;
        p.pid = variant_i64(pid_v);
        p.name = variant_string(name_v);
        p.cmd = variant_string(cmd_v);
        if (p.pid > 0) out.procs.push_back(std::move(p));

        VariantClear(&pid_v); VariantClear(&name_v); VariantClear(&cmd_v);
    }
}

} // namespace

SystemInfo probe_system_info() {
    SystemInfo out;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        out.error = "COM 初始化失败：" + hr_text(init);
        return out;
    }

    const HRESULT sec = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                                             RPC_C_AUTHN_LEVEL_DEFAULT,
                                             RPC_C_IMP_LEVEL_IMPERSONATE,
                                             nullptr, EOAC_NONE, nullptr);
    if (FAILED(sec) && sec != RPC_E_TOO_LATE)
        append_error(out, "COM 安全初始化失败：" + hr_text(sec));

    probe_tasks(out);
    probe_processes(out);
    if (uninit) CoUninitialize();
    return out;
}

} // namespace monitor_hub

#else

namespace monitor_hub {

SystemInfo probe_system_info() {
    SystemInfo out;
    out.error = "live system probe is only available on Windows";
    return out;
}

} // namespace monitor_hub

#endif
