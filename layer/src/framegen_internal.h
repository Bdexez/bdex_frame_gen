#pragma once
// Private to the FrameGen implementation files (framegen*.cpp): the push
// constant blocks of the passes, the frame-state buffer layout and shared
// helpers. Not part of the engine's interface (framegen.h).
#include "config.h"

#include "vk_compat.h"

#include <cstdint>
#include <memory>

namespace bdex {

namespace fg {

// Frames of one present at most (multiplier ceiling): kMaxFrames - 1
// generated ones, and the state's per-output slots t[kMaxFrames], the last
// being the real frame's.
constexpr int kMaxFrames = 8;

struct DownsamplePC { int32_t size[2]; int32_t fromColor; int32_t transfer; };
struct MatchPC { int32_t size[2]; int32_t blocks[2]; int32_t radius; int32_t hasCoarse; float smoothness; float zeroBias; float gradWeight; float censusWeight; float censusMargin; float rotWeight; int32_t temporal; float zeroBiasContrast; float chromaWeight; int32_t chromaHistory; float zeroBiasCap; int32_t altVector; };
struct SizePC { int32_t size[2]; };
struct HistPC { int32_t size[2]; int32_t tiles; };
struct ReducePC { int32_t size[2]; int32_t tiles; float cutHist; float cutLow; };

struct RefinePC { int32_t size[2]; int32_t blocks[2]; int32_t fine[2]; int32_t coarseBlock; int32_t fineBlock; float ownBias; int32_t subpixel; };
// interpolate.comp's kFeatures bits: the opt-ins a synthesis variant compiles in.
enum : uint32_t {
    SYNTH_BICUBIC = 1u, SYNTH_TIMING = 2u, SYNTH_LOCAL_CUT = 4u, SYNTH_ACCEL = 8u, SYNTH_REVEAL = 16u, SYNTH_SPLAT = 32u,
    SYNTH_PIXEL_VECTORS = 64u, SYNTH_ALT = 128u, SYNTH_OCCLUSION = 256u, SYNTH_CONF_FALLBACK = 512u,
    SYNTH_STATIC_GUARD = 1024u, SYNTH_STATIC_MASK = 2048u, SYNTH_DEBUG_CONF = 4096u,
};
struct InterpPC { int32_t size[2]; float t; float flowScale; uint32_t encoding; int32_t debugMode; float cutLow; float cutHigh; float flowInvSize[2]; int32_t iterations; float occlusionStrength; float staticGuard; float extrapHold; int32_t splat; int32_t pixelVectors; int32_t transfer; int32_t timing; int32_t localCut; float accel; int32_t bicubic; float staticFrames; int32_t revealFill; float confFallback; int32_t altVectors; int32_t altBlocks[2]; };
struct UpscalePC { int32_t outSize[2]; int32_t srcSize[2]; uint32_t encoding; int32_t filter; };
struct FrameStatePC { int32_t size[2]; int32_t mode; int32_t multiplier; float sinceLastMs; float intervalMs; int32_t flags; int32_t gateCount; uint32_t dupMaxPixels; float displayMs[kMaxFrames]; int32_t realPos; };
struct CasPC { int32_t size[2]; uint32_t encoding; float sharpness; };

constexpr uint32_t kBlockSize = 8;
constexpr uint32_t kFineBlock = 4;
constexpr uint32_t kPushSize = 128;  // the guaranteed minimum of maxPushConstantsSize
static_assert(sizeof(InterpPC) <= kPushSize && sizeof(MatchPC) <= kPushSize && sizeof(FrameStatePC) <= kPushSize);

// Frame state buffer layout (the State block of frame_state.comp; the
// synthesis shaders read its head).
constexpr VkDeviceSize kStateSize = 2048;
constexpr VkDeviceSize kStateDiffCount = 8;
constexpr VkDeviceSize kStateHead = 64;          // meanCost .. t[8], read back for the tools
constexpr VkDeviceSize kStateGateFull = 320;     // uvec4 per gated dispatch
constexpr VkDeviceSize kStateGateActive = 704;   // VkDispatchIndirectCommand per gated dispatch
constexpr VkDeviceSize kStateTileDiff = 992;     // uint per tile of the 8x8 repeat grid
constexpr uint32_t kGateEntries = 24;
constexpr uint32_t kCutTiles = 8;  // scene_cut_local grid (tileCost[64] in the state)
static_assert(kStateGateFull == kStateHead + 64 * 4 && kStateGateActive == kStateGateFull + kGateEntries * 16);
static_assert(kStateGateActive + kGateEntries * 12 <= kStateTileDiff);
constexpr VkDeviceSize kStateTileHist = 1280;    // float per tile: scene_cut_hist distance (scene_cut_local)
static_assert(kStateTileDiff + 65 * 4 <= kStateTileHist && kStateTileHist + 64 * 4 <= kStateSize);
enum { kStateReset = 1, kStateDedup = 2, kStateExtrap = 4, kStateDisplay = 8, kStateContent = 16 };

inline uint32_t divUp(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

} // namespace fg
} // namespace bdex
