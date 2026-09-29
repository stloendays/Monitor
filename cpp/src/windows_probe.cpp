#include "monitor_hub/windows_probe.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <taskschd.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace monitor_hub {
namespace {

using Microsoft::WRL::ComPtr;

std::string utf8(const wchar_t* text, int length = -1) {
    if (!text) return {};
    if (length < 0) length = static_cast<int>(wcslen(text));
    if (length == 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), n, nullptr, nullptr);
    return out;
}

std::string utf8_bstr(BSTR value) {
    return value ? utf8(value, static_cast<int>(SysStringLen(value))) : std::string{};
}

std::string hr_text(HRESULT hr) {
    std::ostringstream os;
    os << "HRESULT 0x" << std::uppercase << std::hex << static_cast<unsigned long>(hr);
    return os.str();
}

std::string iso_date(DATE date) {
    if (date <= 0.0) return {};
    SYSTEMTIME st{};
    if (!VariantTimeToSystemTime(date, &st) || st.wYear < 2000) return {};
    std::ostringstream os;
    os << std::setfill('0') << std::setw(4) << st.wYear << "-"
       << std::setw(2) << st.wMonth << "-" << std::setw(2) << st.wDay << "T"
       << std::setw(2) << st.wHour << ":" << std::setw(2) << st.wMinute << ":"
       << std::setw(2) << st.wSecond;
    return os.str();
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string task_state(TASK_STATE state) {
    switch (state) {
        case TASK_STATE_DISABLED: return "Disabled";
        case TASK_STATE_QUEUED: return "Queued";
        case TASK_STATE_READY: return "Ready";
        case TASK_STATE_RUNNING: return "Running";
        default: return "Unknown";
    }
}

std::string exec_actions(ITaskDefinition* definition) {
    if (!definition) return {};
    ComPtr<IActionCollection> actions;
    if (FAILED(definition->get_Actions(actions.GetAddressOf())) || !actions) return {};
    LONG count = 0;
    if (FAILED(actions->get_Count(&count))) return {};
    std::string out;
    for (LONG i = 1; i <= count; ++i) {
        ComPtr<IAction> action;
        if (FAILED(actions->get_Item(i, action.GetAddressOf())) || !action) continue;
        TASK_ACTION_TYPE type{};
        if (FAILED(action->get_Type(&type)) || type != TASK_ACTION_EXEC) continue;
        ComPtr<IExecAction> exec;
        if (FAILED(action.As(&exec)) || !exec) continue;
        BSTR path = nullptr;
        BSTR args = nullptr;
        exec->get_Path(&path);
        exec->get_Arguments(&args);
        std::string one = utf8_bstr(path);
        const auto arguments = utf8_bstr(args);
        if (!arguments.empty()) {
            if (!one.empty()) one += " ";
            one += arguments;
        }
        if (path) SysFreeString(path);
        if (args) SysFreeString(args);
        if (!one.empty()) {
            if (!out.empty()) out += " | ";
            out += one;
        }
    }
    return out;
}

std::string repetition_interval(ITaskDefinition* definition) {
    if (!definition) return {};
    ComPtr<ITriggerCollection> triggers;
    if (FAILED(definition->get_Triggers(triggers.GetAddressOf())) || !triggers) return {};
    LONG count = 0;
    if (FAILED(triggers->get_Count(&count))) return {};
    for (LONG i = 1; i <= count; ++i) {
        ComPtr<ITrigger> trigger;
        if (FAILED(triggers->get_Item(i, trigger.GetAddressOf())) || !trigger) continue;
        ComPtr<IRepetitionPattern> repetition;
        if (FAILED(trigger->get_Repetition(repetition.GetAddressOf())) || !repetition) continue;
        BSTR interval = nullptr;
        if (SUCCEEDED(repetition->get_Interval(&interval)) && interval) {
            const auto value = utf8_bstr(interval);
            SysFreeString(interval);
            if (!value.empty()) return value;
        }
    }
    return {};
}

void read_task_folder(ITaskFolder* first, SystemInfo& out) {
    if (!first) return;
    std::vector<ComPtr<ITaskFolder>> stack;
    ComPtr<ITaskFolder> root = first;
    stack.push_back(root);

    while (!stack.empty()) {
        auto folder = stack.back();
        stack.pop_back();

        ComPtr<IRegisteredTaskCollection> tasks;
        if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, tasks.GetAddressOf())) && tasks) {
            LONG count = 0;
            if (SUCCEEDED(tasks->get_Count(&count))) {
                for (LONG i = 1; i <= count; ++i) {
                    VARIANT index{};
                    VariantInit(&index);
                    index.vt = VT_I4;
                    index.lVal = i;
                    ComPtr<IRegisteredTask> task;
                    const auto item_hr = tasks->get_Item(index, task.GetAddressOf());
                    VariantClear(&index);
                    if (FAILED(item_hr) || !task) continue;

                    BSTR name_bstr = nullptr;
                    if (FAILED(task->get_Name(&name_bstr)) || !name_bstr) continue;
                    const auto name = utf8_bstr(name_bstr);
                    SysFreeString(name_bstr);

                    ComPtr<ITaskDefinition> definition;
                    task->get_Definition(definition.GetAddressOf());
                    const auto action = exec_actions(definition.Get());
                    const auto searchable = lower_ascii(name + " " + action);
                    if (searchable.find("monitor") == std::string::npos) continue;

                    TaskInfo info;
                    info.name = name;
                    info.action = action;
                    info.interval = repetition_interval(definition.Get());

                    TASK_STATE state{};
                    if (SUCCEEDED(task->get_State(&state))) info.state = task_state(state);

                    DATE last = 0.0;
                    if (SUCCEEDED(task->get_LastRunTime(&last))) info.last = iso_date(last);

                    LONG result = 0;
                    if (SUCCEEDED(task->get_LastTaskResult(&result)))
                        info.result = static_cast<std::uint32_t>(result);

                    VARIANT_BOOL enabled = VARIANT_FALSE;
                    task->get_Enabled(&enabled);
                    if (enabled == VARIANT_TRUE) {
                        DATE next = 0.0;
                        if (SUCCEEDED(task->get_NextRunTime(&next))) info.next = iso_date(next);
                    }

                    out.tasks[info.name] = std::move(info);
                }
            }
        }

        ComPtr<ITaskFolderCollection> folders;
        if (FAILED(folder->GetFolders(0, folders.GetAddressOf())) || !folders) continue;
        LONG folder_count = 0;
        if (FAILED(folders->get_Count(&folder_count))) continue;
        for (LONG i = 1; i <= folder_count; ++i) {
            VARIANT index{};
            VariantInit(&index);
            index.vt = VT_I4;
            index.lVal = i;
            ComPtr<ITaskFolder> sub;
            const auto item_hr = folders->get_Item(index, sub.GetAddressOf());
            VariantClear(&index);
            if (FAILED(item_hr) || !sub) continue;

            BSTR path = nullptr;
            if (SUCCEEDED(sub->get_Path(&path)) && path) {
                const auto p = lower_ascii(utf8_bstr(path));
                SysFreeString(path);
                if (p.rfind("\\microsoft", 0) == 0) continue;
            }
            stack.push_back(sub);
        }
    }
}

std::string variant_string(const VARIANT& value) {
    if (value.vt == VT_BSTR && value.bstrVal) return utf8_bstr(value.bstrVal);
    return {};
}

std::int64_t variant_integer(const VARIANT& value) {
    switch (value.vt) {
        case VT_I1: return value.cVal;
        case VT_UI1: return value.bVal;
        case VT_I2: return value.iVal;
        case VT_UI2: return value.uiVal;
        case VT_I4:
        case VT_INT: return value.lVal;
        case VT_UI4:
        case VT_UINT: return value.ulVal;
        case VT_I8: return value.llVal;
        case VT_UI8: return static_cast<std::int64_t>(value.ullVal);
        default: return 0;
    }
}

HRESULT initialize_security() {
    const HRESULT hr = CoInitializeSecurity(
        nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    return hr == RPC_E_TOO_LATE ? S_OK : hr;
}

void read_processes(SystemInfo& out) {
    ComPtr<IWbemLocator> locator;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator, reinterpret_cast<void**>(locator.GetAddressOf()));
    if (FAILED(hr) || !locator) {
        out.error += (out.error.empty() ? "" : " ") + std::string("读取进程失败：") + hr_text(hr);
        return;
    }

    ComPtr<IWbemServices> services;
    BSTR ns = SysAllocString(L"ROOT\\CIMV2");
    hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, services.GetAddressOf());
    SysFreeString(ns);
    if (FAILED(hr) || !services) {
        out.error += (out.error.empty() ? "" : " ") + std::string("读取进程失败：") + hr_text(hr);
        return;
    }

    hr = CoSetProxyBlanket(services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                           RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) {
        out.error += (out.error.empty() ? "" : " ") + std::string("读取进程失败：") + hr_text(hr);
        return;
    }

    BSTR lang = SysAllocString(L"WQL");
    BSTR query = SysAllocString(
        L"SELECT ProcessId, Name, CommandLine FROM Win32_Process WHERE "
        L"Name='pwsh.exe' OR Name='powershell.exe' OR Name='python.exe' OR Name='pythonw.exe' OR Name='claude.exe'");
    ComPtr<IEnumWbemClassObject> enumerator;
    hr = services->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                             nullptr, enumerator.GetAddressOf());
    SysFreeString(lang);
    SysFreeString(query);
    if (FAILED(hr) || !enumerator) {
        out.error += (out.error.empty() ? "" : " ") + std::string("读取进程失败：") + hr_text(hr);
        return;
    }

    while (true) {
        ULONG returned = 0;
        ComPtr<IWbemClassObject> item;
        hr = enumerator->Next(WBEM_INFINITE, 1, item.GetAddressOf(), &returned);
        if (FAILED(hr) || returned == 0 || !item) break;

        VARIANT pid{}, name{}, command{};
        VariantInit(&pid);
        VariantInit(&name);
        VariantInit(&command);
        item->Get(L"ProcessId", 0, &pid, nullptr, nullptr);
        item->Get(L"Name", 0, &name, nullptr, nullptr);
        item->Get(L"CommandLine", 0, &command, nullptr, nullptr);

        ProcessInfo process;
        process.pid = variant_integer(pid);
        process.name = variant_string(name);
        process.cmd = variant_string(command);
        out.procs.push_back(std::move(process));

        VariantClear(&pid);
        VariantClear(&name);
        VariantClear(&command);
    }
}

}  // namespace

SystemInfo probe_system_info() {
    SystemInfo out;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        out.error = "COM 初始化失败：" + hr_text(init);
        return out;
    }

    const HRESULT security = initialize_security();
    if (FAILED(security)) out.error = "COM 安全初始化失败：" + hr_text(security);

    ComPtr<ITaskService> service;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(service.GetAddressOf()));
    if (SUCCEEDED(hr) && service) {
        VARIANT empty{};
        VariantInit(&empty);
        hr = service->Connect(empty, empty, empty, empty);
        if (SUCCEEDED(hr)) {
            BSTR root_path = SysAllocString(L"\\");
            ComPtr<ITaskFolder> root;
            hr = service->GetFolder(root_path, root.GetAddressOf());
            SysFreeString(root_path);
            if (SUCCEEDED(hr) && root) read_task_folder(root.Get(), out);
        }
    }
    if (FAILED(hr)) {
        out.error += (out.error.empty() ? "" : " ") + std::string("读取定时任务失败：") + hr_text(hr);
    }

    read_processes(out);

    if (uninitialize) CoUninitialize();
    return out;
}

}  // namespace monitor_hub

#else

namespace monitor_hub {

SystemInfo probe_system_info() {
    SystemInfo out;
    out.error = "实时系统探测只支持 Windows";
    return out;
}

}  // namespace monitor_hub

#endif
