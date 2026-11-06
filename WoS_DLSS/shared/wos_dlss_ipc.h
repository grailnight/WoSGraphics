#pragma once
// Shared-memory contract between the renderer inside the game (x86, winmm.dll)
// and WoS_DLSS_Host.exe (x64). DLSS only ships for x64, so the game hands its
// frame to the helper through Vulkan memory exported from DXVK's device, and
// both sides order their GPU work with one exported timeline semaphore:
//
//   game  : copy colour/depth/motion into the shared images, signal 2N+1,
//           wait 2N+2, copy the shared output back into a D3D9 texture
//   helper: wait 2N+1, DLSS evaluate, signal 2N+2
//
// No CPU waits for the other process on the hot path. Handles in Config are
// NT handles the game has already duplicated into the helper process.
//
// Both builds compile this file; the static_asserts pin the layout so an x86
// and an x64 compiler cannot disagree about it.
#include <stddef.h>
#include <stdint.h>

namespace WosDlssIpc {

constexpr uint32_t kMagic = 0x534C4457u; // "WDLS"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kRing = 16;

enum ImageSlot : uint32_t { kColor = 0, kDepth = 1, kMotion = 2, kOutput = 3, kImageCount = 4 };
enum HostState : uint32_t { kHostStarting = 0, kHostReady = 1, kHostFailed = 2, kHostExited = 3 };

// Names, formatted with the game's process id.
#define WOS_DLSS_SHARED_NAME L"Local\\WoS_DLSS_%lu_shared"
#define WOS_DLSS_EVENT_NAME L"Local\\WoS_DLSS_%lu_frame"

#pragma pack(push, 8)
struct ImageDesc {
    uint32_t format;          // VkFormat
    uint32_t usage;           // VkImageUsageFlags; the helper creates its alias with exactly these
    uint32_t width, height;
    uint32_t flags;           // VkImageCreateFlags
    uint32_t reserved;
    uint64_t allocationSize;  // dedicated allocation size on the exporting side
    uint64_t handle;          // NT handle, valid in the helper process
};

struct Config {
    uint32_t generation;      // written last; the helper re-imports when it differs from hostGeneration
    uint32_t width, height;
    int32_t preset;           // NVSDK_NGX_DLSS_Hint_Render_Preset (0 = driver default)
    uint32_t createFlags;     // NVSDK_NGX_DLSS_Feature_Flags
    uint32_t reserved;
    uint8_t deviceUUID[16];   // VkPhysicalDeviceIDProperties of DXVK's device
    uint8_t driverUUID[16];
    uint64_t timelineHandle;  // NT handle of the timeline semaphore, valid in the helper
    ImageDesc images[kImageCount];
};

struct Frame {
    uint32_t serial;          // N, 1-based; lives in ring[N % kRing]
    uint32_t generation;      // config the frame was recorded against
    uint64_t inputReady;      // 2N+1, signalled by the game after its copy-in
    uint64_t outputReady;     // 2N+2, signalled by the helper after DLSS
    float jitterX, jitterY;   // render pixels, DLSS sign convention
    float mvScaleX, mvScaleY; // motion texture units -> pixels
    float frameMs;
    uint32_t reset;           // history must not be used
};

struct Shared {
    uint32_t magic, version;
    uint32_t gamePid, hostPid;
    Config config;
    // game -> helper
    volatile uint32_t framesWritten;   // last serial published in ring
    uint32_t reserved0;
    Frame ring[kRing];
    // helper -> game
    volatile uint32_t hostState;       // HostState
    volatile uint32_t hostGeneration;  // config imported and DLSS feature created for it
    volatile uint32_t framesSubmitted; // last serial whose DLSS work (and signal) is queued
    volatile uint32_t heartbeat;       // GetTickCount() of the helper loop
    volatile int32_t lastResult;       // NVSDK_NGX_Result of the last create/evaluate
    volatile uint32_t evaluateFailures;
    volatile float gpuMs;              // DLSS evaluate on the GPU, a few frames old
    uint32_t reserved1;
    char status[256];
    char runtime[64];                  // e.g. "nvngx_dlss 310.6.0.0, preset K"
};
#pragma pack(pop)

static_assert(sizeof(ImageDesc) == 40, "ImageDesc layout");
static_assert(sizeof(Config) == 224, "Config layout");
static_assert(sizeof(Frame) == 48, "Frame layout");
static_assert(offsetof(Shared, config) == 16, "Shared layout");
static_assert(offsetof(Shared, ring) == 248, "Shared layout");
static_assert(offsetof(Shared, hostState) == 1016, "Shared layout");
static_assert(offsetof(Shared, status) == 1048, "Shared layout");
static_assert(sizeof(Shared) == 1368, "Shared layout");

inline uint64_t InputValue(uint32_t serial) { return 2ull * serial + 1ull; }
inline uint64_t OutputValue(uint32_t serial) { return 2ull * serial + 2ull; }

} // namespace WosDlssIpc
