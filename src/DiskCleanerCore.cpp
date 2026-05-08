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
        {L"prefetch", L"预读取文件", L"清理系统预读缓存（保留 7 天内）", false},
        {L"thumb", L"缩略图缓存", L"清理图片和图标缓存文件", false},
        {L"browser", L"浏览器缓存", L"清理 Chrome / Edge / Firefox 缓存", false},
        {L"logs", L"系统日志", L"清理 Windows 日志文件", false},
        {L"recent", L"最近文档", L"清理文件访问历史记录", false},
        {L"error", L"错误报告", L"清理 Windows 错误报告文件", false},
        {L"directx", L"DirectX 缓存", L"清理显卡着色器缓存", false},
        {L"font", L"字体缓存", L"清理系统字体缓存文件", false},
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
    return {L"temp", L"recycle", L"prefetch", L"thumb", L"browser"};
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

        if (!IsOlderThan(it->path(), min_age_days)) {
            continue;
        }
        if (!MatchesPattern(it->path().filename().wstring(), file_pattern)) {
            continue;
        }

        std::error_code item_ec;
        if (it->is_regular_file(item_ec)) {
            total += static_cast<std::uint64_t>(it->file_size(item_ec));
        } else if (it->is_directory(item_ec)) {
            total += GetPathSize(it->path());
        }
    }
    return total;
}

std::uint64_t DiskCleanerCore::CleanFolder(const std::wstring& path, const std::wstring& desc,
    const std::wstring& file_pattern, int min_age_days) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return 0;
    }
    if (!CheckPathAccessible(path)) {
        Log(L"  " + desc + L": 无访问权限");
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

    if (cleaned > 0) {
        std::wstring message = L"  " + desc + L": " + FormatSize(cleaned);
        if (failed > 0) {
            message += L" (跳过 " + std::to_wstring(failed) + L" 项)";
        }
        Log(message);
    }

    total_cleaned_ += cleaned;
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
    const auto result = RunProcess(L"vssadmin.exe", {L"list", L"shadowstorage", L"/for=C:"}, 10000);
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
        L"Cache", L"Code Cache", L"GPUCache", L"Service Worker", L"cache2", L"CacheStorage"
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

std::uint64_t DiskCleanerCore::ScanItem(const std::wstring& key) {
    if (key == L"temp") {
        return ScanFolder(LR"(C:\Windows\Temp)") +
            ScanFolder(GetEnvVar(L"TEMP").empty() ? GetEnvVar(L"TMP") : GetEnvVar(L"TEMP"));
    }
    if (key == L"recycle") {
        return GetRecycleSize();
    }
    if (key == L"prefetch") {
        return ScanFolder(LR"(C:\Windows\Prefetch)", L"*.pf", 7);
    }
    if (key == L"thumb") {
        return ScanFolder(GetEnvVar(L"LOCALAPPDATA") + LR"(\Microsoft\Windows\Explorer)");
    }
    if (key == L"browser") {
        return ScanBrowserCacheSize();
    }
    if (key == L"logs") {
        return ScanFolder(LR"(C:\Windows\Logs)", L"", 7) +
            ScanFolder(LR"(C:\Windows\Logs\CBS)", L"*.log", 7) +
            ScanFolder(LR"(C:\Windows\Logs\DISM)", L"", 7);
    }
    if (key == L"recent") {
        return ScanFolder(GetEnvVar(L"APPDATA") + LR"(\Microsoft\Windows\Recent)");
    }
    if (key == L"error") {
        return ScanFolder(GetEnvVar(L"LOCALAPPDATA") + LR"(\Microsoft\Windows\WER)") +
            ScanFolder(LR"(C:\ProgramData\Microsoft\Windows\WER)");
    }
    if (key == L"update") {
        return ScanFolder(LR"(C:\Windows\SoftwareDistribution\Download)");
    }
    if (key == L"hibernate") {
        return GetSize(LR"(C:\hiberfil.sys)");
    }
    if (key == L"restore") {
        return GetRestoreSize();
    }
    if (key == L"oldwin") {
        return GetSize(LR"(C:\Windows.old)");
    }
    if (key == L"dumps") {
        return GetSize(LR"(C:\Windows\MEMORY.DMP)") +
            GetSize(LR"(C:\Windows\Minidump)") +
            GetSize(LR"(C:\Windows\LiveKernelReports)");
    }
    if (key == L"delivery") {
        const auto system_root = GetEnvVar(L"SYSTEMROOT").empty() ? L"C:\\Windows" : GetEnvVar(L"SYSTEMROOT");
        return ScanFolder(system_root + LR"(\ServiceProfiles\NetworkService\AppData\Local\Microsoft\Windows\DeliveryOptimization)");
    }
    if (key == L"events") {
        return ScanFolder(LR"(C:\Windows\System32\winevt\Logs)");
    }
    if (key == L"directx") {
        return ScanFolder(GetEnvVar(L"LOCALAPPDATA") + LR"(\D3DSCache)");
    }
    if (key == L"font") {
        return ScanFolder(LR"(C:\Windows\ServiceProfiles\LocalService\AppData\Local\FontCache)");
    }
    if (key == L"installer") {
        const auto windir = GetEnvVar(L"WINDIR").empty() ? L"C:\\Windows" : GetEnvVar(L"WINDIR");
        return ScanFolder(windir + LR"(\Installer\$PatchCache$)");
    }
    return 0;
}

std::uint64_t DiskCleanerCore::CleanWindowsTemp() {
    Status(L"清理 Windows 临时文件...");
    return CleanFolder(LR"(C:\Windows\Temp)", L"Windows 临时文件");
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
    return CleanFolder(LR"(C:\Windows\Prefetch)", L"预读取文件", L"*.pf", 7);
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
    return CleanFolder(LR"(C:\Windows\Logs)", L"系统日志", L"", 7) +
        CleanFolder(LR"(C:\Windows\Logs\CBS)", L"CBS 日志", L"*.log", 7) +
        CleanFolder(LR"(C:\Windows\Logs\DISM)", L"DISM 日志", L"", 7);
}

std::uint64_t DiskCleanerCore::CleanErrorReports() {
    Status(L"清理错误报告...");
    return CleanFolder(GetEnvVar(L"LOCALAPPDATA") + LR"(\Microsoft\Windows\WER)", L"用户错误报告") +
        CleanFolder(LR"(C:\ProgramData\Microsoft\Windows\WER)", L"系统错误报告");
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
        L"Cache", L"Code Cache", L"GPUCache", L"Service Worker", L"cache2", L"CacheStorage"
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

std::uint64_t DiskCleanerCore::CleanFontCache() {
    Status(L"清理字体缓存...");
    return CleanFolder(LR"(C:\Windows\ServiceProfiles\LocalService\AppData\Local\FontCache)", L"字体缓存");
}

std::uint64_t DiskCleanerCore::CleanInstallerCache() {
    Status(L"清理安装缓存...");
    const auto windir = GetEnvVar(L"WINDIR").empty() ? L"C:\\Windows" : GetEnvVar(L"WINDIR");
    return CleanFolder(windir + LR"(\Installer\$PatchCache$)", L"安装补丁缓存");
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

    const std::uint64_t cleaned = CleanFolder(LR"(C:\Windows\SoftwareDistribution\Download)", L"更新下载缓存") +
        CleanFolder(LR"(C:\Windows\SoftwareDistribution\DataStore\Logs)", L"更新数据日志");

    for (auto it = stopped_services.rbegin(); it != stopped_services.rend(); ++it) {
        Log(L"  启动服务: " + *it);
        StartServiceByName(*it);
    }

    return cleaned;
}

std::uint64_t DiskCleanerCore::CleanWindowsOld() {
    Status(L"清理 Windows.old...");
    const fs::path old_path = LR"(C:\Windows.old)";
    std::error_code ec;
    if (!fs::exists(old_path, ec)) {
        Log(L"  Windows.old: 不存在");
        return 0;
    }

    const auto size = GetPathSize(old_path);
    Log(L"  发现 Windows.old: " + FormatSize(size));

    RunProcess(L"takeown.exe", {L"/F", old_path.wstring(), L"/R", L"/A", L"/D", L"Y"}, 300000);
    RunProcess(L"icacls.exe", {old_path.wstring(), L"/grant", L"Administrators:F", L"/T"}, 300000);
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
    const auto size = GetSize(LR"(C:\hiberfil.sys)");
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
        {L"delete", L"shadows", L"/for=C:", L"/oldest", L"/quiet"}, 120000);
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
        {LR"(C:\Windows\MEMORY.DMP)", L"系统转储"},
        {LR"(C:\Windows\Minidump)", L"小型转储"},
        {LR"(C:\Windows\LiveKernelReports)", L"内核报告"},
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
    const auto system_root = GetEnvVar(L"SYSTEMROOT").empty() ? L"C:\\Windows" : GetEnvVar(L"SYSTEMROOT");
    return CleanFolder(system_root + LR"(\ServiceProfiles\NetworkService\AppData\Local\Microsoft\Windows\DeliveryOptimization)", L"传递优化缓存");
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
