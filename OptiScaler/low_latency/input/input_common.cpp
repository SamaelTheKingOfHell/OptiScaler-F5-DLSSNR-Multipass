#include "pch.h"

// Upstream's low latency input layer, on OptiScaler's terms (OutputRule.h). Without LOW_LATENCY_INPUTS its inputs are
// the game's Reflex, D3D and Vulkan (input_reflex.cpp, input_reflex_vk.cpp), the game's Anti-Lag Vk
// (input_antilag_vk.cpp), OptiScaler's own low latency (native/NativeLowLatency.cpp) and the UE Low Latency mod
// (input_uell.cpp); its outputs Reflex (D3D, Vulkan's VK_NV_low_latency2 or NvAPI_Vulkan), Anti-Lag 2 / Anti-Lag Vk,
// XeLL (also XeFG's) and LatencyFlex. The game's XeLL and D3D Anti-Lag 2 stay on our own hooks.

#include <misc/FrameLimit.h>
#include <misc/IdentifyGpu.h>

#include "input_common.h"
#include "input_xell.h"
#include <low_latency/low_latency_tech/ll_xell.h>
#include <low_latency/low_latency_tech/ll_antilag2.h>
#include <low_latency/low_latency_tech/ll_latencyflex.h>
#include <low_latency/low_latency_tech/ll_reflex.h>
#include <low_latency/low_latency_tech/ll_reflex_vk.h>
#include <low_latency/low_latency_tech/ll_reflex_nvapi_vk.h>
#include <low_latency/low_latency_tech/ll_antilag_vk.h>
#include <low_latency/low_latency_tech/ll_fps_limiter.h>
#include <hooks/Vulkan_Hooks.h>
#ifdef LOW_LATENCY_INPUTS
#include <inputs/FG/XeFG_Inputs_Dx12.h>
#endif
#include <framegen/IFGFeature_Dx12.h>
#include <nvapi/fakenvapi.h>
#include "OutputRule.h"
#include <native/NativeLowLatency.h>
#include <hooks/Reflex_Hooks.h>

#include <deque>
#include <set>

static bool marker_supported(InputMarkerMode mode, MarkerType type)
{
    switch (mode)
    {
    case InputMarkerMode::NoMarkers:
        return false;
    case InputMarkerMode::SimStartOnly:
        return type == MarkerType::SIMULATION_START;
    case InputMarkerMode::SimStartAndPresentStart:
        return type == MarkerType::SIMULATION_START || type == MarkerType::PRESENT_START ||
               type == MarkerType::OUT_OF_BAND_PRESENT_START;
    case InputMarkerMode::SimOnly:
        return type == MarkerType::SIMULATION_START || type == MarkerType::SIMULATION_END;
    default:
        return true;
    }
}

// create_tech_mutex
void InputCommon::retire(std::shared_ptr<LowLatencyTech> tech)
{
    if (tech == nullptr)
        return;

    if (tech->get_mode() != LowLatencyMode::AntiLag2 || State::Instance().activeFgOutput != FGOutput::FSRFG)
    {
        tech->deinit();
        return;
    }

    // One at a time: an older one FSR FG no longer has goes now
    if (retired_antilag2 != nullptr)
        retired_antilag2->deinit();

    retired_antilag2 = std::move(tech);
    retired_presents = 3;
}

// private
bool InputCommon::deinit_current_tech()
{
    // currently_active_tech becomes nullptr
    // but we need to wait for all users of the old one to release
    auto old_tech = currently_active_tech.exchange(nullptr);
    reflex_vk_stale = false;

    if (old_tech)
    {
        LOG_TRACE("Deiniting current tech");

        // XeFG's XeLL stays XeFG's: it only stops being the output (and gets its low latency off next to the new one)
        if (old_tech != xefg_xell.load())
        {
            while (old_tech.use_count() > 1)
                std::this_thread::yield();

            retire(std::move(old_tech));
        }

        output_device = nullptr;
        clear_reports();

        return true;
    }

    return false;
}

// Devices whose VK_NV_low_latency2 sleep never signalled (ReflexVk::gave_up_sleeping), until they go
static std::mutex g_lowLatency2FailedMutex;
static std::set<VkDevice> g_lowLatency2Failed;

static bool LowLatency2Failed(VkDevice device)
{
    std::scoped_lock lock(g_lowLatency2FailedMutex);
    return g_lowLatency2Failed.contains(device);
}

// What the device has for the driver's Reflex now, without asking nvapi
static lloutput::ReflexVkInputs ReflexVkInputsOf(VkDevice device)
{
    const auto swapchain = VulkanHooks::LatencySwapchainOf(device);

    lloutput::ReflexVkInputs in;
    in.lowLatency2Device = VulkanHooks::LowLatency2On(device);
    in.swapchainKnown = swapchain.swapchain != VK_NULL_HANDLE;
    in.swapchainLatencyMode = swapchain.latencyMode;
    in.lowLatency2Failed = LowLatency2Failed(device);
    return in;
}

// The driver's Reflex on a Vulkan device (OutputRule.h ChooseReflexVk): VK_NV_low_latency2 where the device has it,
// else NvAPI_Vulkan's. nvapi is only asked when VK_NV_low_latency2 can't run.
static std::shared_ptr<LowLatencyTech> MakeReflexVk(VkDevice device)
{
    auto in = ReflexVkInputsOf(device);

    if (lloutput::ChooseReflexVk(in) != lloutput::ReflexVkSurface::LowLatency2)
    {
        in.nvapiVulkan = ReflexDriverReady() && ReflexHooks::nvapiVulkanReady();
        in.timelineSemaphores = VulkanHooks::TimelineSemaphoresOn(device);
    }

    switch (lloutput::ChooseReflexVk(in))
    {
    case lloutput::ReflexVkSurface::LowLatency2:
        return std::make_shared<ReflexVk>();
    case lloutput::ReflexVkSurface::NvApi:
        return std::make_shared<ReflexNvApiVk>();
    default:
        LOG_INFO("Reflex on Vulkan: neither VK_NV_low_latency2 on this device nor NVIDIA's NvAPI Vulkan Reflex");
        return nullptr;
    }
}

bool InputCommon::init_tech(IUnknown* pDevice, API api, LowLatencyMode desiredMode, const lloutput::OutputPlan& plan)
{
    if (currently_active_tech.load() || delay_deinit != 0)
        return false;

    // A Vulkan device: Anti-Lag 2 is Anti-Lag Vk there, XeLL is D3D12's, Reflex the driver's Vulkan one (pDevice is the
    // VkDevice as a token, never a COM interface)
    auto make = [&](LowLatencyMode mode) -> std::shared_ptr<LowLatencyTech>
    {
        if (api == API::Vulkan)
            mode = for_api(mode, api);

        switch (mode)
        {
        case LowLatencyMode::XeLL:
            return std::make_shared<XeLL>();
        case LowLatencyMode::AntiLag2:
            return std::make_shared<AntiLag2>();
        case LowLatencyMode::Reflex:
            if (api == API::Vulkan)
                return MakeReflexVk((VkDevice) pDevice);

            return std::make_shared<Reflex>();
        case LowLatencyMode::AntiLagVk:
            return std::make_shared<AntiLagVk>();
        case LowLatencyMode::LatencyFlex:
            return std::make_shared<LatencyFlex>();
        case LowLatencyMode::FpsLimiter:
            return std::make_shared<FpsLimiter>();
        default:
            return nullptr;
        }
    };

    // The plan's outputs in order when the plan's first one is wanted (fakenvapi's Anti-Lag 2, XeLL, LatencyFlex)
    std::array<LowLatencyMode, 3> tries { desiredMode, LowLatencyMode::None, LowLatencyMode::None };

    if (desiredMode == to_mode(plan.first()) ||
        (api == API::Vulkan && desiredMode == for_api(to_mode(plan.first()), api)))
    {
        for (size_t i = 0; i < tries.size(); i++)
            tries[i] = to_mode(plan.tries[i]);
    }

    std::shared_ptr<LowLatencyTech> tech;

    for (auto mode : tries)
    {
        if (mode == LowLatencyMode::None)
            continue;

        if (auto candidate = make(mode); candidate != nullptr && candidate->init(pDevice))
        {
            LOG_INFO("LowLatency algo: {}", magic_enum::enum_name(mode));
            tech = std::move(candidate);
            break;
        }

        LOG_INFO("LowLatency algo {} could not start", magic_enum::enum_name(mode));
    }

    if (tech == nullptr)
    {
        failed_output = desiredMode;
        failed_time = std::chrono::steady_clock::now().time_since_epoch().count();
        failed_device = pDevice;

        // With XeFG the output is XeFG's choice: no stand-in next to its XeLL. Anti-Lag 2 or Reflex that can't start
        // hands the split back to XeLL (native::lowlatency::XefgSide): on Vulkan the game's device may have neither
        // VK_NV_low_latency2 nor NVIDIA's NvAPI Vulkan Reflex.
        if (plan.xefgOwnXell)
        {
            if (desiredMode == LowLatencyMode::AntiLag2 && !antilag2_failed.exchange(true))
                LOG_WARN("Anti-Lag 2 could not start next to XeFG's XeLL, XeLL lowers latency");

            if (desiredMode == LowLatencyMode::Reflex && !reflex_failed.exchange(true))
                LOG_WARN("Reflex could not start next to XeFG's XeLL, XeLL lowers latency");

            return false;
        }

        if (desiredMode != LowLatencyMode::LatencyFlex)
        {
            tech = make(LowLatencyMode::LatencyFlex);

            if (!tech->init(pDevice))
                return false;

            LOG_INFO("LowLatency algo: LatencyFlex (Fallback)");
        }
        else
        {
            return false;
        }
    }

    currently_active_tech.store(tech);
    activeOutput = tech->get_mode();
    output_api = api;
    output_device = pDevice;

    if (api == API::Vulkan)
        LOG_INFO("Low latency output on the Vulkan device: {}", magic_enum::enum_name(activeOutput.load()));

    // XeFG needs XeLL in low latency mode, disabling it only stops sending the sleep and marker calls
    tech->set_forced_mode(xefg_paced());
    tech->set_low_latency_override(Config::Instance()->FN_ForceReflex.value_or_default());

    apply_sleep_mode(tech.get()); // Restore any potential sleep mode
    return true;
}

LowLatencyMode InputCommon::to_mode(lloutput::Tech tech)
{
    switch (tech)
    {
    case lloutput::Tech::Reflex:
        return LowLatencyMode::Reflex;
    case lloutput::Tech::AntiLag2:
        return LowLatencyMode::AntiLag2;
    case lloutput::Tech::XeLL:
        return LowLatencyMode::XeLL;
    case lloutput::Tech::LatencyFlex:
        return LowLatencyMode::LatencyFlex;
    case lloutput::Tech::FrameCap:
        return LowLatencyMode::FpsLimiter;
    default:
        return LowLatencyMode::None;
    }
}

lloutput::OutputPlan InputCommon::plan_output(API api, IUnknown* pDevice)
{
    auto& state = State::Instance();

    // Once: getPrimaryGpu copies the GPU list, and the primary GPU does not change while the game runs
    static const bool nvidiaGpu = IdentifyGpu::getPrimaryGpu().vendorId == VendorId::Nvidia;

    // VK_NV_low_latency2 is the driver's Reflex without nvapi: a Vulkan device with it has Reflex under fakenvapi too
    const bool lowLatency2 = api == API::Vulkan && VulkanHooks::LowLatency2On((VkDevice) pDevice);

    lloutput::OutputInputs in;
    in.realNvapi = nvidiaGpu && (!fakenvapi::isUsingAsMainNvapi() || lowLatency2);
    in.vulkan = api == API::Vulkan;
    in.xefgOutput = UsesXeFGApi(state.activeFgOutput);
    in.side = native::lowlatency::XefgSide();
    in.forceLatencyFlex = Config::Instance()->FN_ForceLatencyFlex.value_or_default();
    in.frameCapOnly = Config::Instance()->FramerateFrameCapOnly.value_or_default();
    in.multiFrame = state.dlssgDetectedInterpolationCount > 1 || state.dlssgLastSetMode == sl::DLSSGMode::eDynamic;
    in.inputFeedsXell = incompatibility(activeInput.load(), LowLatencyMode::XeLL) == nullptr;
    return lloutput::PlanOutput(in);
}

// The tech becomes the output (XeFG's XeLL while XeLL lowers latency), the one before it stops (create_tech_mutex)
void InputCommon::use_output(std::shared_ptr<LowLatencyTech> tech)
{
    auto old_tech = currently_active_tech.exchange(tech);
    reflex_vk_stale = false;

    if (old_tech != nullptr && old_tech != xefg_xell.load())
    {
        while (old_tech.use_count() > 1)
            std::this_thread::yield();

        retire(std::move(old_tech));
    }

    activeOutput = tech->get_mode();
    output_api = API::DX12;
    output_device = nullptr;

    LOG_INFO("Low latency output: {} (XeFG's)", magic_enum::enum_name(activeOutput.load()));

    tech->set_forced_mode(xefg_paced());
    tech->set_low_latency_override(Config::Instance()->FN_ForceReflex.value_or_default());
    apply_sleep_mode(tech.get());
}

LowLatencyMode InputCommon::default_output(API api)
{
    // TODO: add avaliableOutput, somehow ?
    auto vendorId = IdentifyGpu::getPrimaryGpu().vendorId;

    // Only the device's extensions, there's no XeLL for Vulkan
    if (api == API::Vulkan)
    {
        if (vendorId == VendorId::AMD && VulkanHooks::o_vkAntiLagUpdateAMD != nullptr)
            return LowLatencyMode::AntiLagVk;

        if (vendorId == VendorId::Nvidia && !fakenvapi::isUsingAsMainNvapi())
            return LowLatencyMode::Reflex;

        return LowLatencyMode::LatencyFlex;
    }

    // XeLL is D3D12 only
    if (vendorId == VendorId::Intel && api != API::DX11)
        return LowLatencyMode::XeLL;

    if (vendorId == VendorId::AMD)
        return LowLatencyMode::AntiLag2;

    if (vendorId == VendorId::Nvidia && !fakenvapi::isUsingAsMainNvapi())
        return LowLatencyMode::Reflex;

    return LowLatencyMode::LatencyFlex;
}

const char* InputCommon::output_unavailable(LowLatencyMode output, API api)
{
    auto vendorId = IdentifyGpu::getPrimaryGpu().vendorId;

    // On D3D the driver's Reflex is reached through nvapi, fakenvapi's is a translation of its own
    if (output == LowLatencyMode::Reflex &&
        (vendorId != VendorId::Nvidia || (api != API::Vulkan && fakenvapi::isUsingAsMainNvapi())))
    {
        return "Needs an Nvidia GPU";
    }

    if ((output == LowLatencyMode::AntiLag2 || output == LowLatencyMode::AntiLagVk) && vendorId != VendorId::AMD)
        return "Needs an AMD GPU";

    return nullptr;
}

LowLatencyMode InputCommon::for_api(LowLatencyMode mode, API api)
{
    if (api == API::Vulkan && mode == LowLatencyMode::AntiLag2)
        return LowLatencyMode::AntiLagVk;

    if (api != API::Vulkan && mode == LowLatencyMode::AntiLagVk)
        return LowLatencyMode::AntiLag2;

    if (api != API::DX12 && mode == LowLatencyMode::XeLL)
        return default_output(api);

    return mode;
}

API InputCommon::device_api(const InputContext& inputContext, IUnknown* pDevice)
{
    if (inputContext.api != API::NotSelected)
        return inputContext.api;

    // Reflex and UE take either D3D device
    thread_local IUnknown* lastDevice = nullptr;
    thread_local API lastApi = API::NotSelected;

    if (pDevice == nullptr)
        return lastApi;

    if (pDevice != lastDevice)
    {
        ID3D11Device* device11 = nullptr;

        if (pDevice->QueryInterface(IID_PPV_ARGS(&device11)) == S_OK)
        {
            device11->Release();
            lastApi = API::DX11;
        }
        else
        {
            lastApi = API::DX12;
        }

        lastDevice = pDevice;
    }

    return lastApi;
}

API InputCommon::register_call(const InputContext& inputContext, IUnknown* pDevice, bool query)
{
    auto api = device_api(inputContext, pDevice);

    // Ignore context that Opti creates, and calls that only ask
    if (!inputContext.localContext && !query)
    {
        if (api != API::NotSelected)
            input_api[static_cast<size_t>(inputContext.caller)] = api;

        available_inputs.fetch_or(1u << static_cast<uint32_t>(inputContext.caller), std::memory_order_relaxed);
    }

    return api;
}

API InputCommon::uses_api()
{
    if (activeInput != LowLatencyInput::None)
    {
        if (auto api = input_api[static_cast<size_t>(activeInput.load())].load(); api != API::NotSelected)
            return api;
    }

    return State::Instance().swapchainApi;
}

std::optional<LowLatencyMode> InputCommon::forced_output()
{
    // The game's XeFG paces through its own XeLL (no output). OptiScaler's XeFG takes XeLL, Reflex or AntiLag 2
    // (xefg_output), its DLSSG and FSR-FG any output, the game's own FSR-FG and DLSSG default to their own
    // (default_input, default_output).
    if (InputXeLL::IsNative())
        return LowLatencyMode::XeLL;

    return std::nullopt;
}

bool InputCommon::xefg_paced()
{
    // OptiScaler's XeFG needs XeLL in low latency mode and knows its generated frames
    return activeOutput == LowLatencyMode::XeLL && UsesXeFGApi(State::Instance().activeFgOutput);
}

LowLatencyInput InputCommon::default_input()
{
    lloutput::InputChoice in;
#ifdef LOW_LATENCY_INPUTS
    in.lowLatencyInputs = true;
    in.xefgInput = State::Instance().activeFgInput == FGInput::XeFG;
    in.xell = available(LowLatencyInput::XeLL);
    in.xellLowLatency = in.xell && get_sleep_copy(LowLatencyInput::XeLL).low_latency_enabled;
    in.antiLag2 = available(LowLatencyInput::AntiLag2);
    in.antiLag2ReportsFg = in.antiLag2 && reports_frame_generation(LowLatencyInput::AntiLag2);
#endif
    in.reflex = available(LowLatencyInput::Reflex);
    in.optiscalerRunning = optiscaler_running;
    in.ue = available(LowLatencyInput::UeLowLatency);
    in.antiLagVk = available(LowLatencyInput::AntiLag2) &&
                   input_api[static_cast<size_t>(LowLatencyInput::AntiLag2)].load() == API::Vulkan;

    switch (lloutput::ChooseInput(in))
    {
    case lloutput::Input::Reflex:
        return LowLatencyInput::Reflex;
    case lloutput::Input::OptiScaler:
        return LowLatencyInput::OptiScaler;
    case lloutput::Input::UeLowLatency:
        return LowLatencyInput::UeLowLatency;
    case lloutput::Input::XeLL:
        return LowLatencyInput::XeLL;
    case lloutput::Input::AntiLag2:
        return LowLatencyInput::AntiLag2;
    default:
        return LowLatencyInput::None;
    }
}

const char* InputCommon::incompatibility(LowLatencyInput input, LowLatencyMode output)
{
    // Reflex and XeLL send everything, with frame ids. OptiScaler sends its sleep and present markers, with ids.
    if (input != LowLatencyInput::AntiLag2 && input != LowLatencyInput::UeLowLatency &&
        input != LowLatencyInput::OptiScaler)
    {
        return nullptr;
    }

    if (output == LowLatencyMode::XeLL && input != LowLatencyInput::OptiScaler)
    {
        return input == LowLatencyInput::AntiLag2
                   ? "XeLL needs frame ids and frame markers, AntiLag 2 has neither"
                   : "XeLL needs render submit and present markers, UE only sends simulation markers";
    }

    // The other modes end frames on render submit markers, Reflex ID also sleeps on simulation start markers
    if (output == LowLatencyMode::LatencyFlex &&
        (LFXMode) Config::Instance()->FN_LatencyFlexMode.value_or_default() != LFXMode::Conservative)
    {
        return "LatencyFlex only works in the Conservative mode with this input";
    }

    return nullptr;
}

std::optional<LowLatencyInput> InputCommon::forced_input()
{
    // The game's XeFG (also with the XeFG passthrough) runs on the game's XeLL
#ifdef LOW_LATENCY_INPUTS
    if (XeFGInputs::Passthrough() || InputXeLL::IsNative())
        return LowLatencyInput::XeLL;
#endif

    return std::nullopt;
}

bool InputCommon::update_for_call(const InputContext& inputContext, IUnknown* pDevice, bool query)
{
    const auto api = register_call(inputContext, pDevice, query);

    // A query only reads what runs: it starts, switches or stops no output (the menu asks from its own thread), and
    // doesn't count down the Anti-Lag 2 delay
    if (query)
        return true;

    return update_low_latency_tech(pDevice, api);
}

bool InputCommon::update_low_latency_tech(IUnknown* pDevice, API api, std::optional<LowLatencyMode> mode)
{
    // Nobody calls yet
    if (available_inputs.load(std::memory_order_relaxed) == 0 && !optiscaler_running && !mode.has_value())
        return true;

    // Some outputs might call sleep with nullptr: allowed with an output, not to set one
    if (!pDevice)
        return activeOutput != LowLatencyMode::None && !mode.has_value();

    // Who makes the calls that get through
    if (const auto desiredInput = default_input(); activeInput != desiredInput)
    {
        activeInput = desiredInput;
        LOG_INFO("Low latency input: {}", magic_enum::enum_name(desiredInput));

        // The previous input's frame reports would stay in the overlay, its frame ids don't continue
        clear_reports();

        for (auto& id : last_marker_frame_ids)
            id = 0;

        // The new input's sleep mode
        if (auto current_tech = currently_active_tech.load())
            apply_sleep_mode(current_tech.get());
    }

    // The output runs on the active input's device, another API's device can't start it
    if (auto inputApi = input_api[static_cast<size_t>(activeInput.load())].load();
        !mode.has_value() && inputApi != API::NotSelected && api != inputApi)
    {
        return true;
    }

    // OptiScaler's rules decide the output (OutputRule.h PlanOutput)
    const auto plan = plan_output(api, pDevice);
    auto desiredMode = mode.has_value() ? *mode : to_mode(plan.first());

    if (desiredMode == LowLatencyMode::None)
        return true;

    // XeFG paces with its XeLL: while XeLL lowers latency that context is the output, whatever device the call brings
    // (a D3D11 game's through the D3D11-to-D3D12 bridge)
    if (desiredMode == LowLatencyMode::XeLL)
    {
        if (auto own = xefg_xell.load(); own != nullptr)
        {
            if (currently_active_tech.load() != own)
            {
                std::scoped_lock lock(create_tech_mutex);

                if (currently_active_tech.load() != own)
                    use_output(own);
            }

            return true;
        }
    }

    // On a Vulkan device the output is the API's own form (Anti-Lag Vk, the driver's Vulkan Reflex, no XeLL without
    // XeFG): compared as such, or a running one would be made again on every call
    if (api == API::Vulkan)
        desiredMode = for_api(desiredMode, api);

    // An output that couldn't start keeps its fallback instead of being retried every call; on a Vulkan device never
    // again (its extensions and the driver's NvAPI Vulkan Reflex don't come later), elsewhere after 10 s
    const auto failedAt = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(failed_time.load()));
    const bool failedHere = desiredMode == failed_output && (api != API::Vulkan || failed_device.load() == pDevice);
    const bool failedRecently =
        failedHere && (api == API::Vulkan || std::chrono::steady_clock::now() - failedAt < std::chrono::seconds(10));

    if (failedRecently && (currently_active_tech.load() != nullptr || plan.xefgOwnXell))
        return true;

    // A VK_NV_low_latency2 output that can't go on is made again on what its device has now
    if (activeOutput == desiredMode && api == output_api && !reflex_vk_stale.load(std::memory_order_relaxed))
    {
        delay_deinit = 0;
        return true;
    }

    // Beyond this point activeOutput needs changing

    std::scoped_lock lock(create_tech_mutex);

    if (init_tech(pDevice, api, desiredMode, plan))
        return true;

    // Next to XeFG an output that can't start is no error to the game's call (some games turn their Reflex off on one):
    // XeFG keeps its XeLL, and Anti-Lag 2 that can't start hands the split back to XeLL (native::lowlatency::XefgSide)
    if (plan.xefgOwnXell && currently_active_tech.load() == nullptr)
        return true;

    auto try_reinit = [&]() -> bool
    {
        if (!deinit_current_tech())
        {
            // Nothing was running: the output could not start
            return currently_active_tech.load() != nullptr;
        }

        activeOutput = LowLatencyMode::None;
        output_api = API::NotSelected;

        return init_tech(pDevice, api, desiredMode, plan);
    };

    // WAR: FSR FG might still be using AntiLag 2, give Opti time to set AL2 context to null
    if (delay_deinit > 0)
    {
        if (--delay_deinit == 0)
            return try_reinit();
    }
    else
    {
        bool al2 = false;
        {
            auto current_tech = currently_active_tech.load();
            if (current_tech && current_tech->get_mode() == LowLatencyMode::AntiLag2)
                al2 = true;
        }

        if (al2)
        {
            delay_deinit = 50;
        }
        else
        {
            return try_reinit();
        }
    }

    return true;
}

void InputCommon::clear_reports()
{
    std::scoped_lock lock(report_mutex);
    std::memset(frame_reports, 0, sizeof(frame_reports));
}

void InputCommon::add_marker_to_report(const MarkerParams& marker_params)
{
    std::scoped_lock lock(report_mutex);

    auto current_timestamp = Util::GetTimestamp() / 1000;
    static auto last_sim_start = current_timestamp;
    static auto _2nd_last_sim_start = current_timestamp;
    auto current_report = &frame_reports[marker_params.frame_id % FRAME_REPORTS_BUFFER_SIZE];

    if (current_report->frameID != marker_params.frame_id)
    {
        *current_report = FrameReport {};
    }

    current_report->frameID = marker_params.frame_id;
    current_report->gpuFrameTimeUs = (uint32_t) (last_sim_start - _2nd_last_sim_start);
    current_report->gpuActiveRenderTimeUs = 100;
    current_report->driverStartTime = current_timestamp;
    current_report->driverEndTime = current_timestamp + 100;
    current_report->gpuRenderStartTime = current_timestamp;
    current_report->gpuRenderEndTime = current_timestamp + 100;
    current_report->osRenderQueueStartTime = current_timestamp;
    current_report->osRenderQueueEndTime = current_timestamp + 100;
    switch (marker_params.marker_type)
    {
    case MarkerType::SIMULATION_START:
        _2nd_last_sim_start = last_sim_start;
        last_sim_start = Util::GetTimestamp() / 1000;
        current_report->simStartTime = last_sim_start;
        break;
    case MarkerType::SIMULATION_END:
        current_report->simEndTime = Util::GetTimestamp() / 1000;
        break;
    case MarkerType::RENDERSUBMIT_START:
        current_report->renderSubmitStartTime = Util::GetTimestamp() / 1000;
        break;
    case MarkerType::RENDERSUBMIT_END:
        current_report->renderSubmitEndTime = Util::GetTimestamp() / 1000;
        break;
    case MarkerType::PRESENT_START:
        current_report->presentStartTime = Util::GetTimestamp() / 1000;
        break;
    case MarkerType::PRESENT_END:
        current_report->presentEndTime = Util::GetTimestamp() / 1000;
        break;
    case MarkerType::INPUT_SAMPLE:
        current_report->inputSampleTime = Util::GetTimestamp() / 1000;
        break;
    default:
        break;
    }
}

// public
void InputCommon::stop_output(bool release)
{
    std::scoped_lock lock(create_tech_mutex);

    auto old_tech = currently_active_tech.exchange(nullptr);
    reflex_vk_stale = false;

    if (old_tech == nullptr)
        return;

    // XeFG's XeLL is XeFG's to let go of (release_xefg_xell)
    if (old_tech != xefg_xell.load())
    {
        while (old_tech.use_count() > 1)
            std::this_thread::yield();

        if (release)
            retire(std::move(old_tech));
    }

    LOG_INFO("Low latency output {} stopped{}", magic_enum::enum_name(activeOutput.load()),
             release ? "" : " (device gone)");

    clear_reports();
    activeOutput = LowLatencyMode::None;
    output_api = API::NotSelected;
    output_device = nullptr;
}

InputResult InputCommon::set_low_latency_tech(IUnknown* pDevice, LowLatencyMode mode)
{
    if (!update_low_latency_tech(pDevice, API::DX12, mode))
        return InputResult::LowLatencyUpdateFail;

    return InputResult::Ok;
}

InputResult InputCommon::sleep(const InputContext& inputContext, IUnknown* pDevice, std::optional<uint32_t> frame_id)
{
    if (!update_for_call(inputContext, pDevice))
        return InputResult::LowLatencyUpdateFail;

    if (inputContext.caller != activeInput)
        return InputResult::UsingDifferentInput;


    if (auto current_tech = currently_active_tech.load())
    {
        const auto start = Util::MillisecondsNow();
        current_tech->sleep(frame_id);
        slept_ms.fetch_add(Util::MillisecondsNow() - start);
    }
    else
    {
        return InputResult::NoReadyOutput;
    }

    return InputResult::Ok;
}

InputResult InputCommon::set_marker(const InputContext& inputContext, IUnknown* pDevice,
                                    const MarkerParams& marker_params, bool toOutput)
{
    if (!update_for_call(inputContext, pDevice))
        return InputResult::LowLatencyUpdateFail;

    if (inputContext.caller != activeInput)
        return InputResult::UsingDifferentInput;

    if (!marker_supported(inputContext.markerMode, marker_params.marker_type))
        return InputResult::InputNotSupported;

    // Some games send a frame's marker twice (The Witcher 3 ends its render submit again after present), the first
    // one is the right one. Frame id 0 is what inputs without frame ids send.
    if (auto type = (size_t) marker_params.marker_type;
        type < last_marker_frame_ids.size() && marker_params.frame_id != 0)
    {
        if (last_marker_frame_ids[type].exchange(marker_params.frame_id) == marker_params.frame_id)
            return InputResult::Ok;
    }


    if (marker_params.marker_type == MarkerType::PRESENT_START)
        last_present_start_frame_id = marker_params.frame_id;

    // Without frame ids there's nothing to report, the overlay's timings need them
    if (!inputContext.noFrameId)
        add_marker_to_report(marker_params);

    if (!toOutput)
        return InputResult::Ok;

    auto current_tech = currently_active_tech.load();

    // XeFG's own XeLL next to a Reflex or Anti-Lag 2 output gets the game's markers too, as fakenvapi's XeLL did; with
    // its low latency off (forced mode) it drops them itself (XeLL::add_marker)
    if (marker_params.frame_id != 0)
    {
        if (auto own = xefg_xell.load(); own != nullptr && own != current_tech)
            own->set_marker(pDevice, marker_params);
    }

    // Markers without a frame id only let AntiLag 2 and LatencyFlex follow the sleep calls, Reflex and XeLL use ids
    if (marker_params.frame_id == 0 && (activeOutput == LowLatencyMode::Reflex || activeOutput == LowLatencyMode::XeLL))
    {
        return InputResult::Ok;
    }

    if (current_tech)
        current_tech->set_marker(pDevice, marker_params);
    else
        return InputResult::NoReadyOutput;

    LOG_TRACE_LOWLATENCY("{}: {}", magic_enum::enum_name(marker_params.marker_type), marker_params.frame_id);

    return InputResult::Ok;
}

InputResult InputCommon::set_async_marker(const InputContext& inputContext, ID3D12CommandQueue* pCommandQueue,
                                          const MarkerParams& marker_params)
{
    // Async markers only come with frame generation, the game's own when it's the game's input
    if (!inputContext.localContext)
        mark_frame_generation(inputContext.caller);

    // Always allow Opti's local contexts through
    if (inputContext.caller != activeInput && !inputContext.localContext)
        return InputResult::UsingDifferentInput;

    auto current_tech = currently_active_tech.load();

    // Can't init using ID3D12CommandQueue, can only check if available. Opti's own context (the Anti-Lag 2 one injected
    // into FSR-FG) marks frames whether or not a low latency tech runs; without one there is nothing to mark, which is
    // not a failure
    if (current_tech == nullptr)
        return inputContext.localContext ? InputResult::Ok : InputResult::LowLatencyUpdateFail;

    if (!marker_supported(inputContext.markerMode, marker_params.marker_type))
        return InputResult::InputNotSupported;

    // With XeFG its XeLL gets them too, as the output when XeLL lowers latency (the game's DLSSG markers while XeFG
    // follows the game's DLSS FG)
    if (auto own = xefg_xell.load(); own != nullptr && own != current_tech)
        own->set_async_marker(pCommandQueue, marker_params);

    current_tech->set_async_marker(pCommandQueue, marker_params);

    LOG_TRACE_LOWLATENCY("{}: {}", magic_enum::enum_name(marker_params.marker_type), marker_params.frame_id);

    return InputResult::Ok;
}

InputResult InputCommon::set_sleep_mode(const InputContext& inputContext, IUnknown* pDevice, SleepMode* sleep_mode)
{
    // Kept even without an output yet, and for an input that is not the active one: AntiLag 2 and XeLL only send it
    // when it changes, and an input that becomes the active one brings its mode along. OptiScaler's carries a cap
    // planned for XeFG's side of now.
    if (inputContext.caller == LowLatencyInput::OptiScaler)
        optiscaler_mode_side = native::lowlatency::XefgSide();

    set_sleep_copy(inputContext.caller, *sleep_mode);
    sleep_modes_set.fetch_or(1u << static_cast<uint32_t>(inputContext.caller), std::memory_order_relaxed);

    if (!update_for_call(inputContext, pDevice))
        return InputResult::LowLatencyUpdateFail;

    if (inputContext.caller != activeInput)
        return InputResult::UsingDifferentInput;

    if (auto current_tech = currently_active_tech.load())
        apply_sleep_mode(current_tech.get());
    else
        return InputResult::NoReadyOutput;

    return InputResult::Ok;
}

InputResult InputCommon::get_sleep_status(const InputContext& inputContext, IUnknown* pDevice,
                                          SleepParams* sleep_params)
{
    if (!update_for_call(inputContext, pDevice, true))
        return InputResult::LowLatencyUpdateFail;

    // Get functions don't really need to worry about this check
    // if (inputContext.caller != activeInput)
    //     return InputResult::UsingDifferentInput;

    if (auto current_tech = currently_active_tech.load())
        current_tech->get_sleep_status(sleep_params);
    else
        return InputResult::NoReadyOutput;

    return InputResult::Ok;
}

InputResult InputCommon::get_latency(const InputContext& inputContext, IUnknown* pDev, void* latency_params)
{
    // if (inputContext.caller != activeInput)
    //     return InputResult::UsingDifferentInput;

    if (!update_for_call(inputContext, pDev, true))
        return InputResult::LowLatencyUpdateFail;

    if (!latency_params)
        return InputResult::InvalidParameter;

    // OptiScaler's own Streamline uses Reflex too
    if (inputContext.caller == LowLatencyInput::Reflex || inputContext.caller == LowLatencyInput::OptiScaler)
    {
        // The driver's report with a Reflex output (D3D or Vulkan), else the markers' own
        if (bool estimated = true; activeOutput == LowLatencyMode::Reflex &&
                                   output_latency((NV_LATENCY_RESULT_PARAMS*) latency_params, &estimated) == NVAPI_OK &&
                                   !estimated)
        {
            return InputResult::Ok;
        }

        NV_LATENCY_RESULT_PARAMS* reports = (NV_LATENCY_RESULT_PARAMS*) latency_params;

        if (reports->version != NV_LATENCY_RESULT_PARAMS_VER1)
        {
            LOG_ERROR("Unsupported version {}", reports->version);
            return InputResult::InvalidParameter;
        }

        static_assert(sizeof(reports->frameReport) == NVAPI_BUFFER_SIZE * sizeof(FrameReport));

        if (!copy_frame_reports((FrameReport*) reports->frameReport))
            return InputResult::NotEnoughReports;

        return InputResult::Ok;
    }
    else if (inputContext.caller == LowLatencyInput::XeLL)
    {
        xell_frame_report_t* reports = (xell_frame_report_t*) latency_params;
        constexpr size_t reportCount = 64; // 64 reports, if the app allocated less then it's on them

        if (activeOutput == LowLatencyMode::XeLL)
        {
            if (auto current_tech = currently_active_tech.load())
            {
                auto xell_tech = std::static_pointer_cast<XeLL>(current_tech);
                auto result = xell_tech->xellGetFramesReports(reports);

                if (result == XELL_RESULT_SUCCESS)
                    return InputResult::Ok;
            }
        }

        // Hopefully not too slow
        // XeLL doesn't seem to make any guarantees about ordering so no sort needed
        std::scoped_lock lock(report_mutex);

        for (auto i = 0; i < reportCount; i++)
        {
            auto& reportOut = reports[i];
            auto& reportIn = frame_reports[i];

            reportOut.m_frame_id = reportIn.frameID & 0xFFFFFFFF;
            reportOut.m_sim_start_ts = reportIn.simStartTime;
            reportOut.m_sim_end_ts = reportIn.simEndTime;
            reportOut.m_render_submit_start_ts = reportIn.renderSubmitStartTime;
            reportOut.m_render_submit_end_ts = reportIn.renderSubmitEndTime;
            reportOut.m_present_start_ts = reportIn.presentStartTime;
            reportOut.m_present_end_ts = reportIn.presentEndTime;
        }

        return InputResult::Ok;
    }
    else
    {
        return InputResult::InputNotSupported;
    }
}

bool InputCommon::copy_frame_reports(FrameReport* reports)
{
    std::scoped_lock lock(report_mutex);

    // Assume no frame reports collected yet, report all zeros
    if (frame_reports[FRAME_REPORTS_BUFFER_SIZE - 1].frameID == 0)
    {
        std::memset(reports, 0, NVAPI_BUFFER_SIZE * sizeof(FrameReport));
        // spdlog::warn("GetLatency: Not enough data to report");
        return false;
    }

    // Sort frame reports, find the oldest
    size_t minIdx = 0;
    uint64_t minID = frame_reports[0].frameID;
    for (size_t i = 1; i < FRAME_REPORTS_BUFFER_SIZE; i++)
    {
        if (frame_reports[i].frameID < minID)
        {
            minID = frame_reports[i].frameID;
            minIdx = i;
        }
    }

    // Copy starting from older before wrapping around
    size_t firstChunk = std::min<uint64_t>(NVAPI_BUFFER_SIZE, FRAME_REPORTS_BUFFER_SIZE - minIdx);
    std::memcpy(reports, frame_reports + minIdx, firstChunk * sizeof(FrameReport));

    // Copy the rest after wrapping around
    if (firstChunk < NVAPI_BUFFER_SIZE)
        std::memcpy(reports + firstChunk, frame_reports, (NVAPI_BUFFER_SIZE - firstChunk) * sizeof(FrameReport));

    return true;
}

InputResult InputCommon::get_frame_reports(const InputContext& inputContext, IUnknown* pDev, FrameReport* reports)
{
    if (!update_for_call(inputContext, pDev))
        return InputResult::LowLatencyUpdateFail;

    if (reports == nullptr)
        return InputResult::InvalidParameter;

    return copy_frame_reports(reports) ? InputResult::Ok : InputResult::NotEnoughReports;
}

#define UPDATE_TIMING_ENTRY(name, type)                                                                                \
    if (frameReport.name##EndTime >= frameReport.name##StartTime)                                                      \
    {                                                                                                                  \
        double name##Pos = (double) (frameReport.name##StartTime - start) / rangeNs;                                   \
        double name##Length = (double) (frameReport.name##EndTime - frameReport.name##StartTime) / rangeNs;            \
        timingDataOut.type = TimingEntry { .position = name##Pos, .length = name##Length };                            \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
        timingDataOut.type.reset();                                                                                    \
    }

bool InputCommon::get_timing_data(TimingData& timingDataOut)
{
    InputContext tempContext {};

    if (activeOutput == LowLatencyMode::Reflex)
    {
        // TODO: allocate struct and get everything from reflex
        timingDataOut = {};
        return false;
    }

    // auto processFrameReport = [&](const auto& frameReport) -> bool
    //{
    //     uint64_t start = UINT64_MAX;
    //     uint64_t end = 0;

    //    // Please don't look, just thought it would be least work
    //    auto pTimes = (const uint64_t*) &frameReport.simStartTime;
    //    for (auto i = 0; i < 11; i++)
    //    {
    //        auto& time = pTimes[i];
    //        if (time == 0)
    //            continue;

    //        if (time < start)
    //            start = time;

    //        if (time > end)
    //            end = time;
    //    }

    //    if (end < start)
    //        return false;

    //    double rangeNs = static_cast<double>(end - start);

    //    timingData[TimingType::TimeRange] = TimingEntry { .position = 0, .length = rangeNs };
    //    UPDATE_TIMING_ENTRY(sim, Simulation)
    //    UPDATE_TIMING_ENTRY(renderSubmit, RenderSubmit)
    //    UPDATE_TIMING_ENTRY(present, Present)
    //    UPDATE_TIMING_ENTRY(driver, Driver)
    //    UPDATE_TIMING_ENTRY(osRenderQueue, OsRenderQueue)
    //    UPDATE_TIMING_ENTRY(gpuRender, GpuRender)

    //    if (frameReport.frameID != 0)
    //        State::Instance().reflexFrameId = frameReport.frameID;

    //    return true;
    //};

    // if (_lastSleepDev && o_NvAPI_D3D_GetLatency)
    //{
    //     // Not calling free on this but it's static so hopefully fine
    //     static NV_LATENCY_RESULT_PARAMS* results = new NV_LATENCY_RESULT_PARAMS();
    //     results->version = NV_LATENCY_RESULT_PARAMS_VER;

    //    if (auto result = hkNvAPI_D3D_GetLatency(_lastSleepDev, results); result != NVAPI_OK)
    //    {
    //        LOG_WARN("NvAPI_D3D_GetLatency failed: {}", magic_enum::enum_name(result));
    //        return false;
    //    }

    //    // 64th element has the latest data
    //    return processFrameReport(results->frameReport[63]);
    //}

    // if (_lastVkSleepDev && o_NvAPI_Vulkan_GetLatency)
    //{
    //     // Not calling free on this but it's static so hopefully fine
    //     static NV_VULKAN_LATENCY_RESULT_PARAMS* results = new NV_VULKAN_LATENCY_RESULT_PARAMS();
    //     results->version = NV_VULKAN_LATENCY_RESULT_PARAMS_VER;

    //    if (auto result = hkNvAPI_Vulkan_GetLatency(_lastVkSleepDev, results); result != NVAPI_OK)
    //    {
    //        LOG_WARN("NvAPI_Vulkan_GetLatency failed: {}", magic_enum::enum_name(result));
    //        return false;
    //    }

    //    // 64th element has the latest data
    //    return processFrameReport(results->frameReport[63]);
    //}

    {
        std::scoped_lock lock(report_mutex);

        size_t maxIdx = 0;
        uint64_t maxID = frame_reports[0].frameID;
        for (size_t i = 1; i < FRAME_REPORTS_BUFFER_SIZE; i++)
        {
            if (frame_reports[i].frameID > maxID)
            {
                maxID = frame_reports[i].frameID;
                maxIdx = i;
            }
        }

        auto highestValidId = (NVAPI_BUFFER_SIZE + maxIdx) % FRAME_REPORTS_BUFFER_SIZE;
        auto& frameReport = frame_reports[highestValidId]; // grab last one

        uint64_t start = UINT64_MAX;
        uint64_t end = 0;

        // Please don't look, just thought it would be least work
        auto pTimes = (const uint64_t*) &frameReport.simStartTime;
        for (auto i = 0; i < 11; i++)
        {
            auto& time = pTimes[i];
            if (time == 0)
                continue;

            if (time < start)
                start = time;

            if (time > end)
                end = time;
        }

        // No reports (an input without frame ids, or one that just changed) or just one marker
        if (end <= start)
        {
            timingDataOut = {};
            return false;
        }

        double rangeNs = static_cast<double>(end - start);

        timingDataOut.timeRange = TimingEntry { .position = 0, .length = rangeNs };
        UPDATE_TIMING_ENTRY(sim, simulation)
        UPDATE_TIMING_ENTRY(renderSubmit, renderSubmit)
        UPDATE_TIMING_ENTRY(present, present)
        UPDATE_TIMING_ENTRY(driver, driver)
        UPDATE_TIMING_ENTRY(osRenderQueue, osRenderQueue)
        UPDATE_TIMING_ENTRY(gpuRender, gpuRender)

        return true;
    };

    return false;
}

// XeFG presents each batch on its own thread, paced over the game's frame, while the game already submits the next
// one. Each batch keeps the frame id XeFG marked it with and the number of frames XeFG sends for it.
namespace
{
struct XeFGBatch
{
    uint64_t frame_id = 0;
    uint32_t frames = 0; // The multiplier until XeFG reports its own count after the game's present
    uint32_t presented = 0;
};

std::mutex xefg_batch_mutex;
std::deque<XeFGBatch> xefg_batches; // Marked, not presented yet
XeFGBatch xefg_batch;               // Presenting
uint32_t xefg_types_logged = 0;     // AntiLag 2 frame types logged since FG started
constexpr uint32_t xefg_types_log_limit = 16;
constexpr size_t xefg_max_batches = 8;

void reset_xefg_batches()
{
    std::scoped_lock lock(xefg_batch_mutex);
    xefg_batches.clear();
    xefg_batch = {};
    xefg_types_logged = 0;
}
} // namespace

// As DLSSG on its present thread: out of band present markers around each of XeFG's presents, with the game's frame id
// of the batch XeFG marked (xefg_marker). They also give AntiLag 2 the frame type; FSR-FG tells AntiLag 2 itself.
void InputCommon::fg_output_present(ID3D12CommandQueue* presentQueue, bool presented)
{
    // Only XeFG's presenter thread calls this
    auto tech = currently_active_tech.load();
    auto own = xefg_xell.load();

    // Not while XeLL is the output (XeFG marks it itself), and not once XeFG let go of its XeLL (release_xefg_xell
    // released the queue: a present still on its way would keep a reference nobody releases)
    if (presentQueue == nullptr || own == nullptr || tech == nullptr || tech == own)
        return;

    if (xefg_present_queue.load() != presentQueue)
    {
        presentQueue->AddRef();

        if (auto old = xefg_present_queue.exchange(presentQueue))
            old->Release();
    }

    uint64_t frame_id = 0;
    std::optional<bool> interpolated;
    uint32_t position = 0;
    uint32_t frames = 0;
    bool logType = false;

    {
        std::scoped_lock lock(xefg_batch_mutex);

        // The next batch once this one has all its frames, without a next one it's still this one's
        if (!presented && xefg_batch.presented >= xefg_batch.frames && !xefg_batches.empty())
        {
            xefg_batch = xefg_batches.front();
            xefg_batches.pop_front();
        }
        else if (presented && xefg_batch.frame_id != 0)
        {
            xefg_batch.presented++;
        }

        frame_id = xefg_batch.frame_id;

        // XeFG presents a batch's generated frames first and its real frame last: AntiLag 2 takes the frame type of
        // each present from its position in the batch
        if (!presented && frame_id != 0 && tech->get_mode() == LowLatencyMode::AntiLag2)
        {
            position = xefg_batch.presented;
            frames = xefg_batch.frames;

            if (position < frames)
                interpolated = position + 1 < frames;

            if (xefg_types_logged < xefg_types_log_limit)
            {
                xefg_types_logged++;
                logType = true;
            }
        }
    }

    if (frame_id != 0)
    {
        MarkerParams params { frame_id, presented ? MarkerType::OUT_OF_BAND_PRESENT_END
                                                  : MarkerType::OUT_OF_BAND_PRESENT_START };
        params.interpolated = interpolated;

        if (logType)
        {
            LOG_INFO("XeFG AntiLag 2 frame type: frame id {}, position {} of {} frames, interpolated {}", frame_id,
                     position, frames, interpolated.has_value() ? (*interpolated ? "true" : "false") : "inferred");
        }

        tech->set_async_marker(presentQueue, params);
    }
}

void InputCommon::xefg_marker(void* xellContext, uint32_t frame_id, uint32_t marker)
{
    static std::atomic_bool seen[2] {};

    MarkerType type;

    if (marker == 80860000)
        type = MarkerType::OUT_OF_BAND_RENDERSUBMIT_START;
    else if (marker == 80860001)
        type = MarkerType::OUT_OF_BAND_RENDERSUBMIT_END;
    else
    {
        LOG_WARN("Unknown XeFG XeLL marker {} for frame id {}", marker, frame_id);
        return;
    }

    auto index = marker - 80860000;

    if (!seen[index].exchange(true))
        LOG_INFO("XeFG sends XeLL marker {} ({}), frame id {}", marker, magic_enum::enum_name(type), frame_id);

    LOG_TRACE_LOWLATENCY("XeFG {}: {}", magic_enum::enum_name(type), frame_id);

    // Only XeFG's own context next to a Reflex or AntiLag 2 output, the XeLL output's context has them already
    auto own = xefg_xell.load();

    if (own == nullptr || own->get_tech_context() != xellContext || own == currently_active_tech.load())
        return;

    // Without generated frames XeFG sends both right away, DLSSG doesn't send them. Generating, as the consumer
    // (fg_output_present) checks: at Dynamic MFG's 1X no batch would be taken, and a backlog would outlive it
    auto fg = State::Instance().currentFG;

    if (fg == nullptr || !fg->IsGenerating())
    {
        reset_xefg_batches();
        return;
    }

    if (type == MarkerType::OUT_OF_BAND_RENDERSUBMIT_START)
    {
        xefg_frame_id = frame_id;

        std::scoped_lock lock(xefg_batch_mutex);

        // Nothing takes them while XeFG's presents don't reach the wrapped swapchain, only the latest few are kept
        if (xefg_batches.size() >= xefg_max_batches)
            xefg_batches.pop_front();

        xefg_batches.push_back({ frame_id, fg->GetInterpolatedFrameCount() + 1 });
    }

    // Known after XeFG's first present
    auto queue = xefg_present_queue.load();

    if (auto tech = currently_active_tech.load(); tech != nullptr && queue != nullptr)
        tech->set_async_marker(queue, { frame_id, type });
}

// After the game's present: XeFG's own count for the batch it marked during it. The multiplier can be off, for a
// frame it couldn't generate or a changed frame count.
void InputCommon::xefg_present_status(uint64_t frameIdBefore, uint32_t framesPresented)
{
    auto frame_id = xefg_frame_id.load();

    if (frame_id == 0 || frame_id == frameIdBefore || framesPresented == 0)
        return;

    std::scoped_lock lock(xefg_batch_mutex);

    XeFGBatch* batch = xefg_batch.frame_id == frame_id ? &xefg_batch : nullptr;

    for (auto it = xefg_batches.rbegin(); batch == nullptr && it != xefg_batches.rend(); ++it)
    {
        if (it->frame_id == frame_id)
            batch = &*it;
    }

    if (batch != nullptr)
        batch->frames = framesPresented;
}

// The active input's sleep mode on OptiScaler's terms (OutputRule.h SleepModeFor)
SleepMode InputCommon::shaped_sleep_mode(lloutput::Role role)
{
    const auto input = activeInput.load();
    const auto copy = get_sleep_copy(input);

    lloutput::ShapeInputs in;
    in.xefgOutput = UsesXeFGApi(State::Instance().activeFgOutput);
    in.side = native::lowlatency::XefgSide();
    // OptiScaler's own calls carry their cap (native::lowlatency PlanCap); the UE mod's goes only to Frame cap only
    // (update), else it stays with the limiter
    in.capIntervalUs =
        input == LowLatencyInput::Reflex || input == LowLatencyInput::UeLowLatency ? fps_limit_interval_us.load() : 0;
    in.forceDisabled = Config::Instance()->FN_ForceReflex.value_or_default() == ForceReflex::ForceDisable;
    in.blocked = (input == LowLatencyInput::Reflex || input == LowLatencyInput::UeLowLatency) &&
                 native::lowlatency::GameLowLatencyBlocked();
    in.inputSetMode = (sleep_modes_set.load(std::memory_order_relaxed) & (1u << static_cast<uint32_t>(input))) != 0;
    in.modeForOtherSide = input == LowLatencyInput::OptiScaler && optiscaler_mode_side.load() != in.side;

    native::xefgreflex::SleepMode rule;
    rule.lowLatency = copy.low_latency_enabled;
    rule.boost = copy.low_latency_boost;
    rule.minimumIntervalUs = copy.minimum_interval_us;
    rule.useMarkersToOptimize = copy.use_markers_to_optimize;

    const auto shaped = lloutput::SleepModeFor(rule, role, in);

    SleepMode out = copy;
    out.low_latency_enabled = shaped.lowLatency;
    out.low_latency_boost = shaped.boost;
    out.minimum_interval_us = shaped.minimumIntervalUs;
    out.use_markers_to_optimize = shaped.useMarkersToOptimize;
    return out;
}

void InputCommon::apply_sleep_mode(LowLatencyTech* tech)
{
    auto sleepMode = shaped_sleep_mode(lloutput::Role::Output);
    tech->set_sleep_mode(&sleepMode);

    apply_xefg_xell_mode(tech);
}

// XeFG's own XeLL next to another output: everything off, it only keeps XeFG running
void InputCommon::apply_xefg_xell_mode(const LowLatencyTech* output)
{
    if (auto own = xefg_xell.load(); own != nullptr && own.get() != output)
    {
        auto off = shaped_sleep_mode(lloutput::Role::XefgXell);
        own->set_sleep_mode(&off);
    }
}

// Once per frame, on one thread (ReflexHooks::presentUpdate, the present OutputRule.h RunsLowLatencyUpdate picks): the
// static locals below rely on that
void InputCommon::update()
{
    auto& state = State::Instance();
    auto config = Config::Instance();
    auto current_tech = currently_active_tech.load();
    ++present_count;

    // A stopped Anti-Lag 2 output FSR FG had: gone once reportFGPresent handed FSR the new context (retire)
    if (retired_presents.load() != 0)
    {
        std::scoped_lock lock(create_tech_mutex);

        if (retired_presents != 0 && --retired_presents == 0 && retired_antilag2 != nullptr)
        {
            retired_antilag2->deinit();
            retired_antilag2.reset();
        }
    }

    // Force State can change at any time, XeLL takes a changed state through its sleep mode
    if (current_tech != nullptr)
    {
        bool xefgPaced = xefg_paced();

        static bool lastXefgPaced = false;
        static ForceReflex lastOverride = ForceReflex::InGame;
        auto lowLatencyOverride = config->FN_ForceReflex.value_or_default();
        current_tech->set_low_latency_override(lowLatencyOverride);
        current_tech->set_forced_mode(xefgPaced);

        if (auto own = xefg_xell.load(); own != nullptr && own != current_tech)
            own->set_low_latency_override(lowLatencyOverride);

        bool overrideChanged = std::exchange(lastOverride, lowLatencyOverride) != lowLatencyOverride;

        if (overrideChanged)
            LOG_INFO("Low latency Force State: {}", magic_enum::enum_name(lowLatencyOverride));

        if (std::exchange(lastXefgPaced, xefgPaced) != xefgPaced || overrideChanged)
            apply_sleep_mode(current_tech.get());
    }

    // A VK_NV_low_latency2 output whose swapchain was made again without the latency mode, or whose sleep never
    // signals (OutputRule.h ChooseReflexVk): the next call makes the output again on what the device has
    if (auto reflexVk = std::dynamic_pointer_cast<ReflexVk>(current_tech))
    {
        const auto device = (VkDevice) reflexVk->get_tech_context();

        if (reflexVk->gave_up_sleeping())
        {
            std::scoped_lock lock(g_lowLatency2FailedMutex);
            g_lowLatency2Failed.insert(device);
        }

        if (lloutput::ChooseReflexVk(ReflexVkInputsOf(device)) != lloutput::ReflexVkSurface::LowLatency2 &&
            !reflex_vk_stale.exchange(true))
        {
            LOG_INFO("Reflex Vulkan: VK_NV_low_latency2 can't go on on this device, the output is made again");
        }
    }

    // fakenvapi as the nvapi: with more than one generated DLSS FG frame its methods fall apart (the plan picks
    // LatencyFlex), and it was told Force Disable for them unless the player forced Reflex on. Never real Reflex, and
    // not Frame cap only, which counts the game's frames (Force Disable would stop its waits). Decided every frame, so
    // a changed output tells or undoes it too; undone unless the player changed Force State meanwhile.
    if (!state.externalFrameGeneration)
    {
        static bool toldForceDisable = false;
        const auto frames = ReflexHooks::dlssgFrameCountToGenerate();
        const bool multiFrame = state.dlssgLastSetMode == sl::DLSSGMode::eDynamic || frames > 1;
        const bool playerForcedOn =
            config->FN_ForceReflex.value_for_config().value_or(ForceReflex::InGame) == ForceReflex::ForceEnable;
        const bool wanted = multiFrame && fakenvapi::isUsingAsMainNvapi() && !UsesXeFGApi(state.activeFgOutput) &&
                            activeOutput != LowLatencyMode::FpsLimiter && !playerForcedOn;

        if (wanted && !toldForceDisable)
        {
            LOG_INFO("Multi frame generation, fakenvapi's low latency is told Force Disable");
            config->FN_ForceReflex.set_volatile_value(ForceReflex::ForceDisable);
            toldForceDisable = true;
        }
        else if (!wanted && toldForceDisable)
        {
            toldForceDisable = false;

            if (config->FN_ForceReflex.value_or_default() == ForceReflex::ForceDisable)
            {
                LOG_INFO("Multi frame generation ended or the output changed, fakenvapi's low latency goes back to its "
                         "Force State");
                config->FN_ForceReflex = config->FN_ForceReflex.value_for_config();
            }
        }
    }

    // Who keeps the frame cap (OutputRule.h CapForGame): the game's Reflex as the input, or the Streamline of
    // OptiScaler's DLSS FG output (its calls go through the hooks). OptiScaler's own calls carry their cap themselves
    // (native::lowlatency PlanCap, OwnsFrameCap). The UE mod's goes to Frame cap only, which does nothing else, and
    // otherwise stays with the limiter.
    const auto input = activeInput.load();
    const bool frameCapOutput = activeOutput == LowLatencyMode::FpsLimiter;
    const bool ueFrameCap = input == LowLatencyInput::UeLowLatency && frameCapOutput;
    const bool gameInput = input == LowLatencyInput::Reflex;
    const auto streamline = ReflexHooks::streamlineSleepMode();
    const bool streamlineSource = !gameInput && streamline.set;

    lloutput::GameCap cap;

    if (gameInput || streamlineSource || ueFrameCap)
    {
        // The UE mod's sleep mode is no game Reflex one: it stays unset there
        const auto game =
            gameInput ? ReflexHooks::gameSleepMode() : (streamlineSource ? streamline : ReflexHooks::GameSleepMode {});
        const auto side = native::lowlatency::XefgSide();
        const bool xefgOutput = UsesXeFGApi(state.activeFgOutput);
        const bool blocked = native::lowlatency::GameLowLatencyBlocked();
        const bool forceDisabled = config->FN_ForceReflex.value_or_default() == ForceReflex::ForceDisable;
        static const bool intelGpu = IdentifyGpu::getPrimaryGpu().vendorId == VendorId::Intel;

        lloutput::GameCapInputs in;
        in.capFps = FrameLimit::CapFps();
        in.externalFrameGeneration = state.externalFrameGeneration;
        in.markersRecently = ueFrameCap || ReflexHooks::markersRecently(); // the mod's calls made it the input
        in.gameSetSleepMode = game.set;
        in.gameLowLatencyOn = game.lowLatency;
        in.gameSendingMarkers = gameInput && ReflexHooks::gameIsSendingMarkers();
        in.realNvapi = !fakenvapi::isUsingAsMainNvapi();
        in.xefgOutput = xefgOutput;
        in.side = side;
        in.splitLimiterKeepsCap =
            native::lowlatency::SplitLimiterKeepsCap(side, state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12);
        in.xellHoldsCap = native::xefgreflex::XellHoldsCap(
            forceDisabled || blocked || side != lloutput::XefgLatency::XeLL, intelGpu,
            native::lowlatency::XefgGenerating(), state.fgPresentVsync.load(std::memory_order_relaxed));
        in.fsrFgOutput = state.activeFgOutput == FGOutput::FSRFG;
        in.fgActive = state.CurrentFG() != nullptr && state.CurrentFG()->IsGenerating();
        in.blocked = blocked;
        in.xefgShownPerRendered = native::lowlatency::XefgShownPerRendered();
        in.xellCountsShown = native::lowlatency::XellCountsShown();
        in.driverMultipliesCap = native::lowlatency::DriverMultipliesReflexCap();
        in.dlssgFramesToGenerate = ReflexHooks::dlssgFrameCountToGenerate();
        in.outputIsXell = activeOutput == LowLatencyMode::XeLL;
        in.outputIsFrameCap = frameCapOutput;
        in.forceDisabled = forceDisabled;
        in.outputRunning = current_tech != nullptr;
        cap = lloutput::CapForGame(in);
    }

    state.reflexLimitsFps = cap.outputHoldsCap;
    state.reflexShowWarning = cap.showWarning;

    const auto interval = cap.outputHoldsCap ? lloutput::IntervalUs(cap.fps) : 0u;

    // XeFG's Output changed between XeLL and the other side, the output follows on the next call: the new cap is for
    // the new output (XeLL counts shown frames, the others rendered ones) and goes in when it starts (init_tech,
    // use_output)
    const bool outputPending = lloutput::OutputSwitchPending(
        UsesXeFGApi(state.activeFgOutput), current_tech != nullptr, native::lowlatency::XefgSide(),
        activeOutput == LowLatencyMode::XeLL, incompatibility(input, LowLatencyMode::XeLL) == nullptr);

    // The game's (or the UE mod's to Frame cap only): in its sleep mode through the output (and XeFG's XeLL)
    const auto inputInterval = gameInput || ueFrameCap ? interval : 0u;

    if (fps_limit_interval_us.exchange(inputInterval) != inputInterval && !outputPending && current_tech != nullptr)
    {
        apply_sleep_mode(current_tech.get());
    }

    // Who keeps the cap, said whenever it or the cap changes (the limiter's changes reach no sleep mode)
    {
        static std::string lastText;
        const float capFps = FrameLimit::CapFps();
        std::string text;

        if ((gameInput || ueFrameCap) && cap.outputHoldsCap)
            text = std::format("Frame cap {:.1f} fps through {}: {} us", cap.fps,
                               magic_enum::enum_name(activeOutput.load()), interval);
        else if (gameInput && capFps > 0.0f)
            text =
                std::format("Frame cap {:.0f} fps in OptiScaler's limiter{}", capFps,
                            activeOutput == LowLatencyMode::XeLL && state.fgPresentVsync.load(std::memory_order_relaxed)
                                ? " (XeLL goes by the refresh rate with V-Sync)"
                                : "");

        if (!outputPending && text != lastText)
        {
            if (!text.empty())
                LOG_INFO("{}", text);

            lastText = std::move(text);
        }
    }

    // Streamline's: in its own Reflex mode, also next to the game's Reflex output (it sets the same device's mode)
    static uint32_t lastStreamlineInterval = 0;
    const auto streamlineInterval = lloutput::StreamlineCapUs(streamline.set, interval);

    if (std::exchange(lastStreamlineInterval, streamlineInterval) != streamlineInterval)
        ReflexHooks::streamlineCap(streamlineInterval);
}

InputResult InputCommon::mark_present_start(IUnknown* pDevice)
{
    auto fg = State::Instance().currentFG;
    bool fsrfgTypesFrames =
        State::Instance().activeFgOutput == FGOutput::FSRFG && fg != nullptr && fg->IsActive() && !fg->IsPaused();

    // UE only sends simulation markers. AL2 sends present markers only when FSR-FG is the one typing the frames and
    // AL2 itself doesn't report frame generation.
    bool marksEnds =
        activeInput == LowLatencyInput::UeLowLatency || (activeInput == LowLatencyInput::AntiLag2 && fsrfgTypesFrames &&
                                                         !reports_frame_generation(LowLatencyInput::AntiLag2));

    if (!marksEnds)
        return InputResult::InputNotSupported;

    // TODO: this is missing the frame id required by other outputs
    if (activeOutput != LowLatencyMode::AntiLag2)
        return InputResult::GenericError;

    if (auto current_tech = currently_active_tech.load())
    {
        MarkerParams marker_params {};
        marker_params.frame_id = 0; // TODO: will be needed if used with non-AL2
        marker_params.marker_type = MarkerType::PRESENT_START;

        // AL2 doesn't actually use pDevice but whatever
        current_tech->set_marker(pDevice, marker_params);
    }
    else
    {
        return InputResult::NoReadyOutput;
    }

    return InputResult::Ok;
}

std::shared_ptr<ReflexVk> InputCommon::reflex_vk_output()
{
    return std::dynamic_pointer_cast<ReflexVk>(currently_active_tech.load());
}

std::shared_ptr<ReflexNvApiVk> InputCommon::reflex_nvapi_vk_output()
{
    return std::dynamic_pointer_cast<ReflexNvApiVk>(currently_active_tech.load());
}

xell_context_handle_t InputCommon::xell_output_context()
{
    auto current_tech = currently_active_tech.load();

    if (current_tech == nullptr || current_tech->get_mode() != LowLatencyMode::XeLL)
        return nullptr;

    return (xell_context_handle_t) current_tech->get_tech_context();
}

xell_context_handle_t InputCommon::acquire_xefg_xell(IUnknown* pDevice)
{
    std::scoped_lock lock(create_tech_mutex);

    auto own = xefg_xell.load();

    if (own == nullptr)
    {
        // A XeLL output that runs already (started before XeFG) becomes XeFG's: one XeLL context of ours, the hooked
        // libxell knows one (XellHooks)
        if (auto current = currently_active_tech.load();
            current != nullptr && current->get_mode() == LowLatencyMode::XeLL)
        {
            own = current;
        }
        else
        {
            own = std::make_shared<XeLL>();

            if (!own->init(pDevice))
            {
                LOG_ERROR("Couldn't create XeFG's XeLL context");
                return nullptr;
            }

            // XeFG needs it in low latency mode from the start
            SleepMode sleepMode {};
            sleepMode.low_latency_enabled = true;
            own->set_sleep_mode(&sleepMode);
        }

        own->set_forced_mode(true);
        own->set_low_latency_override(Config::Instance()->FN_ForceReflex.value_or_default());
        xefg_xell.store(own);
        ++xefg_generation;

        LOG_INFO("XeFG paces with OptiScaler's XeLL context");
    }

    // Its mode now: the output's while XeLL lowers latency, else off next to the output; with no input yet it keeps
    // low latency on
    if (activeInput.load() != LowLatencyInput::None)
    {
        if (auto current = currently_active_tech.load(); current == own)
            apply_sleep_mode(own.get());
        else
            apply_xefg_xell_mode(current.get());
    }

    return (xell_context_handle_t) own->get_tech_context();
}

void InputCommon::release_xefg_xell()
{
    std::scoped_lock lock(create_tech_mutex);

    // The markers may still be on their way to it
    if (auto own = xefg_xell.exchange(nullptr))
    {
        // As the output it stops too; the next call starts the output its rules pick
        if (currently_active_tech.load() == own)
        {
            currently_active_tech.store(nullptr);
            activeOutput = LowLatencyMode::None;
            output_api = API::NotSelected;
            output_device = nullptr;
        }

        while (own.use_count() > 1)
            std::this_thread::yield();

        own->deinit();
        LOG_INFO("XeFG's XeLL context released");
    }

    xefg_frame_id = 0;
    reset_xefg_batches();

    // A new XeFG (another device, a driver update) tries Anti-Lag 2 and Reflex again
    antilag2_failed = false;
    reflex_failed = false;

    if (auto queue = xefg_present_queue.exchange(nullptr))
        queue->Release();
}

void InputCommon::set_optiscaler_running(bool running)
{
    if (optiscaler_running.exchange(running) != running)
        LOG_DEBUG("OptiScaler's own low latency {} as an input", running ? "runs" : "stopped");
}

void InputCommon::device_released(IUnknown* pDevice, bool alive)
{
    if (pDevice != nullptr && output_device.load() == pDevice)
        stop_output(alive);

    // A new device at the same address starts over
    if (pDevice != nullptr && failed_device.load() == pDevice)
        failed_device = nullptr;

    std::scoped_lock lock(g_lowLatency2FailedMutex);
    g_lowLatency2Failed.erase((VkDevice) pDevice);
}

void* InputCommon::output_context()
{
    auto current_tech = currently_active_tech.load();
    return current_tech != nullptr ? current_tech->get_tech_context() : nullptr;
}

NvAPI_Status InputCommon::output_latency(NV_LATENCY_RESULT_PARAMS* params, bool* estimated)
{
    auto current_tech = currently_active_tech.load();

    if (current_tech == nullptr || params == nullptr)
        return NVAPI_ERROR;

    if (auto reflex_tech = std::dynamic_pointer_cast<Reflex>(current_tech))
    {
        *estimated = false;
        return reflex_tech->get_latency(params);
    }

    if (auto reflex_vk = std::dynamic_pointer_cast<ReflexVk>(current_tech))
    {
        *estimated = false;
        return reflex_vk->get_latency(params);
    }

    if (auto reflex_nvapi_vk = std::dynamic_pointer_cast<ReflexNvApiVk>(current_tech))
    {
        // The Vulkan report is the D3D one without the GPU's active and frame times: copied field by field. Static and
        // big: not freed, like the others.
        static std::mutex reportMutex;
        static auto* report = new NV_VULKAN_LATENCY_RESULT_PARAMS();

        std::scoped_lock lock(reportMutex);
        *report = {};
        report->version = NV_VULKAN_LATENCY_RESULT_PARAMS_VER;

        if (auto result = reflex_nvapi_vk->get_latency(report); result != NVAPI_OK)
            return result;

        for (size_t i = 0; i < std::size(report->frameReport); i++)
        {
            const auto& from = report->frameReport[i];
            auto& to = params->frameReport[i];

            to = {};
            to.frameID = from.frameID;
            to.inputSampleTime = from.inputSampleTime;
            to.simStartTime = from.simStartTime;
            to.simEndTime = from.simEndTime;
            to.renderSubmitStartTime = from.renderSubmitStartTime;
            to.renderSubmitEndTime = from.renderSubmitEndTime;
            to.presentStartTime = from.presentStartTime;
            to.presentEndTime = from.presentEndTime;
            to.driverStartTime = from.driverStartTime;
            to.driverEndTime = from.driverEndTime;
            to.osRenderQueueStartTime = from.osRenderQueueStartTime;
            to.osRenderQueueEndTime = from.osRenderQueueEndTime;
            to.gpuRenderStartTime = from.gpuRenderStartTime;
            to.gpuRenderEndTime = from.gpuRenderEndTime;
        }

        *estimated = false;
        return NVAPI_OK;
    }

    *estimated = true;
    return copy_frame_reports((FrameReport*) params->frameReport) ? NVAPI_OK : NVAPI_ERROR;
}

void InputCommon::report_fg_present(bool fgState, bool interpolated)
{
    if (auto current_tech = currently_active_tech.load())
    {
        current_tech->set_effective_fg_state(fgState);

        if (fgState)
            current_tech->set_fg_type(interpolated, 0);
    }
}

bool InputCommon::output_enabled()
{
    auto current_tech = currently_active_tech.load();
    return current_tech != nullptr && current_tech->is_enabled();
}

LowLatencyMode InputCommon::current_mode() { return activeOutput.load(); }

bool InputCommon::low_latency_active() { return activeOutput.load() != LowLatencyMode::None && output_enabled(); }
