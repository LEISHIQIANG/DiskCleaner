#include "DiskCleanerCore.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <winsvc.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

struct CommandResult {
    DWORD exit_code = ERROR_GEN_FAILURE;
    std::wstring output;
    bool timed_out = false;
};

const std::vector<CleanItemDefinition>& BuildItems() {
    static const std::vector<CleanItemDefinition> items = {
        {L"temp", L"临时文件", L"清理 Windows 和用户临时目录", false},
        {L"recycle", L"回收站", L"清空所有驱动器的回收站", false},
        {L"thumb", L"缩略图缓存", L"清理图片和图标缓存文件", false},
        {L"wincache", L"Windows 缓存", L"清理系统缓存和兼容性缓存", false},
        {L"appcache", L"应用缓存", L"清理常见软件和开发工具缓存", false},
        {L"browser", L"浏览器缓存", L"清理 Chrome / Edge / Firefox 缓存", false},
        {L"logs", L"系统日志", L"清理 Windows 日志文件", false},
        {L"recent", L"最近文档", L"清理文件访问历史记录", false},
        {L"error", L"错误报告", L"清理 Windows 错误报告文件", false},
        {L"directx", L"DirectX 缓存", L"清理显卡着色器缓存", false},
        {L"gpu", L"显卡缓存", L"清理 NVIDIA / AMD / Intel 着色器缓存", false},
        {L"inet", L"Internet 缓存", L"清理 Windows WinINet 缓存", false},
        {L"crashdumps", L"崩溃转储", L"清理应用 CrashDumps 文件", false},
        {L"setup", L"安装残留", L"清理 Windows 安装和升级残留文件", false},
        {L"font", L"字体缓存", L"清理系统字体缓存文件", false},
        {L"prefetch", L"预读取文件", L"清理系统预读缓存（保留 7 天内）", false},
        {L"update", L"更新缓存", L"清理 Windows Update 下载文件", true},
        {L"hibernate", L"休眠文件", L"禁用休眠功能并删除休眠文件", true},
        {L"restore", L"系统还原点", L"清理旧的系统还原点", true},
        {L"oldwin", L"Windows.old", L"删除旧系统备份文件夹", true},
        {L"dumps", L"转储文件", L"清理蓝屏分析转储文件", true},
        {L"delivery", L"传递优化", L"清理更新分发缓存文件", true},
        {L"events", L"事件日志", L"清理系统事件日志记录", true},
        {L"installer", L"安装缓存", L"清理 Installer 补丁缓存（慎用）", true},
    };
    return items;
}

std::wstring GetEnvVar(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) {
        return {};
    }
    std::wstring value(size, L'\0');
    GetEnvironmentVariableW(name, value.data(), size);
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

std::wstring SystemRoot() {
    const auto value = GetEnvVar(L"SYSTEMROOT");
    return value.empty() ? L"C:\\Windows" : value;
}

std::wstring SystemDrive() {
    const auto value = GetEnvVar(L"SYSTEMDRIVE");
    return value.empty() ? L"C:" : value;
}

std::wstring SystemPath(const wchar_t* suffix) {
    return SystemRoot() + suffix;
}

std::wstring DrivePath(const wchar_t* suffix) {
    return SystemDrive() + suffix;
}

bool IsOlderThan(const fs::path& path, int min_age_days) {
    if (min_age_days <= 0) {
        return true;
    }

    std::error_code ec;
    const auto written = fs::last_write_time(path, ec);
    if (ec) {
        return false;
    }
    const auto age = fs::file_time_type::clock::now() - written;
    const auto days = std::chrono::duration_cast<std::chrono::hours>(age).count() / 24;
    return days >= min_age_days;
}

bool MatchesPattern(const std::wstring& name, const std::wstring& pattern) {
    if (pattern.empty()) {
        return true;
    }
    return PathMatchSpecW(name.c_str(), pattern.c_str()) == TRUE;
}

void NormalizeAttributesRecursive(const fs::path& path) {
    const auto raw = path.wstring();
    SetFileAttributesW(raw.c_str(), FILE_ATTRIBUTE_NORMAL);

    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        return;
    }

    for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        SetFileAttributesW(it->path().wstring().c_str(), FILE_ATTRIBUTE_NORMAL);
    }
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::uint64_t GetPathSize(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return 0;
    }
    if (fs::is_regular_file(path, ec)) {
        return static_cast<std::uint64_t>(fs::file_size(path, ec));
    }

    std::uint64_t total = 0;
    for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::error_code item_ec;
        if (it->is_regular_file(item_ec)) {
            total += static_cast<std::uint64_t>(it->file_size(item_ec));
        }
    }
    return total;
}

bool RemovePath(const fs::path& path) {
    NormalizeAttributesRecursive(path);
    std::error_code ec;
    fs::remove_all(path, ec);
    return !fs::exists(path);
}

std::wstring QuoteArgument(const std::wstring& arg) {
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring quoted = L"\"";
    for (const wchar_t ch : arg) {
        if (ch == L'"') {
            quoted += L'\\';
        }
        quoted += ch;
    }
    quoted += L"\"";
    return quoted;
}

CommandResult RunProcess(const std::wstring& exe_path, const std::vector<std::wstring>& args, DWORD timeout_ms) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        return {};
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring command_line = QuoteArgument(exe_path);
    for (const auto& arg : args) {
        command_line += L" ";
        command_line += QuoteArgument(arg);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;

    PROCESS_INFORMATION pi{};
    std::wstring mutable_command = command_line;

    CommandResult result;
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        result.exit_code = GetLastError();
        return result;
    }

    CloseHandle(write_pipe);
    const DWORD wait_result = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, ERROR_TIMEOUT);
        result.timed_out = true;
    }

    GetExitCodeProcess(pi.hProcess, &result.exit_code);

    std::string output;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        output.append(buffer, buffer + read);
    }

    if (!output.empty()) {
        const int wide_len = MultiByteToWideChar(CP_OEMCP, 0, output.data(),
            static_cast<int>(output.size()), nullptr, 0);
        if (wide_len > 0) {
            result.output.resize(wide_len);
            MultiByteToWideChar(CP_OEMCP, 0, output.data(),
                static_cast<int>(output.size()), result.output.data(), wide_len);
        }
    }

    CloseHandle(read_pipe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}

bool QueryServiceRunning(const std::wstring& service_name) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, service_name.c_str(), SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(manager);
        return false;
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes_needed = 0;
    const bool running = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes_needed) != FALSE &&
        status.dwCurrentState == SERVICE_RUNNING;
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return running;
}

bool WaitForServiceState(SC_HANDLE service, DWORD desired_state, DWORD timeout_ms) {
    const auto start = GetTickCount64();
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes_needed = 0;

    while (GetTickCount64() - start < timeout_ms) {
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes_needed)) {
            return false;
        }
        if (status.dwCurrentState == desired_state) {
            return true;
        }
        Sleep(250);
    }
    return false;
}

bool StopServiceByName(const std::wstring& service_name) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, service_name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(manager);
        return false;
    }
    SERVICE_STATUS status{};
    const bool ok = ControlService(service, SERVICE_CONTROL_STOP, &status) != FALSE &&
        WaitForServiceState(service, SERVICE_STOPPED, 30000);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ok;
}

bool StartServiceByName(const std::wstring& service_name) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, service_name.c_str(), SERVICE_START | SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(manager);
        return false;
    }
    const bool started = StartServiceW(service, 0, nullptr) != FALSE || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    const bool ok = started && WaitForServiceState(service, SERVICE_RUNNING, 30000);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ok;
}

std::wstring CommandText(const std::wstring& output) {
    std::wstring cleaned = output;
    cleaned.erase(std::remove(cleaned.begin(), cleaned.end(), L'\r'), cleaned.end());
    return cleaned;
}

const CleanItemDefinition* FindItem(const std::wstring& key) {
    const auto& items = BuildItems();
    const auto it = std::find_if(items.begin(), items.end(), [&](const CleanItemDefinition& item) {
        return item.key == key;
    });
    return it == items.end() ? nullptr : &(*it);
}

} // namespace

DiskCleanerCore::DiskCleanerCore(LogCallback log_callback, StatusCallback status_callback)
    : log_callback_(std::move(log_callback)), status_callback_(std::move(status_callback)) {}

const std::vector<CleanItemDefinition>& DiskCleanerCore::AllItems() {
    return BuildItems();
}

std::vector<std::wstring> DiskCleanerCore::DefaultSelection() {
    return {L"temp", L"recycle", L"thumb", L"wincache", L"browser", L"appcache",
        L"logs", L"directx", L"gpu", L"inet", L"crashdumps"};
}

std::wstring DiskCleanerCore::FormatSize(std::uint64_t size) const {
    static const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(size);
    size_t unit_index = 0;
    while (value >= 1024.0 && unit_index < std::size(units) - 1) {
        value /= 1024.0;
        ++unit_index;
    }
    std::wostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    oss << value << units[unit_index];
    return oss.str();
}

void DiskCleanerCore::Log(const std::wstring& message) const {
    if (log_callback_) {
        log_callback_(message);
    }
}

void DiskCleanerCore::Status(const std::wstring& message) const {
    if (status_callback_) {
        status_callback_(message);
    }
}

std::uint64_t DiskCleanerCore::GetSize(const std::wstring& path) const {
    return GetPathSize(fs::path(path));
}

bool DiskCleanerCore::CheckPathAccessible(const std::wstring& path) const {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return false;
    }

    if (fs::is_directory(path, ec)) {
        fs::directory_iterator it(path, ec);
        return !ec;
    }
    return true;
}

std::uint64_t DiskCleanerCore::ScanFolder(const std::wstring& path, const std::wstring& file_pattern, int min_age_days) const {
    std::error_code ec;
    if (!fs::exists(path, ec) || !CheckPathAccessible(path)) {
        return 0;
    }

    std::uint64_t total = 0;
    for (fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const auto entry_path = it->path();
        std::error_code type_ec;
        if (fs::is_directory(entry_path, type_ec)) {
            if (min_age_days > 0) {
                total += ScanFolder(entry_path.wstring(), file_pattern, min_age_days);
            } else {
                total += GetPathSize(entry_path);
            }
            continue;
        }

        if (!IsOlderThan(entry_path, min_age_days)) {
            continue;
        }
        if (!MatchesPattern(entry_path.filename().wstring(), file_pattern)) {
            continue;
        }

        std::error_code item_ec;
        if (fs::is_regular_file(entry_path, item_ec)) {
            total += static_cast<std::uint64_t>(it->file_size(item_ec));
        }
    }
    return total;
}

std::uint64_t DiskCleanerCore::CleanFolder(const std::wstring& path, const std::wstring& desc,
    const std::wstring& file_pattern, int min_age_days, bool log_result, bool is_recursive_call) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return 0;
    }
    if (!CheckPathAccessible(path)) {
        if (log_result) {
            Log(L"  " + desc + L": 无访问权限");
        }
        return 0;
    }

    std::uint64_t cleaned = 0;
    int failed = 0;

    for (fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            ++failed;
            continue;
        }

        const auto entry_path = it->path();
        std::error_code type_ec;
        if (fs::is_directory(entry_path, type_ec)) {
            if (min_age_days > 0) {
                cleaned += CleanFolder(entry_path.wstring(), desc, file_pattern, min_age_days, false, true);
                std::error_code empty_ec;
                if (fs::is_empty(entry_path, empty_ec)) {
                    fs::remove(entry_path, empty_ec);
                }
                continue;
            }
        }

        if (!IsOlderThan(entry_path, min_age_days)) {
            continue;
        }
        if (!MatchesPattern(entry_path.filename().wstring(), file_pattern)) {
            continue;
        }

        const auto size_before = GetPathSize(entry_path);
        try {
            if (RemovePath(entry_path)) {
                cleaned += size_before;
            } else {
                const auto size_after = GetPathSize(entry_path);
                if (size_before > size_after) {
                    cleaned += size_before - size_after;
                }
                ++failed;
            }
        } catch (const std::exception&) {
            ++failed;
            errors_.push_back(entry_path.wstring());
        }
    }

    if (log_result && cleaned > 0) {
        std::wstring message = L"  " + desc + L": " + FormatSize(cleaned);
        if (failed > 0) {
            message += L" (跳过 " + std::to_wstring(failed) + L" 项)";
        }
        Log(message);
    }

    if (!is_recursive_call) {
        total_cleaned_ += cleaned;
    }
    return cleaned;
}

std::uint64_t DiskCleanerCore::GetRecycleSize() const {
    SHQUERYRBINFO info{};
    info.cbSize = sizeof(info);
    if (SHQueryRecycleBinW(nullptr, &info) == S_OK) {
        return static_cast<std::uint64_t>(info.i64Size);
    }
    return 0;
}

std::uint64_t DiskCleanerCore::GetRestoreSize() const {
    const auto result = RunProcess(L"vssadmin.exe", {L"list", L"shadowstorage", L"/for=" + SystemDrive()}, 10000);
    if (result.timed_out) {
        return 0;
    }

    std::wregex pattern(LR"(([\d\.]+)\s*([KMGT]B))", std::regex::icase);
    std::wsmatch match;
    std::wstringstream stream(CommandText(result.output));
    std::wstring line;
    while (std::getline(stream, line)) {
        if (line.find(L"Used") == std::wstring::npos && line.find(L"已用") == std::wstring::npos) {
            continue;
        }
        if (std::regex_search(line, match, pattern)) {
            double value = std::stod(match[1].str());
            const auto unit = match[2].str();
            if (_wcsicmp(unit.c_str(), L"TB") == 0) {
                value *= 1024.0 * 1024.0 * 1024.0 * 1024.0;
            } else if (_wcsicmp(unit.c_str(), L"GB") == 0) {
                value *= 1024.0 * 1024.0 * 1024.0;
            } else if (_wcsicmp(unit.c_str(), L"MB") == 0) {
                value *= 1024.0 * 1024.0;
            } else if (_wcsicmp(unit.c_str(), L"KB") == 0) {
                value *= 1024.0;
            }
            return static_cast<std::uint64_t>(value);
        }
    }
    return 0;
}

std::uint64_t DiskCleanerCore::ScanBrowserCacheSize() const {
    const std::wstring local = GetEnvVar(L"LOCALAPPDATA");
    const std::wstring roaming = GetEnvVar(L"APPDATA");

    const std::vector<std::pair<std::wstring, std::wstring>> browsers = {
        {local + LR"(\Google\Chrome\User Data)", L"Chrome"},
        {local + LR"(\Microsoft\Edge\User Data)", L"Edge"},
        {local + LR"(\Mozilla\Firefox\Profiles)", L"Firefox"},
        {roaming + LR"(\Opera Software\Opera Stable)", L"Opera"},
        {local + LR"(\BraveSoftware\Brave-Browser\User Data)", L"Brave"},
    };

    const std::vector<std::wstring> cache_dirs = {
        L"Cache", L"Code Cache", L"GPUCache", L"Service Worker\\CacheStorage",
        L"Service Worker\\ScriptCache", L"cache2", L"CacheStorage", L"Media Cache",
        L"ShaderCache", L"GrShaderCache", L"GraphiteDawnCache", L"DawnCache", L"startupCache"
    };

    std::uint64_t total = 0;
    for (const auto& [base_path, name] : browsers) {
        UNREFERENCED_PARAMETER(name);
        std::error_code ec;
        if (!fs::exists(base_path, ec)) {
            continue;
        }

        std::vector<fs::path> profiles;
        if (name == L"Firefox") {
            for (fs::directory_iterator it(base_path, fs::directory_options::skip_permission_denied, ec), end;
                 it != end; it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (it->is_directory(ec)) {
                    profiles.push_back(it->path());
                }
            }
        } else {
            profiles.push_back(base_path);
            for (fs::directory_iterator it(base_path, fs::directory_options::skip_permission_denied, ec), end;
                 it != end; it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                const auto name_part = it->path().filename().wstring();
                if (it->is_directory(ec) && (name_part == L"Default" || name_part.rfind(L"Profile", 0) == 0)) {
                    profiles.push_back(it->path());
                }
            }
        }

        for (const auto& profile : profiles) {
            for (const auto& cache_dir : cache_dirs) {
                total += ScanFolder((profile / cache_dir).wstring());
            }
        }
    }
    return total;
}

std::uint64_t DiskCleanerCore::ScanWindowsCacheSize() const {
    const auto local = GetEnvVar(L"LOCALAPPDATA");
    const std::vector<std::wstring> paths = {
        local + LR"(\Microsoft\Windows\Caches)",
        local + LR"(\Microsoft\Windows\IECompatCache)",
        local + LR"(\Microsoft\Windows\IECompatUaCache)",
        local + LR"(\Microsoft\Windows\IETldCache)",
    };
    std::uint64_t total = 0;
    for (const auto& path : paths) {
        total += ScanFolder(path);
    }
    return total;
}

std::uint64_t DiskCleanerCore::ScanApplicationCacheSize() const {
    const auto local = GetEnvVar(L"LOCALAPPDATA");
    const auto roaming = GetEnvVar(L"APPDATA");
    const auto program_files_x86 = GetEnvVar(L"PROGRAMFILES(X86)");
    const auto user_profile = GetEnvVar(L"USERPROFILE");
    const std::vector<std::wstring> paths = {
        local + LR"(\Code\Cache)", local + LR"(\Code\CachedData)", local + LR"(\Code\Code Cache)", local + LR"(\Code\GPUCache)",
        local + LR"(\Cursor\Cache)", local + LR"(\Cursor\CachedData)", local + LR"(\Cursor\Code Cache)", local + LR"(\Cursor\GPUCache)",
        roaming + LR"(\discord\Cache)", roaming + LR"(\discord\Code Cache)", roaming + LR"(\discord\GPUCache)",
        roaming + LR"(\Slack\Cache)", roaming + LR"(\Notion\Cache)", roaming + LR"(\Figma\Cache)",
        local + LR"(\GitHub Desktop\Cache)", roaming + LR"(\Microsoft\Teams\Cache)",
        local + LR"(\Packages\MSTeams_8wekyb3d8bbwe\LocalCache\Microsoft\MSTeams\EBWebView\Default)",
        local + LR"(\Adobe\CameraRaw\Cache)", local + LR"(\Adobe\Common\Media Cache)", local + LR"(\Adobe\Common\Media Cache Files)",
        local + LR"(\EpicGamesLauncher\Saved\webcache)", program_files_x86 + LR"(\Steam\config\htmlcache)",
        user_profile + LR"(\.nuget\packages)", user_profile + LR"(\AppData\Local\pip\cache)",
        user_profile + LR"(\AppData\Local\npm-cache)", user_profile + LR"(\AppData\Roaming\npm-cache)",
        user_profile + LR"(\AppData\Local\Yarn\Cache)", user_profile + LR"(\.cargo\registry\cache)",
    };
    std::uint64_t total = 0;
    for (const auto& path : paths) {
        total += ScanFolder(path);
    }
    total += ScanFolder(local + LR"(\Microsoft\Windows\WebCache)", L"webcache*");
    return total;
}

std::uint64_t DiskCleanerCore::ScanGpuCacheSize() const {
    const auto local = GetEnvVar(L"LOCALAPPDATA");
    const auto user_profile = GetEnvVar(L"USERPROFILE");
    const std::vector<std::wstring> paths = {
        local + LR"(\NVIDIA\DXCache)", local + LR"(\NVIDIA\GLCache)", local + LR"(\NVIDIA Corporation\NV_Cache)",
        local + LR"(\AMD\DxCache)", local + LR"(\AMD\GLCache)", local + LR"(\Intel\ShaderCache)",
        user_profile + LR"(\AppData\LocalLow\Intel\ShaderCache)",
    };
    std::uint64_t total = 0;
    for (const auto& path : paths) {
        total += ScanFolder(path);
    }
    return total;
}

std::uint64_t DiskCleanerCore::ScanInternetCacheSize() const {
    const auto local = GetEnvVar(L"LOCALAPPDATA");
    const std::vector<std::wstring> paths = {
        local + LR"(\Microsoft\Windows\INetCache)",
        SystemPath(L"\\System32\\config\\systemprofile\\AppData\\Local\\Microsoft\\Windows\\INetCache"),
        SystemPath(L"\\ServiceProfiles\\LocalService\\AppData\\Local\\Microsoft\\Windows\\INetCache"),
        SystemPath(L"\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\INetCache"),
    };
    std::uint64_t total = 0;
    for (const auto& path : paths) {
        total += ScanFolder(path);
    }
    return total;
}

std::uint64_t DiskCleanerCore::ScanCrashDumpSize() const {
    return ScanFolder(GetEnvVar(L"LOCALAPPDATA") + LR"(\CrashDumps)");
}

std::uint64_t DiskCleanerCore::ScanSetupResidueSize() const {
    return ScanFolder(SystemPath(L"\\Panther")) +
        ScanFolder(SystemPath(L"\\Logs\\MoSetup")) +
        ScanFolder(DrivePath(L"\\$WinREAgent"));
}

std::uint64_t DiskCleanerCore::ScanItem(const std::wstring& key) {
    if (key == L"temp") {
        return ScanFolder(SystemPath(L"\\Temp")) +
            ScanFolder(GetEnvVar(L"TEMP").empty() ? GetEnvVar(L"TMP") : GetEnvVar(L"TEMP"));
    }
    if (key == L"recycle") {
        return GetRecycleSize();
    }
    if (key == L"prefetch") {
        return ScanFolder(SystemPath(L"\\Prefetch"), L"*.pf", 7);
    }
    if (key == L"thumb") {
        return ScanFolder(GetEnvVar(L"LOCALAPPDATA") + LR"(\Microsoft\Windows\Explorer)");
    }
    if (key == L"wincache") {
        return ScanWindowsCacheSize();
    }
    if (key == L"appcache") {
        return ScanApplicationCacheSize();
    }
    if (key == L"browser") {
        return ScanBrowserCacheSize();
    }
    if (key == L"logs") {
        return ScanFolder(SystemPath(L"\\Logs"), L"", 7) +
            ScanFolder(SystemPath(L"\\Logs\\CBS"), L"*.log", 7) +
            ScanFolder(SystemPath(L"\\Logs\\DISM"), L"", 7);
    }
    if (key == L"recent") {
        return ScanFolder(GetEnvVar(L"APPDATA") + LR"(\Microsoft\Windows\Recent)");
    }
    if (key == L"error") {
        return ScanFolder(GetEnvVar(L"LOCALAPPDATA") + LR"(\Microsoft\Windows\WER)") +
            ScanFolder(GetEnvVar(L"PROGRAMDATA") + LR"(\Microsoft\Windows\WER)");
    }
    if (key == L"update") {
        return ScanFolder(SystemPath(L"\\SoftwareDistribution\\Download"));
    }
    if (key == L"hibernate") {
        return GetSize(DrivePath(L"\\hiberfil.sys"));
    }
    if (key == L"restore") {
        return GetRestoreSize();
    }
    if (key == L"oldwin") {
        return GetSize(DrivePath(L"\\Windows.old"));
    }
    if (key == L"dumps") {
        return GetSize(SystemPath(L"\\MEMORY.DMP")) +
            GetSize(SystemPath(L"\\Minidump")) +
            GetSize(SystemPath(L"\\LiveKernelReports"));
    }
    if (key == L"delivery") {
        return ScanFolder(SystemPath(L"\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\DeliveryOptimization"));
    }
    if (key == L"events") {
        return ScanFolder(SystemPath(L"\\System32\\winevt\\Logs"));
    }
    if (key == L"directx") {
        return ScanFolder(GetEnvVar(L"LOCALAPPDATA") + LR"(\D3DSCache)");
    }
    if (key == L"gpu") {
        return ScanGpuCacheSize();
    }
    if (key == L"inet") {
        return ScanInternetCacheSize();
    }
    if (key == L"crashdumps") {
        return ScanCrashDumpSize();
    }
    if (key == L"setup") {
        return ScanSetupResidueSize();
    }
    if (key == L"font") {
        return ScanFolder(SystemPath(L"\\ServiceProfiles\\LocalService\\AppData\\Local\\FontCache"));
    }
    if (key == L"installer") {
        return ScanFolder(SystemPath(L"\\Installer\\$PatchCache$"));
    }
    return 0;
}

std::uint64_t DiskCleanerCore::CleanWindowsTemp() {
    Status(L"清理 Windows 临时文件...");
    return CleanFolder(SystemPath(L"\\Temp"), L"Windows 临时文件");
}

std::uint64_t DiskCleanerCore::CleanUserTemp() {
    Status(L"清理用户临时文件...");
    const auto temp = GetEnvVar(L"TEMP").empty() ? GetEnvVar(L"TMP") : GetEnvVar(L"TEMP");
    return CleanFolder(temp, L"用户临时文件");
}

std::uint64_t DiskCleanerCore::CleanRecent() {
    Status(L"清理最近文档...");
    return CleanFolder(GetEnvVar(L"APPDATA") + LR"(\Microsoft\Windows\Recent)", L"最近文档记录");
}

bool DiskCleanerCore::CleanRecycleBin() {
    Status(L"清空回收站...");
    const auto size_before = GetRecycleSize();
    const HRESULT hr = SHEmptyRecycleBinW(nullptr, nullptr,
        SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    if (SUCCEEDED(hr)) {
        if (size_before > 0) {
            total_cleaned_ += size_before;
            Log(L"  回收站: " + FormatSize(size_before));
        } else {
            Log(L"  回收站: 已空或清理完成");
        }
        return true;
    }
    Log(L"  回收站: 清理失败");
    return false;
}

std::uint64_t DiskCleanerCore::CleanPrefetch() {
    Status(L"清理预读取文件...");
    return CleanFolder(SystemPath(L"\\Prefetch"), L"预读取文件", L"*.pf", 7);
}

std::uint64_t DiskCleanerCore::CleanThumbnailCache() {
    Status(L"清理缩略图缓存...");
    const auto cache_path = GetEnvVar(L"LOCALAPPDATA") + LR"(\Microsoft\Windows\Explorer)";
    std::error_code ec;
    if (!fs::exists(cache_path, ec)) {
        return 0;
    }

    std::uint64_t cleaned = 0;
    for (fs::directory_iterator it(cache_path, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const auto name = it->path().filename().wstring();
        const auto lower = ToLower(name);
        if (lower.find(L"thumbcache") == std::wstring::npos && lower.find(L"iconcache") == std::wstring::npos) {
            continue;
        }
        const auto size = GetPathSize(it->path());
        if (RemovePath(it->path())) {
            cleaned += size;
        }
    }

    if (cleaned > 0) {
        Log(L"  缩略图缓存: " + FormatSize(cleaned));
        total_cleaned_ += cleaned;
    }
    return cleaned;
}

std::uint64_t DiskCleanerCore::CleanWindowsLogs() {
    Status(L"清理 Windows 日志...");
    return CleanFolder(SystemPath(L"\\Logs"), L"系统日志", L"", 7) +
        CleanFolder(SystemPath(L"\\Logs\\CBS"), L"CBS 日志", L"*.log", 7) +
        CleanFolder(SystemPath(L"\\Logs\\DISM"), L"DISM 日志", L"", 7);
}

std::uint64_t DiskCleanerCore::CleanErrorReports() {
    Status(L"清理错误报告...");
    return CleanFolder(GetEnvVar(L"LOCALAPPDATA") + LR"(\Microsoft\Windows\WER)", L"用户错误报告") +
        CleanFolder(GetEnvVar(L"PROGRAMDATA") + LR"(\Microsoft\Windows\WER)", L"系统错误报告");
}

std::uint64_t DiskCleanerCore::CleanBrowserCache() {
    Status(L"清理浏览器缓存...");
    const std::wstring local = GetEnvVar(L"LOCALAPPDATA");
    const std::wstring roaming = GetEnvVar(L"APPDATA");
    const std::vector<std::pair<std::wstring, std::wstring>> browsers = {
        {local + LR"(\Google\Chrome\User Data)", L"Chrome"},
        {local + LR"(\Microsoft\Edge\User Data)", L"Edge"},
        {local + LR"(\Mozilla\Firefox\Profiles)", L"Firefox"},
        {roaming + LR"(\Opera Software\Opera Stable)", L"Opera"},
        {local + LR"(\BraveSoftware\Brave-Browser\User Data)", L"Brave"},
    };
    const std::vector<std::wstring> cache_dirs = {
        L"Cache", L"Code Cache", L"GPUCache", L"Service Worker\\CacheStorage",
        L"Service Worker\\ScriptCache", L"cache2", L"CacheStorage", L"Media Cache",
        L"ShaderCache", L"GrShaderCache", L"GraphiteDawnCache", L"DawnCache", L"startupCache"
    };

    std::uint64_t cleaned = 0;
    for (const auto& [base_path, name] : browsers) {
        std::error_code ec;
        if (!fs::exists(base_path, ec)) {
            continue;
        }

        std::vector<fs::path> profiles;
        if (name == L"Firefox") {
            for (fs::directory_iterator it(base_path, fs::directory_options::skip_permission_denied, ec), end;
                 it != end; it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (it->is_directory(ec)) {
                    profiles.push_back(it->path());
                }
            }
        } else {
            profiles.push_back(base_path);
            for (fs::directory_iterator it(base_path, fs::directory_options::skip_permission_denied, ec), end;
                 it != end; it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                const auto profile_name = it->path().filename().wstring();
                if (it->is_directory(ec) && (profile_name == L"Default" || profile_name.rfind(L"Profile", 0) == 0)) {
                    profiles.push_back(it->path());
                }
            }
        }

        for (const auto& profile : profiles) {
            for (const auto& cache_dir : cache_dirs) {
                const auto cache_path = (profile / cache_dir).wstring();
                if (fs::exists(cache_path, ec)) {
                    cleaned += CleanFolder(cache_path, name + L" 缓存");
                }
            }
        }
    }
    return cleaned;
}

std::uint64_t DiskCleanerCore::CleanWindowsCache() {
    Status(L"清理 Windows 缓存...");
    const auto local = GetEnvVar(L"LOCALAPPDATA");
    const std::vector<std::pair<std::wstring, std::wstring>> paths = {
        {local + LR"(\Microsoft\Windows\Caches)", L"Windows 缓存"},
        {local + LR"(\Microsoft\Windows\IECompatCache)", L"IE 兼容性缓存"},
        {local + LR"(\Microsoft\Windows\IECompatUaCache)", L"IE UA 缓存"},
        {local + LR"(\Microsoft\Windows\IETldCache)", L"IE 域名缓存"},
    };
    std::uint64_t cleaned = 0;
    for (const auto& [path, name] : paths) {
        cleaned += CleanFolder(path, name);
    }
    return cleaned;
}

std::uint64_t DiskCleanerCore::CleanApplicationCache() {
    Status(L"清理应用缓存...");
    const auto local = GetEnvVar(L"LOCALAPPDATA");
    const auto roaming = GetEnvVar(L"APPDATA");
    const auto program_files_x86 = GetEnvVar(L"PROGRAMFILES(X86)");
    const auto user_profile = GetEnvVar(L"USERPROFILE");
    const std::vector<std::pair<std::wstring, std::wstring>> paths = {
        {local + LR"(\Code\Cache)", L"VS Code 缓存"}, {local + LR"(\Code\CachedData)", L"VS Code 缓存"}, {local + LR"(\Code\Code Cache)", L"VS Code 缓存"}, {local + LR"(\Code\GPUCache)", L"VS Code GPU 缓存"},
        {local + LR"(\Cursor\Cache)", L"Cursor 缓存"}, {local + LR"(\Cursor\CachedData)", L"Cursor 缓存"}, {local + LR"(\Cursor\Code Cache)", L"Cursor 缓存"}, {local + LR"(\Cursor\GPUCache)", L"Cursor GPU 缓存"},
        {roaming + LR"(\discord\Cache)", L"Discord 缓存"}, {roaming + LR"(\discord\Code Cache)", L"Discord 缓存"}, {roaming + LR"(\discord\GPUCache)", L"Discord GPU 缓存"},
        {roaming + LR"(\Slack\Cache)", L"Slack 缓存"}, {roaming + LR"(\Notion\Cache)", L"Notion 缓存"}, {roaming + LR"(\Figma\Cache)", L"Figma 缓存"},
        {local + LR"(\GitHub Desktop\Cache)", L"GitHub Desktop 缓存"}, {roaming + LR"(\Microsoft\Teams\Cache)", L"Teams 缓存"},
        {local + LR"(\Packages\MSTeams_8wekyb3d8bbwe\LocalCache\Microsoft\MSTeams\EBWebView\Default)", L"新版 Teams 缓存"},
        {local + LR"(\Adobe\CameraRaw\Cache)", L"Adobe Camera Raw 缓存"}, {local + LR"(\Adobe\Common\Media Cache)", L"Adobe 媒体缓存"}, {local + LR"(\Adobe\Common\Media Cache Files)", L"Adobe 媒体缓存"},
        {local + LR"(\EpicGamesLauncher\Saved\webcache)", L"Epic Games 缓存"}, {program_files_x86 + LR"(\Steam\config\htmlcache)", L"Steam 缓存"},
        {user_profile + LR"(\.nuget\packages)", L"NuGet 缓存"}, {user_profile + LR"(\AppData\Local\pip\cache)", L"Python Pip 缓存"},
        {user_profile + LR"(\AppData\Local\npm-cache)", L"npm 本地缓存"}, {user_profile + LR"(\AppData\Roaming\npm-cache)", L"npm 漫游缓存"},
        {user_profile + LR"(\AppData\Local\Yarn\Cache)", L"Yarn 缓存"}, {user_profile + LR"(\.cargo\registry\cache)", L"Cargo 缓存"},
    };
    std::uint64_t cleaned = 0;
    for (const auto& [path, name] : paths) {
        cleaned += CleanFolder(path, name);
    }
    return cleaned + CleanFolder(local + LR"(\Microsoft\Windows\WebCache)", L"WebCache", L"webcache*");
}

std::uint64_t DiskCleanerCore::CleanGpuCache() {
    Status(L"清理显卡缓存...");
    const auto local = GetEnvVar(L"LOCALAPPDATA");
    const auto user_profile = GetEnvVar(L"USERPROFILE");
    const std::vector<std::pair<std::wstring, std::wstring>> paths = {
        {local + LR"(\NVIDIA\DXCache)", L"NVIDIA DX 缓存"}, {local + LR"(\NVIDIA\GLCache)", L"NVIDIA OpenGL 缓存"}, {local + LR"(\NVIDIA Corporation\NV_Cache)", L"NVIDIA 缓存"},
        {local + LR"(\AMD\DxCache)", L"AMD DX 缓存"}, {local + LR"(\AMD\GLCache)", L"AMD OpenGL 缓存"}, {local + LR"(\Intel\ShaderCache)", L"Intel 缓存"},
        {user_profile + LR"(\AppData\LocalLow\Intel\ShaderCache)", L"Intel 低权限缓存"},
    };
    std::uint64_t cleaned = 0;
    for (const auto& [path, name] : paths) {
        cleaned += CleanFolder(path, name);
    }
    return cleaned;
}

std::uint64_t DiskCleanerCore::CleanInternetCache() {
    Status(L"清理 Internet 缓存...");
    const auto local = GetEnvVar(L"LOCALAPPDATA");
    const std::vector<std::pair<std::wstring, std::wstring>> paths = {
        {local + LR"(\Microsoft\Windows\INetCache)", L"用户 WinINet 缓存"},
        {SystemPath(L"\\System32\\config\\systemprofile\\AppData\\Local\\Microsoft\\Windows\\INetCache"), L"系统 WinINet 缓存"},
        {SystemPath(L"\\ServiceProfiles\\LocalService\\AppData\\Local\\Microsoft\\Windows\\INetCache"), L"LocalService WinINet 缓存"},
        {SystemPath(L"\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\INetCache"), L"NetworkService WinINet 缓存"},
    };
    std::uint64_t cleaned = 0;
    for (const auto& [path, name] : paths) {
        cleaned += CleanFolder(path, name);
    }

    Status(L"刷新 DNS 缓存...");
    RunProcess(L"ipconfig.exe", {L"/flushdns"}, 10000);
    Log(L"  DNS 缓存: 已刷新");

    return cleaned;
}

std::uint64_t DiskCleanerCore::CleanCrashDumps() {
    Status(L"清理崩溃转储...");
    return CleanFolder(GetEnvVar(L"LOCALAPPDATA") + LR"(\CrashDumps)", L"应用 CrashDumps");
}

std::uint64_t DiskCleanerCore::CleanSetupResidue() {
    Status(L"清理安装残留...");
    return CleanFolder(SystemPath(L"\\Panther"), L"Windows Panther 日志") +
        CleanFolder(SystemPath(L"\\Logs\\MoSetup"), L"MoSetup 日志") +
        CleanFolder(DrivePath(L"\\$WinREAgent"), L"Windows 安装残留");
}

std::uint64_t DiskCleanerCore::CleanFontCache() {
    Status(L"清理字体缓存...");
    return CleanFolder(SystemPath(L"\\ServiceProfiles\\LocalService\\AppData\\Local\\FontCache"), L"字体缓存");
}

std::uint64_t DiskCleanerCore::CleanInstallerCache() {
    Status(L"清理安装缓存...");
    return CleanFolder(SystemPath(L"\\Installer\\$PatchCache$"), L"安装补丁缓存");
}

std::uint64_t DiskCleanerCore::CleanWindowsUpdate() {
    Status(L"清理 Windows Update 缓存...");
    const std::vector<std::wstring> services = {L"wuauserv", L"bits", L"cryptsvc"};
    std::vector<std::wstring> stopped_services;
    for (const auto& service : services) {
        if (QueryServiceRunning(service)) {
            Log(L"  停止服务: " + service);
            if (StopServiceByName(service)) {
                stopped_services.push_back(service);
            }
        }
    }

    const std::uint64_t cleaned = CleanFolder(SystemPath(L"\\SoftwareDistribution\\Download"), L"更新下载缓存") +
        CleanFolder(SystemPath(L"\\SoftwareDistribution\\DataStore\\Logs"), L"更新数据日志");

    for (auto it = stopped_services.rbegin(); it != stopped_services.rend(); ++it) {
        Log(L"  启动服务: " + *it);
        StartServiceByName(*it);
    }

    return cleaned;
}

std::uint64_t DiskCleanerCore::CleanWindowsOld() {
    Status(L"清理 Windows.old...");
    const fs::path old_path = DrivePath(L"\\Windows.old");
    std::error_code ec;
    if (!fs::exists(old_path, ec)) {
        Log(L"  Windows.old: 不存在");
        return 0;
    }

    const auto size = GetPathSize(old_path);
    Log(L"  发现 Windows.old: " + FormatSize(size));

    RunProcess(L"takeown.exe", {L"/F", old_path.wstring(), L"/R", L"/A", L"/D", L"Y"}, 300000);
    RunProcess(L"icacls.exe", {old_path.wstring(), L"/grant", L"*S-1-5-32-544:F", L"/T"}, 300000);
    RunProcess(L"cmd.exe", {L"/C", L"rd", L"/s", L"/q", old_path.wstring()}, 600000);

    if (!fs::exists(old_path, ec)) {
        total_cleaned_ += size;
        Log(L"  Windows.old: 已删除 " + FormatSize(size));
        return size;
    }

    Log(L"  Windows.old: 部分删除");
    return 0;
}

std::uint64_t DiskCleanerCore::DisableHibernation() {
    Status(L"禁用休眠...");
    const auto size = GetSize(DrivePath(L"\\hiberfil.sys"));
    if (size > 0) {
        Log(L"  休眠文件大小: " + FormatSize(size));
    }

    const auto result = RunProcess(L"powercfg.exe", {L"-h", L"off"}, 30000);
    if (!result.timed_out && result.exit_code == 0) {
        if (size > 0) {
            total_cleaned_ += size;
        }
        Log(L"  休眠: 已禁用");
        return size;
    }

    Log(L"  休眠: " + CommandText(result.output));
    return 0;
}

bool DiskCleanerCore::CleanRestorePoints() {
    Status(L"清理系统还原点...");
    const auto list_result = RunProcess(L"vssadmin.exe", {L"list", L"shadows"}, 60000);
    const auto output = CommandText(list_result.output);
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = output.find(L"Shadow Copy ID", pos)) != std::wstring::npos) {
        ++count;
        pos += 14;
    }

    if (count == 0) {
        Log(L"  还原点: 无还原点");
        return false;
    }

    Log(L"  发现 " + std::to_wstring(count) + L" 个还原点");
    const auto delete_result = RunProcess(L"vssadmin.exe",
        {L"delete", L"shadows", L"/for=" + SystemDrive(), L"/oldest", L"/quiet"}, 120000);
    if (!delete_result.timed_out && delete_result.exit_code == 0) {
        Log(L"  还原点: 已清理旧还原点");
        return true;
    }

    Log(L"  还原点: 清理失败");
    return false;
}

std::uint64_t DiskCleanerCore::CleanMemoryDumps() {
    Status(L"清理内存转储文件...");
    const std::vector<std::pair<std::wstring, std::wstring>> dump_locations = {
        {SystemPath(L"\\MEMORY.DMP"), L"系统转储"},
        {SystemPath(L"\\Minidump"), L"小型转储"},
        {SystemPath(L"\\LiveKernelReports"), L"内核报告"},
    };

    std::uint64_t cleaned = 0;
    for (const auto& [path, desc] : dump_locations) {
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            continue;
        }
        const auto size_before = GetPathSize(path);
        if (RemovePath(path)) {
            cleaned += size_before;
            Log(L"  " + desc + L": " + FormatSize(size_before));
        } else {
            const auto size_after = GetPathSize(path);
            if (size_before > size_after) {
                const auto diff = size_before - size_after;
                cleaned += diff;
                Log(L"  " + desc + L": " + FormatSize(diff) + L" (部分清理)");
            } else {
                Log(L"  " + desc + L": 清理失败");
            }
        }
    }
    total_cleaned_ += cleaned;
    return cleaned;
}

std::uint64_t DiskCleanerCore::CleanDeliveryOptimization() {
    Status(L"清理传递优化...");
    return CleanFolder(SystemPath(L"\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\DeliveryOptimization"), L"传递优化缓存");
}

int DiskCleanerCore::CleanEventLogs() {
    Status(L"清理事件日志...");
    const std::vector<std::wstring> logs = {L"System", L"Application", L"Security", L"Setup"};
    int cleared = 0;
    for (const auto& log : logs) {
        const auto result = RunProcess(L"wevtutil.exe", {L"cl", log}, 30000);
        if (!result.timed_out && result.exit_code == 0) {
            ++cleared;
        }
    }
    if (cleared > 0) {
        Log(L"  事件日志: 清理了 " + std::to_wstring(cleared) + L" 个日志");
    }
    return cleared;
}

std::uint64_t DiskCleanerCore::CleanDirectXCache() {
    Status(L"清理 DirectX 缓存...");
    return CleanFolder(GetEnvVar(L"LOCALAPPDATA") + LR"(\D3DSCache)", L"DirectX 缓存");
}

bool DiskCleanerCore::RunCleanAction(const std::wstring& key) {
    if (key == L"temp") {
        CleanWindowsTemp();
        CleanUserTemp();
        return true;
    }
    if (key == L"recycle") {
        return CleanRecycleBin();
    }
    if (key == L"prefetch") {
        CleanPrefetch();
        return true;
    }
    if (key == L"thumb") {
        CleanThumbnailCache();
        return true;
    }
    if (key == L"wincache") {
        CleanWindowsCache();
        return true;
    }
    if (key == L"appcache") {
        CleanApplicationCache();
        return true;
    }
    if (key == L"browser") {
        CleanBrowserCache();
        return true;
    }
    if (key == L"logs") {
        CleanWindowsLogs();
        return true;
    }
    if (key == L"recent") {
        CleanRecent();
        return true;
    }
    if (key == L"error") {
        CleanErrorReports();
        return true;
    }
    if (key == L"update") {
        CleanWindowsUpdate();
        return true;
    }
    if (key == L"hibernate") {
        DisableHibernation();
        return true;
    }
    if (key == L"restore") {
        return CleanRestorePoints();
    }
    if (key == L"oldwin") {
        CleanWindowsOld();
        return true;
    }
    if (key == L"dumps") {
        CleanMemoryDumps();
        return true;
    }
    if (key == L"delivery") {
        CleanDeliveryOptimization();
        return true;
    }
    if (key == L"events") {
        CleanEventLogs();
        return true;
    }
    if (key == L"directx") {
        CleanDirectXCache();
        return true;
    }
    if (key == L"gpu") {
        CleanGpuCache();
        return true;
    }
    if (key == L"inet") {
        CleanInternetCache();
        return true;
    }
    if (key == L"crashdumps") {
        CleanCrashDumps();
        return true;
    }
    if (key == L"setup") {
        CleanSetupResidue();
        return true;
    }
    if (key == L"font") {
        CleanFontCache();
        return true;
    }
    if (key == L"installer") {
        CleanInstallerCache();
        return true;
    }

    const auto* item = FindItem(key);
    if (item) {
        Log(L"  未实现项目: " + item->name);
    }
    return false;
}
