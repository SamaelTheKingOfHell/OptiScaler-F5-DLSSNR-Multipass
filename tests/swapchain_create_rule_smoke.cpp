// Host check of SwapchainCreateRule.h: a CreateSwapChain* call that succeeded with a status other than S_OK still
// gives the game a swapchain OptiScaler must wrap. No GPU and no game needed.
// cl /std:c++20 /EHsc tests/swapchain_create_rule_smoke.cpp
#include "../OptiScaler/hooks/SwapchainCreateRule.h"

#include <dxgi1_2.h>
#include <cstdio>

using SwapchainCreate::Created;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main()
{
    // Stands in for the swapchain DXGI returned, only its address matters
    int dummy = 0;
    IDXGISwapChain* swapChain = reinterpret_cast<IDXGISwapChain*>(&dummy);
    IDXGISwapChain* none = nullptr;

    CHECK(Created(S_OK, &swapChain));

    // Full-screen swapchain while the window is not in front: valid, must be wrapped (was left unwrapped before)
    static_assert(DXGI_STATUS_OCCLUDED != S_OK);
    CHECK(Created(DXGI_STATUS_OCCLUDED, &swapChain));
    CHECK(Created(DXGI_STATUS_MODE_CHANGED, &swapChain));

    // Failures stay failures
    CHECK(!Created(E_FAIL, &swapChain));
    CHECK(!Created(E_INVALIDARG, &swapChain));
    CHECK(!Created(E_ACCESSDENIED, &swapChain)); // another swapchain still owns the window
    CHECK(!Created(DXGI_ERROR_INVALID_CALL, &swapChain));

    // A success without a swapchain is not one to wrap (the hooks dereference it)
    CHECK(!Created(S_OK, &none));
    CHECK(!Created(DXGI_STATUS_OCCLUDED, &none));
    CHECK(!Created(S_OK, static_cast<IDXGISwapChain**>(nullptr)));

    // The IDXGISwapChain1 out parameter of CreateSwapChainForHwnd / ForCoreWindow
    IDXGISwapChain1* swapChain1 = reinterpret_cast<IDXGISwapChain1*>(&dummy);
    CHECK(Created(DXGI_STATUS_OCCLUDED, &swapChain1));

    if (fails == 0)
        printf("swapchain_create_rule_smoke: PASS\n");

    return fails == 0 ? 0 : 1;
}
