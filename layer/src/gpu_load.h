#pragma once
// How busy the GPU itself is, for gpu_headroom (pacing::HeadroomCap): the
// share of the last sampling period during which it ran any work — the game's,
// ours, the compositor's. NVIDIA: NVML (libnvidia-ml.so.1, the driver's,
// found inside Steam's container too), the device matched by its UUID (the
// Vulkan deviceUUID), else the only one, else by name. AMD: amdgpu's
// gpu_busy_percent in sysfs, the card matched by its PCI vendor and device
// ids. Nothing elsewhere (Intel, Windows for now): open() returns null and the
// game's waits on the GPU stand in.
#include <cstdint>
#include <memory>
#include <string>

namespace bdex {

class GpuLoad {
public:
    virtual ~GpuLoad() = default;
    // vendorId, deviceId and deviceName as in VkPhysicalDeviceProperties;
    // uuid: VkPhysicalDeviceIDProperties::deviceUUID, or null.
    static std::unique_ptr<GpuLoad> open(uint32_t vendorId, uint32_t deviceId, const uint8_t* uuid, const char* name);
    // Busy percent over the driver's last sampling period (0..100), -1 if the
    // reading failed.
    virtual int busyPercent() = 0;
    // "NVML", "amdgpu": for the log and the status.
    virtual const char* source() const = 0;
};

// NVML's UUID string for a Vulkan deviceUUID: "GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
std::string nvmlUuidString(const uint8_t* uuid);

} // namespace bdex
