#include "SessionStore.h"
#include <Windows.h>
#include <winioctl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
namespace {
int checks = 0;
void Check(bool value, const char* name) {
    ++checks;
    if (!value) throw std::runtime_error(name);
}
void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    if (!stream) throw std::runtime_error("cannot write fixture");
}
std::string Message(const std::string& role, const std::string& text) {
    return json{{"type", "response_item"}, {"payload", {{"type", "message"}, {"role", role},
        {"content", json::array({{{"type", role == "user" ? "input_text" : "output_text"}, {"text", text}}})}}}}.dump() + "\n";
}
std::string Metadata(const std::string& id, const std::string& parent = {}, const std::string& source = "cli", const std::string& fork = {}) {
    json meta{{"id", id}, {"source", source}};
    if (!parent.empty()) meta["source"] = {{"subagent", {{"thread_spawn", {{"parent_thread_id", parent}, {"depth", 1}}}}}};
    if (!fork.empty()) meta["forked_from_id"] = fork;
    return json{{"type", "session_meta"}, {"payload", meta}}.dump() + "\n";
}
bool Junction(const fs::path& link, const fs::path& destination, bool create = true) {
    if (create && !CreateDirectoryW(link.c_str(), nullptr)) return false;
    const HANDLE directory = CreateFileW(link.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE) { if (create) RemoveDirectoryW(link.c_str()); return false; }
    const std::wstring substitute = L"\\??\\" + destination.wstring();
    const std::wstring print = destination.wstring();
    struct Header { DWORD tag; USHORT length; USHORT reserved; USHORT substituteOffset; USHORT substituteLength; USHORT printOffset; USHORT printLength; };
    const auto pathBytes = (substitute.size() + print.size() + 2) * sizeof(wchar_t);
    std::vector<unsigned char> buffer(sizeof(Header) + pathBytes, 0);
    auto* header = reinterpret_cast<Header*>(buffer.data());
    header->tag = IO_REPARSE_TAG_MOUNT_POINT;
    header->length = static_cast<USHORT>(8 + pathBytes);
    header->substituteLength = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
    header->printOffset = static_cast<USHORT>((substitute.size() + 1) * sizeof(wchar_t));
    header->printLength = static_cast<USHORT>(print.size() * sizeof(wchar_t));
    auto* text = reinterpret_cast<wchar_t*>(buffer.data() + sizeof(Header));
    std::copy(substitute.begin(), substitute.end(), text);
    std::copy(print.begin(), print.end(), text + substitute.size() + 1);
    DWORD returned = 0;
    const bool success = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(directory);
    if (!success && create) RemoveDirectoryW(link.c_str());
    return success;
}
bool ClearJunction(const fs::path& link) {
    const HANDLE directory = CreateFileW(link.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE) return false;
    struct Header { DWORD tag; USHORT length; USHORT reserved; } header{IO_REPARSE_TAG_MOUNT_POINT, 0, 0};
    DWORD returned = 0;
    const bool success = DeviceIoControl(directory, FSCTL_DELETE_REPARSE_POINT, &header, sizeof(header), nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(directory);
    return success;
}
struct Fixtures {
    fs::path root;
    std::vector<fs::path> links;
    Fixtures() {
        wchar_t temp[MAX_PATH + 1]{};
        if (!GetTempPathW(MAX_PATH, temp)) throw std::runtime_error("GetTempPath failed");
        root = fs::path(temp) / (L"codex_session_tests_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
        fs::create_directory(root);
    }
    ~Fixtures() {
        // All paths originate from this unique temporary root. Remove junctions
        // explicitly so recursive cleanup cannot traverse a linked destination.
        for (const auto& link : links) RemoveDirectoryW(link.c_str());
        const auto absolute = fs::absolute(root).lexically_normal();
        std::error_code error;
        if (absolute.filename().wstring().find(L"codex_session_tests_") != 0 || !fs::equivalent(absolute.parent_path(), fs::temp_directory_path(), error)) return;
        fs::remove_all(absolute, error);
    }
};
}

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    try {
        Fixtures temp;
        const auto root = temp.root / L"会话";
        fs::create_directory(root);
        auto scan = sessions::Scan(root);
        Check(scan.files.empty() && scan.error.empty(), "empty directory");
        scan = sessions::Scan(root / L"不存在");
        Check(scan.files.empty() && !scan.error.empty(), "missing directory");
        const auto oldFile = root / L"旧目录" / L"同名.jsonl";
        const auto newFile = root / L"新目录" / L"同名.JSONL";
        Write(oldFile, Metadata("old") + Message("user", "旧消息"));
        Write(newFile, Metadata("new", {}, "vscode") + Message("assistant", "新消息"));
        Write(root / L"ignored.txt", Message("user", "ignored"));
        fs::last_write_time(oldFile, fs::file_time_type::clock::now() - std::chrono::hours(2));
        scan = sessions::Scan(root);
        Check(scan.files.size() == 2, "recursive jsonl scan and extension filtering");
        Check(scan.files.front().path == newFile && scan.files.front().relativePath == fs::path(L"新目录") / L"同名.JSONL", "newest first, Unicode and relative paths");
        Check(scan.files[0].relativePath != scan.files[1].relativePath, "same filename stays distinct");
        scan = sessions::Scan(root, [] { return true; });
        Check(scan.files.empty(), "cancel scan");

        const auto previewFile = root / L"preview.jsonl";
        Write(previewFile, "\xEF\xBB\xBF\n \r\n" + Message("user", "你好\n第二行") + Message("assistant", "回答"));
        auto preview = sessions::Preview(previewFile);
        Check(preview.messages == 2 && preview.damagedLines == 0 && preview.error.empty(), "fewer than five messages, BOM and blank lines");
        Check(preview.text.find(L"【用户】\r\n你好\r\n第二行") != std::wstring::npos && preview.text.find(L"【助手】\r\n回答") != std::wstring::npos, "roles, Unicode and multiline");
        std::string many;
        for (int i = 0; i < 7; ++i) many += Message(i % 2 ? "assistant" : "user", "message_" + std::to_string(i));
        Write(previewFile, many);
        preview = sessions::Preview(previewFile);
        Check(preview.messages == 5 && preview.text.find(L"message_4") != std::wstring::npos && preview.text.find(L"message_5") == std::wstring::npos, "first five messages only");
        json fragments{{"type", "response_item"}, {"payload", {{"type", "message"}, {"role", "assistant"}, {"content", json::array({
            {{"type", "output_text"}, {"text", "甲"}}, {{"type", "image"}, {"text", "忽略图片"}}, {{"type", "text"}, {"text", "乙"}}})}}}};
        const std::string irrelevant =
            "{\"type\":\"session_meta\",\"payload\":{\"message\":\"config\"}}\n"
            "{\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"duplicate\"}}\n"
            "{\"type\":\"response_item\",\"payload\":{\"type\":\"function_call\",\"role\":\"assistant\"}}\n"
            "{\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"tool\",\"content\":[{\"type\":\"text\",\"text\":\"tool\"}]}}\n"
            "{\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_image\"}]}}\n";
        Write(previewFile, irrelevant + "broken json\n" + fragments.dump() + "\n" + Message("user", "末尾"));
        preview = sessions::Preview(previewFile);
        Check(preview.messages == 2 && preview.damagedLines == 1, "filter config tools events and nontext; count damaged line");
        Check(preview.text.find(L"甲乙") != std::wstring::npos && preview.text.find(L"忽略图片") == std::wstring::npos, "merge text fragments only");
        Write(previewFile, "");
        preview = sessions::Preview(previewFile);
        Check(preview.messages == 0 && !preview.text.empty() && preview.error.empty(), "empty file message");
        preview = sessions::Preview(root / L"missing.jsonl");
        Check(!preview.error.empty(), "missing preview failure");
        preview = sessions::Preview(previewFile, [] { return true; });
        Check(preview.messages == 0 && preview.text.empty(), "cancel preview before read");
        Write(previewFile, std::string(200000, ' ') + "\n" + Message("user", "cancelled"));
        int cancellationChecks = 0;
        preview = sessions::Preview(previewFile, [&] { return ++cancellationChecks >= 4; });
        Check(preview.text.empty() && preview.messages == 0 && cancellationChecks >= 4, "cancel during chunked read");
        Write(previewFile, std::string("{\"bad\":\"") + '\xFF' + "\"}\n" + Message("user", "valid"));
        preview = sessions::Preview(previewFile);
        Check(preview.messages == 1 && preview.damagedLines == 1, "invalid UTF-8 skipped");

        Write(previewFile, Metadata("preview") + Message("user", "delete fixture"));
        auto deletion = sessions::Delete(root, previewFile, false);
        Check(deletion.status == sessions::DeleteStatus::Cancelled && fs::exists(previewFile), "cancel deletion keeps file");
        deletion = sessions::Delete(root, previewFile, true);
        Check(deletion.status == sessions::DeleteStatus::Deleted && !fs::exists(previewFile), "confirm deletion");
        deletion = sessions::Delete(root, previewFile, true);
        Check(deletion.status == sessions::DeleteStatus::Missing, "externally deleted file");
        deletion = sessions::Delete(root, root / L"ignored.txt", true);
        Check(deletion.status == sessions::DeleteStatus::Failed && fs::exists(root / L"ignored.txt"), "reject non-jsonl");
        const auto outside = temp.root / L"outside.jsonl";
        Write(outside, "outside");
        deletion = sessions::Delete(root, outside, true);
        Check(deletion.status == sessions::DeleteStatus::Failed && fs::exists(outside), "reject outside root");
        deletion = sessions::Delete(root, root / L".." / L"outside.jsonl", true);
        Check(deletion.status == sessions::DeleteStatus::Failed && fs::exists(outside), "reject traversal");
        const auto similarRoot = temp.root / L"会话2";
        Write(similarRoot / L"sibling.jsonl", "sibling");
        deletion = sessions::Delete(root, similarRoot / L"sibling.jsonl", true);
        Check(deletion.status == sessions::DeleteStatus::Failed && fs::exists(similarRoot / L"sibling.jsonl"), "reject sibling prefix");
        const auto directoryFile = root / L"directory.jsonl";
        fs::create_directory(directoryFile);
        deletion = sessions::Delete(root, directoryFile, true);
        Check(deletion.status == sessions::DeleteStatus::Failed && fs::is_directory(directoryFile), "reject directory");
        const auto readOnly = root / L"readonly.jsonl";
        Write(readOnly, Metadata("readonly"));
        SetFileAttributesW(readOnly.c_str(), FILE_ATTRIBUTE_READONLY);
        deletion = sessions::Delete(root, readOnly, true);
        Check(deletion.status == sessions::DeleteStatus::Failed && !deletion.error.empty() && fs::exists(readOnly), "readonly deletion failure keeps file");
        SetFileAttributesW(readOnly.c_str(), FILE_ATTRIBUTE_NORMAL);
        const HANDLE held = CreateFileW(readOnly.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(held != INVALID_HANDLE_VALUE, "open locked fixture");
        deletion = sessions::Delete(root, readOnly, true);
        CloseHandle(held);
        Check(deletion.status == sessions::DeleteStatus::Failed && fs::exists(readOnly), "sharing violation keeps file");
        deletion = sessions::Delete(root, newFile, true);
        Check(deletion.status == sessions::DeleteStatus::Deleted && !fs::exists(newFile), "uppercase extension delete");
        const fs::path trailingRoot(root.wstring() + L"\\");
        scan = sessions::Scan(trailingRoot);
        Check(scan.error.empty() && !scan.files.empty(), "scan root with trailing separator");
        const auto trailingFile = root / L"trailing-root.jsonl";
        Write(trailingFile, Metadata("trailing"));
        deletion = sessions::Delete(trailingRoot, trailingFile, true);
        Check(deletion.status == sessions::DeleteStatus::Deleted && !fs::exists(trailingFile), "delete under root with trailing separator");

        const auto junction = root / L"linked";
        Check(Junction(junction, similarRoot), "create directory junction fixture");
        temp.links.push_back(junction);
        scan = sessions::Scan(root);
        bool foundLinked = false;
        for (const auto& file : scan.files) if (file.relativePath.wstring().find(L"linked") != std::wstring::npos) foundLinked = true;
        Check(!foundLinked && scan.skipped >= 1, "skip junction during scan");
        deletion = sessions::Delete(root, junction / L"sibling.jsonl", true);
        Check(deletion.status == sessions::DeleteStatus::Failed && fs::exists(similarRoot / L"sibling.jsonl"), "reject deletion through junction");
        preview = sessions::Preview(junction / L"sibling.jsonl");
        Check(preview.messages == 0 && !preview.error.empty(), "reject preview through replaced directory junction");
        scan = sessions::Scan(junction);
        Check(scan.files.empty() && !scan.error.empty(), "reject linked root");
        deletion = sessions::Delete(junction, junction / L"sibling.jsonl", true);
        Check(deletion.status == sessions::DeleteStatus::Failed, "reject linked deletion root");

        const auto families = temp.root / L"families";
        const auto main = families / L"2026" / L"11" / L"16" / L"main.jsonl";
        const auto other = families / L"other.jsonl";
        const auto fork = families / L"fork.jsonl";
        const auto child = families / L"2026" / L"11" / L"17" / L"child.jsonl";
        const auto sibling = families / L"sibling.jsonl";
        const auto grandchild = families / L"2026" / L"12" / L"01" / L"grandchild.jsonl";
        const auto otherChild = families / L"other-child.jsonl";
        const auto orphan = families / L"orphan.jsonl";
        Write(main, "\xEF\xBB\xBF\n\n" + Metadata("main"));
        Write(other, Metadata("other", {}, "vscode"));
        Write(fork, Metadata("fork", {}, "cli", "main"));
        Write(child, Metadata("child", "main"));
        Write(sibling, Metadata("sibling", "main"));
        Write(grandchild, Metadata("grandchild", "child"));
        Write(otherChild, Metadata("other-child", "other"));
        Write(orphan, Metadata("orphan", "absent-parent"));
        Write(families / L"exec.jsonl", Metadata("exec", {}, "exec"));
        Write(families / L"mcp.jsonl", Metadata("mcp", {}, "mcp"));
        Write(families / L"unknown.jsonl", Metadata("unknown", {}, "future-source"));
        scan = sessions::Scan(families);
        Check(scan.error.empty() && scan.skipped == 0 && scan.files.size() == 3, "picker shows only interactive main sessions including ordinary fork");
        Check(std::all_of(scan.files.begin(), scan.files.end(), [](const sessions::File& file) {
            return !file.subagent && file.interactive && !file.sessionId.empty();
        }), "scan captures main IDs and excludes children, orphans and noninteractive sources");
        deletion = sessions::Delete(families, main, false, "main");
        Check(deletion.status == sessions::DeleteStatus::Cancelled && fs::exists(main) && fs::exists(child) && fs::exists(grandchild), "cancel keeps entire session family");
        deletion = sessions::Delete(families, child, true, "child");
        Check(deletion.status == sessions::DeleteStatus::Failed && fs::exists(child), "reject deleting hidden child directly");
        const auto lateChild = families / L"late-child.jsonl";
        Write(lateChild, Metadata("late-child", "child"));
        deletion = sessions::Delete(families, main, true, "main");
        Check(deletion.status == sessions::DeleteStatus::Deleted && deletion.deletedFiles == 5
            && !fs::exists(main) && !fs::exists(child) && !fs::exists(sibling) && !fs::exists(grandchild) && !fs::exists(lateChild), "delete main and all cross-date descendants including children created since scan");
        Check(fs::exists(other) && fs::exists(otherChild) && fs::exists(fork) && fs::exists(orphan)
            && fs::exists(families / L"exec.jsonl"), "unrelated sessions, regular fork and orphan stay intact");
        scan = sessions::Scan(families);
        Check(scan.error.empty() && scan.files.size() == 2, "main session count updates after family deletion");
        fs::remove(other);
        deletion = sessions::Delete(families, other, true, "other");
        Check(deletion.status == sessions::DeleteStatus::Deleted && deletion.deletedFiles == 1 && !fs::exists(otherChild), "cached main ID cleans descendants after main externally deleted");

        const auto guarded = temp.root / L"guarded";
        const auto guardedMain = guarded / L"main.jsonl";
        const auto guardedChild = guarded / L"child.jsonl";
        Write(guardedMain, Metadata("guarded"));
        Write(guardedChild, Metadata("guarded-child", "guarded"));
        const auto damaged = guarded / L"damaged.jsonl";
        Write(damaged, "broken header\n");
        scan = sessions::Scan(guarded);
        Check(scan.files.size() == 1 && scan.skipped == 1 && !scan.error.empty(), "invalid metadata skipped with explicit scan error");
        deletion = sessions::Delete(guarded, guardedMain, true, "guarded");
        Check(deletion.status == sessions::DeleteStatus::Failed && deletion.deletedFiles == 0 && fs::exists(guardedMain) && fs::exists(guardedChild), "incomplete metadata prevents unsafe family deletion");
        fs::remove(damaged);
        Write(damaged, Metadata("guarded-child", "guarded"));
        deletion = sessions::Delete(guarded, guardedMain, true, "guarded");
        Check(deletion.status == sessions::DeleteStatus::Failed && deletion.deletedFiles == 0 && fs::exists(guardedMain), "duplicate IDs block family deletion");
        fs::remove(damaged);
        deletion = sessions::Delete(guarded, guardedMain, true, "stale-id");
        Check(deletion.status == sessions::DeleteStatus::Failed && fs::exists(guardedChild), "changed main identity blocks deletion");
        SetFileAttributesW(guardedChild.c_str(), FILE_ATTRIBUTE_READONLY);
        deletion = sessions::Delete(guarded, guardedMain, true, "guarded");
        Check(deletion.status == sessions::DeleteStatus::Failed && deletion.deletedFiles == 0 && fs::exists(guardedMain) && fs::exists(guardedChild), "child deletion failure preserves main for retry");
        SetFileAttributesW(guardedChild.c_str(), FILE_ATTRIBUTE_NORMAL);
        SetFileAttributesW(guardedMain.c_str(), FILE_ATTRIBUTE_READONLY);
        deletion = sessions::Delete(guarded, guardedMain, true, "guarded");
        Check(deletion.status == sessions::DeleteStatus::Failed && deletion.deletedFiles == 1 && fs::exists(guardedMain)
            && !fs::exists(guardedChild) && !deletion.error.empty(), "partial failure reports count and keeps main until all children removed");
        SetFileAttributesW(guardedMain.c_str(), FILE_ATTRIBUTE_NORMAL);
        deletion = sessions::Delete(guarded, guardedMain, true, "guarded");
        Check(deletion.status == sessions::DeleteStatus::Deleted && deletion.deletedFiles == 1 && !fs::exists(guardedMain), "retry completes partial family deletion");

        const auto racingRoot = temp.root / L"racing-root";
        const auto racingOutside = temp.root / L"racing-outside";
        fs::create_directory(racingRoot);
        temp.links.push_back(racingRoot);
        Write(racingOutside / L"victim.jsonl", "outside sentinel");
        std::atomic<bool> stop{false};
        std::atomic<unsigned> changes{0};
        std::thread modifier([&] {
            while (!stop.load()) {
                if (Junction(racingRoot, racingOutside, false)) {
                    ++changes;
                    std::this_thread::yield();
                    ClearJunction(racingRoot);
                }
                std::this_thread::yield();
            }
            ClearJunction(racingRoot);
        });
        bool escaped = false;
        for (int attempt = 0; attempt < 300; ++attempt) {
            const auto result = sessions::Delete(racingRoot, racingRoot / L"victim.jsonl", true);
            if (result.status == sessions::DeleteStatus::Deleted) escaped = true;
            std::this_thread::yield();
        }
        stop = true;
        modifier.join();
        Check(changes.load() > 0, "exercise in-place reparse mutation using attribute-only handle");
        Check(!escaped && fs::exists(racingOutside / L"victim.jsonl"), "root reparse race cannot delete outside sentinel");

        std::cout << "PASS: " << checks << " core checks (all deletion fixtures used a unique temporary directory).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << "\n";
        return 1;
    }
}
