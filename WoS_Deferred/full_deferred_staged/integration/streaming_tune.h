#pragma once
// Native streamer poll interval (WoS_Research/streaming/STREAMING_CONTRACT_RU.md).
// The I/O thread A17310 sleeps *(DWORD*)0xFB84A8 ms between StreamUpdate passes; a finished
// ReadFileEx is only picked up by the next pass, and there is one read in flight at a time,
// so this sleep caps how fast districts, textures and FX packs arrive. Measured in game:
// request -> completion 5.7 ms at 5 ms, 1.8 ms at 1 ms, no I/O errors.
// The value is read fresh every iteration (single reader A173A3), so it can change live.
#include <windows.h>
#include <cstdio>

namespace WosStreaming {

constexpr uintptr_t kPollMsVa = 0x00FB84A8;
constexpr uintptr_t kReaderVa = 0x00A173A3;   // mov eax,[0xFB84A8]; push eax; call [Sleep]
constexpr DWORD kNativePollMs = 5;

inline bool active = false;
inline int pollMs = 1;
inline char status[160] = "Стриминг: не применён";

inline bool CodeMatches()
{
    static const unsigned char expected[] = {0xA1, 0xA8, 0x84, 0xFB, 0x00, 0x50, 0xFF, 0x15, 0xC0, 0x90, 0xB7, 0x00};
    __try {
        return std::memcmp(reinterpret_cast<const void *>(kReaderVa), expected, sizeof(expected)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline void Apply(int ms)
{
    if (!active) return;
    if (ms < 1) ms = 1;
    if (ms > 5) ms = 5;
    pollMs = ms;
    *reinterpret_cast<volatile DWORD *>(kPollMsVa) = static_cast<DWORD>(ms);
    std::snprintf(status, sizeof(status), "Стриминг: опрос %d мс (штатно %lu)", ms, kNativePollMs);
}

// Called from DllMain before the game creates its streaming thread. Leaves the game alone
// unless both the reader instruction and the original value match this EXE build.
inline void Install()
{
    if (!CodeMatches()) {
        std::snprintf(status, sizeof(status), "Стриминг: EXE не совпадает, опрос не изменён");
        return;
    }
    const DWORD current = *reinterpret_cast<volatile DWORD *>(kPollMsVa);
    if (current != kNativePollMs && current != 1) {
        std::snprintf(status, sizeof(status), "Стриминг: неожиданное значение %lu, не изменено", current);
        return;
    }
    active = true;
    Apply(pollMs);
}

} // namespace WosStreaming
