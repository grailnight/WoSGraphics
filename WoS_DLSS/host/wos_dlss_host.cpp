// WoS_DLSS_Host.exe: runs NVIDIA DLSS in DLAA mode for Spider-Man: Web of Shadows.
//
// The game is a 32-bit D3D9 program and DLSS only exists for x64, so the
// renderer (winmm.dll, running under DXVK) starts this helper and shares four
// images and one timeline semaphore with it (see ../shared/wos_dlss_ipc.h).
// Per frame the helper waits on the GPU for the game's copy-in, runs DLSS and
// signals; the game's copy-out waits for that signal. Nothing here makes the
// game's CPU wait.
//
// Usage: WoS_DLSS_Host.exe <game pid>
// Log:   host.log next to the executable (rewritten each start).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "../shared/wos_dlss_ipc.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace WosDlssIpc;

// ----------------------------------------------------------------------- log
static FILE* g_log = nullptr;
static void Log(const char* format, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    std::fprintf(g_log, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args; va_start(args, format); std::vfprintf(g_log, format, args); va_end(args);
    std::fputc('\n', g_log); std::fflush(g_log);
}
static void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    if (!message) return;
    std::string line(message);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    Log("ngx: %s", line.c_str());
}

// -------------------------------------------------------------------- vulkan
#define WOS_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define WOS_VK_INSTANCE(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFeatures2) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define WOS_VK_DEVICE(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateImageView) \
    X(vkDestroyImageView) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkImportSemaphoreWin32HandleKHR) X(vkWaitSemaphores) \
    X(vkSignalSemaphore) X(vkGetSemaphoreCounterValue) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdPipelineBarrier) X(vkCmdBlitImage) X(vkCmdWriteTimestamp) X(vkCmdResetQueryPool) X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkQueueSubmit) X(vkQueueWaitIdle)
#define WOS_VK_DECLARE(name) static PFN_##name name = nullptr;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
WOS_VK_GLOBAL(WOS_VK_DECLARE)
WOS_VK_INSTANCE(WOS_VK_DECLARE)
WOS_VK_DEVICE(WOS_VK_DECLARE)

constexpr uint32_t kCmdRing = 8;
static const char* kProjectId = "6c0f2a9e-31d4-4b8e-9a57-3d1e0b7c5a42";

struct SharedImage {
    VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
    ImageDesc desc = {};
};

static struct State {
    Shared* shared = nullptr; HANDLE mapping = nullptr, event = nullptr, game = nullptr;
    std::wstring folder;
    HMODULE vulkan = nullptr;
    VkInstance instance = VK_NULL_HANDLE; VkPhysicalDevice physical = VK_NULL_HANDLE; VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE; uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory = {};
    float timestampPeriod = 0.0f; bool timestamps = false;
    VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cmd[kCmdRing] = {}; uint32_t cmdSerial[kCmdRing] = {};
    VkQueryPool queries = VK_NULL_HANDLE;
    VkSemaphore local = VK_NULL_HANDLE;   // host-local timeline: value = last serial whose command buffer finished
    VkSemaphore timeline = VK_NULL_HANDLE; // shared with the game
    SharedImage images[kImageCount];
    NVSDK_NGX_Parameter* params = nullptr; NVSDK_NGX_Handle* feature = nullptr;
    bool ngx = false, ready = false, resetNext = true;
    uint32_t generation = 0, width = 0, height = 0;
    std::wstring dataPath; std::vector<const wchar_t*> pathList;
    NVSDK_NGX_FeatureCommonInfo featureInfo = {};
} s;

static void SetStatus(const char* text) {
    std::snprintf(s.shared->status, sizeof(s.shared->status), "%s", text);
    Log("status: %s", text);
}
static bool Fail(const char* what, long code = 0) {
    char text[200]; std::snprintf(text, sizeof(text), "%s (%ld / 0x%08lx)", what, code, code);
    SetStatus(text);
    s.shared->hostState = kHostFailed;
    return false;
}

static NVSDK_NGX_FeatureDiscoveryInfo Discovery() {
    NVSDK_NGX_FeatureDiscoveryInfo info = {};
    info.SDKVersion = NVSDK_NGX_Version_API;
    info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    info.Identifier.v.ProjectDesc.EngineVersion = "1.0";
    info.ApplicationDataPath = s.dataPath.c_str();
    info.FeatureInfo = &s.featureInfo;
    return info;
}

static std::string FileVersion(const std::wstring& path) {
    DWORD ignored = 0, size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return "missing";
    std::vector<unsigned char> data(size);
    VS_FIXEDFILEINFO* info = nullptr; UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) || !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info)
        return "unknown";
    char text[64];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                  HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return text;
}

// Instance, the physical device DXVK uses, a device with what NGX asks for plus
// external memory/semaphore import, NGX itself.
static bool InitVulkan(const Config& config) {
    s.vulkan = LoadLibraryW(L"vulkan-1.dll");
    if (!s.vulkan) return Fail("vulkan-1.dll not found", GetLastError());
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(s.vulkan, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr) return Fail("vkGetInstanceProcAddr missing");
#define WOS_VK_LOAD_GLOBAL(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(VK_NULL_HANDLE, #name)); if (!name) return Fail("missing " #name);
    WOS_VK_GLOBAL(WOS_VK_LOAD_GLOBAL)

    const NVSDK_NGX_FeatureDiscoveryInfo discovery = Discovery();
    uint32_t ngxInstanceCount = 0; VkExtensionProperties* ngxInstance = nullptr;
    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery, &ngxInstanceCount, &ngxInstance);
    if (NVSDK_NGX_FAILED(result)) return Fail("NGX instance extension query failed", long(result));
    std::vector<const char*> instanceExtensions;
    for (uint32_t i = 0; i < ngxInstanceCount; ++i) { instanceExtensions.push_back(ngxInstance[i].extensionName); Log("instance extension (NGX): %s", ngxInstance[i].extensionName); }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "WoS_DLSS_Host"; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = uint32_t(instanceExtensions.size()); instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
    VkResult vr = vkCreateInstance(&instanceInfo, nullptr, &s.instance);
    if (vr != VK_SUCCESS) return Fail("vkCreateInstance failed", vr);
#define WOS_VK_LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(s.instance, #name)); if (!name) return Fail("missing " #name);
    WOS_VK_INSTANCE(WOS_VK_LOAD_INSTANCE)

    uint32_t count = 0; vkEnumeratePhysicalDevices(s.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count); vkEnumeratePhysicalDevices(s.instance, &count, devices.data());
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 properties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
        vkGetPhysicalDeviceProperties2(candidate, &properties);
        const bool same = !std::memcmp(id.deviceUUID, config.deviceUUID, 16) && !std::memcmp(id.driverUUID, config.driverUUID, 16);
        Log("device %s %s", properties.properties.deviceName, same ? "(DXVK's)" : "");
        if (same) { s.physical = candidate; s.timestampPeriod = properties.properties.limits.timestampPeriod; }
    }
    if (!s.physical) return Fail("DXVK's GPU (device and driver UUID) not found");
    vkGetPhysicalDeviceMemoryProperties(s.physical, &s.memory);

    uint32_t familyCount = 0; vkGetPhysicalDeviceQueueFamilyProperties(s.physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount); vkGetPhysicalDeviceQueueFamilyProperties(s.physical, &familyCount, families.data());
    s.family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i)
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { s.family = i; break; }
    if (s.family == UINT32_MAX) return Fail("no graphics+compute queue");
    s.timestamps = families[s.family].timestampValidBits != 0 && s.timestampPeriod > 0.0f;

    uint32_t ngxDeviceCount = 0; VkExtensionProperties* ngxDevice = nullptr;
    result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(s.instance, s.physical, &discovery, &ngxDeviceCount, &ngxDevice);
    if (NVSDK_NGX_FAILED(result)) return Fail("NGX device extension query failed", long(result));
    std::vector<const char*> deviceExtensions = { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
    for (uint32_t i = 0; i < ngxDeviceCount; ++i) {
        Log("device extension (NGX): %s", ngxDevice[i].extensionName);
        if (std::none_of(deviceExtensions.begin(), deviceExtensions.end(), [&](const char* e) { return !std::strcmp(e, ngxDevice[i].extensionName); }))
            deviceExtensions.push_back(ngxDevice[i].extensionName);
    }
    uint32_t available = 0; vkEnumerateDeviceExtensionProperties(s.physical, nullptr, &available, nullptr);
    std::vector<VkExtensionProperties> availableList(available); vkEnumerateDeviceExtensionProperties(s.physical, nullptr, &available, availableList.data());
    for (const char* wanted : deviceExtensions)
        if (std::none_of(availableList.begin(), availableList.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, wanted); })) {
            char text[160]; std::snprintf(text, sizeof(text), "device extension %s unavailable", wanted); return Fail(text);
        }

    // Enable the features DLSS kernels commonly rely on, where supported.
    VkPhysicalDeviceVulkan13Features have13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features have12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &have13 };
    VkPhysicalDeviceVulkan11Features have11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &have12 };
    VkPhysicalDeviceFeatures2 have = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &have11 };
    vkGetPhysicalDeviceFeatures2(s.physical, &have);
    if (!have12.timelineSemaphore) return Fail("timeline semaphores unsupported");
    VkPhysicalDeviceVulkan13Features use13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features use12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &use13 };
    VkPhysicalDeviceVulkan11Features use11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &use12 };
    VkPhysicalDeviceFeatures2 use = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &use11 };
    use.features.shaderStorageImageReadWithoutFormat = have.features.shaderStorageImageReadWithoutFormat;
    use.features.shaderStorageImageWriteWithoutFormat = have.features.shaderStorageImageWriteWithoutFormat;
    use.features.shaderImageGatherExtended = have.features.shaderImageGatherExtended;
    use.features.shaderInt16 = have.features.shaderInt16;
    use.features.shaderInt64 = have.features.shaderInt64;
    use.features.fragmentStoresAndAtomics = have.features.fragmentStoresAndAtomics;
    use11.storageBuffer16BitAccess = have11.storageBuffer16BitAccess;
    use11.uniformAndStorageBuffer16BitAccess = have11.uniformAndStorageBuffer16BitAccess;
    use12.timelineSemaphore = VK_TRUE;
    use12.bufferDeviceAddress = have12.bufferDeviceAddress;
    use12.shaderFloat16 = have12.shaderFloat16;
    use12.shaderInt8 = have12.shaderInt8;
    use12.storageBuffer8BitAccess = have12.storageBuffer8BitAccess;
    use12.scalarBlockLayout = have12.scalarBlockLayout;
    use12.hostQueryReset = have12.hostQueryReset;
    use12.descriptorIndexing = have12.descriptorIndexing;
    use12.runtimeDescriptorArray = have12.runtimeDescriptorArray;
    use12.shaderSampledImageArrayNonUniformIndexing = have12.shaderSampledImageArrayNonUniformIndexing;
    use12.shaderStorageImageArrayNonUniformIndexing = have12.shaderStorageImageArrayNonUniformIndexing;
    use12.descriptorBindingPartiallyBound = have12.descriptorBindingPartiallyBound;
    use12.descriptorBindingVariableDescriptorCount = have12.descriptorBindingVariableDescriptorCount;
    use12.vulkanMemoryModel = have12.vulkanMemoryModel;
    use12.vulkanMemoryModelDeviceScope = have12.vulkanMemoryModelDeviceScope;
    use13.synchronization2 = have13.synchronization2;
    use13.maintenance4 = have13.maintenance4;
    use13.shaderIntegerDotProduct = have13.shaderIntegerDotProduct;
    use13.subgroupSizeControl = have13.subgroupSizeControl;
    use13.computeFullSubgroups = have13.computeFullSubgroups;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = s.family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &use };
    deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = uint32_t(deviceExtensions.size()); deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    vr = vkCreateDevice(s.physical, &deviceInfo, nullptr, &s.device);
    if (vr != VK_SUCCESS) return Fail("vkCreateDevice failed", vr);
#define WOS_VK_LOAD_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(s.device, #name)); if (!name) return Fail("missing " #name);
    WOS_VK_DEVICE(WOS_VK_LOAD_DEVICE)
    vkGetDeviceQueue(s.device, s.family, 0, &s.queue);

    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; poolInfo.queueFamilyIndex = s.family;
    if (vkCreateCommandPool(s.device, &poolInfo, nullptr, &s.pool) != VK_SUCCESS) return Fail("command pool");
    VkCommandBufferAllocateInfo allocate = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocate.commandPool = s.pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = kCmdRing;
    if (vkAllocateCommandBuffers(s.device, &allocate, s.cmd) != VK_SUCCESS) return Fail("command buffers");
    VkSemaphoreTypeCreateInfo timelineType = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE; timelineType.initialValue = 0;
    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timelineType };
    if (vkCreateSemaphore(s.device, &semaphoreInfo, nullptr, &s.local) != VK_SUCCESS) return Fail("local timeline");
    if (s.timestamps) {
        VkQueryPoolCreateInfo queryInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP; queryInfo.queryCount = 2 * kCmdRing;
        if (vkCreateQueryPool(s.device, &queryInfo, nullptr, &s.queries) != VK_SUCCESS) s.timestamps = false;
    }

    result = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", s.dataPath.c_str(), s.instance, s.physical, s.device,
                                                  vkGetInstanceProcAddr, vkGetDeviceProcAddr, &s.featureInfo, NVSDK_NGX_Version_API);
    s.shared->lastResult = int32_t(result);
    if (NVSDK_NGX_FAILED(result)) return Fail("NGX init failed", long(result));
    s.ngx = true;
    result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&s.params);
    if (NVSDK_NGX_FAILED(result) || !s.params) return Fail("NGX capability parameters failed", long(result));
    int dlss = 0, needsDriver = 0; unsigned major = 0, minor = 0;
    NVSDK_NGX_Parameter_GetI(s.params, NVSDK_NGX_Parameter_SuperSampling_Available, &dlss);
    NVSDK_NGX_Parameter_GetI(s.params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
    NVSDK_NGX_Parameter_GetUI(s.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
    NVSDK_NGX_Parameter_GetUI(s.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
    Log("DLSS available %d, needs updated driver %d (min %u.%u)", dlss, needsDriver, major, minor);
    if (!dlss) return Fail(needsDriver ? "DLSS needs a newer NVIDIA driver" : "DLSS not available on this GPU/driver");
    return true;
}

static void ReleaseShared() {
    if (s.feature) { NVSDK_NGX_VULKAN_ReleaseFeature(s.feature); s.feature = nullptr; }
    for (SharedImage& image : s.images) {
        if (image.view) vkDestroyImageView(s.device, image.view, nullptr);
        if (image.image) vkDestroyImage(s.device, image.image, nullptr);
        if (image.memory) vkFreeMemory(s.device, image.memory, nullptr);
        image = SharedImage{};
    }
}

static bool ImportImage(SharedImage& out, const ImageDesc& desc) {
    out.desc = desc;
    VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external };
    info.flags = desc.flags; info.imageType = VK_IMAGE_TYPE_2D; info.format = VkFormat(desc.format);
    info.extent = { desc.width, desc.height, 1 }; info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = desc.usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s.device, &info, nullptr, &out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements = {}; vkGetImageMemoryRequirements(s.device, out.image, &requirements);
    if (requirements.size > desc.allocationSize) { Log("image needs %llu bytes, export has %llu", requirements.size, desc.allocationSize); return false; }
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < s.memory.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) && (s.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    if (type == UINT32_MAX) return false;
    VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicated.image = out.image;
    VkImportMemoryWin32HandleInfoKHR import = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated };
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT; import.handle = HANDLE(uintptr_t(desc.handle));
    VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import };
    allocate.allocationSize = desc.allocationSize; allocate.memoryTypeIndex = type;
    VkResult vr = vkAllocateMemory(s.device, &allocate, nullptr, &out.memory);
    if (vr != VK_SUCCESS) { Log("import allocation failed %d", vr); return false; }
    if (vkBindImageMemory(s.device, out.image, out.memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = out.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = info.format;
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return vkCreateImageView(s.device, &view, nullptr, &out.view) == VK_SUCCESS;
}

static bool SubmitAndWait(VkCommandBuffer cmd) {
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    return vkQueueSubmit(s.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(s.queue) == VK_SUCCESS;
}

// A new generation: drop everything tied to the old images, import the new
// ones, create the DLSS feature for their size.
static bool ApplyConfig() {
    // Seqlock read: the game writes the generation last. It waits for our
    // acceptance before writing the next one, so a retry should never happen.
    Config config = {};
    for (int attempt = 0;; ++attempt) {
        const uint32_t before = s.shared->config.generation;
        MemoryBarrier();
        config = s.shared->config;
        MemoryBarrier();
        if (before == config.generation && before == s.shared->config.generation) break;
        if (attempt == 100) return Fail("configuration kept changing while being read");
        Sleep(1);
    }
    if (!s.device && !InitVulkan(config)) return false;
    vkDeviceWaitIdle(s.device);
    ReleaseShared();
    s.ready = false;
    char text[200];
    // The game's timeline is the same object in every generation. An NT handle
    // may be exported only once per semaphore (VUID-VkSemaphoreGetWin32HandleInfoKHR-
    // handleType-01128), so the game sends it with the first generation only;
    // later configurations repeat a value this process already closed.
    if (!s.timeline) {
        VkSemaphoreTypeCreateInfo timelineType = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timelineType };
        if (vkCreateSemaphore(s.device, &semaphoreInfo, nullptr, &s.timeline) != VK_SUCCESS) return Fail("timeline semaphore");
        VkImportSemaphoreWin32HandleInfoKHR import = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
        import.semaphore = s.timeline; import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        import.handle = HANDLE(uintptr_t(config.timelineHandle));
        const VkResult vr = vkImportSemaphoreWin32HandleKHR(s.device, &import);
        CloseHandle(import.handle);
        if (vr != VK_SUCCESS) {
            vkDestroySemaphore(s.device, s.timeline, nullptr); s.timeline = VK_NULL_HANDLE;
            return Fail("timeline import failed", vr);
        }
    }
    bool imported = true;
    for (uint32_t i = 0; i < kImageCount; ++i) {
        imported = imported && ImportImage(s.images[i], config.images[i]);
        CloseHandle(HANDLE(uintptr_t(config.images[i].handle)));
    }
    if (!imported) return Fail("shared image import failed");

    VkCommandBuffer cmd = s.cmd[0];
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &begin);
    NVSDK_NGX_Parameter_SetUI(s.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, unsigned(config.preset));
    NVSDK_NGX_DLSS_Create_Params create = {};
    create.Feature.InWidth = config.width; create.Feature.InHeight = config.height;
    create.Feature.InTargetWidth = config.width; create.Feature.InTargetHeight = config.height;
    create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    create.InFeatureCreateFlags = int(config.createFlags);
    NVSDK_NGX_Result result = NGX_VULKAN_CREATE_DLSS_EXT1(s.device, cmd, 1, 1, &s.feature, s.params, &create);
    vkEndCommandBuffer(cmd);
    s.shared->lastResult = int32_t(result);
    if (NVSDK_NGX_FAILED(result) || !s.feature) return Fail("DLSS feature creation failed", long(result));
    if (!SubmitAndWait(cmd)) return Fail("DLSS creation submit failed");
    for (uint32_t& serial : s.cmdSerial) serial = 0;

    s.width = config.width; s.height = config.height; s.generation = config.generation;
    s.resetNext = true; s.ready = true;
    std::snprintf(s.shared->runtime, sizeof(s.shared->runtime), "nvngx_dlss %s, preset %c", FileVersion(s.folder + L"\\nvngx_dlss.dll").c_str(),
                  config.preset ? char('A' + config.preset - 1) : 'D');
    std::snprintf(text, sizeof(text), "DLAA ready %ux%u generation %u (%s)", config.width, config.height, config.generation, s.shared->runtime);
    SetStatus(text);
    s.shared->hostGeneration = config.generation;
    s.shared->hostState = kHostReady;
    return true;
}

static void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, uint32_t srcFamily, uint32_t dstFamily,
                    VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = srcAccess; b.dstAccessMask = dstAccess; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = srcFamily; b.dstQueueFamilyIndex = dstFamily; b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Waits for this slot's previous use, then reads its GPU timestamps.
static bool ClaimSlot(uint32_t slot, uint32_t serial) {
    const uint32_t previous = s.cmdSerial[slot];
    if (previous) {
        const uint64_t value = previous;
        VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wait.semaphoreCount = 1; wait.pSemaphores = &s.local; wait.pValues = &value;
        if (vkWaitSemaphores(s.device, &wait, 2000000000ull) != VK_SUCCESS) { Log("slot %u still busy with serial %u", slot, previous); return false; }
        if (s.timestamps) {
            uint64_t stamps[2] = {};
            if (vkGetQueryPoolResults(s.device, s.queries, 2 * slot, 2, sizeof(stamps), stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && stamps[1] >= stamps[0])
                s.shared->gpuMs = float(double(stamps[1] - stamps[0]) * s.timestampPeriod * 1e-6);
        }
    }
    s.cmdSerial[slot] = serial;
    return true;
}

// Last resort when a frame cannot be evaluated: wait for the game's input on
// the CPU, then release its output wait so the game's queue never hangs.
// Releases a frame without DLSS. Only after its input is reached: the input
// comes after every earlier output on the game's queue, so the signal never
// overtakes an earlier frame's value (a timeline must only grow). If the input
// never comes, the frame stays owed and the game's watchdog takes over.
static void SignalFromHost(const Frame& f) {
    const uint64_t input = f.inputReady;
    VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wait.semaphoreCount = 1; wait.pSemaphores = &s.timeline; wait.pValues = &input;
    if (vkWaitSemaphores(s.device, &wait, 2000000000ull) != VK_SUCCESS) { Log("frame %u: input never arrived; not released", f.serial); return; }
    uint64_t current = 0; vkGetSemaphoreCounterValue(s.device, s.timeline, &current);
    if (current < f.outputReady) {
        VkSemaphoreSignalInfo signal = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
        signal.semaphore = s.timeline; signal.value = f.outputReady;
        vkSignalSemaphore(s.device, &signal);
    }
}

static void ProcessFrame(const Frame& f) {
    if (!s.ready || f.generation != s.generation || !s.timeline) {
        Log("frame %u for generation %u while ready=%d generation %u: released from the host", f.serial, f.generation, int(s.ready), s.generation);
        if (s.timeline) SignalFromHost(f);
        return;
    }
    const uint32_t slot = f.serial % kCmdRing;
    if (!ClaimSlot(slot, f.serial)) { SignalFromHost(f); return; }
    VkCommandBuffer cmd = s.cmd[slot];
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &begin);
    if (s.timestamps) vkCmdResetQueryPool(cmd, s.queries, 2 * slot, 2);
    const uint32_t ext = VK_QUEUE_FAMILY_EXTERNAL, own = s.family;
    const VkAccessFlags any = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    // Ownership comes over in GENERAL; layouts change afterwards on our queue.
    for (const SharedImage& image : s.images) Barrier(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ext, own, 0, any);
    for (uint32_t i : { kColor, kDepth, kMotion })
        Barrier(cmd, s.images[i].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, any, VK_ACCESS_SHADER_READ_BIT);
    if (s.timestamps) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queries, 2 * slot);

    const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    auto resource = [&](uint32_t i, bool write) {
        const SharedImage& image = s.images[i];
        return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, range, VkFormat(image.desc.format), image.desc.width, image.desc.height, write);
    };
    NVSDK_NGX_Resource_VK color = resource(kColor, false), depth = resource(kDepth, false), motion = resource(kMotion, false), output = resource(kOutput, true);
    NVSDK_NGX_VK_DLSS_Eval_Params eval = {};
    eval.Feature.pInColor = &color; eval.Feature.pInOutput = &output;
    eval.pInDepth = &depth; eval.pInMotionVectors = &motion;
    eval.InJitterOffsetX = f.jitterX; eval.InJitterOffsetY = f.jitterY;
    eval.InRenderSubrectDimensions = { s.width, s.height };
    eval.InReset = (f.reset || s.resetNext) ? 1 : 0;
    eval.InMVScaleX = f.mvScaleX; eval.InMVScaleY = f.mvScaleY;
    eval.InFrameTimeDeltaInMsec = f.frameMs;
    const NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, s.feature, s.params, &eval);
    s.shared->lastResult = int32_t(result);
    if (NVSDK_NGX_FAILED(result)) {
        // Keep the frame visible: the aliased input instead of DLSS.
        if (++s.shared->evaluateFailures <= 8) Log("evaluate failed 0x%08x on serial %u", unsigned(result), f.serial);
        Barrier(cmd, s.images[kColor].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        Barrier(cmd, s.images[kOutput].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, any, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageBlit blit = {};
        blit.srcSubresource = blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1] = blit.dstOffsets[1] = { int32_t(s.width), int32_t(s.height), 1 };
        vkCmdBlitImage(cmd, s.images[kColor].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.images[kOutput].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        Barrier(cmd, s.images[kColor].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
        Barrier(cmd, s.images[kOutput].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_TRANSFER_WRITE_BIT, any);
    }
    if (s.timestamps) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queries, 2 * slot + 1);
    for (uint32_t i : { kColor, kDepth, kMotion })
        Barrier(cmd, s.images[i].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_SHADER_READ_BIT, any);
    for (const SharedImage& image : s.images) Barrier(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, own, ext, any, 0);
    vkEndCommandBuffer(cmd);

    const uint64_t waitValue = f.inputReady, signalValues[2] = { f.outputReady, f.serial };
    const VkSemaphore signals[2] = { s.timeline, s.local };
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo values = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    values.waitSemaphoreValueCount = 1; values.pWaitSemaphoreValues = &waitValue;
    values.signalSemaphoreValueCount = 2; values.pSignalSemaphoreValues = signalValues;
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, &values };
    submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &s.timeline; submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 2; submit.pSignalSemaphores = signals;
    const VkResult vr = vkQueueSubmit(s.queue, 1, &submit, VK_NULL_HANDLE);
    if (vr != VK_SUCCESS) {
        Log("submit failed %d on serial %u", vr, f.serial);
        s.cmdSerial[slot] = 0;
        SignalFromHost(f);
        Fail("DLSS submit failed", vr);
        s.ready = false;
        return;
    }
    s.resetNext = false;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    const DWORD gamePid = DWORD(_wtoi(argv[1]));
    wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    s.folder = exe; s.folder.resize(s.folder.find_last_of(L"\\/"));
    _wfopen_s(&g_log, (s.folder + L"\\host.log").c_str(), L"wt");
    Log("WoS_DLSS_Host started for game pid %lu", gamePid);

    wchar_t name[96];
    std::swprintf(name, 96, WOS_DLSS_SHARED_NAME, gamePid);
    s.mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (s.mapping) s.shared = static_cast<Shared*>(MapViewOfFile(s.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    std::swprintf(name, 96, WOS_DLSS_EVENT_NAME, gamePid);
    s.event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    s.game = OpenProcess(SYNCHRONIZE, FALSE, gamePid);
    if (!s.shared || !s.event || !s.game || s.shared->magic != kMagic || s.shared->version != kVersion) {
        Log("shared state unavailable (mapping %p, event %p, game %p)", s.mapping, s.event, s.game);
        return 3;
    }
    s.shared->hostPid = GetCurrentProcessId();
    s.dataPath = s.folder + L"\\ngx_data";
    CreateDirectoryW(s.dataPath.c_str(), nullptr);
    s.pathList = { s.folder.c_str() };
    s.featureInfo.PathListInfo.Path = s.pathList.data(); s.featureInfo.PathListInfo.Length = 1;
    s.featureInfo.LoggingInfo.LoggingCallback = NgxLog;
    s.featureInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    SetStatus("helper started, waiting for the first configuration");

    const HANDLE waits[2] = { s.game, s.event };
    for (bool quit = false; !quit;) {
        const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, 100);
        if (woke == WAIT_OBJECT_0 || woke == WAIT_FAILED) break;
        s.shared->heartbeat = GetTickCount();
        const uint32_t generation = s.shared->config.generation;
        if (generation && generation != s.generation && s.shared->hostState != kHostFailed) {
            if (!ApplyConfig()) { s.generation = generation; Log("configuration %u rejected", generation); }
        }
        for (uint32_t done = s.shared->framesSubmitted; done != s.shared->framesWritten; done = s.shared->framesSubmitted) {
            const uint32_t serial = done + 1;
            Frame f = s.shared->ring[serial % kRing];
            MemoryBarrier();
            if (f.serial != serial) {
                // Exit: the game's watchdog sees the process end and releases the owed frames in order.
                Log("ring slot holds serial %u, expected %u; exiting", f.serial, serial);
                SetStatus("ring out of sync"); s.shared->hostState = kHostFailed; quit = true; break;
            }
            ProcessFrame(f);
            s.shared->framesSubmitted = serial;
        }
    }
    Log("shutting down");
    if (s.shared->hostState != kHostFailed) s.shared->hostState = kHostExited;
    if (s.device) {
        // Bounded: a batch whose input never comes (the game died first) would
        // keep vkDeviceWaitIdle waiting forever. Windows frees what is left.
        uint32_t newest = 0;
        for (uint32_t serial : s.cmdSerial) newest = (std::max)(newest, serial);
        const uint64_t value = newest;
        VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wait.semaphoreCount = 1; wait.pSemaphores = &s.local; wait.pValues = &value;
        if (newest && vkWaitSemaphores(s.device, &wait, 1000000000ull) != VK_SUCCESS) { Log("GPU work still pending; leaving cleanup to Windows"); return 0; }
        vkDeviceWaitIdle(s.device);
        ReleaseShared();
        if (s.timeline) vkDestroySemaphore(s.device, s.timeline, nullptr);
        if (s.params) NVSDK_NGX_VULKAN_DestroyParameters(s.params);
        if (s.ngx) NVSDK_NGX_VULKAN_Shutdown1(s.device);
    }
    return 0;
}
