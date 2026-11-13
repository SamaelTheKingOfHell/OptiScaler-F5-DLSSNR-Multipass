#pragma once

// Did a DXGI CreateSwapChain* call give the game a swapchain? DXGI can succeed with a status other than S_OK:
// DXGI_STATUS_OCCLUDED for a full-screen swapchain whose window is not in front. That swapchain is valid; taking only
// S_OK handed it to the game unwrapped (no frame generation, menu or HDR output on it, and nothing logged).
// When frame generation makes the chain, FGHooks::CreateSwapChain* answers exactly S_OK (the library's own inner create
// went through these hooks), so the game is not told about an inner status. Pure rule, host-tested in
// tests/swapchain_create_rule_smoke.cpp.
#include <windows.h>

namespace SwapchainCreate
{
template <typename T> constexpr bool Created(HRESULT result, T* const* ppSwapChain)
{
    return SUCCEEDED(result) && ppSwapChain != nullptr && *ppSwapChain != nullptr;
}
} // namespace SwapchainCreate
