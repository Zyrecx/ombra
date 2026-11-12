#include "ombra/clip_ops.h"

#include "ombra/log.h"
#include "ombra/paths.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <cwctype>
#include <thread>

namespace ombra {
namespace {

using Microsoft::WRL::ComPtr;

// Vetoes a delete the shell would do permanently rather than into the Recycle Bin (a file too
// big for the bin, a drive without one): with FOF_NOCONFIRMATION it would otherwise go silently.
// Delete and Replace both promise the Recycle Bin.
class RecycleOnlySink : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                            IFileOperationProgressSink> {
public:
    bool recycled = false;
    STDMETHODIMP PreDeleteItem(DWORD flags, IShellItem*) override {
        return (flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) ? S_OK : HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    STDMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT hr, IShellItem* newly_created) override {
        recycled = SUCCEEDED(hr) && newly_created != nullptr;  // the item now in the Recycle Bin
        return S_OK;
    }
    STDMETHODIMP StartOperations() override { return S_OK; }
    STDMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
    STDMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    STDMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    STDMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    STDMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    STDMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    STDMETHODIMP ResetTimer() override { return S_OK; }
    STDMETHODIMP PauseTimer() override { return S_OK; }
    STDMETHODIMP ResumeTimer() override { return S_OK; }
};

// IFileOperation is apartment-threaded: run it on a thread of its own in an STA.
bool RecycleOnSta(const std::wstring& path, HWND owner) {
    bool result = false;
    std::thread([&] {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        {
            ComPtr<IFileOperation> op;
            ComPtr<IShellItem> item;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) &&
                SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                op->SetOwnerWindow(owner);
                op->SetOperationFlags(FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI);
                auto sink = Microsoft::WRL::Make<RecycleOnlySink>();
                if (SUCCEEDED(op->DeleteItem(item.Get(), sink.Get()))) {
                    op->PerformOperations();  // a veto reports failure; `recycled` says what happened
                    if (!sink->recycled) Log::Warn("clips: %ls was not moved to the Recycle Bin", path.c_str());
                    result = sink->recycled;
                }
            }
        }
        if (SUCCEEDED(init)) CoUninitialize();
    }).join();
    return result && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
}

std::wstring LockName(const std::wstring& clip) {
    wchar_t full[MAX_PATH * 4];
    const DWORD n = GetFullPathNameW(clip.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
    std::wstring path = (n > 0 && n < std::size(full)) ? std::wstring(full, n) : clip;
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the case-folded path
    for (wchar_t c : path) {
        h ^= static_cast<uint64_t>(std::towlower(c == L'/' ? L'\\' : c));
        h *= 1099511628211ull;
    }
    wchar_t hex[17];
    swprintf_s(hex, L"%016llx", static_cast<unsigned long long>(h));
    return Instance().instance_mutex + L"-clip-" + hex;
}

}  // namespace

bool RecycleClip(const std::wstring& path, HWND owner) {
    if (Instance().isolated) return DeleteFileW(path.c_str()) != 0;
    return RecycleOnSta(path, owner);
}

bool ReplaceWithEdited(const std::wstring& src, const std::wstring& edited) {
    FILETIME created{}, accessed{}, written{};
    HANDLE h = CreateFileW(src.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    const bool have_times = h != INVALID_HANDLE_VALUE && GetFileTime(h, &created, &accessed, &written);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!RecycleClip(src)) return false;
    if (have_times) {
        HANDLE w = CreateFileW(edited.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (w != INVALID_HANDLE_VALUE) {
            SetFileTime(w, &created, nullptr, &written);
            CloseHandle(w);
        }
    }
    if (MoveFileExW(edited.c_str(), src.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
    // The original is in the Recycle Bin and the edit kept its own name: still both recoverable.
    Log::Warn("clips: replace: rename failed (%lu)", GetLastError());
    return false;
}

ClipBusyLock::ClipBusyLock(const std::wstring& clip) : mutex_(CreateMutexW(nullptr, FALSE, LockName(clip).c_str())) {}

ClipBusyLock::~ClipBusyLock() {
    if (mutex_) CloseHandle(mutex_);
}

bool ClipBusy(const std::wstring& clip) {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, LockName(clip).c_str());
    if (!h) return false;
    CloseHandle(h);
    return true;
}

}  // namespace ombra
