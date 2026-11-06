#pragma once
// DLAA for the native D3D9 renderer: NVIDIA DLSS at native resolution.
//
// DLSS is x64-only and the game is a 32-bit D3D9 program, so this only works
// under DXVK. The renderer starts WoS_DLSS_Host.exe (x64) and shares four
// Vulkan images and one timeline semaphore with it, all allocated on DXVK's
// own VkDevice through DXVK's interop interface (dxvk_interop.h). The contract
// is in WoS_DLSS/shared/wos_dlss_ipc.h.
//
// Per frame, at the TAA resolve point (after the bloom composite, before HUD):
// 1. D3D9: the display image is copied (StretchRect); one MRT pass turns the
//    scene INTZ and the engine camera of both frames into motion (G16R16F,
//    UV delta to the previous frame, jitter excluded) and depth (R32F).
// 2. Vulkan, inside DXVK's queue lock: copy colour/depth/motion into the
//    shared images and signal 2N+1; a second batch waits for 2N+2 (the
//    helper's DLSS) and copies the shared output into a D3D9 16F texture.
// 3. D3D9: the output goes back into the display image, alpha kept.
//
// The CPU never waits for the helper. The only wait is DXVK draining its own
// command stream (FlushRenderingCommands + LockSubmissionQueue). A watchdog
// thread releases the GPU wait if the helper dies or stalls, and the frame
// then falls back to TAA. Jitter comes from the TAA module.
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include "dxvk_interop.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_DLSS/shared/wos_dlss_ipc.h"

namespace WosDlss {

struct Host {
    HRESULT (STDMETHODCALLTYPE* setVS)(IDirect3DDevice9*, IDirect3DVertexShader9*) = nullptr;
    HRESULT (STDMETHODCALLTYPE* setPS)(IDirect3DDevice9*, IDirect3DPixelShader9*) = nullptr;
    HRESULT (STDMETHODCALLTYPE* drawUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT) = nullptr;
};
static Host host;

// ------------------------------------------------------------------ settings
static bool enabled = false;                 // DLAA chosen in the menu
static int preset = 11;                      // NVSDK_NGX_DLSS_Hint_Render_Preset_K (0 = driver default)
// TAA pixel jitter (geometry shift, y down) -> DLSS jitter offset, and the
// UV-delta motion -> DLSS pixels. Both measured by WoS_DLSS/test/test_dlss.exe
// against a 4x4 supersampled reference (2026-10-12): (+1,+1) converges to
// 0.020 edge error vs 0.049 for (-1,-1); motion +1 pans at 0.017 vs 0.034.
static float jitterSign[2] = { 1.0f, 1.0f };
static float motionSign[2] = { 1.0f, 1.0f };
static int debugView = 0;                    // 0 DLAA, 1 input without AA, 2 motion
static std::wstring hostExe;                 // ...\WoS_DLSS\bin\WoS_DLSS_Host.exe
static std::string shaderFolder;             // ...\WoS_Deferred\dlss
static std::string logPath;                  // ...\WoS_DLSS\bin\dlaa_game.log

// -------------------------------------------------------------------- status
static const char* status = "DLAA off";
static char detail[256] = "";
static unsigned long frames = 0, skipped = 0, failures = 0, generations = 0, resets = 0;
static float flushMs = 0.0f;                 // FlushRenderingCommands + queue lock, smoothed
static bool unavailable = false;             // the device is not DXVK
static bool faulted = false;

template<class T> static void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

static void Log(const char* format, ...) {
    if (logPath.empty()) return;
    FILE* f = std::fopen(logPath.c_str(), "a");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    std::fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args; va_start(args, format); std::vfprintf(f, format, args); va_end(args);
    std::fputc('\n', f); std::fclose(f);
}
static void Detail(const char* format, ...) {
    va_list args; va_start(args, format); std::vsnprintf(detail, sizeof(detail), format, args); va_end(args);
}

// --------------------------------------------------------------------- vulkan
#define WOS_DLSS_VK_INSTANCE(X) X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceExternalSemaphoreProperties)
#define WOS_DLSS_VK_DEVICE(X) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkBindImageMemory) X(vkGetMemoryWin32HandleKHR) X(vkCreateSemaphore) X(vkGetSemaphoreWin32HandleKHR) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyImage) X(vkQueueSubmit) X(vkSignalSemaphore) X(vkGetSemaphoreCounterValue) X(vkWaitSemaphores) X(vkCreateFence) \
    X(vkWaitForFences) X(vkResetFences)
struct Vk {
#define WOS_DLSS_VK_MEMBER(name) PFN_##name name = nullptr;
    WOS_DLSS_VK_INSTANCE(WOS_DLSS_VK_MEMBER)
    WOS_DLSS_VK_DEVICE(WOS_DLSS_VK_MEMBER)
#undef WOS_DLSS_VK_MEMBER
};
static Vk vk;

constexpr uint32_t kSlots = 4;

struct Bridge {
    IDirect3DDevice9* device = nullptr;      // the D3D9 device the bridge belongs to (not referenced)
    ID3D9VkInteropDevice* interop = nullptr;
    VkInstance instance = VK_NULL_HANDLE; VkPhysicalDevice physical = VK_NULL_HANDLE; VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE; uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory = {};
    uint8_t deviceUUID[16] = {}, driverUUID[16] = {};
    VkSemaphore timeline = VK_NULL_HANDLE;   // shared with the helper
    VkSemaphore recycle = VK_NULL_HANDLE;    // local: value = last serial whose copy-out finished
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd[2 * kSlots] = {}, initCmd = VK_NULL_HANDLE;
    VkFence initFence = VK_NULL_HANDLE; bool initPending = false;
    uint32_t slotSerial[kSlots] = {};
    HANDLE mapping = nullptr, event = nullptr;
    WosDlssIpc::Shared* shared = nullptr;
    PROCESS_INFORMATION hostProcess = {};
    HANDLE job = nullptr;                    // kill-on-close: the helper dies with the game
    HANDLE watchdog = nullptr;
    // The render thread and the watchdog share no lock: the render thread can
    // sit in DXVK (flush, queue lock) exactly while the GPU waits for a stalled
    // helper, and only the watchdog's release can move it again.
    volatile LONG hostDead = 0;              // set once by the watchdog; Submit refuses afterwards
    volatile LONG submitting = 0;            // render thread inside Submit (between the check and the serial)
    volatile uint32_t serial = 0;            // last frame submitted on DXVK's queue
    bool started = false;
};
static Bridge b;

struct SharedImage { VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkFormat format = VK_FORMAT_UNDEFINED; uint32_t usage = 0; VkDeviceSize size = 0; };

// Targets of one size. D3D9 objects go on device Reset; the shared Vulkan
// images and the published configuration survive it.
struct Targets {
    UINT width = 0, height = 0; D3DFORMAT colorFormat = D3DFMT_UNKNOWN;
    IDirect3DTexture9* tex[WosDlssIpc::kImageCount] = {};
    IDirect3DSurface9* surf[WosDlssIpc::kImageCount] = {};
    ID3D9VkInteropTexture* interop[WosDlssIpc::kImageCount] = {};
    IDirect3DPixelShader9* prepPS = nullptr, * presentPS = nullptr;
    void ReleaseD3D9() {
        for (uint32_t i = 0; i < WosDlssIpc::kImageCount; ++i) { Release(interop[i]); Release(surf[i]); Release(tex[i]); }
        Release(prepPS); Release(presentPS);
    }
    // Shared side
    UINT sharedWidth = 0, sharedHeight = 0; VkFormat sharedColor = VK_FORMAT_UNDEFINED;
    SharedImage shared[WosDlssIpc::kImageCount];
    uint32_t generation = 0;
};
static Targets t;
static std::vector<unsigned char> prepCode, presentCode;

// History bookkeeping, mirroring the TAA resolve.
static unsigned frameNumber = 1, lastResolvedFrame = 0;
static bool historyValid = false;
static float prevRelativeInverse[16] = {};
static double prevOrigin[3] = {};
static LARGE_INTEGER lastFrameTime = {};

static bool LoadShaders(const std::string& folder) {
    auto read = [](const std::string& path) {
        std::vector<unsigned char> bytes;
        FILE* f = std::fopen(path.c_str(), "rb"); if (!f) return bytes;
        std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::rewind(f);
        if (n > 0 && n < (1 << 20) && n % 4 == 0) { bytes.resize(size_t(n)); if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear(); }
        std::fclose(f); return bytes;
    };
    shaderFolder = folder;
    prepCode = read(folder + "\\dlss_prep.cso");
    presentCode = read(folder + "\\dlss_present.cso");
    Release(t.prepPS); Release(t.presentPS);
    return !prepCode.empty() && !presentCode.empty();
}

static bool HostAlive() {
    return b.hostProcess.hProcess && !b.hostDead && WaitForSingleObject(b.hostProcess.hProcess, 0) == WAIT_TIMEOUT;
}

static uint64_t TimelineValue() {
    uint64_t value = 0;
    if (b.timeline) vk.vkGetSemaphoreCounterValue(b.vkDevice, b.timeline, &value);
    return value;
}

// The helper is dead: host-signal the outputs it owes, strictly in order. An
// output is signalled only once the GPU has reached that frame's input, so a
// later copy-in never signals a value below the counter (a timeline only
// grows; on Windows a monitored fence would really go backwards and hang).
// Runs on the watchdog thread only, after hostDead is set: it ends when the
// last submitted frame is released and no Submit is still in flight.
static void ReleaseInOrder() {
    unsigned released = 0;
    for (;;) {
        const LONG busy = b.submitting;
        MemoryBarrier();
        const uint32_t last = b.serial;
        uint64_t value = 0;
        if (vk.vkGetSemaphoreCounterValue(b.vkDevice, b.timeline, &value) != VK_SUCCESS) { Log("release: counter unreadable"); return; }
        if (value >= WosDlssIpc::OutputValue(last)) {
            if (!busy && last == b.serial) break;
            Sleep(1);                                   // a Submit is finishing; its frame comes next
        } else if (value & 1) {                         // input of frame (value-1)/2 reached, its output owed
            VkSemaphoreSignalInfo signal = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
            signal.semaphore = b.timeline; signal.value = value + 1;
            const VkResult r = vk.vkSignalSemaphore(b.vkDevice, &signal);
            if (r != VK_SUCCESS) { Log("release: signal %llu failed (%d)", signal.value, int(r)); return; }
            ++released;
        } else {                                        // wait for the next copy-in to run
            const uint64_t next = value + 1;
            VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
            wait.semaphoreCount = 1; wait.pSemaphores = &b.timeline; wait.pValues = &next;
            vk.vkWaitSemaphores(b.vkDevice, &wait, 50000000ull);
        }
    }
    Log("released DXVK's queue: %u frame(s) host-signalled, last frame %u, timeline %llu", released, b.serial, TimelineValue());
}

static void HostLost(const char* why, bool terminate) {
    InterlockedExchange(&b.hostDead, 1);
    if (terminate) { TerminateProcess(b.hostProcess.hProcess, 0xDEAD); WaitForSingleObject(b.hostProcess.hProcess, 5000); }
    Log("helper lost: %s; last frame %u", why, b.serial);
    ReleaseInOrder();
}

// Watches the GPU, not the CPU counters: the helper is stalled when the GPU
// owes output and the timeline has not moved for 2 s.
static DWORD WINAPI Watchdog(void*) {
    DWORD stallSince = 0; uint64_t lastValue = 0; uint32_t lastSubmitted = 0;
    for (;;) {
        if (WaitForSingleObject(b.hostProcess.hProcess, 50) == WAIT_OBJECT_0) {
            DWORD code = 0; GetExitCodeProcess(b.hostProcess.hProcess, &code);
            char why[64]; std::snprintf(why, sizeof(why), "helper exited with code %lu", code);
            HostLost(why, false);
            return 0;
        }
        const uint32_t last = b.serial, submitted = b.shared->framesSubmitted;
        const uint64_t value = TimelineValue();
        const bool owed = last && value < WosDlssIpc::OutputValue(last);
        if (!owed || value != lastValue || submitted != lastSubmitted) { stallSince = 0; lastValue = value; lastSubmitted = submitted; continue; }
        const DWORD now = GetTickCount();
        if (!stallSince) stallSince = now;
        else if (now - stallSince > 2000) {
            HostLost("no DLSS progress on the GPU for 2 s; helper terminated", true);
            return 0;
        }
    }
}

static bool Fault(const char* why) {
    faulted = true; ++failures; status = "DLAA stopped; TAA used"; Detail("%s", why);
    Log("fault: %s", why);
    return false;
}

// Interop, function pointers, the shared semaphore, the helper process.
static bool Start(IDirect3DDevice9* d) {
    if (b.started) {
        if (b.device != d) { status = "DLAA bound to an earlier D3D9 device; TAA used"; return false; }
        return true;
    }
    if (unavailable) return false;
    if (FAILED(d->QueryInterface(__uuidof(ID3D9VkInteropDevice), reinterpret_cast<void**>(&b.interop))) || !b.interop) {
        unavailable = true; status = "DLAA needs DXVK (Play - DXVK Test.cmd); TAA used";
        Log("no DXVK interop on this device; DLAA unavailable");
        return false;
    }
    b.device = d;
    b.started = true;
    b.interop->GetVulkanHandles(&b.instance, &b.physical, &b.vkDevice);
    uint32_t queueIndex = 0;
    b.interop->GetSubmissionQueue(&b.queue, &queueIndex, &b.family);
    HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    auto gipa = loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr")) : nullptr;
    if (!gipa || !b.instance || !b.vkDevice || !b.queue) return Fault("DXVK interop returned no Vulkan handles");
#define WOS_DLSS_VK_LOAD_I(name) vk.name = reinterpret_cast<PFN_##name>(gipa(b.instance, #name)); if (!vk.name) return Fault("missing " #name);
    WOS_DLSS_VK_INSTANCE(WOS_DLSS_VK_LOAD_I)
#define WOS_DLSS_VK_LOAD_D(name) vk.name = reinterpret_cast<PFN_##name>(vk.vkGetDeviceProcAddr(b.vkDevice, #name)); if (!vk.name) return Fault("missing " #name " (extension not enabled by DXVK?)");
    WOS_DLSS_VK_DEVICE(WOS_DLSS_VK_LOAD_D)
#undef WOS_DLSS_VK_LOAD_I
#undef WOS_DLSS_VK_LOAD_D
    vk.vkGetPhysicalDeviceMemoryProperties(b.physical, &b.memory);
    VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 properties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
    vk.vkGetPhysicalDeviceProperties2(b.physical, &properties);
    std::memcpy(b.deviceUUID, id.deviceUUID, 16); std::memcpy(b.driverUUID, id.driverUUID, 16);
    if (properties.properties.vendorID != 0x10DE) return Fault("DLSS needs an NVIDIA GPU");

    VkSemaphoreTypeCreateInfo timelineType = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkPhysicalDeviceExternalSemaphoreInfo query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &timelineType };
    query.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkExternalSemaphoreProperties semaphoreSupport = { VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
    vk.vkGetPhysicalDeviceExternalSemaphoreProperties(b.physical, &query, &semaphoreSupport);
    if (!(semaphoreSupport.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) return Fault("timeline semaphore cannot be exported");
    VkExportSemaphoreCreateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, &timelineType };
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &exportInfo };
    if (vk.vkCreateSemaphore(b.vkDevice, &semaphoreInfo, nullptr, &b.timeline) != VK_SUCCESS) return Fault("shared timeline creation failed");
    semaphoreInfo.pNext = &timelineType;
    if (vk.vkCreateSemaphore(b.vkDevice, &semaphoreInfo, nullptr, &b.recycle) != VK_SUCCESS) return Fault("local timeline creation failed");
    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; poolInfo.queueFamilyIndex = b.family;
    if (vk.vkCreateCommandPool(b.vkDevice, &poolInfo, nullptr, &b.pool) != VK_SUCCESS) return Fault("command pool");
    VkCommandBufferAllocateInfo allocate = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocate.commandPool = b.pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 2 * kSlots;
    if (vk.vkAllocateCommandBuffers(b.vkDevice, &allocate, b.cmd) != VK_SUCCESS) return Fault("command buffers");
    allocate.commandBufferCount = 1;
    if (vk.vkAllocateCommandBuffers(b.vkDevice, &allocate, &b.initCmd) != VK_SUCCESS) return Fault("command buffers");
    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vk.vkCreateFence(b.vkDevice, &fenceInfo, nullptr, &b.initFence) != VK_SUCCESS) return Fault("fence");

    const DWORD pid = GetCurrentProcessId();
    wchar_t name[96];
    std::swprintf(name, 96, WOS_DLSS_SHARED_NAME, pid);
    b.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(WosDlssIpc::Shared), name);
    if (b.mapping) b.shared = static_cast<WosDlssIpc::Shared*>(MapViewOfFile(b.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(WosDlssIpc::Shared)));
    std::swprintf(name, 96, WOS_DLSS_EVENT_NAME, pid);
    b.event = CreateEventW(nullptr, FALSE, FALSE, name);
    if (!b.shared || !b.event) return Fault("shared memory or event creation failed");
    std::memset(b.shared, 0, sizeof(WosDlssIpc::Shared));
    b.shared->magic = WosDlssIpc::kMagic; b.shared->version = WosDlssIpc::kVersion; b.shared->gamePid = pid;

    std::wstring command = L"\"" + hostExe + L"\" " + std::to_wstring(pid);
    std::wstring folder = hostExe.substr(0, hostExe.find_last_of(L"\\/"));
    STARTUPINFOW si = { sizeof(si) };
    // Suspended until it sits in a kill-on-close job, so it cannot outlive a
    // crashed game. Above-normal priority and all CPUs: its wake-up is on the
    // GPU's critical path, and the game pins itself to 4 CPUs.
    if (!CreateProcessW(hostExe.c_str(), &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED | ABOVE_NORMAL_PRIORITY_CLASS,
                        nullptr, folder.c_str(), &si, &b.hostProcess)) {
        char why[128]; std::snprintf(why, sizeof(why), "cannot start WoS_DLSS_Host.exe (error %lu)", GetLastError());
        return Fault(why);
    }
    b.job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!b.job || !SetInformationJobObject(b.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) || !AssignProcessToJobObject(b.job, b.hostProcess.hProcess))
        Log("helper not in a kill-on-close job (error %lu); it exits on its own when the game does", GetLastError());
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(b.hostProcess.hProcess, &processMask, &systemMask) && systemMask) SetProcessAffinityMask(b.hostProcess.hProcess, systemMask);
    ResumeThread(b.hostProcess.hThread);
    CloseHandle(b.hostProcess.hThread); b.hostProcess.hThread = nullptr;
    b.watchdog = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    Log("started: DXVK queue family %u, helper pid %lu, GPU %s", b.family, b.hostProcess.dwProcessId, properties.properties.deviceName);
    status = "DLAA: helper starting";
    return true;
}

// Exportable image on DXVK's device, dedicated allocation, same create info
// the helper uses for its alias.
static bool CreateShared(SharedImage& out, VkFormat format, uint32_t usage, UINT w, UINT h) {
    out.format = format; out.usage = usage;
    VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external };
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = { w, h, 1 }; info.mipLevels = 1; info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE; info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.vkCreateImage(b.vkDevice, &info, nullptr, &out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements = {}; vk.vkGetImageMemoryRequirements(b.vkDevice, out.image, &requirements);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < b.memory.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) && (b.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    if (type == UINT32_MAX) return false;
    VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicated.image = out.image;
    VkExportMemoryAllocateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated };
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exportInfo };
    allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
    if (vk.vkAllocateMemory(b.vkDevice, &allocate, nullptr, &out.memory) != VK_SUCCESS) return false;
    out.size = requirements.size;
    return vk.vkBindImageMemory(b.vkDevice, out.image, out.memory, 0) == VK_SUCCESS;
}

static void DestroyShared() {
    for (SharedImage& image : t.shared) {
        if (image.image) vk.vkDestroyImage(b.vkDevice, image.image, nullptr);
        if (image.memory) vk.vkFreeMemory(b.vkDevice, image.memory, nullptr);
        image = SharedImage{};
    }
    t.sharedWidth = t.sharedHeight = 0; t.sharedColor = VK_FORMAT_UNDEFINED;
}

// Everything submitted so far has finished on the GPU (the copy-out of the
// last frame waited for the helper, so its DLSS is done too).
static bool WaitIdleFrames(uint64_t timeoutNs) {
    if (!b.serial) return true;
    const uint64_t value = b.serial;
    VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wait.semaphoreCount = 1; wait.pSemaphores = &b.recycle; wait.pValues = &value;
    return vk.vkWaitSemaphores(b.vkDevice, &wait, timeoutNs) == VK_SUCCESS;
}

static void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, uint32_t srcFamily, uint32_t dstFamily,
                         VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier m = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    m.srcAccessMask = srcAccess; m.dstAccessMask = dstAccess; m.oldLayout = from; m.newLayout = to;
    m.srcQueueFamilyIndex = srcFamily; m.dstQueueFamilyIndex = dstFamily; m.image = image;
    m.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &m);
}

static HANDLE ToHost(HANDLE own) {
    HANDLE remote = nullptr;
    if (!own) return nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), own, b.hostProcess.hProcess, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) return nullptr;
    return remote;
}

// Our copies use the D3D9 images' VkImages behind DXVK's back, so DXVK does
// not keep them alive for us: let the last frame finish before releasing.
static void ReleaseD3D9Targets() {
    if (b.started && b.recycle && t.tex[0] && !WaitIdleFrames(2000000000ull)) {
        // Frames still owe the helper: stop it, and the watchdog releases them in order.
        Log("D3D9 targets: DLAA frames still pending after 2 s; stopping the helper");
        // No further submits from here (the watchdog's release still runs on
        // the helper's exit): a dying helper's timeline jumps to UINT64_MAX on
        // NVIDIA, and a later copy-in would signal below it.
        InterlockedExchange(&b.hostDead, 1);
        if (b.hostProcess.hProcess) TerminateProcess(b.hostProcess.hProcess, 0xDEAD);
        if (!WaitIdleFrames(5000000000ull)) Log("D3D9 targets released while DLAA frames were still pending");
    }
    t.ReleaseD3D9();
    t.width = t.height = 0;
}

// D3D9 textures of this size and their Vulkan images.
static bool EnsureD3D9(IDirect3DDevice9* d, const D3DSURFACE_DESC& desc) {
    if (t.tex[0] && t.width == desc.Width && t.height == desc.Height && t.colorFormat == desc.Format) return true;
    ReleaseD3D9Targets();
    t.width = t.height = 0;
    if (prepCode.empty() || presentCode.empty()) return Fault("DLAA shaders missing in WoS_Deferred\\dlss");
    const D3DFORMAT formats[WosDlssIpc::kImageCount] = { desc.Format, D3DFMT_R32F, D3DFMT_G16R16F, D3DFMT_A16B16G16R16F };
    for (uint32_t i = 0; i < WosDlssIpc::kImageCount; ++i) {
        if (FAILED(d->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, formats[i], D3DPOOL_DEFAULT, &t.tex[i], nullptr)) ||
            FAILED(t.tex[i]->GetSurfaceLevel(0, &t.surf[i])) ||
            FAILED(t.tex[i]->QueryInterface(__uuidof(ID3D9VkInteropTexture), reinterpret_cast<void**>(&t.interop[i]))) || !t.interop[i]) {
            t.ReleaseD3D9(); return Fault("DLAA target creation failed");
        }
        d->ColorFill(t.surf[i], nullptr, 0);
    }
    if (FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(prepCode.data()), &t.prepPS)) ||
        FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(presentCode.data()), &t.presentPS))) { t.ReleaseD3D9(); return Fault("DLAA shader creation failed"); }
    t.width = desc.Width; t.height = desc.Height; t.colorFormat = desc.Format;
    historyValid = false;
    return true;
}

// Shared images of this size, the one-time layout init, and the published
// configuration. Returns true once the helper has accepted it.
static bool EnsureShared() {
    VkImage image = VK_NULL_HANDLE; VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    if (FAILED(t.interop[WosDlssIpc::kColor]->GetVulkanImageInfo(&image, &layout, &info))) return Fault("colour image info unavailable");
    const VkFormat color = info.format;
    if (b.shared->hostState == WosDlssIpc::kHostFailed) {
        char why[300]; std::snprintf(why, sizeof(why), "helper: %s", b.shared->status);
        return Fault(why);
    }
    if (t.sharedWidth != t.width || t.sharedHeight != t.height || t.sharedColor != color) {
        // One configuration at a time: the helper copies the config while it
        // applies it, so a newer one must wait until the previous is accepted
        // (a resize drag otherwise tears the copy and leaks skipped handles).
        if (t.generation && b.shared->hostGeneration != t.generation) {
            status = "DLAA: helper still applying the previous size; TAA meanwhile";
            return false;
        }
        if (!WaitIdleFrames(2000000000ull)) return Fault("previous DLAA frames did not finish");
        if (b.initPending) {
            if (vk.vkWaitForFences(b.vkDevice, 1, &b.initFence, VK_TRUE, 2000000000ull) != VK_SUCCESS) return Fault("shared image init did not finish");
            b.initPending = false;
        }
        DestroyShared();
        const VkFormat formats[WosDlssIpc::kImageCount] = { color, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT };
        const uint32_t input = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        const uint32_t output = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        for (uint32_t i = 0; i < WosDlssIpc::kImageCount; ++i)
            if (!CreateShared(t.shared[i], formats[i], i == WosDlssIpc::kOutput ? output : input, t.width, t.height)) { DestroyShared(); return Fault("shared image creation failed"); }
        t.sharedWidth = t.width; t.sharedHeight = t.height; t.sharedColor = color;

        // UNDEFINED -> GENERAL, then hand ownership to the helper side.
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.vkResetCommandBuffer(b.initCmd, 0);
        vk.vkBeginCommandBuffer(b.initCmd, &begin);
        const VkAccessFlags any = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        for (const SharedImage& s : t.shared) {
            ImageBarrier(b.initCmd, s.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, 0, any);
            ImageBarrier(b.initCmd, s.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, b.family, VK_QUEUE_FAMILY_EXTERNAL, any, 0);
        }
        vk.vkEndCommandBuffer(b.initCmd);
        vk.vkResetFences(b.vkDevice, 1, &b.initFence);
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1; submit.pCommandBuffers = &b.initCmd;
        b.interop->LockSubmissionQueue();
        const VkResult vr = vk.vkQueueSubmit(b.queue, 1, &submit, b.initFence);
        b.interop->ReleaseSubmissionQueue();
        if (vr != VK_SUCCESS) return Fault("shared image init submit failed");
        b.initPending = true;

        WosDlssIpc::Config& config = b.shared->config;
        config.width = t.width; config.height = t.height; config.preset = preset; config.createFlags = 0; // LDR, full-res MVs without jitter, depth 0 = near
        std::memcpy(config.deviceUUID, b.deviceUUID, 16); std::memcpy(config.driverUUID, b.driverUUID, 16);
        // An NT handle may be exported only once per semaphore: the first
        // generation carries it, the helper keeps its import for later ones.
        HANDLE own = nullptr;
        if (!config.timelineHandle) {
            VkSemaphoreGetWin32HandleInfoKHR semaphoreHandle = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
            semaphoreHandle.semaphore = b.timeline; semaphoreHandle.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            if (vk.vkGetSemaphoreWin32HandleKHR(b.vkDevice, &semaphoreHandle, &own) != VK_SUCCESS || !(config.timelineHandle = uint64_t(uintptr_t(ToHost(own)))))
                return Fault("timeline export failed");
        }
        for (uint32_t i = 0; i < WosDlssIpc::kImageCount; ++i) {
            VkMemoryGetWin32HandleInfoKHR memoryHandle = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
            memoryHandle.memory = t.shared[i].memory; memoryHandle.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            own = nullptr;
            WosDlssIpc::ImageDesc& d = config.images[i];
            d.format = uint32_t(t.shared[i].format); d.usage = t.shared[i].usage; d.width = t.width; d.height = t.height; d.flags = 0;
            d.allocationSize = t.shared[i].size;
            if (vk.vkGetMemoryWin32HandleKHR(b.vkDevice, &memoryHandle, &own) != VK_SUCCESS || !(d.handle = uint64_t(uintptr_t(ToHost(own)))))
                return Fault("image export failed");
        }
        t.generation = ++generations;
        MemoryBarrier();
        config.generation = t.generation;
        SetEvent(b.event);
        historyValid = false;
        Log("published %ux%u colour format %d as generation %u", t.width, t.height, int(color), t.generation);
    }
    if (b.shared->hostState != WosDlssIpc::kHostReady || b.shared->hostGeneration != t.generation) {
        status = "DLAA: waiting for the helper; TAA meanwhile";
        Detail("%s", b.shared->status);
        return false;
    }
    return true;
}

// D3DSBT_ALL omits render targets and depth surfaces. Keep those explicitly.
struct State {
    IDirect3DDevice9* d; IDirect3DStateBlock9* block = nullptr;
    IDirect3DSurface9* rt[4] = {}, * depth = nullptr;
    D3DVIEWPORT9 vp = {}; bool valid = false;
    explicit State(IDirect3DDevice9* dev) : d(dev) {
        for (UINT i = 0; i < 4; ++i) { HRESULT hr = d->GetRenderTarget(i, &rt[i]); if (FAILED(hr) && (i == 0 || hr != D3DERR_NOTFOUND)) return; }
        HRESULT hr = d->GetDepthStencilSurface(&depth); if (FAILED(hr) && hr != D3DERR_NOTFOUND) return;
        if (FAILED(d->GetViewport(&vp))) return;
        valid = SUCCEEDED(d->CreateStateBlock(D3DSBT_ALL, &block)) && SUCCEEDED(block->Capture());
    }
    bool Restore() {
        bool ok = true; auto check = [&](HRESULT h) { ok = SUCCEEDED(h) && ok; };
        for (UINT i = 0; i < 16; ++i) check(d->SetTexture(i, nullptr));
        check(d->SetDepthStencilSurface(nullptr));
        for (UINT i = 1; i < 4; ++i) check(d->SetRenderTarget(i, nullptr));
        for (UINT i = 0; i < 4; ++i) if (rt[i]) check(d->SetRenderTarget(i, rt[i]));
        check(d->SetDepthStencilSurface(depth)); check(block->Apply()); check(d->SetViewport(&vp));
        return ok;
    }
    ~State() { Release(block); for (auto*& p : rt) Release(p); Release(depth); }
};

inline bool Inverse(const double m[16], double out[16]) {
    double a[4][8] = {};
    for (int i = 0; i < 4; ++i) { for (int j = 0; j < 4; ++j) a[i][j] = m[i * 4 + j]; a[i][4 + i] = 1; }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int i = c + 1; i < 4; ++i) if (std::fabs(a[i][c]) > std::fabs(a[pivot][c])) pivot = i;
        if (!(std::fabs(a[pivot][c]) > 1e-300)) return false;
        if (pivot != c) for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
        const double inv = 1.0 / a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] *= inv;
        for (int i = 0; i < 4; ++i) if (i != c) { const double f = a[i][c]; if (f != 0) for (int j = 0; j < 8; ++j) a[i][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 16; ++i) { out[i] = a[i / 4][4 + i % 4]; if (!std::isfinite(out[i])) return false; }
    return true;
}

// c0-c3 current camera-relative inverse VP rows, c4-c7 previous VP rows that
// accept current camera-relative positions, c8 metrics, c9 jitter (clip) and
// history flag. Same layout and maths as the TAA resolve. False = reset.
static bool PrepConstants(const float relativeInverse[16], const double origin[3], bool history, const float jitterNdc[2], float c[40]) {
    std::memset(c, 0, 40 * sizeof(float));
    std::memcpy(c, relativeInverse, 64);
    double delta[3];
    for (int i = 0; i < 3; ++i) delta[i] = origin[i] - prevOrigin[i];
    if (history && !(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2] < 32.0 * 32.0)) history = false;
    if (history) {
        double m[16], prev[16];
        for (int i = 0; i < 16; ++i) m[i] = prevRelativeInverse[i];
        history = Inverse(m, prev);
        for (int row = 0; row < 4 && history; ++row)
            for (int col = 0; col < 4; ++col) {
                double v = prev[row * 4 + col];
                if (col == 3) v += prev[row * 4] * delta[0] + prev[row * 4 + 1] * delta[1] + prev[row * 4 + 2] * delta[2];
                if (!std::isfinite(v)) { history = false; break; }
                c[16 + row * 4 + col] = float(v);
            }
    }
    c[32] = 1.0f / float(t.width); c[33] = 1.0f / float(t.height); c[34] = float(t.width); c[35] = float(t.height);
    c[36] = jitterNdc[0]; c[37] = jitterNdc[1];
    c[38] = history ? 1.0f : 0.0f;
    return history;
}

static bool Quad(IDirect3DDevice9* d) {
    struct V { float x, y, z, rhw, u, v; };
    const float w = float(t.width), h = float(t.height);
    const V q[] = { {-.5f,-.5f,0,1,0,0}, {w - .5f,-.5f,0,1,1,0}, {-.5f,h - .5f,0,1,0,1}, {w - .5f,h - .5f,0,1,1,1} };
    D3DVIEWPORT9 vp = { 0, 0, t.width, t.height, 0, 1 };
    return SUCCEEDED(d->SetViewport(&vp)) && SUCCEEDED(host.drawUP(d, D3DPT_TRIANGLESTRIP, 2, q, sizeof(V)));
}

enum class Sent { Ok, Skip, Fault };

// Copy-in (batch 1) and copy-out (batch 2) on DXVK's queue.
static Sent Submit(uint32_t serial) {
    const uint32_t slot = serial % kSlots;
    if (b.slotSerial[slot]) {
        // A slot still busy means the helper is 4 frames behind: TAA this
        // frame. A real stall is the watchdog's call, not one hitch's.
        const uint64_t value = b.slotSerial[slot];
        VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wait.semaphoreCount = 1; wait.pSemaphores = &b.recycle; wait.pValues = &value;
        if (vk.vkWaitSemaphores(b.vkDevice, &wait, 2000000ull) != VK_SUCCESS) return Sent::Skip;
    }
    // Announce the submit before checking hostDead (both interlocked): either
    // this thread sees the helper gone, or the watchdog's release sees us busy
    // and keeps releasing until our frame is through.
    InterlockedIncrement(&b.submitting);
    struct Leave { ~Leave() { InterlockedDecrement(&b.submitting); } } leave;
    if (b.hostDead || WaitForSingleObject(b.hostProcess.hProcess, 0) != WAIT_TIMEOUT) { Fault("helper is gone"); return Sent::Fault; }

    // DXVK must have submitted everything recorded so far (our D3D9 passes
    // included) before our batches enter its queue; image handles are read
    // after the flush, when DXVK's recording has stopped.
    LARGE_INTEGER t0, t1, freq; QueryPerformanceCounter(&t0);
    b.interop->FlushRenderingCommands();
    VkImage d3d9[WosDlssIpc::kImageCount] = {}; VkImageLayout rest[WosDlssIpc::kImageCount] = {};
    for (uint32_t i = 0; i < WosDlssIpc::kImageCount; ++i) {
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        if (FAILED(t.interop[i]->GetVulkanImageInfo(&d3d9[i], &rest[i], &info)) || !d3d9[i]) { Fault("D3D9 image info unavailable"); return Sent::Fault; }
        if (rest[i] == VK_IMAGE_LAYOUT_UNDEFINED || rest[i] == VK_IMAGE_LAYOUT_PREINITIALIZED) rest[i] = VK_IMAGE_LAYOUT_GENERAL;
    }
    const VkAccessFlags any = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    const uint32_t ext = VK_QUEUE_FAMILY_EXTERNAL, own = b.family, none = VK_QUEUE_FAMILY_IGNORED;
    VkImageCopy region = {};
    region.srcSubresource = region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { t.width, t.height, 1 };
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkCommandBuffer in = b.cmd[2 * slot], out = b.cmd[2 * slot + 1];
    vk.vkResetCommandBuffer(in, 0); vk.vkBeginCommandBuffer(in, &begin);
    for (uint32_t i : { WosDlssIpc::kColor, WosDlssIpc::kDepth, WosDlssIpc::kMotion }) {
        ImageBarrier(in, d3d9[i], rest[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, none, none, any, VK_ACCESS_TRANSFER_READ_BIT);
        ImageBarrier(in, t.shared[i].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ext, own, 0, any);
        ImageBarrier(in, t.shared[i].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, none, none, any, VK_ACCESS_TRANSFER_WRITE_BIT);
        vk.vkCmdCopyImage(in, d3d9[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.shared[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        ImageBarrier(in, d3d9[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rest[i], none, none, VK_ACCESS_TRANSFER_READ_BIT, any);
        ImageBarrier(in, t.shared[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, none, none, VK_ACCESS_TRANSFER_WRITE_BIT, any);
        ImageBarrier(in, t.shared[i].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, own, ext, any, 0);
    }
    vk.vkEndCommandBuffer(in);

    const uint32_t o = WosDlssIpc::kOutput;
    vk.vkResetCommandBuffer(out, 0); vk.vkBeginCommandBuffer(out, &begin);
    ImageBarrier(out, t.shared[o].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ext, own, 0, any);
    ImageBarrier(out, t.shared[o].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, none, none, any, VK_ACCESS_TRANSFER_READ_BIT);
    ImageBarrier(out, d3d9[o], rest[o], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, none, none, any, VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.vkCmdCopyImage(out, t.shared[o].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d3d9[o], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(out, d3d9[o], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rest[o], none, none, VK_ACCESS_TRANSFER_WRITE_BIT, any);
    ImageBarrier(out, t.shared[o].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, none, none, VK_ACCESS_TRANSFER_READ_BIT, any);
    ImageBarrier(out, t.shared[o].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, own, ext, any, 0);
    vk.vkEndCommandBuffer(out);

    const uint64_t inputValue = WosDlssIpc::InputValue(serial), outputValue = WosDlssIpc::OutputValue(serial), recycleValue = serial;
    VkTimelineSemaphoreSubmitInfo inValues = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    inValues.signalSemaphoreValueCount = 1; inValues.pSignalSemaphoreValues = &inputValue;
    VkTimelineSemaphoreSubmitInfo outValues = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    outValues.waitSemaphoreValueCount = 1; outValues.pWaitSemaphoreValues = &outputValue;
    outValues.signalSemaphoreValueCount = 1; outValues.pSignalSemaphoreValues = &recycleValue;
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submits[2] = { { VK_STRUCTURE_TYPE_SUBMIT_INFO, &inValues }, { VK_STRUCTURE_TYPE_SUBMIT_INFO, &outValues } };
    submits[0].commandBufferCount = 1; submits[0].pCommandBuffers = &in;
    submits[0].signalSemaphoreCount = 1; submits[0].pSignalSemaphores = &b.timeline;
    submits[1].waitSemaphoreCount = 1; submits[1].pWaitSemaphores = &b.timeline; submits[1].pWaitDstStageMask = &stage;
    submits[1].commandBufferCount = 1; submits[1].pCommandBuffers = &out;
    submits[1].signalSemaphoreCount = 1; submits[1].pSignalSemaphores = &b.recycle;

    b.interop->LockSubmissionQueue();
    const VkResult vr = vk.vkQueueSubmit(b.queue, 2, submits, VK_NULL_HANDLE);
    b.interop->ReleaseSubmissionQueue();
    if (vr == VK_SUCCESS) { b.slotSerial[slot] = serial; b.serial = serial; } // visible before 'submitting' drops
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&freq);
    const float ms = float(double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart));
    flushMs = flushMs > 0.0f ? flushMs * 0.95f + ms * 0.05f : ms;
    if (vr != VK_SUCCESS) { char why[64]; std::snprintf(why, sizeof(why), "vkQueueSubmit failed (%d)", int(vr)); Fault(why); return Sent::Fault; }
    return Sent::Ok;
}

// Device lost: D3D9 targets go; shared images, the helper and its
// configuration stay (a new size republishes).
static void Reset() { ReleaseD3D9Targets(); historyValid = false; }

static void FrameEnd() { ++frameNumber; }

// Runs where the TAA resolve would. Returns true when the display image now
// holds the DLAA result; false leaves the frame to TAA.
static bool Resolve(IDirect3DDevice9* d, IDirect3DTexture9* depth, const float relativeInverse[16], const double origin[3],
                    bool jitterActive, const float jitterPixel[2], const float jitterNdc[2]) {
    if (!enabled || faulted || !host.setPS || !host.drawUP) return false;
    if (!Start(d) || faulted) return false;
    if (!HostAlive()) return Fault(b.hostDead ? "helper lost (see dlaa_game.log)" : "helper not running");
    State state(d);
    D3DSURFACE_DESC desc = {}, depthDesc = {};
    if (!state.valid || !state.rt[0] || FAILED(state.rt[0]->GetDesc(&desc)) || !depth || FAILED(depth->GetLevelDesc(0, &depthDesc))) {
        ++skipped; historyValid = false; return false;
    }
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE || state.vp.X || state.vp.Y || state.vp.Width != desc.Width || state.vp.Height != desc.Height ||
        desc.Width < 64 || desc.Height < 64 || depthDesc.Width != desc.Width || depthDesc.Height != desc.Height ||
        (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A16B16G16R16F)) {
        ++skipped; historyValid = false; return false;
    }
    if (!EnsureD3D9(d, desc) || !EnsureShared()) { historyValid = false; return false; }
    if (b.shared->framesWritten - b.shared->framesSubmitted >= WosDlssIpc::kRing - 2) {
        ++skipped; historyValid = false; status = "DLAA: helper behind; TAA this frame"; return false;
    }

    const bool consecutive = historyValid && lastResolvedFrame + 1 == frameNumber;
    const float zero[2] = { 0.0f, 0.0f };
    float c[40];
    const bool history = PrepConstants(relativeInverse, origin, consecutive, jitterActive ? jitterNdc : zero, c);

    bool ok = true; auto check = [&](HRESULT h) { ok = SUCCEEDED(h) && ok; };
    check(d->StretchRect(state.rt[0], nullptr, t.surf[WosDlssIpc::kColor], nullptr, D3DTEXF_NONE));
    for (UINT i = 0; i < 16; ++i) check(d->SetTexture(i, nullptr));
    check(d->SetDepthStencilSurface(nullptr));
    for (UINT i = 1; i < 4; ++i) check(d->SetRenderTarget(i, nullptr));
    if (host.setVS) check(host.setVS(d, nullptr));
    check(d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
    check(d->SetStreamSourceFreq(0, 1));
    const D3DRENDERSTATETYPE off[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE,
                                       D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_LIGHTING, D3DRS_CLIPPLANEENABLE };
    for (auto s : off) check(d->SetRenderState(s, 0));
    check(d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE)); check(d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID));
    check(d->SetRenderState(D3DRS_COLORWRITEENABLE, 15)); check(d->SetRenderState(D3DRS_COLORWRITEENABLE1, 15));
    check(d->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xffffffff));
    check(d->SetRenderState(D3DRS_WRAP0, 0));
    for (UINT s = 0; s < 3; ++s) {
        check(d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT)); check(d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
        check(d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
        check(d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP)); check(d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
        check(d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, 0)); check(d->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0));
        check(d->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0));
    }
    // 1. Motion and depth.
    check(d->SetPixelShaderConstantF(0, c, 10));
    check(d->SetRenderTarget(0, t.surf[WosDlssIpc::kMotion]));
    check(d->SetRenderTarget(1, t.surf[WosDlssIpc::kDepth]));
    check(d->SetTexture(0, depth));
    check(host.setPS(d, t.prepPS));
    if (ok) ok = Quad(d);
    // Motion and depth leave the framebuffer before the flush, so DXVK ends
    // the pass and puts them back in their resting layout for our copies.
    check(d->SetRenderTarget(0, state.rt[0]));
    check(d->SetRenderTarget(1, nullptr));
    check(d->SetTexture(0, nullptr));
    if (!ok) { state.Restore(); historyValid = false; ++failures; status = "DLAA prep pass failed; TAA this frame"; return false; }

    // 2. Hand over to the helper.
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    const float frameMs = lastFrameTime.QuadPart ? float(double(now.QuadPart - lastFrameTime.QuadPart) * 1000.0 / double(freq.QuadPart)) : 16.7f;
    lastFrameTime = now;
    const uint32_t serial = b.serial + 1;
    const Sent sent = Submit(serial);
    if (sent != Sent::Ok) {
        state.Restore(); historyValid = false;
        if (sent == Sent::Skip) { ++skipped; status = "DLAA: helper behind; TAA this frame"; }
        return false;
    }
    WosDlssIpc::Frame& f = b.shared->ring[serial % WosDlssIpc::kRing];
    f.serial = serial; f.generation = t.generation;
    f.inputReady = WosDlssIpc::InputValue(serial); f.outputReady = WosDlssIpc::OutputValue(serial);
    f.jitterX = jitterActive ? jitterSign[0] * jitterPixel[0] : 0.0f;
    f.jitterY = jitterActive ? jitterSign[1] * jitterPixel[1] : 0.0f;
    f.mvScaleX = motionSign[0] * float(t.width); f.mvScaleY = motionSign[1] * float(t.height);
    f.frameMs = (std::min)(frameMs, 250.0f);
    f.reset = history ? 0u : 1u;
    if (!history) ++resets;
    MemoryBarrier();
    b.shared->framesWritten = serial;
    SetEvent(b.event);

    // 3. Back into the display image.
    const float view[4] = { float(debugView), 0.0f, float(t.width), float(t.height) };
    check(d->SetRenderTarget(0, state.rt[0]));
    check(d->SetTexture(0, t.tex[WosDlssIpc::kOutput])); check(d->SetTexture(1, t.tex[WosDlssIpc::kColor])); check(d->SetTexture(2, t.tex[WosDlssIpc::kMotion]));
    check(d->SetPixelShaderConstantF(0, view, 1));
    check(host.setPS(d, t.presentPS));
    if (ok) ok = Quad(d);
    const bool restored = state.Restore();
    if (!ok || !restored) { historyValid = false; ++failures; if (!restored) return Fault("device state restore failed"); status = "DLAA present failed"; return false; }

    historyValid = true; lastResolvedFrame = frameNumber;
    std::memcpy(prevRelativeInverse, relativeInverse, sizeof(prevRelativeInverse));
    for (int i = 0; i < 3; ++i) prevOrigin[i] = origin[i];
    ++frames;
    status = "DLAA ON";
    Detail("%s | GPU %.2f ms | DXVK flush %.2f ms", b.shared->runtime, b.shared->gpuMs, flushMs);
    if (frames == 1 || frames % 3600 == 0)
        Log("frame %lu: %s, DLSS GPU %.2f ms, flush %.2f ms, resets %lu, skipped %lu, evaluate failures %u", frames, b.shared->runtime, b.shared->gpuMs, flushMs, resets, skipped, b.shared->evaluateFailures);
    return true;
}

} // namespace WosDlss
