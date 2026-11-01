#pragma once
#include <windows.h>
#include <dxgiformat.h>
#include <cstdint>

namespace DlssNr::Worker
{
// Private inherited pipe only. Handles below are duplicated directly into the intended child process.
inline constexpr uint32_t ProtocolMagic = 0x4E524D47; // NRMG
inline constexpr uint32_t ProtocolVersion = 7;
inline constexpr uint32_t MaxWorkerPasses = 30;
struct PassSettings
{
    int32_t preset = 0, style = 0, autoMask = 1, uiCorrection = 1;
    float intensity = 1.0f, structure = 0.0f, tone = 0.0f, skin = 0.0f;
};
struct InitialFrameWire
{
    uint32_t magic = ProtocolMagic, version = ProtocolVersion, size = sizeof(InitialFrameWire);
    uint32_t width = 256, height = 144, frameCount = 2, passCount = 2, enableDebug = 0;
    uint32_t colorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uint32_t depthFormat = DXGI_FORMAT_R32_FLOAT, depthWidth = 256, depthHeight = 144;
    uint32_t motionFormat = DXGI_FORMAT_R16G16_FLOAT, motionWidth = 256, motionHeight = 144;
    float passFeedback = 1.0f;
    uint64_t applicationId = 0x24480451ull;
    PassSettings passes[MaxWorkerPasses] {};
    uint64_t heap = 0, ready = 0, done = 0, parent = 0, initialized = 0;
    uint64_t bytes = 0, colorBytes = 0, depthBytes = 0, motionBytes = 0;
    uint64_t depthOffset = 0, motionOffset = 0;
    uint32_t colorPitch = 0, depthPitch = 0, motionPitch = 0;
    LUID primary {}, secondary {};
};
enum class CommandKind : uint32_t
{
    Frame = 1,
    Shutdown = 2
};
struct RuntimeTuning
{
    float intensity = 1.0f, structure = 0.0f, tone = 0.0f, skin = 0.0f;
    int32_t style = 0, autoMask = 1;
};
struct FrameCommand
{
    uint32_t size = sizeof(FrameCommand);
    CommandKind kind = CommandKind::Frame;
    uint64_t frameNumber = 0;
    uint32_t resetMask = 0, invertedDepth = 0;
    uint32_t guideWidth = 256, guideHeight = 144, motionWidth = 256, motionHeight = 144;
    uint32_t depthX = 0, depthY = 0, motionX = 0, motionY = 0;
    float motionScaleX = 1.0f, motionScaleY = 1.0f;
    RuntimeTuning passes[MaxWorkerPasses] {};
};
} // namespace DlssNr::Worker
