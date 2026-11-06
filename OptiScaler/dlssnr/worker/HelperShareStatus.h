#pragma once
#include <atomic>
#include <cstdint>

namespace DlssNr::Worker::HelperShareStatus
{
// Renderer-owned snapshot of the helper's share for the menu. Approximate; never a control input.
// ```cpp
// auto rows = HelperShareStatus::rows.load(std::memory_order_relaxed);
// ```
inline std::atomic<uint32_t> rows { 0 }, height { 0 };
inline std::atomic<float> turnaroundMs { 0 }, periodMs { 0 };
inline std::atomic<bool> saturated { false };
} // namespace DlssNr::Worker::HelperShareStatus
