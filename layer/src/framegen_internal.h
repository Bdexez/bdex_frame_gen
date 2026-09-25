#pragma once
// Private to the FrameGen implementation files (framegen*.cpp): the push
// constant blocks of the passes, the frame-state buffer layout and shared
// helpers. Not part of the engine's interface (framegen.h).
#include "config.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

namespace bdex {

namespace fg {

struct DownsamplePC { int32_t size[2]; int32_t fromColor; int32_t transfer; };
struct MatchPC { int32_t size[2]; int32_t blocks[2]; int32_t radius; int32_t hasCoarse; float smoothness; float zeroBias; float gradWeight; float censusWeight; float censusMargin; float rotWeight; int32_t temporal; float zeroBiasContrast; float chromaWeight; int32_t chromaHistory; };
struct SizePC { int32_t size[2]; };
struct ReducePC { int32_t size[2]; int32_t tiles; };

struct RefinePC { int32_t size[2]; int32_t blocks[2]; int32_t fine[2]; int32_t coarseBlock; int32_t fineBlock; float ownBias; int32_t subpixel; };
struct InterpPC { int32_t size[2]; float t; float flowScale; uint32_t encoding; int32_t debugMode; float cutLow; float cutHigh; float flowInvSize[2]; int32_t iterations; int32_t hudMode; int32_t hudGame; int32_t hudOut; int32_t hudLow1; int32_t hudLow01; float occlusionStrength; float staticGuard; float extrapHold; int32_t splat; int32_t pixelVectors; int32_t transfer; int32_t timing; int32_t localCut; float accel; int32_t hudLatency; int32_t bicubic; float staticFrames; };
struct UpscalePC { int32_t outSize[2]; int32_t srcSize[2]; uint32_t encoding; int32_t filter; int32_t hudMode; int32_t hudGame; int32_t hudOut; int32_t hudLow1; int32_t hudLow01; int32_t hudLatency; };
struct SplatPC { int32_t size[2]; float s; float flowScale; float flowInvSize[2]; int32_t timing; float accel; };
struct FrameStatePC { int32_t size[2]; int32_t mode; int32_t multiplier; float sinceLastMs; float intervalMs; int32_t flags; int32_t gateCount; uint32_t dupMaxPixels; float displayMs[4]; };
struct CasPC { int32_t size[2]; uint32_t encoding; float sharpness; };

constexpr uint32_t kBlockSize = 8;
constexpr uint32_t kFineBlock = 4;
constexpr uint32_t kPushSize = 128;  // the guaranteed minimum of maxPushConstantsSize
static_assert(sizeof(InterpPC) <= kPushSize && sizeof(MatchPC) <= kPushSize);

// Frame state buffer layout (the State block of frame_state.comp; the
// synthesis shaders read its head).
constexpr VkDeviceSize kStateSize = 2048;
constexpr VkDeviceSize kStateDiffCount = 8;
constexpr VkDeviceSize kStateHead = 48;          // meanCost .. t[4], read back for the tools
constexpr VkDeviceSize kStateGateFull = 304;     // uvec4 per gated dispatch
constexpr VkDeviceSize kStateGateActive = 688;   // VkDispatchIndirectCommand per gated dispatch
constexpr VkDeviceSize kStateTileDiff = 976;     // uint per tile of the 8x8 repeat grid
constexpr uint32_t kGateEntries = 24;
constexpr uint32_t kCutTiles = 8;  // scene_cut_local grid (tileCost[64] in the state)
static_assert(kStateGateActive + kGateEntries * 12 <= kStateTileDiff);
static_assert(kStateTileDiff + 65 * 4 <= kStateSize);
enum { kStateReset = 1, kStateDedup = 2, kStateExtrap = 4, kStateDisplay = 8, kStateContent = 16 };

inline uint32_t divUp(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

} // namespace fg
} // namespace bdex
