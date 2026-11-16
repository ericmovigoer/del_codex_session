#include <afxwin.h>
#include <afxext.h>
#include <shellapi.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include "SessionStore.h"

namespace {
constexpr UINT kResults = WM_APP + 1;
constexpr UINT kList = 1001, kPreview = 1002, kRefresh = 1003, kDelete = 1004, kPath = 1005;
struct Result {
    bool scan = false;
    std::uint64_t version = 0;
    sessions::ScanResult listing;
    sessions::PreviewResult preview;
};

class SessionWindow : public CFrameWnd {
public:
    explicit SessionWindow(sessions::fs::path root) : root_(std::move(root)) {}
    BOOL Open() {
        return Create(nullptr, L"Codex 会话删除工具", WS_OVERLAPPEDWINDOW,
            CRect(0, 0, 1180, 760), nullptr, nullptr);
    }
protected:
    afx_msg int OnCreate(LPCREATESTRUCT create) {
        if (CFrameWnd::OnCreate(create) == -1) return -1;
        font_.CreatePointFont(100, L"Microsoft YaHei UI");
        titleList_.Create(L"主会话文件（按修改时间从新到旧）", WS_CHILD | WS_VISIBLE, {}, this);
        titlePreview_.Create(L"前 5 条用户 / 助手文字消息", WS_CHILD | WS_VISIBLE, {}, this);
        list_.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL |
            WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, {}, this, kList);
        preview_.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, {}, this, kPreview);
        preview_.SetLimitText(0x7ffffffe);
        refresh_.Create(L"刷新", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, {}, this, kRefresh);
        remove_.Create(L"删除选中会话…", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, {}, this, kDelete);
        path_.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_READONLY | ES_AUTOHSCROLL,
            {}, this, kPath);
        hint_.Create(L"删除会永久移除主会话及其所有子 Agent 文件。请先核对预览和路径。", WS_CHILD | WS_VISIBLE, {}, this);
        status_.Create(this);
        const UINT indicators[] = { ID_SEPARATOR };
        status_.SetIndicators(indicators, 1);
        for (CWnd* control : std::initializer_list<CWnd*>{ &titleList_, &titlePreview_, &list_, &preview_,
             &refresh_, &remove_, &path_, &hint_, &status_ }) control->SetFont(&font_);
        remove_.EnableWindow(FALSE);
        scanThread_ = std::thread([this] { ScanWorker(); });
        previewThread_ = std::thread([this] { PreviewWorker(); });
        OnRefresh();
        return 0;
    }
    afx_msg void OnSize(UINT type, int cx, int cy) {
        CFrameWnd::OnSize(type, cx, cy);
        if (!list_.GetSafeHwnd()) return;
        const auto scale = [this](int v) { return MulDiv(v, GetDpiForWindow(m_hWnd), 96); };
        const int pad = scale(14), row = scale(30), gap = scale(10);
        const int bottom = cy - scale(26), split = std::max(scale(240), cx * 37 / 100);
        refresh_.MoveWindow(pad, pad, scale(90), row);
        remove_.MoveWindow(pad + scale(104), pad, scale(160), row);
        hint_.MoveWindow(pad + scale(280), pad + scale(4), std::max(0, cx - scale(308)), row);
        titleList_.MoveWindow(pad, pad + row + gap, split - pad - gap, row);
        titlePreview_.MoveWindow(split, pad + row + gap, std::max(0, cx - split - pad), row);
        const int top = pad + 2 * row + gap, height = std::max(0, bottom - row - gap - top);
        list_.MoveWindow(pad, top, split - pad - gap, height);
        preview_.MoveWindow(split, top, std::max(0, cx - split - pad), height);
        path_.MoveWindow(pad, bottom - row, std::max(0, cx - 2 * pad), row - scale(3));
        status_.MoveWindow(0, bottom, cx, cy - bottom);
    }
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* info) {
        info->ptMinTrackSize = { 800, 480 };
        CFrameWnd::OnGetMinMaxInfo(info);
    }
    afx_msg LRESULT OnDpiChanged(WPARAM, LPARAM value) {
        const auto* rect = reinterpret_cast<RECT*>(value);
        SetWindowPos(nullptr, rect->left, rect->top, rect->right - rect->left,
            rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    void SetStatus(const std::wstring& message) {
        const std::wstring text = L"主会话数：" + std::to_wstring(files_.size()) + L"    " + message;
        status_.SetPaneText(0, text.c_str());
    }
    afx_msg void OnRefresh() {
        ++previewVersion_;
        ++scanVersion_;
        files_.clear();
        list_.ResetContent();
        preview_.SetWindowTextW(L"");
        path_.SetWindowTextW(root_.c_str());
        remove_.EnableWindow(FALSE);
        SetStatus(L"正在扫描…");
        {
            std::lock_guard<std::mutex> lock(workMutex_);
            scanRequested_ = true;
            previewRequested_ = false;
        }
        workReady_.notify_all();
    }
    afx_msg void OnSelect() {
        const int index = list_.GetCurSel();
        const auto version = ++previewVersion_;
        if (index == LB_ERR || static_cast<std::size_t>(index) >= files_.size()) {
            remove_.EnableWindow(FALSE);
            preview_.SetWindowTextW(L"");
            return;
        }
        const auto path = files_[index].path;
        path_.SetWindowTextW(path.c_str());
        preview_.SetWindowTextW(L"正在读取预览…");
        remove_.EnableWindow(TRUE);
        SetStatus(L"正在读取预览…");
        {
            std::lock_guard<std::mutex> lock(workMutex_);
            requestedPath_ = path;
            requestedVersion_ = version;
            previewRequested_ = true;
        }
        workReady_.notify_all();
    }
    afx_msg void OnDelete() {
        const int index = list_.GetCurSel();
        if (index == LB_ERR || static_cast<std::size_t>(index) >= files_.size()) return;
        const auto path = files_[index].path;
        const std::wstring question = L"确定永久删除以下主会话及其所有层级的子 Agent 会话文件吗？\r\n此操作无法撤销。\r\n\r\n" + path.wstring();
        if (MessageBox(question.c_str(), L"确认永久删除", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) {
            SetStatus(L"已取消删除。");
            return;
        }
        // 再次由核心核对目录、扩展名和链接属性，并通过文件句柄删除。
        const auto result = sessions::Delete(root_, path, true, files_[index].sessionId);
        if (result.status == sessions::DeleteStatus::Deleted || result.status == sessions::DeleteStatus::Missing) {
            ++previewVersion_;
            files_.erase(files_.begin() + index);
            list_.DeleteString(index);
            list_.SetCurSel(-1);
            preview_.SetWindowTextW(L"");
            path_.SetWindowTextW(root_.c_str());
            remove_.EnableWindow(FALSE);
            SetStatus(result.status == sessions::DeleteStatus::Deleted
                ? L"已永久删除主会话及其子 Agent 文件，共 " + std::to_wstring(result.deletedFiles) + L" 个。"
                : L"文件已被外部删除，列表已更新。");
        } else {
            SetStatus(L"删除失败：" + result.error);
            MessageBox((L"删除未完成，主会话仍保留在列表中，可重试。\r\n\r\n" + result.error).c_str(), L"删除失败", MB_OK | MB_ICONERROR);
        }
    }
    afx_msg LRESULT OnResults(WPARAM, LPARAM) {
        std::deque<std::unique_ptr<Result>> results;
        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            results.swap(results_);
        }
        for (auto& result : results) {
            if (result->scan) {
                if (result->version != scanVersion_) continue;
                files_ = std::move(result->listing.files);
                list_.SetRedraw(FALSE);
                list_.ResetContent();
                int widest = 0;
                CClientDC dc(&list_);
                const auto old = dc.SelectObject(&font_);
                for (const auto& file : files_) {
                    list_.AddString(file.relativePath.c_str());
                    widest = std::max(widest, static_cast<int>(dc.GetTextExtent(file.relativePath.c_str()).cx));
                }
                dc.SelectObject(old);
                list_.SetHorizontalExtent(widest + 20);
                list_.SetRedraw(TRUE);
                list_.Invalidate();
                std::wstring message = result->listing.error;
                if (message.empty()) message = files_.empty() ? L"没有找到主会话文件。" : L"扫描完成，请选择主会话查看预览。";
                if (result->listing.skipped) message += L" 已跳过链接、无法读取或元数据无效的项目：" + std::to_wstring(result->listing.skipped);
                SetStatus(message);
                if (files_.empty()) preview_.SetWindowTextW(message.c_str());
            } else {
                if (result->version != previewVersion_) continue;
                auto& value = result->preview;
                preview_.SetWindowTextW((value.error.empty() ? value.text : value.error).c_str());
                std::wstring message = value.error.empty() ? L"预览完成：" + std::to_wstring(value.messages) + L" 条文字消息。" : value.error;
                if (value.damagedLines) message += L" 已跳过损坏行：" + std::to_wstring(value.damagedLines);
                SetStatus(message);
            }
        }
        return 0;
    }
    void Deliver(std::unique_ptr<Result> result) {
        std::lock_guard<std::mutex> lock(resultMutex_);
        if (stopping_) return;
        results_.push_back(std::move(result));
        PostMessage(kResults);
    }
    void ScanWorker() {
        for (;;) {
            std::uint64_t version;
            {
                std::unique_lock<std::mutex> lock(workMutex_);
                workReady_.wait(lock, [this] { return stopping_ || scanRequested_; });
                if (stopping_) return;
                scanRequested_ = false;
                version = scanVersion_;
            }
            auto result = std::make_unique<Result>();
            result->scan = true;
            result->version = version;
            try {
                result->listing = sessions::Scan(root_, [this, version] { return stopping_ || version != scanVersion_; });
            } catch (...) { result->listing.error = L"扫描失败：无法完成目录读取。"; }
            Deliver(std::move(result));
        }
    }
    void PreviewWorker() {
        for (;;) {
            sessions::fs::path path;
            std::uint64_t version;
            {
                std::unique_lock<std::mutex> lock(workMutex_);
                workReady_.wait(lock, [this] { return stopping_ || previewRequested_; });
                if (stopping_) return;
                previewRequested_ = false;
                path = requestedPath_;
                version = requestedVersion_;
            }
            auto result = std::make_unique<Result>();
            result->version = version;
            try {
                result->preview = sessions::Preview(path, [this, version] { return stopping_ || version != previewVersion_; });
            } catch (...) { result->preview.error = L"读取失败：无法完成文件预览。"; }
            Deliver(std::move(result));
        }
    }
    afx_msg void OnDestroy() {
        {
            std::lock_guard<std::mutex> lock(workMutex_);
            stopping_ = true;
        }
        workReady_.notify_all();
        if (scanThread_.joinable()) scanThread_.join();
        if (previewThread_.joinable()) previewThread_.join();
        CFrameWnd::OnDestroy();
    }
    BOOL PreTranslateMessage(MSG* message) override {
        if (message->message == WM_KEYDOWN && message->wParam == 'A' && GetKeyState(VK_CONTROL) < 0) {
            const HWND focus = ::GetFocus();
            if (focus == preview_.GetSafeHwnd() || focus == path_.GetSafeHwnd()) {
                ::SendMessage(focus, EM_SETSEL, 0, -1);
                return TRUE;
            }
        }
        if (message->message >= WM_KEYFIRST && message->message <= WM_KEYLAST && IsDialogMessage(message)) return TRUE;
        return CFrameWnd::PreTranslateMessage(message);
    }
    DECLARE_MESSAGE_MAP()
private:
    sessions::fs::path root_;
    std::vector<sessions::File> files_;
    CFont font_;
    CStatic titleList_, titlePreview_, hint_;
    CListBox list_;
    CEdit preview_, path_;
    CButton refresh_, remove_;
    CStatusBar status_;
    std::atomic<bool> stopping_{ false };
    std::atomic<std::uint64_t> scanVersion_{ 0 }, previewVersion_{ 0 };
    std::thread scanThread_, previewThread_;
    std::mutex workMutex_, resultMutex_;
    std::condition_variable workReady_;
    bool scanRequested_ = false, previewRequested_ = false;
    sessions::fs::path requestedPath_;
    std::uint64_t requestedVersion_ = 0;
    std::deque<std::unique_ptr<Result>> results_;
};
BEGIN_MESSAGE_MAP(SessionWindow, CFrameWnd)
    ON_WM_CREATE()
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_WM_DESTROY()
    ON_MESSAGE(WM_DPICHANGED, OnDpiChanged)
    ON_MESSAGE(kResults, OnResults)
    ON_COMMAND(kRefresh, OnRefresh)
    ON_COMMAND(kDelete, OnDelete)
    ON_LBN_SELCHANGE(kList, OnSelect)
END_MESSAGE_MAP()

class SessionApp : public CWinApp {
    BOOL InitInstance() override {
        CWinApp::InitInstance();
        auto root = sessions::DefaultRoot();
        int count = 0;
        LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (args) {
            if (count == 3 && wcscmp(args[1], L"--sessions-root") == 0) {
                root = sessions::fs::absolute(args[2]).lexically_normal();
            } else if (count != 1) {
                LocalFree(args);
                AfxMessageBox(L"用法：CodexSessionCleaner.exe [--sessions-root 完整目录路径]", MB_ICONINFORMATION);
                return FALSE;
            }
            LocalFree(args);
        }
        auto* window = new SessionWindow(root);
        m_pMainWnd = window;
        if (!window->Open()) return FALSE;
        window->CenterWindow();
        window->ShowWindow(SW_SHOW);
        window->UpdateWindow();
        return TRUE;
    }
} app;
}
