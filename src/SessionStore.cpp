#include "SessionStore.h"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <climits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace sessions {
namespace {
class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(Handle&& other) noexcept : value_(other.value_) { other.value_ = INVALID_HANDLE_VALUE; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); value_ = other.value_; other.value_ = INVALID_HANDLE_VALUE; }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};
bool IsCancelled(const Cancelled& cancelled) { return cancelled && cancelled(); }
bool Missing(DWORD code) { return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND; }
std::wstring Error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = length ? std::wstring(buffer, length) : L"未知系统错误";
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text + L"（错误 " + std::to_wstring(code) + L"）";
}
std::wstring Native(const fs::path& path) {
    const auto value = path.wstring();
    if (value.compare(0, 4, L"\\\\?\\") == 0) return value;
    if (value.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}
bool Same(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
std::wstring WithoutTrailingSeparators(std::wstring value) {
    while (!value.empty() && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
    return value;
}
bool Jsonl(const fs::path& path) { return Same(path.extension().wstring(), L".jsonl"); }
fs::path Normalize(const fs::path& path) {
    fs::path normalized = path.lexically_normal();
    normalized.make_preferred();
    while (normalized.has_relative_path() && normalized.filename().empty()) normalized = normalized.parent_path();
    return normalized;
}
bool SafeInput(const fs::path& path) {
    if (!path.is_absolute()) return false;
    const auto text = path.wstring();
    if (text.compare(0, 4, L"\\\\?\\") == 0 || text.compare(0, 4, L"\\\\.\\") == 0) return false;
    for (const auto& part : path.relative_path()) {
        const auto name = part.wstring();
        if (name == L".." || name.find(L':') != std::wstring::npos || (!name.empty() && (name.back() == L'.' || name.back() == L' '))) return false;
    }
    return true;
}
std::wstring Final(HANDLE handle) {
    const DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!needed) return {};
    std::wstring value(needed, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(handle, value.data(), needed, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= needed) return {};
    value.resize(length);
    return value;
}
// These handles prevent renames. Attribute writes can still change reparse
// metadata, so callers must also verify the final paths against the root anchor.
bool LockDirectories(const fs::path& directory, std::vector<Handle>& locks, DWORD& code, std::wstring& error) {
    fs::path current = directory.root_path();
    auto open = [&](const fs::path& part) {
        Handle handle(CreateFileW(Native(part).c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!handle) { code = GetLastError(); error = L"无法检查目录：" + Error(code); return false; }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle.get(), &info)) { code = GetLastError(); error = L"无法读取目录属性：" + Error(code); return false; }
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            code = ERROR_ACCESS_DENIED; error = L"目录路径包含链接或不是普通目录，已拒绝操作。"; return false;
        }
        locks.push_back(std::move(handle));
        return true;
    };
    if (!open(current)) return false;
    for (const auto& part : directory.relative_path()) {
        if (part.empty() || part == L".") continue;
        current /= part;
        if (!open(current)) return false;
    }
    return true;
}
bool VerifyDirectories(const fs::path& directory, const std::vector<Handle>& locks, std::wstring& error) {
    fs::path current = directory.root_path();
    std::size_t index = 0;
    auto verify = [&]() {
        if (index >= locks.size()) { error = L"目录链核验失败。"; return false; }
        const HANDLE handle = locks[index++].get();
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info)) { error = L"无法重新核对目录：" + Error(GetLastError()); return false; }
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            error = L"目录属性已变化或包含链接，已拒绝操作。"; return false;
        }
        const auto actual = WithoutTrailingSeparators(Final(handle));
        if (actual.empty() || !Same(actual, WithoutTrailingSeparators(Native(current)))) {
            error = L"目录的实际路径与规范路径不一致，已拒绝操作。"; return false;
        }
        return true;
    };
    if (!verify()) return false;
    for (const auto& part : directory.relative_path()) {
        if (part.empty() || part == L".") continue;
        current /= part;
        if (!verify()) return false;
    }
    return index == locks.size();
}
std::wstring Utf8(const std::string& text) {
    if (text.empty()) return {};
    if (text.size() > INT_MAX) throw std::runtime_error("text too large");
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw std::runtime_error("invalid UTF-8");
    std::wstring value(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), value.data(), size);
    return value;
}
std::wstring FilterPreviewText(const std::wstring& text) {
    struct Block { std::wstring_view open, close; };
    constexpr Block blocks[] = {
        { L"<INSTRUCTIONS>", L"</INSTRUCTIONS>" },
        { L"<environment_context>", L"</environment_context>" }
    };
    std::wstring visible;
    std::size_t position = 0;
    while (position < text.size()) {
        std::size_t start = std::wstring::npos;
        const Block* next = nullptr;
        for (const auto& block : blocks) {
            const auto found = text.find(block.open, position);
            if (found < start) { start = found; next = &block; }
        }
        if (!next) { visible.append(text, position); break; }
        visible.append(text, position, start - position);
        const auto end = text.find(next->close, start + next->open.size());
        // An unfinished block stays hidden through the end of this message.
        if (end == std::wstring::npos) break;
        position = end + next->close.size();
    }
    return visible;
}
struct CancelParsing {};

// Read only the header. Message bodies may be very large and are irrelevant to
// both picker filtering and the parent/child relationship.
bool ReadMetadata(HANDLE input, File& file, std::wstring& error, const Cancelled& cancelled = {}) {
    std::array<char, 4096> buffer{};
    std::string line;
    bool first = true;
    for (;;) {
        if (IsCancelled(cancelled)) throw CancelParsing{};
        DWORD count = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            error = L"无法读取会话元数据：" + Error(GetLastError()); return false;
        }
        for (DWORD i = 0; i < count; ++i) {
            if (buffer[i] != '\n') line += buffer[i];
            if (line.size() > 4 * 1024 * 1024) { error = L"会话元数据超过大小限制。"; return false; }
            if (buffer[i] != '\n' && i + 1 != count) continue;
            if (buffer[i] != '\n') continue;
            if (first) { first = false; if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3); }
            if (line.find_first_not_of(" \r\t") != std::string::npos) goto parse;
            line.clear();
        }
        if (!count) {
            if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
            break;
        }
    }
parse:
    const auto record = nlohmann::json::parse(line, nullptr, false);
    if (!record.is_object() || !record.contains("type") || record["type"] != "session_meta"
        || !record.contains("payload") || !record["payload"].is_object()) {
        error = L"缺少有效的 session_meta 会话元数据。"; return false;
    }
    const auto& meta = record["payload"];
    if (!meta.contains("id") || !meta["id"].is_string() || meta["id"].get_ref<const std::string&>().empty()) {
        error = L"会话元数据缺少有效 ID。"; return false;
    }
    file.sessionId = meta["id"].get<std::string>();
    if (!meta.contains("source")) { error = L"会话元数据缺少来源。"; return false; }
    const auto& source = meta["source"];
    file.interactive = source == "cli" || source == "vscode";
    file.subagent = source == "subagent" || (source.is_object() && source.contains("subagent"));
    if (source.is_object() && source.contains("subagent") && source["subagent"].is_object()) {
        const auto& agent = source["subagent"];
        if (agent.contains("thread_spawn")) {
            const auto& spawn = agent["thread_spawn"];
            if (!spawn.is_object() || !spawn.contains("parent_thread_id") || !spawn["parent_thread_id"].is_string()
                || spawn["parent_thread_id"].get_ref<const std::string&>().empty()) {
                error = L"子 Agent 会话缺少有效的父会话 ID。"; return false;
            }
            file.parentId = spawn["parent_thread_id"].get<std::string>();
        }
    }
    return true;
}

bool ReadMetadata(const fs::path& path, File& file, std::wstring& error, const Cancelled& cancelled) {
    std::vector<Handle> locks;
    DWORD code = 0;
    if (!LockDirectories(path.parent_path(), locks, code, error) || !VerifyDirectories(path.parent_path(), locks, error)) return false;
    Handle input(CreateFileW(Native(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!input) { error = L"无法打开会话元数据：" + Error(GetLastError()); return false; }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(input.get(), &info)
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        || !Same(Final(input.get()), Native(path))) {
        error = L"无法核对会话文件的实际路径或属性。"; return false;
    }
    return VerifyDirectories(path.parent_path(), locks, error) && ReadMetadata(input.get(), file, error, cancelled);
}
}

fs::path DefaultRoot() {
    const DWORD needed = GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
    if (!needed) return {};
    std::wstring profile(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile.data(), needed);
    if (!length || length >= needed) return {};
    profile.resize(length);
    return fs::path(profile) / L".codex" / L"sessions";
}

static ScanResult ScanFiles(const fs::path& root, const Cancelled& cancelled) {
    ScanResult result;
    try {
        if (!SafeInput(root)) { result.error = L"扫描目录必须是有效的绝对路径。"; return result; }
        const fs::path base = Normalize(root);
        std::vector<Handle> rootLocks;
        DWORD code = 0;
        if (!LockDirectories(base, rootLocks, code, result.error)) {
            if (Missing(code)) result.error = L"会话目录不存在：" + base.wstring();
            return result;
        }
        if (!VerifyDirectories(base, rootLocks, result.error)) return result;
        std::function<void(const fs::path&)> visit = [&](const fs::path& directory) {
            if (IsCancelled(cancelled)) return;
            WIN32_FIND_DATAW data{};
            const HANDLE search = FindFirstFileW(Native(directory / L"*").c_str(), &data);
            if (search == INVALID_HANDLE_VALUE) {
                const DWORD findCode = GetLastError();
                if (findCode != ERROR_FILE_NOT_FOUND) { ++result.skipped; if (result.error.empty()) result.error = L"部分目录读取失败：" + Error(findCode); }
                return;
            }
            struct FindCloser { HANDLE h; ~FindCloser() { FindClose(h); } } closer{search};
            do {
                if (IsCancelled(cancelled)) return;
                if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
                const fs::path path = directory / data.cFileName;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ++result.skipped; continue; }
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    std::vector<Handle> directoryLocks;
                    std::wstring directoryError;
                    DWORD directoryCode = 0;
                    if (LockDirectories(path, directoryLocks, directoryCode, directoryError) && VerifyDirectories(path, directoryLocks, directoryError)) visit(path);
                    else { ++result.skipped; if (result.error.empty()) result.error = directoryError; }
                } else if (Jsonl(path)) {
                    const std::uint64_t time = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
                    File file;
                    file.path = path;
                    file.relativePath = path.lexically_relative(base);
                    file.modified = time;
                    std::wstring metadataError;
                    if (ReadMetadata(path, file, metadataError, cancelled)
                        && VerifyDirectories(base, rootLocks, metadataError)) {
                        result.files.push_back(std::move(file));
                    } else {
                        ++result.skipped;
                        if (result.error.empty()) result.error = path.wstring() + L"：" + metadataError;
                    }
                }
            } while (FindNextFileW(search, &data));
            const DWORD last = GetLastError();
            if (last != ERROR_NO_MORE_FILES) { ++result.skipped; if (result.error.empty()) result.error = L"目录扫描未完成：" + Error(last); }
        };
        visit(base);
        if (IsCancelled(cancelled)) { result.files.clear(); return result; }
        std::sort(result.files.begin(), result.files.end(), [](const File& a, const File& b) {
            return a.modified != b.modified ? a.modified > b.modified : a.relativePath.native() < b.relativePath.native();
        });
    } catch (const CancelParsing&) { return {}; }
      catch (const std::exception&) { result.error = L"扫描失败：路径无效或可用内存不足。"; }
    return result;
}

ScanResult Scan(const fs::path& root, const Cancelled& cancelled) {
    auto result = ScanFiles(root, cancelled);
    result.files.erase(std::remove_if(result.files.begin(), result.files.end(), [](const File& file) {
        return file.subagent || !file.interactive;
    }), result.files.end());
    return result;
}

PreviewResult Preview(const fs::path& path, const Cancelled& cancelled) {
    PreviewResult result;
    if (IsCancelled(cancelled)) return result;
    try {
        if (!SafeInput(path)) { result.error = L"无法读取：文件路径无效。"; return result; }
        std::vector<Handle> directoryLocks;
        DWORD directoryCode = 0;
        if (!LockDirectories(Normalize(path).parent_path(), directoryLocks, directoryCode, result.error)) {
            result.error = L"无法读取文件：" + result.error;
            return result;
        }
        if (!VerifyDirectories(Normalize(path).parent_path(), directoryLocks, result.error)) return result;
        Handle input(CreateFileW(Native(path.lexically_normal()).c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!input) { result.error = L"无法读取文件：" + Error(GetLastError()); return result; }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(input.get(), &info)) { result.error = L"无法检查文件：" + Error(GetLastError()); return result; }
        if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) { result.error = L"无法读取：所选路径不是普通文件。"; return result; }
        if (!Same(Final(input.get()), Native(Normalize(path))) || !VerifyDirectories(Normalize(path).parent_path(), directoryLocks, result.error)) {
            if (result.error.empty()) result.error = L"文件的实际路径已变化，无法预览。";
            return result;
        }
        bool firstLine = true;
        auto parseLine = [&](std::string& line) {
            if (IsCancelled(cancelled)) throw CancelParsing{};
            if (firstLine) { firstLine = false; if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3); }
            if (line.find_first_not_of(" \r\n\t") == std::string::npos) return;
            try {
                const auto record = nlohmann::json::parse(line, [&](int, nlohmann::json::parse_event_t, nlohmann::json&) {
                    if (IsCancelled(cancelled)) throw CancelParsing{};
                    return true;
                });
                if (!record.is_object() || !record.contains("type") || record["type"] != "response_item" || !record.contains("payload")) return;
                const auto& message = record["payload"];
                if (!message.is_object() || !message.contains("type") || message["type"] != "message" || !message.contains("role")) return;
                const bool user = message["role"] == "user";
                if (!user && message["role"] != "assistant") return;
                if (!message.contains("content") || !message["content"].is_array()) return;
                std::wstring text;
                for (const auto& part : message["content"]) {
                    if (!part.is_object() || !part.contains("type") || !part.contains("text") || !part["text"].is_string()) continue;
                    if (part["type"] != "input_text" && part["type"] != "output_text" && part["type"] != "text") continue;
                    text += Utf8(part["text"].get_ref<const std::string&>());
                }
                text = FilterPreviewText(text);
                if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) return;
                if (result.messages) result.text += L"\r\n\r\n";
                result.text += user ? L"【用户】\r\n" : L"【助手】\r\n";
                // The Windows edit control requires CRLF for multiline display.
                for (std::size_t i = 0; i < text.size(); ++i) {
                    if (text[i] == L'\r') { result.text += L"\r\n"; if (i + 1 < text.size() && text[i + 1] == L'\n') ++i; }
                    else if (text[i] == L'\n') result.text += L"\r\n";
                    else if (text[i] == L'\0') result.text += L"\uFFFD";
                    else result.text += text[i];
                }
                ++result.messages;
            } catch (const nlohmann::json::exception&) { ++result.damagedLines; }
              catch (const std::runtime_error&) { ++result.damagedLines; }
        };
        std::array<char, 64 * 1024> buffer{};
        std::string line;
        while (result.messages < 5) {
            if (IsCancelled(cancelled)) throw CancelParsing{};
            DWORD count = 0;
            if (!ReadFile(input.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
                result.error = L"读取中断：" + Error(GetLastError()); return result;
            }
            if (!count) { if (!line.empty()) parseLine(line); break; }
            std::size_t start = 0;
            for (std::size_t i = 0; i < count; ++i) {
                if (buffer[i] != '\n') continue;
                line.append(buffer.data() + start, i - start);
                parseLine(line); line.clear(); start = i + 1;
                if (result.messages == 5) break;
            }
            if (result.messages < 5) line.append(buffer.data() + start, count - start);
        }
        if (!result.messages) result.text = L"没有可预览的用户或助手文字消息。";
    } catch (const CancelParsing&) { return {}; }
      catch (const std::exception&) { result.error = L"预览失败：文件过大、路径无效或可用内存不足。"; }
    return result;
}

static DeleteResult DeleteFile(const fs::path& root, const fs::path& path, const File& expected) {
    try {
        if (!SafeInput(root) || !SafeInput(path) || !Jsonl(path)) return {DeleteStatus::Failed, L"已拒绝删除：路径无效或扩展名不是 .jsonl。"};
        const fs::path base = Normalize(root);
        const fs::path target = Normalize(path);
        auto basePart = base.begin();
        auto targetPart = target.begin();
        for (; basePart != base.end(); ++basePart, ++targetPart) {
            if (targetPart == target.end() || !Same(basePart->wstring(), targetPart->wstring())) return {DeleteStatus::Failed, L"已拒绝删除：文件不在扫描目录内。"};
        }
        if (targetPart == target.end()) return {DeleteStatus::Failed, L"已拒绝删除：目标不能是扫描目录。"};
        // Build from the checked root spelling, rather than trusting a caller's
        // differently cased prefix on a case-sensitive Windows directory.
        fs::path checkedTarget = base;
        for (; targetPart != target.end(); ++targetPart) checkedTarget /= *targetPart;
        std::vector<Handle> locks;
        std::vector<Handle> rootLocks;
        std::wstring error;
        DWORD code = 0;
        if (!LockDirectories(base, rootLocks, code, error)) return {Missing(code) ? DeleteStatus::Missing : DeleteStatus::Failed, error};
        if (!VerifyDirectories(base, rootLocks, error)) return {DeleteStatus::Failed, error};
        const std::wstring expectedRoot = WithoutTrailingSeparators(Final(rootLocks.back().get()));
        if (expectedRoot.empty()) return {DeleteStatus::Failed, L"无法核对扫描目录的实际路径。"};
        if (!LockDirectories(checkedTarget.parent_path(), locks, code, error)) return {Missing(code) ? DeleteStatus::Missing : DeleteStatus::Failed, error};
        Handle file(CreateFileW(Native(checkedTarget).c_str(), DELETE | GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!file) {
            code = GetLastError();
            return {Missing(code) ? DeleteStatus::Missing : DeleteStatus::Failed, L"删除失败：" + Error(code)};
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file.get(), &info)) return {DeleteStatus::Failed, L"无法检查文件：" + Error(GetLastError())};
        if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return {DeleteStatus::Failed, L"已拒绝删除：目标是目录或链接。"};
        const auto finalFile = Final(file.get());
        const auto relative = checkedTarget.lexically_relative(base).wstring();
        if (finalFile.empty() || !Same(finalFile, expectedRoot + L"\\" + relative)) {
            return {DeleteStatus::Failed, L"已拒绝删除：文件的实际路径与核对结果不一致。"};
        }
        if (!VerifyDirectories(base, rootLocks, error) || !VerifyDirectories(checkedTarget.parent_path(), locks, error)
            || !Same(expectedRoot, WithoutTrailingSeparators(Final(rootLocks.back().get())))) return {DeleteStatus::Failed, L"已拒绝删除：扫描目录已变化。" + error};
        File metadata;
        if (!ReadMetadata(file.get(), metadata, error) || metadata.sessionId != expected.sessionId
            || metadata.parentId != expected.parentId || metadata.subagent != expected.subagent
            || metadata.interactive != expected.interactive) {
            return {DeleteStatus::Failed, L"会话元数据已变化，已停止删除。" + error};
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition))) return {DeleteStatus::Failed, L"删除失败：" + Error(GetLastError())};
        return {DeleteStatus::Deleted, {}, 1};
    } catch (const std::exception&) { return {DeleteStatus::Failed, L"删除失败：路径无效或可用内存不足。"}; }
}

DeleteResult Delete(const fs::path& root, const fs::path& path, bool confirmed, const std::string& sessionId) {
    if (!confirmed) return {DeleteStatus::Cancelled, {}};
    try {
        if (!SafeInput(root) || !SafeInput(path) || !Jsonl(path)) return {DeleteStatus::Failed, L"已拒绝删除：路径无效或扩展名不是 .jsonl。"};
        const auto base = Normalize(root);
        const auto target = Normalize(path);
        const auto relative = target.lexically_relative(base);
        if (relative.empty() || relative == L"." || !SafeInput(base / relative)) return {DeleteStatus::Failed, L"已拒绝删除：文件不在扫描目录内。"};
        auto listing = ScanFiles(base, {});
        if (!listing.error.empty()) return {DeleteStatus::Failed, L"无法完整确认子 Agent 会话，未执行删除。" + listing.error};
        // IDs only establish relationships. Every deletion still uses a scanned
        // path and repeats the existing handle-based containment checks.
        std::unordered_map<std::string, std::size_t> ids;
        std::unordered_multimap<std::string, std::size_t> children;
        std::size_t mainIndex = listing.files.size();
        for (std::size_t i = 0; i < listing.files.size(); ++i) {
            const auto& file = listing.files[i];
            if (!ids.emplace(file.sessionId, i).second) return {DeleteStatus::Failed, L"存在重复会话 ID，无法安全确认归属，未执行删除。"};
            if (file.subagent && !file.parentId.empty()) children.emplace(file.parentId, i);
            if (Same(file.path.wstring(), target.wstring())) mainIndex = i;
        }
        std::string mainId = sessionId;
        if (mainIndex != listing.files.size()) {
            const auto& main = listing.files[mainIndex];
            if (main.subagent || !main.interactive) return {DeleteStatus::Failed, L"只能从交互主会话发起删除。"};
            if (!mainId.empty() && mainId != main.sessionId) return {DeleteStatus::Failed, L"选中的主会话 ID 已变化，请刷新后重试。"};
            mainId = main.sessionId;
        } else {
            const DWORD attributes = GetFileAttributesW(Native(target).c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES || !Missing(GetLastError())) return {DeleteStatus::Failed, L"无法确认选中的主会话文件，请刷新后重试。"};
            if (mainId.empty()) return {DeleteStatus::Missing, {}};
            if (ids.count(mainId)) return {DeleteStatus::Failed, L"主会话 ID 对应了其他文件，请刷新后重试。"};
        }
        std::vector<std::size_t> order;
        std::unordered_set<std::string> visited{mainId};
        std::vector<std::string> pending{mainId};
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto range = children.equal_range(pending[i]);
            for (auto it = range.first; it != range.second; ++it) {
                const auto index = it->second;
                const auto& file = listing.files[index];
                if (!visited.insert(file.sessionId).second) return {DeleteStatus::Failed, L"子 Agent 归属存在循环，未执行删除。"};
                pending.push_back(file.sessionId);
                order.push_back(index);
            }
        }
        // Reverse breadth-first order deletes descendants before their parents.
        std::reverse(order.begin(), order.end());
        if (mainIndex != listing.files.size()) order.push_back(mainIndex);
        std::size_t deleted = 0;
        for (const auto index : order) {
            const auto& file = listing.files[index];
            auto result = DeleteFile(base, file.path, file);
            if (result.status != DeleteStatus::Deleted && result.status != DeleteStatus::Missing) {
                result.error = L"已删除 " + std::to_wstring(deleted) + L" 个文件；其余文件未删除，可重试。\r\n"
                    + file.path.wstring() + L"\r\n" + result.error;
                result.deletedFiles = deleted;
                return result;
            }
            deleted += result.deletedFiles;
        }
        return {deleted ? DeleteStatus::Deleted : DeleteStatus::Missing, {}, deleted};
    } catch (const std::exception&) { return {DeleteStatus::Failed, L"删除失败：路径无效或可用内存不足，请刷新后检查。"}; }
}
}
