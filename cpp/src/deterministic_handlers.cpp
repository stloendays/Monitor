#include "monitor_hub/deterministic_handlers.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace monitor_hub {
namespace {

struct ProcessResult {
    bool launched = false;
    int exit_code = -1;
    std::uint64_t process_id = 0;
    std::string error;
};

std::string str(const json::value* value, std::string fallback = {}) {
    if (!value) return fallback;
    if (value->is_string()) return std::string(value->as_string());
    if (value->is_int64()) return std::to_string(value->as_int64());
    if (value->is_uint64()) return std::to_string(value->as_uint64());
    return fallback;
}

const json::array* array(const json::value* value) {
    return value && value->is_array() ? &value->as_array() : nullptr;
}

std::vector<std::string> string_array(const json::value* value) {
    std::vector<std::string> result;
    const auto* values = array(value);
    if (!values) return result;
    for (const auto& item : *values) {
        if (!item.is_string()) continue;
        result.emplace_back(item.as_string());
    }
    return result;
}

json::array json_strings(const std::vector<std::string>& values) {
    json::array result;
    for (const auto& item : values) result.emplace_back(item);
    return result;
}

class CurrentDirectoryGuard {
public:
    explicit CurrentDirectoryGuard(const fs::path& directory) {
        if (directory.empty()) return;
        std::error_code ec;
        previous_ = fs::current_path(ec);
        if (ec) {
            error_ = "cannot read current working directory: " + ec.message();
            return;
        }
        fs::current_path(directory, ec);
        if (ec) {
            error_ =
                "cannot enter working directory " + directory.string() +
                ": " + ec.message();
            return;
        }
        changed_ = true;
    }

    ~CurrentDirectoryGuard() {
        if (!changed_) return;
        std::error_code ec;
        fs::current_path(previous_, ec);
    }

    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

private:
    fs::path previous_;
    std::string error_;
    bool changed_ = false;
};

#ifdef _WIN32

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (count <= 0) return {};

    std::wstring result(static_cast<std::size_t>(count), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        count);
    if (written != count) return {};
    return result;
}

ProcessResult spawn_process(
    const fs::path& program,
    const std::vector<std::string>& arguments,
    const fs::path& working_directory,
    bool wait_for_exit,
    bool search_path) {

    ProcessResult result;
    CurrentDirectoryGuard cwd(working_directory);
    if (!cwd.ok()) {
        result.error = cwd.error();
        return result;
    }

    std::vector<std::wstring> owned;
    owned.reserve(arguments.size() + 1);
    owned.push_back(program.wstring());
    for (const auto& argument : arguments) {
        auto wide = utf8_to_wide(argument);
        if (wide.empty() && !argument.empty()) {
            result.error = "argument is not valid UTF-8";
            return result;
        }
        owned.push_back(std::move(wide));
    }

    std::vector<const wchar_t*> argv;
    argv.reserve(owned.size() + 1);
    for (const auto& item : owned) argv.push_back(item.c_str());
    argv.push_back(nullptr);

    errno = 0;
    const intptr_t raw =
        search_path
        ? _wspawnvp(
              wait_for_exit ? _P_WAIT : _P_NOWAIT,
              program.wstring().c_str(),
              argv.data())
        : _wspawnv(
              wait_for_exit ? _P_WAIT : _P_NOWAIT,
              program.wstring().c_str(),
              argv.data());

    if (raw == -1) {
        result.error =
            "process launch failed: " +
            std::string(std::strerror(errno));
        return result;
    }

    result.launched = true;
    if (wait_for_exit) {
        result.exit_code = static_cast<int>(raw);
        return result;
    }

    const HANDLE process = reinterpret_cast<HANDLE>(raw);
    result.process_id =
        static_cast<std::uint64_t>(GetProcessId(process));
    CloseHandle(process);
    result.exit_code = 0;
    return result;
}

#else

ProcessResult spawn_process(
    const fs::path& program,
    const std::vector<std::string>& arguments,
    const fs::path& working_directory,
    bool wait_for_exit,
    bool search_path) {

    ProcessResult result;
    CurrentDirectoryGuard cwd(working_directory);
    if (!cwd.ok()) {
        result.error = cwd.error();
        return result;
    }

    std::vector<std::string> owned;
    owned.reserve(arguments.size() + 1);
    owned.push_back(program.string());
    owned.insert(owned.end(), arguments.begin(), arguments.end());

    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (auto& item : owned) argv.push_back(item.data());
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawn_error =
        search_path
        ? posix_spawnp(
              &pid,
              program.string().c_str(),
              nullptr,
              nullptr,
              argv.data(),
              environ)
        : posix_spawn(
              &pid,
              program.string().c_str(),
              nullptr,
              nullptr,
              argv.data(),
              environ);

    if (spawn_error != 0) {
        result.error =
            "process launch failed: " +
            std::string(std::strerror(spawn_error));
        return result;
    }

    result.launched = true;
    result.process_id = static_cast<std::uint64_t>(pid);
    if (!wait_for_exit) {
        result.exit_code = 0;
        return result;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        result.error =
            "waitpid failed: " +
            std::string(std::strerror(errno));
        result.launched = false;
        return result;
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        return result;
    }

    result.error = "process terminated abnormally";
    result.launched = false;
    return result;
}

#endif

std::optional<fs::path> absolute_existing_directory(
    const json::object& config,
    const char* key,
    std::string& error) {

    const auto value = str(config.if_contains(key));
    if (value.empty()) return std::nullopt;

    const fs::path path(value);
    if (!path.is_absolute()) {
        error = std::string(key) + " must be an absolute path";
        return std::nullopt;
    }

    std::error_code ec;
    if (!fs::is_directory(path, ec) || ec) {
        error =
            std::string(key) + " is not an existing directory: " +
            path.string();
        return std::nullopt;
    }
    return path;
}

DeterministicHandlerResult run_read_only_probe(
    const DispatchRecord& dispatch) {

    DeterministicHandlerResult result;
    result.supported = true;
    result.success = true;
    result.handler = dispatch.handler;

    json::array observations;
    std::size_t existing = 0;

    for (const auto& ref : dispatch.context_refs) {
        json::object observation;
        observation["path"] = ref;

        const fs::path path(ref);
        if (!path.is_absolute()) {
            observation["exists"] = false;
            observation["error"] =
                "read_only_probe requires absolute context_refs";
            observations.emplace_back(std::move(observation));
            continue;
        }

        std::error_code ec;
        const bool exists = fs::exists(path, ec) && !ec;
        observation["exists"] = exists;
        if (!exists) {
            if (ec) observation["error"] = ec.message();
            observations.emplace_back(std::move(observation));
            continue;
        }

        ++existing;
        result.evidence_refs.push_back(ref);

        ec.clear();
        if (fs::is_regular_file(path, ec) && !ec) {
            observation["kind"] = "file";
            ec.clear();
            const auto size = fs::file_size(path, ec);
            if (!ec)
                observation["size"] =
                    static_cast<std::uint64_t>(size);
        } else if (fs::is_directory(path, ec) && !ec) {
            observation["kind"] = "directory";
        } else {
            observation["kind"] = "other";
        }

        const auto modified = mtime_seconds(path);
        if (modified) observation["mtime"] = *modified;
        observations.emplace_back(std::move(observation));
    }

    result.summary =
        "Read-only probe observed " +
        std::to_string(existing) + "/" +
        std::to_string(dispatch.context_refs.size()) +
        " context references";
    result.metadata["observations"] = std::move(observations);
    return result;
}

DeterministicHandlerResult run_local_process_restart(
    const DispatchRecord& dispatch) {

    DeterministicHandlerResult result;
    result.supported = true;
    result.handler = dispatch.handler;

    const auto program_text =
        str(dispatch.handler_config.if_contains("program"));
    if (program_text.empty()) {
        result.error = "handler_config.program is required";
        return result;
    }

    const fs::path program(program_text);
    if (!program.is_absolute()) {
        result.error =
            "local_process_restart_v1 program must be an absolute path";
        return result;
    }

    std::error_code ec;
    if (!fs::is_regular_file(program, ec) || ec) {
        result.error =
            "local restart program does not exist: " +
            program.string();
        return result;
    }

    std::string cwd_error;
    auto working_directory = absolute_existing_directory(
        dispatch.handler_config,
        "working_directory",
        cwd_error);
    if (!cwd_error.empty()) {
        result.error = cwd_error;
        return result;
    }
    if (!working_directory)
        working_directory = program.parent_path();

    const auto arguments =
        string_array(dispatch.handler_config.if_contains("arguments"));

    const auto process = spawn_process(
        program,
        arguments,
        *working_directory,
        false,
        false);
    if (!process.launched) {
        result.error = process.error;
        return result;
    }

    result.success = true;
    result.action_applied = true;
    result.process_id = process.process_id;
    result.exit_code = process.exit_code;
    result.summary =
        "Pre-authorized local restart process launched";
    result.evidence_refs = dispatch.context_refs;

    result.metadata["program"] = program.string();
    result.metadata["arguments"] = json_strings(arguments);
    result.metadata["working_directory"] =
        working_directory->string();
    result.metadata["process_id"] = process.process_id;
    return result;
}

DeterministicHandlerResult run_pbs_qsub_restart(
    const DispatchRecord& dispatch) {

    DeterministicHandlerResult result;
    result.supported = true;
    result.handler = dispatch.handler;

    const auto script_text =
        str(dispatch.handler_config.if_contains("script_path"));
    if (script_text.empty()) {
        result.error = "handler_config.script_path is required";
        return result;
    }

    const fs::path script(script_text);
    if (!script.is_absolute()) {
        result.error =
            "pbs_qsub_restart_v1 script_path must be absolute";
        return result;
    }

    std::error_code ec;
    if (!fs::is_regular_file(script, ec) || ec) {
        result.error =
            "PBS submission script does not exist: " +
            script.string();
        return result;
    }

    std::string cwd_error;
    auto working_directory = absolute_existing_directory(
        dispatch.handler_config,
        "working_directory",
        cwd_error);
    if (!cwd_error.empty()) {
        result.error = cwd_error;
        return result;
    }
    if (!working_directory)
        working_directory = script.parent_path();

    fs::path qsub("qsub");
    if (const auto* configured = std::getenv("MONITOR_HUB_QSUB");
        configured && *configured)
        qsub = fs::path(configured);

    const bool search_path =
        !qsub.is_absolute() && !qsub.has_parent_path();
    if (!search_path) {
        ec.clear();
        if (!fs::is_regular_file(qsub, ec) || ec) {
            result.error =
                "configured qsub executable does not exist: " +
                qsub.string();
            return result;
        }
    }

    const auto process = spawn_process(
        qsub,
        {script.string()},
        *working_directory,
        true,
        search_path);
    result.exit_code = process.exit_code;
    result.process_id = process.process_id;
    if (!process.launched || process.exit_code != 0) {
        std::ostringstream message;
        message
            << "PBS qsub failed";
        if (process.exit_code >= 0)
            message << " with exit code " << process.exit_code;
        if (!process.error.empty())
            message << ": " << process.error;
        result.error = message.str();
        return result;
    }

    result.success = true;
    result.action_applied = true;
    result.summary =
        "Pre-authorized PBS submission script accepted by qsub";
    result.evidence_refs = dispatch.context_refs;
    result.evidence_refs.push_back(script.string());

    result.metadata["qsub_executable"] = qsub.string();
    result.metadata["script_path"] = script.string();
    result.metadata["working_directory"] =
        working_directory->string();
    result.metadata["exit_code"] = process.exit_code;
    if (process.process_id != 0)
        result.metadata["process_id"] = process.process_id;
    return result;
}

}  // namespace

bool is_registered_deterministic_handler(
    const std::string& handler) {
    return handler == "read_only_probe" ||
           handler == "local_process_restart_v1" ||
           handler == "pbs_qsub_restart_v1";
}

std::vector<std::string> registered_deterministic_handlers() {
    return {
        "read_only_probe",
        "local_process_restart_v1",
        "pbs_qsub_restart_v1",
    };
}

DeterministicHandlerResult execute_deterministic_handler(
    const DispatchRecord& dispatch) {

    if (dispatch.handler == "read_only_probe")
        return run_read_only_probe(dispatch);
    if (dispatch.handler == "local_process_restart_v1")
        return run_local_process_restart(dispatch);
    if (dispatch.handler == "pbs_qsub_restart_v1")
        return run_pbs_qsub_restart(dispatch);

    DeterministicHandlerResult result;
    result.handler = dispatch.handler;
    result.error =
        "No audited deterministic handler is registered for: " +
        dispatch.handler;
    return result;
}

}  // namespace monitor_hub
