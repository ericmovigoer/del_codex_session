#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace sessions {
namespace fs = std::filesystem;
using Cancelled = std::function<bool()>;
struct File {
    fs::path path;
    fs::path relativePath;
    std::uint64_t modified = 0;
    std::string sessionId;
    std::string parentId;
    bool subagent = false;
    bool interactive = false;
};
struct ScanResult {
    std::vector<File> files;
    std::wstring error;
    std::size_t skipped = 0;
};
struct PreviewResult {
    std::wstring text;
    std::wstring error;
    std::size_t messages = 0;
    std::size_t damagedLines = 0;
};
enum class DeleteStatus { Deleted, Missing, Failed, Cancelled };
struct DeleteResult {
    DeleteStatus status = DeleteStatus::Failed;
    std::wstring error;
    std::size_t deletedFiles = 0;
};
fs::path DefaultRoot();
ScanResult Scan(const fs::path& root, const Cancelled& cancelled = {});
PreviewResult Preview(const fs::path& path, const Cancelled& cancelled = {});
// confirmed=false must never touch the filesystem.
// sessionId is the ID captured by Scan, also used if the main file disappeared.
DeleteResult Delete(const fs::path& root, const fs::path& path, bool confirmed, const std::string& sessionId = {});
}
