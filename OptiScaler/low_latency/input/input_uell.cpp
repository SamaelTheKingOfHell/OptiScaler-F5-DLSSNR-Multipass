#include "pch.h"

#include "input_uell.h"
#include "UeInputRule.h"

#include <hooks/Reflex_Hooks.h>
#include <native/NativeLowLatency.h>

#include <DirectXMath.h>

using namespace DirectX;

static const char* OwnerText(ueinput::Owner owner)
{
    switch (owner)
    {
    case ueinput::Owner::Game:
        return "stands aside, the game's own Reflex lowers latency";
    case ueinput::Owner::OwnLowLatency:
        return "stands aside, OptiScaler's own low latency runs";
    case ueinput::Owner::FrameGeneration:
        return "stands aside, the frame generation output runs its own low latency";
    default:
        return "its frames drive the low latency output";
    }
}

void InputUeLowLatency::tickStart(int64_t frameId, float DeltaSeconds, bool bIdleMode)
{
    auto& state = State::Instance();

    // The swap chain's device, taken every frame: the game can make a new one. D3D11 games can have a D3D12 device too.
    IUnknown* device = nullptr;

    if (state.swapchainApi == DX11)
        device = state.currentD3D11Device;
    else if (state.swapchainApi == DX12)
        device = state.currentD3D12Device;

    const auto owner =
        ueinput::OtherOwner(ReflexHooks::gameCalledReflex(), native::lowlatency::Running(),
                            native::lowlatency::FrameGenerationNeedsReflex() || UsesXeFGApi(state.activeFgOutput));

    const auto previousOwner = memory.owner;
    const auto step = ueinput::Next(memory, device, owner, InputCommon::active_output() == LowLatencyMode::Reflex,
                                    native::lowlatency::GameLowLatencyBlocked());

    if (owner != previousOwner)
        LOG_INFO("UE Low Latency mod: {}", OwnerText(owner));

    if (step.dropOutput || step.stopOutput)
        InputCommon::stop_output(!step.dropOutput);

    // The frame's end marker follows its start
    drivenFrame = step.drive ? frameId : INT64_MIN;

    if (!step.drive)
        return;

    if (step.sleepMode.has_value())
    {
        // No interval: the frame cap stays with OptiScaler's limiter. InputCommon keeps the mode even when it fails,
        // for an output that starts later.
        SleepMode sleepMode {};
        sleepMode.low_latency_enabled = *step.sleepMode;

        if (auto result = InputCommon::set_sleep_mode(inputContext, device, &sleepMode);
            result != InputResult::Ok && result != InputResult::UsingDifferentInput)
        {
            LOG_ERROR("set_sleep_mode result: {}", magic_enum::enum_name(result));
        }
    }

    // Most backends use 32bit frameId so it is what it is
    auto result = InputCommon::sleep(inputContext, device, (uint32_t) frameId);

    if (result == InputResult::UsingDifferentInput)
        return;

    if (result != InputResult::Ok)
    {
        LOG_ERROR("sleep result: {}", magic_enum::enum_name(result));
        return;
    }

    MarkerParams markerParams {};
    markerParams.frame_id = frameId;
    markerParams.marker_type = MarkerType::SIMULATION_START;

    result = InputCommon::set_marker(inputContext, device, markerParams);

    if (result != InputResult::Ok)
    {
        LOG_ERROR("set_marker result: {}", magic_enum::enum_name(result));
        return;
    }
}

void InputUeLowLatency::tickEnd(int64_t frameId, float DeltaSeconds, bool bIdleMode)
{
    // Only for a frame whose start went to the output
    if (drivenFrame != frameId)
        return;

    MarkerParams markerParams {};
    markerParams.frame_id = frameId;
    markerParams.marker_type = MarkerType::SIMULATION_END;

    auto result = InputCommon::set_marker(inputContext, (IUnknown*) memory.device, markerParams);

    if (result != InputResult::Ok)
    {
        LOG_ERROR("set_marker result: {}", magic_enum::enum_name(result));
        return;
    }
}

static void GetCameraBasis(const float cameraRotation[3], float cameraUp[3], float cameraRight[3],
                           float cameraForward[3])
{
    float pitch = DirectX::XMConvertToRadians(cameraRotation[0]);
    float yaw = DirectX::XMConvertToRadians(cameraRotation[1]);
    float roll = DirectX::XMConvertToRadians(cameraRotation[2]);

    float SP, CP, SY, CY, SR, CR;
    DirectX::XMScalarSinCos(&SP, &CP, pitch);
    DirectX::XMScalarSinCos(&SY, &CY, yaw);
    DirectX::XMScalarSinCos(&SR, &CR, roll);

    cameraForward[0] = CP * CY;
    cameraForward[1] = CP * SY;
    cameraForward[2] = SP;

    cameraRight[0] = SR * SP * CY - CR * SY;
    cameraRight[1] = SR * SP * SY + CR * CY;
    cameraRight[2] = -SR * CP;

    cameraUp[0] = -(CR * SP * CY + SR * SY);
    cameraUp[1] = CY * SR - CR * SP * SY;
    cameraUp[2] = CR * CP;
}

void InputUeLowLatency::cameraUpdate(int64_t frameId, float cameraPosition[3], float cameraRotation[3], float fovAngle)
{
    // Technically FSRFG input might have the camera data but not guaranteed
    // if (State::Instance().activeFgInput == FGInput::DLSSG)
    //    return;

    if (auto fg = State::Instance().currentFG)
    {
        auto index = (int) (frameId % BUFFER_COUNT);

        float cameraUp[3];
        float cameraRight[3];
        float cameraForward[3];

        GetCameraBasis(cameraRotation, cameraUp, cameraRight, cameraForward);

        fg->SetCameraData(cameraPosition, cameraUp, cameraRight, cameraForward, index);

        UINT64 width = 0;
        UINT height = 0;
        fg->GetInterpolationRect(width, height, index);
        float aspectRatio = (float) width / (float) height;

        // TODO: camera near and far are placeholders
        fg->SetCameraValues(0.01f, 10000.f, DirectX::XMConvertToRadians(fovAngle), aspectRatio, 0.0f, index);
    }
}
