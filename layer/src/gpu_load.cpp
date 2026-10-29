#include "gpu_load.h"

#include <cstdio>
#include <cstring>

#ifdef __linux__
#include <dirent.h>
#include <dlfcn.h>
#endif

namespace bdex {

std::string nvmlUuidString(const uint8_t* u) {
    char s[48];
    snprintf(s, sizeof s, "GPU-%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2], u[3],
             u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    return s;
}

#ifdef __linux__

namespace {

// The few NVML entry points needed, by their C signatures (nvml.h: an
// nvmlReturn_t is an int, 0 success; an nvmlDevice_t an opaque pointer).
struct Nvml final : GpuLoad {
    struct Utilization { unsigned int gpu, memory; };
    void* lib = nullptr;
    void* device = nullptr;
    int (*shutdown)() = nullptr;
    int (*utilization)(void*, Utilization*) = nullptr;
    ~Nvml() override {
        if (shutdown) shutdown();
        if (lib) dlclose(lib);
    }
    int busyPercent() override {
        Utilization u{};
        return utilization(device, &u) == 0 && u.gpu <= 100 ? int(u.gpu) : -1;
    }
    const char* source() const override { return "NVML"; }
};

std::unique_ptr<GpuLoad> openNvml(const uint8_t* uuid, const char* name) {
    auto n = std::make_unique<Nvml>();
    n->lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!n->lib) return nullptr;
    auto sym = [&](const char* s) { return dlsym(n->lib, s); };
    auto init = reinterpret_cast<int (*)()>(sym("nvmlInit_v2"));
    auto count = reinterpret_cast<int (*)(unsigned int*)>(sym("nvmlDeviceGetCount_v2"));
    auto byIndex = reinterpret_cast<int (*)(unsigned int, void**)>(sym("nvmlDeviceGetHandleByIndex_v2"));
    auto getUuid = reinterpret_cast<int (*)(void*, char*, unsigned int)>(sym("nvmlDeviceGetUUID"));
    auto getName = reinterpret_cast<int (*)(void*, char*, unsigned int)>(sym("nvmlDeviceGetName"));
    n->utilization = reinterpret_cast<int (*)(void*, Nvml::Utilization*)>(sym("nvmlDeviceGetUtilizationRates"));
    auto shutdown = reinterpret_cast<int (*)()>(sym("nvmlShutdown"));
    unsigned int devices = 0;
    if (!init || !count || !byIndex || !getUuid || !getName || !n->utilization || !shutdown || init() != 0) return nullptr;
    n->shutdown = shutdown;  // (initialised: shut down with it)
    if (count(&devices) != 0 || devices == 0) return nullptr;
    const std::string want = uuid ? nvmlUuidString(uuid) : std::string();
    void* byName = nullptr;
    void* only = nullptr;
    for (unsigned int i = 0; i < devices; ++i) {
        void* d = nullptr;
        if (byIndex(i, &d) != 0) continue;
        char text[96] = {};
        if (!want.empty() && getUuid(d, text, sizeof text) == 0 && want == text) {
            n->device = d;
            break;
        }
        if (getName(d, text, sizeof text) == 0 && name && strcmp(text, name) == 0 && !byName) byName = d;
        if (devices == 1) only = d;
    }
    if (!n->device) n->device = only ? only : byName;
    if (!n->device || n->busyPercent() < 0) return nullptr;
    return n;
}

struct AmdGpu final : GpuLoad {
    std::string path;
    int busyPercent() override {
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return -1;
        int v = -1;
        if (fscanf(f, "%d", &v) != 1 || v < 0 || v > 100) v = -1;
        fclose(f);
        return v;
    }
    const char* source() const override { return "amdgpu"; }
};

std::unique_ptr<GpuLoad> openAmdgpu(uint32_t deviceId) {
    DIR* dir = opendir("/sys/class/drm");
    if (!dir) return nullptr;
    std::unique_ptr<GpuLoad> found;
    while (dirent* e = readdir(dir)) {
        // card0, card1... (not card0-DP-1, renderD128)
        if (strncmp(e->d_name, "card", 4) != 0 || strchr(e->d_name, '-')) continue;
        const std::string base = std::string("/sys/class/drm/") + e->d_name + "/device/";
        auto readHex = [](const std::string& p) {
            unsigned int v = 0;
            FILE* f = fopen(p.c_str(), "r");
            if (f) {
                if (fscanf(f, "%x", &v) != 1) v = 0;
                fclose(f);
            }
            return v;
        };
        if (readHex(base + "vendor") != 0x1002 || readHex(base + "device") != deviceId) continue;
        auto a = std::make_unique<AmdGpu>();
        a->path = base + "gpu_busy_percent";
        if (a->busyPercent() >= 0) {
            found = std::move(a);
            break;
        }
    }
    closedir(dir);
    return found;
}

} // namespace

std::unique_ptr<GpuLoad> GpuLoad::open(uint32_t vendorId, uint32_t deviceId, const uint8_t* uuid, const char* name) {
    if (vendorId == 0x10de) return openNvml(uuid, name);
    if (vendorId == 0x1002) return openAmdgpu(deviceId);
    return nullptr;
}

#else

std::unique_ptr<GpuLoad> GpuLoad::open(uint32_t, uint32_t, const uint8_t*, const char*) { return nullptr; }

#endif

} // namespace bdex
