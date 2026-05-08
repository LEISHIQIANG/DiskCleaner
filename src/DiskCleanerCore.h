#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct CleanItemDefinition {
    std::wstring key;
    std::wstring name;
    std::wstring description;
    bool cautious = false;
};

class DiskCleanerCore {
public:
    using LogCallback = std::function<void(const std::wstring&)>;
    using StatusCallback = std::function<void(const std::wstring&)>;

    explicit DiskCleanerCore(LogCallback log_callback = {}, StatusCallback status_callback = {});

    static const std::vector<CleanItemDefinition>& AllItems();
    static std::vector<std::wstring> DefaultSelection();

    std::wstring FormatSize(std::uint64_t size) const;
    std::uint64_t ScanItem(const std::wstring& key);
    bool RunCleanAction(const std::wstring& key);

    std::uint64_t total_cleaned() const noexcept { return total_cleaned_; }
    const std::vector<std::wstring>& errors() const noexcept { return errors_; }

private:
    void Log(const std::wstring& message) const;
    void Status(const std::wstring& message) const;

    std::uint64_t GetSize(const std::wstring& path) const;
    bool CheckPathAccessible(const std::wstring& path) const;
    std::uint64_t ScanFolder(const std::wstring& path, const std::wstring& file_pattern = L"", int min_age_days = 0) const;
    std::uint64_t CleanFolder(const std::wstring& path, const std::wstring& desc,
        const std::wstring& file_pattern = L"", int min_age_days = 0);

    std::uint64_t GetRecycleSize() const;
    std::uint64_t GetRestoreSize() const;
    std::uint64_t ScanBrowserCacheSize() const;

    std::uint64_t CleanWindowsTemp();
    std::uint64_t CleanUserTemp();
    std::uint64_t CleanRecent();
    bool CleanRecycleBin();
    std::uint64_t CleanPrefetch();
    std::uint64_t CleanThumbnailCache();
    std::uint64_t CleanWindowsLogs();
    std::uint64_t CleanErrorReports();
    std::uint64_t CleanBrowserCache();
    std::uint64_t CleanFontCache();
    std::uint64_t CleanInstallerCache();
    std::uint64_t CleanWindowsUpdate();
    std::uint64_t CleanWindowsOld();
    std::uint64_t DisableHibernation();
    bool CleanRestorePoints();
    std::uint64_t CleanMemoryDumps();
    std::uint64_t CleanDeliveryOptimization();
    int CleanEventLogs();
    std::uint64_t CleanDirectXCache();

    LogCallback log_callback_;
    StatusCallback status_callback_;
    std::uint64_t total_cleaned_ = 0;
    std::vector<std::wstring> errors_;
};
