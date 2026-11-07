#include "pch.h"
#include "DlssNr_StreamlinePicture.h"
#include "DlssNrFeature_Dx12.h"
#include <Config.h>
#include <State.h>
#include <Util.h>
#include <cstring>
#include <atomic>
#include <new>
#include <framegen/dlssg/DlssgTelemetry.h>
#include <shaders/dlssnr/DlssNr_FinishedReady.h>
#include <inputs/FG/Streamline_Delay.h>

namespace DlssNr::StreamlinePicture
{
namespace
{
using Present = HRESULT (*)(IDXGISwapChain*, UINT, UINT, bool&);
using Present1 = HRESULT (*)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*, bool&);
using CreateHwnd = HRESULT (*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**, bool&);
using Create = HRESULT (*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, bool&);

// A swapchain-owned COM token keeps the availability count exact across destruction and
// re-registration without querying DXGI while the evaluate path holds the NR mutex.
std::atomic<unsigned> registrations[2] {};
inline constexpr GUID registrationKey = {
    0xe975f055, 0xc073, 0x4548, { 0xb6, 0xc2, 0x11, 0xa7, 0xc6, 0xef, 0x45, 0x1d }
};
inline constexpr GUID localRegistrationKey = {
    0x1295ab84, 0x6b71, 0x4359, { 0x9f, 0xe0, 0x69, 0x2e, 0xee, 0x5b, 0x20, 0x83 }
};
class Registration final : public IUnknown
{
    std::atomic<ULONG> refs { 1 };
    const unsigned runtime;
    ~Registration() { registrations[runtime].fetch_sub(1, std::memory_order_release); }

  public:
    explicit Registration(bool local) : runtime(local ? 1u : 0u)
    {
        registrations[runtime].fetch_add(1, std::memory_order_release);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown))
            return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto left = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!left)
            delete this;
        return left;
    }
};

void RegisterQueue(HRESULT result, bool handled, IUnknown* device, IDXGISwapChain* swapchain, bool local)
{
    if (State::Instance().isShuttingDown)
        return;
    // Keep the application's render queue, not DLSSG's asynchronous presentation queue.
    if (FAILED(result) || !handled || !device || !swapchain)
        return;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue))))
    {
        ID3D12CommandQueue* real = nullptr;
        if (Util::CheckForRealObject(__FUNCTION__, queue.Get(), (IUnknown**) &real))
            queue = real;
        const auto& queueKey = local ? localRenderQueueKey : renderQueueKey;
        if (FAILED(swapchain->SetPrivateDataInterface(queueKey, queue.Get())))
            return;
        const auto& tokenKey = local ? localRegistrationKey : registrationKey;
        auto* token = new (std::nothrow) Registration(local);
        if (!token || FAILED(swapchain->SetPrivateDataInterface(tokenKey, token)))
        {
            swapchain->SetPrivateDataInterface(queueKey, nullptr);
            swapchain->SetPrivateDataInterface(tokenKey, nullptr); // also clear an older registration on retry
            if (token)
                token->Release();
            return;
        }
        token->Release(); // now owned exclusively by the swapchain, no raw-pointer registry
        LOG_INFO("DLSS-NR: Streamline finished-picture handoff registered on the game render queue");
    }
}

// A successful Off call is authoritative even while the previous FG evaluate is still recent.
// Without an options call, retain the existing conservative recent-evaluate check. A failed call
// does not prove that FG stopped, so do not fall back onto its asynchronous display buffers.
bool GameFgMayBeRunning()
{
    const auto& state = State::Instance();
    // SetOptions records this before entering the runtime; an On transition in progress must
    // not use a previous successful Off result to enable the ordinary-picture fallback.
    if (state.dlssgLastSetMode != sl::DLSSGMode::eOff)
        return true;
    const auto& counts = DlssgTelemetry::Get();
    if (counts.optionsSeen.load(std::memory_order_acquire))
        return counts.result.load(std::memory_order_relaxed) != 0 || counts.active.load(std::memory_order_relaxed);
    return GameFrameGenerationOn(false, false, false, state.dlssgLastSetMode != sl::DLSSGMode::eOff,
                                 PresentsSince(state.frameCount, state.dlssgLastEvaluateFrame));
}

bool Supported(bool local)
{
    const auto& state = State::Instance();
    // The bundled local plugin uses the SL2 before-hook ABI. For a game's runtime, reject
    // unknown/SL1/future major ABIs rather than calling an export with an assumed signature.
    return state.activeFgNvngx == FGNvngxReplacement::None &&
           (local ? state.gameQuirks[GameQuirk::Kcd2NrBeforeFg] : state.streamlineVersion.major == 2);
}

// The game's and OptiScaler's plugins can coexist. Never call one runtime's hooks on the other's buffers.
template <bool Local> struct Hooks
{
    inline static std::atomic<Present> originalPresent { nullptr };
    inline static std::atomic<Present1> originalPresent1 { nullptr };
    inline static std::atomic<CreateHwnd> originalCreateHwnd { nullptr };
    inline static std::atomic<Create> originalCreate { nullptr };
    inline static std::atomic<GetIndex> getIndex { nullptr };
    inline static std::atomic<GetBuffer> getBuffer { nullptr };

    static void Apply(IDXGISwapChain* swapchain, UINT flags, bool skip)
    {
        if (State::Instance().isShuttingDown || skip || (flags & DXGI_PRESENT_TEST))
            return;
        auto queue = RenderQueue(swapchain, Local);
        if (!queue || !Supported(Local))
            return;
        const auto snapshot = BeginStreamlinePicture();
        const auto& cfg = *Config::Instance();
        const char* unavailable = nullptr;
        if (cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrFinishedPicture.value_or_default())
        {
            auto picture = Read(swapchain, getIndex.load(), getBuffer.load());
            // Pipelined neural rendering presents the previous frame's picture: frame generation then needs the
            // previous frame's inputs too (inputs/FG/Streamline_Delay.h).
            SlFgDelay::SetEnabled(cfg.DlssNrNetworkSplit.value_or_default());
            if (picture)
            {
                ApplyToStreamlinePicture(swapchain, picture.Get(), queue.Get(), snapshot);
                SlFgDelay::AtPresent(queue.Get(), LastPresentDelayed());
            }
            else if (!GameFgMayBeRunning())
                ApplyToStreamlinePassthrough(swapchain, queue.Get(), snapshot);
            else
                unavailable = "Waiting for Streamline's application picture; native frame generation may be active.";
        }
        // Every real app Present is a capture boundary, including menus/declined buffers/NR Off.
        // Never carry this picture's guides into a later application frame.
        EndStreamlinePicture(snapshot, unavailable);
    }

    static HRESULT BeforePresent(IDXGISwapChain* swapchain, UINT interval, UINT flags, bool& skip)
    {
        Apply(swapchain, flags, skip);
        return originalPresent.load()(swapchain, interval, flags, skip);
    }
    static HRESULT BeforePresent1(IDXGISwapChain* swapchain, UINT interval, UINT flags,
                                  const DXGI_PRESENT_PARAMETERS* parameters, bool& skip)
    {
        Apply(swapchain, flags, skip);
        return originalPresent1.load()(swapchain, interval, flags, parameters, skip);
    }
    static HRESULT CreateForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window,
                                 const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                 IDXGIOutput* output, IDXGISwapChain1** swapchain, bool& skip)
    {
        const auto result =
            originalCreateHwnd.load()(factory, device, window, desc, fullscreen, output, swapchain, skip);
        if (SUCCEEDED(result) && skip && swapchain)
            RegisterQueue(result, skip, device, *swapchain, Local);
        return result;
    }
    static HRESULT CreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                   IDXGISwapChain** swapchain, bool& skip)
    {
        const auto result = originalCreate.load()(factory, device, desc, swapchain, skip);
        if (SUCCEEDED(result) && skip && swapchain)
            RegisterQueue(result, skip, device, *swapchain, Local);
        return result;
    }

    static void* Wrap(const char* name, GetFunction getFunction)
    {
        if (!name || !getFunction)
            return nullptr;
        const bool present = std::strcmp(name, "slHookPresent") == 0;
        const bool present1 = std::strcmp(name, "slHookPresent1") == 0;
        const bool createHwnd = std::strcmp(name, "slHookCreateSwapChainForHwnd") == 0;
        const bool create = Local && std::strcmp(name, "slHookCreateSwapChain") == 0;
        if (!present && !present1 && !createHwnd && !create)
            return nullptr;
        auto* function = getFunction(name);
        const auto index = reinterpret_cast<GetIndex>(getFunction("slHookGetCurrentBackBufferIndex"));
        const auto buffer = reinterpret_cast<GetBuffer>(getFunction("slHookGetBuffer"));
        if (!function || !index || !buffer || !getFunction("slHookPresent") || !getFunction("slHookPresent1"))
            return nullptr;
        getIndex = index;
        getBuffer = buffer;
        if (present)
        {
            originalPresent = reinterpret_cast<Present>(function);
            return &BeforePresent;
        }
        if (present1)
        {
            originalPresent1 = reinterpret_cast<Present1>(function);
            return &BeforePresent1;
        }
        if (createHwnd)
        {
            originalCreateHwnd = reinterpret_cast<CreateHwnd>(function);
            return &CreateForHwnd;
        }
        originalCreate = reinterpret_cast<Create>(function);
        return &CreateSwapChain;
    }
};
} // namespace

bool Owns(IDXGISwapChain* swapchain)
{
    return (Supported(false) && RenderQueue(swapchain) != nullptr) ||
           (Supported(true) && RenderQueue(swapchain, true) != nullptr);
}

bool NativeFgRunning()
{
    const auto& state = State::Instance();
    auto* fg = state.currentFG;
    const bool replaced = state.activeFgNvngx != FGNvngxReplacement::None;
    return !replaced &&
           GameFrameGenerationOn(fg && fg->IsActive() && !fg->IsPaused(), state.activeFgInput == FGInput::DLSSG,
                                 state.activeFgOutput == FGOutput::DLSSG, GameFgMayBeRunning(), UINT64_MAX);
}

bool PauseEvaluation()
{
    return NativeFgRunning() && !(Supported(false) && registrations[0].load(std::memory_order_acquire)) &&
           !(Supported(true) && registrations[1].load(std::memory_order_acquire));
}

void* Wrap(const char* name, GetFunction getFunction, bool local)
{
    if (!Supported(local))
        return nullptr;
    return local ? Hooks<true>::Wrap(name, getFunction) : Hooks<false>::Wrap(name, getFunction);
}
} // namespace DlssNr::StreamlinePicture
