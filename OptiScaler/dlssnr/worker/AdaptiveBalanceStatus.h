#pragma once
#include <atomic>
#include <cstdint>

namespace DlssNr::Worker::AdaptiveStatus
{
// Renderer-owned measured snapshot. Menu reads are approximate, never control inputs.
// ```cpp
// auto primary = AdaptiveStatus::primaryPasses.load(std::memory_order_relaxed);
// ```
inline std::atomic<uint32_t> phase { 0 }, primaryPasses { 0 }, totalPasses { 0 };
inline std::atomic<float> medianFrameMs { 0 }, p95FrameMs { 0 };
inline std::atomic<bool> pipelineActive { false };
} // namespace DlssNr::Worker::AdaptiveStatus
