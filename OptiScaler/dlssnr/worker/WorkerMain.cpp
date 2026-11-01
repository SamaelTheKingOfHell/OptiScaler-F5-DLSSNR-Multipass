#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cwchar>
#include <array>
#include <cmath>
#include <memory>
#include <iostream>
#include <limits>
#include <vector>
#include "../DlssNr_CrossAdapterIpc.h"
#include "../DlssNr_WorkerFenceWait.h"
#include "WorkerModelSession.h"
#include "WorkerSurface.h"
#include "InterpassClamp.h"
#include "WorkerProtocol.h"

using namespace DlssNr::Worker;
struct OwnedHandle
{
    HANDLE value = nullptr;
    ~OwnedHandle()
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
struct Frame
{
    Surface color, depth, motion, output;
    Frame(ID3D12Device* device, const InitialFrameWire& wire)
        : color(MakeSurface(device, wire.width, wire.height, static_cast<DXGI_FORMAT>(wire.colorFormat), false)),
          motion(MakeSurface(device, wire.motionWidth, wire.motionHeight, static_cast<DXGI_FORMAT>(wire.motionFormat),
                             false)),
          output(MakeSurface(device, wire.width, wire.height, static_cast<DXGI_FORMAT>(wire.colorFormat), true))
    {
        if (wire.depthFormat != DXGI_FORMAT_UNKNOWN)
            depth = MakeSurface(device, wire.depthWidth, wire.depthHeight, static_cast<DXGI_FORMAT>(wire.depthFormat),
                                false);
        if (color.bytes != output.bytes)
            throw std::runtime_error("Incompatible output footprint");
    }
    uint64_t DepthOffset() const { return (color.bytes + 511u) & ~511ull; }
    uint64_t MotionOffset() const { return (DepthOffset() + depth.bytes + 511u) & ~511ull; }
    uint64_t Bytes() const { return MotionOffset() + motion.bytes; }
};
static void CheckDebug(ID3D12Device* device)
{
    ComPtr<ID3D12InfoQueue> messages;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&messages))))
        return;
    const auto count = messages->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T length = 0;
        Check(messages->GetMessage(i, nullptr, &length), "Get debug message size");
        std::vector<uint8_t> storage(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(messages->GetMessage(i, message, &length), "Get debug message");
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
            throw std::runtime_error(std::string("D3D12 debug error: ") + message->pDescription);
        if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING)
            std::cerr << "D3D12 debug warning: " << message->pDescription << "\n";
    }
}
static void Retire(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    ComPtr<ID3D12Fence> fence;
    OwnedHandle event { CreateEventW(nullptr, FALSE, FALSE, nullptr) };
    if (!event.value || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
        FAILED(queue->Signal(fence.Get(), 1)) || FAILED(fence->SetEventOnCompletion(1, event.value)) ||
        WaitForSingleObject(event.value, 30000) != WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason()))
    {
        std::cerr << "Could not retire submitted neural work; terminating probe.\n" << std::flush;
        ExitProcess(3);
    }
}

static Evaluation MakeEvaluation(Frame& frame, unsigned width, unsigned height, bool reset)
{
    Evaluation request {};
    request.color = frame.color.texture.Get();
    request.depth = frame.depth.texture.Get();
    request.motion = frame.motion.texture.Get();
    request.output = frame.output.texture.Get();
    request.width = request.guideWidth = request.motionWidth = width;
    request.height = request.guideHeight = request.motionHeight = height;
    request.reset = reset ? 1 : 0;
    return request;
}
static void Submit(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* commands)
{
    Check(commands->Close(), "Close neural command list");
    ID3D12CommandList* lists[] = { commands };
    queue->ExecuteCommandLists(1, lists);
}
static ComPtr<ID3D12GraphicsCommandList> Commands(ID3D12Device* device, ComPtr<ID3D12CommandAllocator>& allocator)
{
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
          "Create neural allocator");
    ComPtr<ID3D12GraphicsCommandList> commands;
    Check(
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)),
        "Create neural command list");
    return commands;
}
static int Worker(HANDLE pipe, const wchar_t* core, const wchar_t* forwarder, const wchar_t* model, const wchar_t* data,
                  const wchar_t* log)
{
    FILE* unused = nullptr;
    _wfreopen_s(&unused, log, L"w", stderr);
    InitialFrameWire wire {};
    auto readExact = [&](void* buffer, DWORD bytes)
    {
        DWORD received = 0;
        while (received < bytes)
        {
            DWORD part = 0;
            if (!ReadFile(pipe, static_cast<uint8_t*>(buffer) + received, bytes - received, &part, nullptr) || !part)
                return false;
            received += part;
        }
        return true;
    };
    if (!readExact(&wire, static_cast<DWORD>(sizeof(wire))))
        return 10;
    if (wire.magic != ProtocolMagic || wire.version != ProtocolVersion || wire.size != sizeof(wire) || !wire.width ||
        !wire.height || wire.width > 16384 || wire.height > 16384 || !wire.passCount ||
        wire.passCount > MaxWorkerPasses || wire.enableDebug > 1 || !std::isfinite(wire.passFeedback) ||
        wire.passFeedback < 0.0f || wire.passFeedback > 1.0f || !wire.heap || !wire.ready || !wire.done ||
        !wire.parent || !wire.initialized || !wire.bytes ||
        (wire.colorFormat != DXGI_FORMAT_R16G16B16A16_FLOAT && wire.colorFormat != DXGI_FORMAT_R8G8B8A8_UNORM) ||
        (wire.depthFormat != DXGI_FORMAT_UNKNOWN && wire.depthFormat != DXGI_FORMAT_R32_FLOAT &&
         wire.depthFormat != DXGI_FORMAT_R16_FLOAT && wire.depthFormat != DXGI_FORMAT_R16_UNORM) ||
        (wire.motionFormat != DXGI_FORMAT_R16G16_FLOAT && wire.motionFormat != DXGI_FORMAT_R32G32_FLOAT &&
         wire.motionFormat != DXGI_FORMAT_R16G16_SNORM) ||
        !wire.motionWidth || !wire.motionHeight || wire.motionWidth > 16384 || wire.motionHeight > 16384 ||
        (wire.depthFormat != DXGI_FORMAT_UNKNOWN &&
         (!wire.depthWidth || !wire.depthHeight || wire.depthWidth > 16384 || wire.depthHeight > 16384)))
        throw std::runtime_error("Invalid worker handshake");
    OwnedHandle heap { reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.heap)) };
    OwnedHandle ready { reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.ready)) };
    OwnedHandle done { reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.done)) };
    OwnedHandle parent { reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.parent)) };
    OwnedHandle initialized { reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.initialized)) };
    ComPtr<ID3D12Debug> debug;
    if (wire.enableDebug)
    {
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "Worker D3D12 debug interface");
        debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Worker factory");
    ComPtr<IDXGIAdapter1> adapter;
    Check(factory->EnumAdapterByLuid(wire.secondary, IID_PPV_ARGS(&adapter)), "Worker adapter");
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "Worker device");
    DlssNr::MultiGpu::CrossAdapterPeer peer;
    Check(DlssNr::MultiGpu::CrossAdapterPeer::Open(device.Get(), wire.primary, wire.secondary, wire.bytes, heap.value,
                                                   ready.value, done.value, peer),
          "Worker open handles");
    Frame frame(device.Get(), wire);
    if (frame.Bytes() != wire.bytes || frame.color.bytes != wire.colorBytes || frame.depth.bytes != wire.depthBytes ||
        frame.motion.bytes != wire.motionBytes || frame.DepthOffset() != wire.depthOffset ||
        frame.MotionOffset() != wire.motionOffset || frame.color.footprint.Footprint.RowPitch != wire.colorPitch ||
        frame.depth.footprint.Footprint.RowPitch != wire.depthPitch ||
        frame.motion.footprint.Footprint.RowPitch != wire.motionPitch)
        throw std::runtime_error("Worker footprint mismatch");
    const auto colorFormat = static_cast<DXGI_FORMAT>(wire.colorFormat);
    Surface extraOutput = MakeSurface(device.Get(), wire.width, wire.height, colorFormat, true);
    Surface clampA = MakeSurface(device.Get(), wire.width, wire.height, colorFormat, true);
    Surface clampB = MakeSurface(device.Get(), wire.width, wire.height, colorFormat, true);
    std::array<Surface*, 2> outputs { &frame.output, &extraOutput }, clamps { &clampA, &clampB };
    std::array<D3D12_RESOURCE_STATES, 2> outputStates { D3D12_RESOURCE_STATE_COPY_DEST,
                                                        D3D12_RESOURCE_STATE_COPY_DEST };
    std::array<D3D12_RESOURCE_STATES, 2> clampStates { D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST };
    for (auto* surface : outputs)
        FillZero(*surface);
    for (auto* surface : clamps)
        FillZero(*surface);
    // Each boundary has its own shader-visible descriptor heap; changing a single heap before
    // submission would make earlier dispatches read the last boundary's resources.
    std::vector<std::unique_ptr<InterpassClamp>> clampShaders;
    for (uint32_t i = 1; i < wire.passCount; ++i)
        clampShaders.push_back(
            std::make_unique<InterpassClamp>(device.Get(), wire.width, wire.height, wire.passFeedback));
    std::vector<FeatureTuning> tuning;
    tuning.reserve(wire.passCount);
    for (uint32_t i = 0; i < wire.passCount; ++i)
    {
        const auto& pass = wire.passes[i];
        if (!std::isfinite(pass.intensity) || !std::isfinite(pass.structure) || !std::isfinite(pass.tone) ||
            !std::isfinite(pass.skin) || pass.preset < 0 || pass.preset > 3 || pass.style < 0 || pass.style > 2 ||
            (pass.autoMask != 0 && pass.autoMask != 1) || (pass.uiCorrection != 0 && pass.uiCorrection != 1))
            throw std::runtime_error("Invalid worker pass tuning");
        tuning.push_back({ pass.preset, pass.intensity, pass.style, pass.structure, pass.tone, pass.skin, pass.autoMask,
                           pass.uiCorrection });
    }
    ModelSession session;
    session.Initialize(device.Get(), core, forwarder, model, data, wire.width, wire.height, tuning, wire.applicationId);
    if (!SetEvent(initialized.value))
        throw std::runtime_error("Could not signal worker initialization");
    for (uint64_t frameNumber = 1; !wire.frameCount || frameNumber <= wire.frameCount; ++frameNumber)
    {
        FrameCommand frameCommand {};
        if (!readExact(&frameCommand, static_cast<DWORD>(sizeof(frameCommand))))
        {
            if (WaitForSingleObject(parent.value, 0) == WAIT_OBJECT_0)
                break;
            throw std::runtime_error("Worker frame control pipe closed unexpectedly");
        }
        if (frameCommand.size != sizeof(frameCommand))
            throw std::runtime_error("Invalid frame control size");
        if (frameCommand.kind == CommandKind::Shutdown)
            break;
        if (frameCommand.kind != CommandKind::Frame || frameCommand.frameNumber != frameNumber ||
            frameCommand.invertedDepth > 1 || !frameCommand.guideWidth || !frameCommand.guideHeight ||
            !frameCommand.motionWidth || !frameCommand.motionHeight ||
            static_cast<uint64_t>(frameCommand.motionX) + frameCommand.motionWidth > wire.motionWidth ||
            static_cast<uint64_t>(frameCommand.motionY) + frameCommand.motionHeight > wire.motionHeight ||
            (wire.depthFormat != DXGI_FORMAT_UNKNOWN &&
             (static_cast<uint64_t>(frameCommand.depthX) + frameCommand.guideWidth > wire.depthWidth ||
              static_cast<uint64_t>(frameCommand.depthY) + frameCommand.guideHeight > wire.depthHeight)) ||
            !std::isfinite(frameCommand.motionScaleX) || !std::isfinite(frameCommand.motionScaleY))
            throw std::runtime_error("Invalid frame control");
        ComPtr<ID3D12CommandAllocator> allocator;
        auto commands = Commands(device.Get(), allocator);
        if (frameNumber > 1)
            for (auto* input : { &frame.color, &frame.depth, &frame.motion })
                if (input->texture)
                    Barrier(commands.Get(), input->texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_DEST);
        auto prepare = [&](Surface& surface, D3D12_RESOURCE_STATES& state)
        {
            if (state == D3D12_RESOURCE_STATE_COPY_DEST)
                CopyUpload(commands.Get(), surface);
            else if (state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                Barrier(commands.Get(), surface.texture.Get(), state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        };
        for (size_t i = 0; i < outputs.size(); ++i)
            prepare(*outputs[i], outputStates[i]);
        for (size_t i = 0; i < clamps.size(); ++i)
            prepare(*clamps[i], clampStates[i]);
        Barrier(commands.Get(), peer.Buffer(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        CopyFromShared(commands.Get(), peer.Buffer(), frame.color, 0);
        if (frame.depth.texture)
            CopyFromShared(commands.Get(), peer.Buffer(), frame.depth, wire.depthOffset);
        CopyFromShared(commands.Get(), peer.Buffer(), frame.motion, wire.motionOffset);
        ID3D12Resource* passInput = frame.color.texture.Get();
        Surface* finalOutput = nullptr;
        for (uint32_t pass = 0; pass < wire.passCount; ++pass)
        {
            const size_t slot = pass % 2;
            Surface& output = *outputs[slot];
            if (outputStates[slot] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                Barrier(commands.Get(), output.texture.Get(), outputStates[slot],
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                outputStates[slot] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
            auto request = MakeEvaluation(frame, wire.width, wire.height,
                                          frameNumber == 1 || (frameCommand.resetMask & (1u << pass)) != 0);
            request.color = passInput;
            request.output = output.texture.Get();
            const auto& dynamic = frameCommand.passes[pass];
            if (!std::isfinite(dynamic.intensity) || !std::isfinite(dynamic.structure) ||
                !std::isfinite(dynamic.tone) || !std::isfinite(dynamic.skin) || dynamic.style < 0 ||
                dynamic.style > 2 || (dynamic.autoMask != 0 && dynamic.autoMask != 1))
                throw std::runtime_error("Invalid frame tuning");
            request.intensity = dynamic.intensity;
            request.style = dynamic.style;
            request.structure = dynamic.structure;
            request.tone = dynamic.tone;
            request.skin = dynamic.skin;
            request.autoMask = dynamic.autoMask;
            request.guideWidth = frameCommand.guideWidth;
            request.guideHeight = frameCommand.guideHeight;
            request.motionWidth = frameCommand.motionWidth;
            request.motionHeight = frameCommand.motionHeight;
            request.depthX = frameCommand.depthX;
            request.depthY = frameCommand.depthY;
            request.motionX = frameCommand.motionX;
            request.motionY = frameCommand.motionY;
            request.invertedDepth = static_cast<int>(frameCommand.invertedDepth);
            request.motionScaleX = frameCommand.motionScaleX;
            request.motionScaleY = frameCommand.motionScaleY;
            if (session.Record(commands.Get(), request, pass) != 1)
                throw std::runtime_error("Secondary neural pass failed");
            if (pass + 1 == wire.passCount)
            {
                Barrier(commands.Get(), output.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
                outputStates[slot] = D3D12_RESOURCE_STATE_COPY_SOURCE;
                finalOutput = &output;
                break;
            }
            Barrier(commands.Get(), output.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            outputStates[slot] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            Surface& clamped = *clamps[slot];
            if (clampStates[slot] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                Barrier(commands.Get(), clamped.texture.Get(), clampStates[slot],
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                clampStates[slot] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
            clampShaders[pass]->Record(commands.Get(), output.texture.Get(), passInput, clamped.texture.Get());
            Barrier(commands.Get(), clamped.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            clampStates[slot] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            passInput = clamped.texture.Get();
        }
        Barrier(commands.Get(), peer.Buffer(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        CopyToShared(commands.Get(), peer.Buffer(), *finalOutput, 0);
        Barrier(commands.Get(), peer.Buffer(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        if (DlssNr::MultiGpu::WaitForFenceOrPeer(peer.Ready(), frameNumber, parent.value, 30000) !=
            DlssNr::MultiGpu::WorkerFenceWaitResult::Ready)
            throw std::runtime_error("Parent exited or failed before sending the frame");
        Submit(session.Queue(), commands.Get());
        if (FAILED(session.Queue()->Signal(peer.Done(), frameNumber)))
        {
            std::cerr << "Cannot signal worker completion.\n" << std::flush;
            ExitProcess(3);
        }
        Retire(device.Get(), session.Queue());
    }
    CloseHandle(pipe);
    if (wire.enableDebug)
        CheckDebug(device.Get());
    std::cerr << "Worker neural pass retired successfully.\n" << std::flush;
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc != 8 || std::wcscmp(argv[1], L"--worker") != 0)
            return 2;
        return Worker(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[2], nullptr, 10))), argv[3],
                      argv[4], argv[5], argv[6], argv[7]);
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
