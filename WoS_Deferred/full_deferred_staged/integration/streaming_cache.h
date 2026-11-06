#pragma once
// NOT BUILT (2026-09-23). Verified safe (buffered vs unbuffered ReadFileEx identical on 21 cases,
// incl. EOF) but measured useless on the NVMe install: 486 KB read p50 0.41 ms unbuffered vs
// 0.44 ms from the file cache. Only worth including for an HDD install.
// Pack cache: the native streamer opens packs with FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING
// (A17BD0, reads rounded to 2048), so every district load goes to the disk even when the same
// pack was read seconds ago. We strip NO_BUFFERING from exactly those opens and warm all of
// packs\*.PCPACK (1.3 GB) into the Windows file cache on a low-priority thread. The cache lives
// in system memory, not in the 32-bit game process.
// Disable for A/B: create an empty file "streaming_cache.off" next to the game EXE.
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace WosPackCache {

constexpr uintptr_t kCreateFileASlot = 0x00B79114;   // IAT slot read by call [0xB79114] at A17C17
constexpr DWORD kStreamerFlags = FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING;

using CreateFileAFn = HANDLE(WINAPI *)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

inline CreateFileAFn originalCreateFileA = nullptr;
inline bool active = false;
inline std::atomic<unsigned> bufferedOpens{0};
inline std::atomic<unsigned long long> warmedBytes{0};
inline unsigned long long totalBytes = 0;
inline std::atomic<unsigned> warmMs{0};
inline std::atomic<bool> warmDone{false};
inline char status[200] = "Кэш паков: не применён";

inline HANDLE WINAPI HookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                     DWORD disposition, DWORD flags, HANDLE templateFile)
{
    if ((flags & kStreamerFlags) == kStreamerFlags && (access & GENERIC_WRITE) == 0) {
        flags &= ~FILE_FLAG_NO_BUFFERING;
        bufferedOpens.fetch_add(1, std::memory_order_relaxed);
    }
    return originalCreateFileA(name, access, share, security, disposition, flags, templateFile);
}

inline std::wstring GameDirectory()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
}

inline DWORD WINAPI WarmThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    const std::wstring packs = GameDirectory() + L"packs\\";

    struct Pack { std::wstring path; unsigned long long size; };
    std::vector<Pack> list;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((packs + L"*.PCPACK").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const unsigned long long size = (static_cast<unsigned long long>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
            list.push_back({packs + found.cFileName, size});
            totalBytes += size;
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    // City and shared packs first, then the biggest mission packs.
    auto rank = [](const std::wstring &path) {
        const std::wstring file = path.substr(path.find_last_of(L'\\') + 1);
        if (_wcsicmp(file.c_str(), L"MEGACITY.PCPACK") == 0) return 0;
        if (_wcsicmp(file.c_str(), L"GAME.PCPACK") == 0) return 1;
        return 2;
    };
    std::sort(list.begin(), list.end(), [&](const Pack &a, const Pack &b) {
        const int ra = rank(a.path), rb = rank(b.path);
        return ra != rb ? ra < rb : a.size > b.size;
    });

    const ULONGLONG start = GetTickCount64();
    std::vector<char> buffer(1 << 20);
    for (const Pack &pack : list) {
        HANDLE file = CreateFileW(pack.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) continue;
        FILE_IO_PRIORITY_HINT_INFO hint = {IoPriorityHintLow};
        SetFileInformationByHandle(file, FileIoPriorityHintInfo, &hint, sizeof(hint));
        DWORD read = 0;
        while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read)
            warmedBytes.fetch_add(read, std::memory_order_relaxed);
        CloseHandle(file);
    }
    warmMs = static_cast<unsigned>(GetTickCount64() - start);
    warmDone = true;
    return 0;
}

inline void UpdateStatus()
{
    if (!active) return;
    const double warmed = static_cast<double>(warmedBytes.load()) / (1024.0 * 1024.0);
    const double total = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
    if (warmDone)
        std::snprintf(status, sizeof(status), "Кэш паков: %.0f МБ в памяти за %.1f с, буферизованных открытий: %u",
                      warmed, warmMs.load() / 1000.0, bufferedOpens.load());
    else
        std::snprintf(status, sizeof(status), "Кэш паков: прогрев %.0f / %.0f МБ, буферизованных открытий: %u",
                      warmed, total, bufferedOpens.load());
}

// The streamer's open must still be `call [0xB79114]` in this EXE build.
inline void *ReadStreamerSlot()
{
    static const unsigned char expected[] = {0xFF, 0x15, 0x14, 0x91, 0xB7, 0x00};
    __try {
        if (std::memcmp(reinterpret_cast<const void *>(0x00A17C17), expected, sizeof(expected)) != 0) return nullptr;
        return *reinterpret_cast<void **>(kCreateFileASlot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Called from DllMain. Patches the fixed IAT slot so the hook chains to whatever is already there
// (another mod may have hooked CreateFileA first); only the flag test decides what is changed.
inline void Install()
{
    const std::wstring off = GameDirectory() + L"streaming_cache.off";
    if (GetFileAttributesW(off.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::snprintf(status, sizeof(status), "Кэш паков: выключен файлом streaming_cache.off");
        return;
    }
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    void *exported = kernel ? reinterpret_cast<void *>(GetProcAddress(kernel, "CreateFileA")) : nullptr;
    auto *slot = reinterpret_cast<void **>(kCreateFileASlot);
    void *current = ReadStreamerSlot();
    // Accept the genuine export or an existing hook living outside the EXE image.
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto target = reinterpret_cast<uintptr_t>(current);
    if (!current || (current != exported && target >= exe && target < exe + 0x1200000)) {
        std::snprintf(status, sizeof(status), "Кэш паков: слот CreateFileA не совпадает, не применён");
        return;
    }
    DWORD old = 0;
    VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old);
    originalCreateFileA = reinterpret_cast<CreateFileAFn>(current);
    *slot = reinterpret_cast<void *>(&HookCreateFileA);
    VirtualProtect(slot, sizeof(void *), old, &old);
    active = true;

    HANDLE thread = CreateThread(nullptr, 0, WarmThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    UpdateStatus();
}

} // namespace WosPackCache
