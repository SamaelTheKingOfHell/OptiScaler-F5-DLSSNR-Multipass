#include "pch.h"
#include "../../OptiScaler/dlssnr/DlssNr_CrossAdapterIpc.h"
#include "../../OptiScaler/dlssnr/DlssNr_CrossAdapterSelect.h"
#include "../../OptiScaler/dlssnr/DlssNr_OwnedCommandChain.h"
#include "../../OptiScaler/dlssnr/DlssNr_WorkerFenceWait.h"
#include "../../OptiScaler/dlssnr/worker/WorkerModelSession.h"
#include "nr_ngx_evaluate_fixture.h"
#include "nr_cross_adapter_test_helpers.h"
#include "../../OptiScaler/dlssnr/worker/InterpassClamp.h"
#include "../../OptiScaler/dlssnr/worker/WorkerProtocol.h"
#include "../../OptiScaler/dlssnr/worker/WorkerClient.h"
#include "../../OptiScaler/dlssnr/worker/WorkerFrameTransfer.h"
#include "../../OptiScaler/dlssnr/worker/WorkerFrameHandoff.h"
#include "../../OptiScaler/dlssnr/worker/WorkerGeneration.h"
#include <d3d12sdklayers.h>
#include <psapi.h>
#include <cwchar>
#include <limits>
#include <chrono>
#include <algorithm>

struct OwnedHandle
{
    HANDLE value = nullptr;
    ~OwnedHandle()
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
using Wire = DlssNr::Worker::InitialFrameWire;
using DlssNr::Worker::InterpassClamp;
struct Frame
{
    NrFixture::Surface color, depth, motion, output;
    unsigned width, height, guideWidth, guideHeight, guideX, guideY;
    DXGI_FORMAT colorFormat;
    explicit Frame(ID3D12Device* device, unsigned requestedWidth, unsigned requestedHeight, bool noDepth,
                   bool halfGuides, bool rgba8, bool offsetGuides, bool depthStencil)
        : width(requestedWidth), height(requestedHeight),
          guideWidth(halfGuides ? (requestedWidth + 1) / 2 : requestedWidth),
          guideHeight(halfGuides ? (requestedHeight + 1) / 2 : requestedHeight), guideX(offsetGuides ? 2u : 0u),
          guideY(offsetGuides ? 2u : 0u),
          colorFormat(rgba8 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        if (!NrFixture::MakeSurface(device, colorFormat, false, color, width, height) ||
            (!noDepth && !NrFixture::MakeSurface(device, depthStencil ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS :
                                                 DXGI_FORMAT_R32_FLOAT, false, depth, guideWidth + guideX,
                                                 guideHeight + guideY)) ||
            !NrFixture::MakeSurface(device, DXGI_FORMAT_R16G16_FLOAT, false, motion, guideWidth + guideX,
                                    guideHeight + guideY) ||
            !NrFixture::MakeSurface(device, colorFormat, true, output, width, height))
            throw std::runtime_error("Could not create neural textures");
        if (color.bytes != output.bytes)
            throw std::runtime_error("Incompatible output footprint");
    }
    uint64_t DepthOffset() const { return (color.bytes + 511u) & ~511ull; }
    uint64_t MotionOffset() const { return (DepthOffset() + depth.bytes + 511u) & ~511ull; }
    uint64_t Bytes() const { return MotionOffset() + motion.bytes; }
};

static void Fill(NrFixture::Surface& surface, unsigned kind)
{
    D3D12_RANGE noRead { 0, 0 };
    uint8_t* mapped = nullptr;
    Check(surface.upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)), "Map synthetic upload");
    std::memset(mapped, 0, static_cast<size_t>(surface.bytes));
    for (UINT y = 0; y < surface.rows; ++y)
    {
        auto* row = mapped + y * surface.footprint.Footprint.RowPitch;
        if (kind == 0 || kind == 4)
        {
            for (unsigned x = 0; x < surface.texture->GetDesc().Width; ++x)
            {
                if (surface.texture->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM)
                {
                    row[4 * x] = row[4 * x + 1] = row[4 * x + 2] = kind == 0 ? 128 : 64;
                    row[4 * x + 3] = 255;
                }
                else
                {
                    auto* half = reinterpret_cast<uint16_t*>(row);
                    half[4 * x] = half[4 * x + 1] = half[4 * x + 2] = kind == 0 ? 0x3800 : 0x3400;
                    half[4 * x + 3] = 0x3c00;
                }
            }
        }
        else if (kind == 1)
        {
            auto* values = reinterpret_cast<float*>(row);
            for (unsigned x = 0; x < surface.texture->GetDesc().Width; ++x)
                values[x] = 0.5f;
        }
    }
    surface.upload->Unmap(0, nullptr);
}
static void CopyUpload(ID3D12GraphicsCommandList* commands, NrFixture::Surface& surface, D3D12_RESOURCE_STATES next)
{
    auto dst = NrFixture::Texture(surface.texture.Get());
    auto src = NrFixture::Buffer(surface.upload.Get(), surface.footprint);
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    NrFixture::Barrier(commands, surface.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, next);
}
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
static ComPtr<ID3D12Fence> Retire(ID3D12Device* device, ID3D12CommandQueue* queue)
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
    return fence;
}
static DlssNr::Worker::Evaluation MakeEvaluation(Frame& frame, bool reset)
{
    DlssNr::Worker::Evaluation request {};
    request.color = frame.color.texture.Get();
    request.depth = frame.depth.texture.Get();
    request.motion = frame.motion.texture.Get();
    request.output = frame.output.texture.Get();
    request.width = frame.width;
    request.height = frame.height;
    request.guideWidth = request.motionWidth = frame.guideWidth;
    request.guideHeight = request.motionHeight = frame.guideHeight;
    request.depthX = request.motionX = frame.guideX;
    request.depthY = request.motionY = frame.guideY;
    request.reset = reset ? 1 : 0;
    return request;
}
static void Evaluate(DlssNr::Worker::ModelSession& session, ID3D12GraphicsCommandList* commands, Frame& frame,
                     bool reset)
{
    const int result = session.Record(commands, MakeEvaluation(frame, reset), 0);
    if (result != 1)
    {
        std::cerr << "Neural evaluation failed: " << std::hex << static_cast<unsigned>(result) << " "
                  << (session.LastError() ? session.LastError() : "") << "\n";
        throw std::runtime_error("Neural evaluation failed");
    }
}

static void CheckWorkerDxgi(HANDLE process)
{
    HMODULE modules[1024] {};
    DWORD bytes = 0;
    if (!EnumProcessModules(process, modules, sizeof(modules), &bytes) || bytes > sizeof(modules))
        throw std::runtime_error("Could not inspect worker modules");
    wchar_t system[MAX_PATH] {};
    if (!GetSystemDirectoryW(system, MAX_PATH))
        throw std::runtime_error("Could not identify the system directory");
    const std::wstring expected = std::wstring(system) + L"\\dxgi.dll";
    bool found = false;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i)
    {
        wchar_t path[32768] {};
        if (!GetModuleFileNameExW(process, modules[i], path, static_cast<DWORD>(std::size(path)))) continue;
        const wchar_t* name = std::wcsrchr(path, L'\\');
        if (name && _wcsicmp(name + 1, L"dxgi.dll") == 0)
        {
            if (_wcsicmp(path, expected.c_str()) != 0)
                throw std::runtime_error("Worker loaded a non-system DXGI proxy");
            found = true;
        }
    }
    if (!found) throw std::runtime_error("Worker system DXGI was not found");
    std::cout << "Worker uses system DXGI rather than the game proxy.\n";
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
static uint64_t ReturnedChecksum(ID3D12Resource* readback, const NrFixture::Surface& surface, unsigned width, bool rgba8)
{
    D3D12_RANGE range { 0, static_cast<SIZE_T>(surface.bytes) }, noWrite { 0, 0 };
    uint8_t* mapped = nullptr;
    Check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "Map returned checksum");
    bool finite = true, nonzero = false;
    uint64_t checksum = 1469598103934665603ull;
    for (UINT y = 0; y < surface.rows; ++y)
        for (unsigned x = 0; x < width; ++x)
            for (unsigned c = 0; c < 3; ++c)
            {
                const auto* row = mapped + y * surface.footprint.Footprint.RowPitch;
                const uint16_t value = rgba8 ? row[x * 4 + c] : reinterpret_cast<const uint16_t*>(row)[x * 4 + c];
                if (!rgba8) finite = finite && (value & 0x7c00u) != 0x7c00u;
                nonzero = nonzero || (rgba8 ? value != 0 : (value & 0x7fffu) != 0);
                checksum = (checksum ^ value) * 1099511628211ull;
            }
    readback->Unmap(0, &noWrite);
    if (!finite || !nonzero) throw std::runtime_error("Returned neural output was nonfinite or empty");
    return checksum;
}

// Two additional real neural frames prove A's queued return is read before B
// overwrites shared storage. A reference copy is taken before the local gate.
static void CheckQueuedReturn(DlssNr::Worker::WorkerGeneration& generation, ID3D12Device* device,
                              ID3D12CommandQueue* queue, Frame& frame, ID3D12Resource* color,
                              unsigned width, unsigned height, bool rgba8)
{
    ComPtr<ID3D12CommandAllocator> firstAllocator, secondAllocator;
    auto firstCommands = Commands(device, firstAllocator), secondCommands = Commands(device, secondAllocator);
    DlssNr::MultiGpu::OwnedCommandChain first, second;
    Check(first.Initialize(device, queue, firstCommands.Get()), "Initialize queued-return first chain");
    Check(second.Initialize(device, queue, secondCommands.Get()), "Initialize queued-return second chain");
    D3D12_COMMAND_QUEUE_DESC description {};
    description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> wrongQueue;
    Check(device->CreateCommandQueue(&description, IID_PPV_ARGS(&wrongQueue)), "Create wrong return queue");
    ComPtr<ID3D12Fence> gate, firstRetirement, secondRetirement, foreignRetirement;
    ComPtr<ID3D12Device> foreignDevice;
    Check(generation.Transfer().Buffer().SecondaryBuffer()->GetDevice(IID_PPV_ARGS(&foreignDevice)),
          "Get secondary fence device");
    Check(foreignDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&foreignRetirement)),
          "Create foreign return fence");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "Create local queued-return gate");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&firstRetirement)), "Create first return fence");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&secondRetirement)), "Create second return fence");
    std::array<NrFixture::Surface, 3> returned;
    std::array<ComPtr<ID3D12Resource>, 3> readbacks;
    for (size_t i = 0; i < returned.size(); ++i)
    {
        if (!NrFixture::MakeSurface(device, frame.colorFormat, false, returned[i], width, height))
            throw std::runtime_error("Create queued-return texture failed");
        readbacks[i] = Buffer(device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, frame.color.bytes);
    }
    // Declared after the resources so cleanup releases the local gate and retires
    // submitted primary commands before any referenced object is destroyed.
    struct Cleanup
    {
        ID3D12Device* device;
        ID3D12CommandQueue* queue;
        ID3D12Fence* gate;
        ~Cleanup() { gate->Signal(1); Retire(device, queue); }
    } cleanup { device, queue, gate.Get() };
    auto copy = [&](ID3D12GraphicsCommandList* commands, size_t i)
    {
        Transition(commands, returned[i].texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto from = NrFixture::Texture(returned[i].texture.Get());
        auto to = NrFixture::Buffer(readbacks[i].Get(), returned[i].footprint);
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    };
    DlssNr::Worker::FrameCommand command {};
    command.guideWidth = command.motionWidth = frame.guideWidth;
    command.guideHeight = command.motionHeight = frame.guideHeight;
    command.depthX = command.motionX = frame.guideX;
    command.depthY = command.motionY = frame.guideY;
    Check(generation.SubmitAsync(first, queue, command, color, frame.depth.texture.Get(), frame.motion.texture.Get()).result,
          "Submit queued-return first frame");
    Check(generation.WaitReturn(60000), "Wait for queued-return first frame");
    Check(generation.Transfer().RecordInbound(first.Current(), returned[0].texture.Get()), "Record reference return");
    copy(first.Current(), 0);
    Check(first.Split(), "Submit reference return");
    Retire(device, queue);
    Check(generation.RecordReturn(first.Current(), returned[1].texture.Get()), "Record queued actual return");
    copy(first.Current(), 1);
    Check(queue->Wait(gate.Get(), 1), "Gate queued actual return");
    Check(first.SubmitFinal(), "Submit queued actual return");
    Check(queue->Signal(firstRetirement.Get(), 1), "Signal queued actual return");
    if (generation.TrackRetirement(firstRetirement.Get(), 1, wrongQueue.Get()) != E_INVALIDARG ||
        generation.TrackRetirement(firstRetirement.Get(), 0, queue) != E_INVALIDARG ||
        generation.TrackRetirement(firstRetirement.Get(), (std::numeric_limits<uint64_t>::max)(), queue) != E_INVALIDARG ||
        generation.TrackRetirement(foreignRetirement.Get(), 1, queue) != E_INVALIDARG)
        throw std::runtime_error("Accepted invalid queue-ordered retirement");
    Check(generation.TrackRetirement(firstRetirement.Get(), 1, queue), "Track queued actual return");
    if (generation.Poll() != E_PENDING || generation.PollForQueue(wrongQueue.Get()) != E_INVALIDARG)
        throw std::runtime_error("Strict polling or queue identity guard failed");
    Check(generation.PollForQueue(queue), "Acknowledge same-queue ordered return");
    if (generation.SubmitAsync(second, wrongQueue.Get(), command, color, frame.depth.texture.Get(),
                               frame.motion.texture.Get()).result != E_INVALIDARG)
        throw std::runtime_error("Accepted another queue after ordered acknowledgment");
    Check(generation.SubmitAsync(second, queue, command, color, frame.depth.texture.Get(), frame.motion.texture.Get()).result,
          "Submit next frame before preceding return retires");
    if (generation.PollReturn() != E_PENDING || firstRetirement->GetCompletedValue() != 0)
        throw std::runtime_error("Next frame escaped local return gate");
    Check(gate->Signal(1), "Release queued actual return");
    Check(generation.WaitReturn(60000), "Wait for queued-return second frame");
    Check(generation.RecordReturn(second.Current(), returned[2].texture.Get()), "Record queued-return second image");
    copy(second.Current(), 2);
    Check(generation.TrackRetirement(secondRetirement.Get(), 1), "Track strict second retirement");
    Check(second.SubmitFinal(), "Submit queued-return second image");
    Check(queue->Signal(secondRetirement.Get(), 1), "Signal strict second retirement");
    Retire(device, queue);
    Check(generation.Poll(), "Acknowledge strict second retirement");
    const auto reference = ReturnedChecksum(readbacks[0].Get(), returned[0], width, rgba8);
    const auto actual = ReturnedChecksum(readbacks[1].Get(), returned[1], width, rgba8);
    const auto next = ReturnedChecksum(readbacks[2].Get(), returned[2], width, rgba8);
    if (reference != actual) throw std::runtime_error("Next frame overwrote the queued return image");
    CheckDebug(device);
    std::cout << "Two additional queued-return frames preserved first-image checksum " << std::hex << actual
              << "; next-image checksum " << next << std::dec << ".\n" << std::flush;
}

static int Parent(const wchar_t* core, const wchar_t* forwarder, const wchar_t* model, const wchar_t* parentData,
                  const wchar_t* workerData, const wchar_t* log, const wchar_t* workerExe, bool primaryPass,
                  unsigned secondaryPasses, unsigned width, unsigned height, bool failureTest, bool resetSecondFrame,
                  bool noDepth, bool halfGuides, bool rgba8, bool offsetGuides, bool invalidGuides, bool depthStencil,
                  bool benchmark, bool verifyIsolation, bool mainPreset, bool swapAdapters,
                  const wchar_t* secondaryModel, bool asyncHandoff)
{
    MEMORYSTATUSEX memory { sizeof(memory) };
    if (!GlobalMemoryStatusEx(&memory) || memory.ullAvailPageFile < 8ull * 1024 * 1024 * 1024)
        throw std::runtime_error("Probe requires at least 8 GiB system commit headroom");
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Parent factory");
    const POINT origin { 0, 0 };
    const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    LUID primaryLuid {};
    ComPtr<IDXGIAdapter1> primaryAdapter;
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        Check(hr, "Enumerate adapters");
        for (UINT j = 0;; ++j)
        {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(j, &output);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            Check(hr, "Enumerate outputs");
            DXGI_OUTPUT_DESC desc {};
            Check(output->GetDesc(&desc), "Output description");
            if (desc.AttachedToDesktop && desc.Monitor == monitor)
            {
                DXGI_ADAPTER_DESC1 adapterDesc {};
                Check(adapter->GetDesc1(&adapterDesc), "Adapter description");
                primaryLuid = adapterDesc.AdapterLuid;
                primaryAdapter = adapter;
            }
        }
    }
    if (!primaryAdapter)
        throw std::runtime_error("Primary adapter missing");
    ComPtr<ID3D12Device> primary;
    Check(D3D12CreateDevice(primaryAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&primary)), "Primary device");
    DlssNr::MultiGpu::SelectedAdapter secondary;
    Check(DlssNr::MultiGpu::SelectSecondary(factory.Get(), primaryLuid, nullptr, 0x10de, &D3D12CreateDevice, secondary),
          "Select secondary");
    // Diagnostic only: compare the same model with the two physical devices
    // exchanging transport roles. This never changes a game's presentation GPU.
    if (swapAdapters)
    {
        primary.Swap(secondary.device);
        primaryAdapter.Swap(secondary.adapter);
        Check(secondary.adapter->GetDesc1(&secondary.description), "Swapped secondary description");
    }
    DXGI_ADAPTER_DESC1 primaryDesc {};
    Check(primaryAdapter->GetDesc1(&primaryDesc), "Primary adapter description");
    std::wcout << L"Primary: " << primaryDesc.Description << L"; secondary: " << secondary.description.Description
               << L"\n"
               << std::flush;
    Frame frame(primary.Get(), width, height, noDepth, halfGuides, rgba8, offsetGuides, depthStencil);
    Fill(frame.color, 0);
    if (frame.depth.texture)
        Fill(frame.depth, 1);
    Fill(frame.motion, 2);
    Fill(frame.output, 3);
    NrFixture::Surface primaryClamped;
    if (!NrFixture::MakeSurface(primary.Get(), frame.colorFormat, true, primaryClamped, width, height))
        throw std::runtime_error("Create primary clamp texture failed");
    Fill(primaryClamped, 3);
    InterpassClamp primaryClamp(primary.Get(), width, height);
    DlssNr::Worker::FrameTransfer frameTransfer;
    Check(DlssNr::Worker::FrameTransfer::Create(primary.Get(), secondary.device.Get(), frame.color.texture.Get(),
                                                frame.depth.texture.Get(), frame.motion.texture.Get(), frameTransfer),
          "Create frame transfer");
    DlssNr::Worker::ModelSession session;
    session.Initialize(primary.Get(), core, forwarder, model, parentData, width, height,
                       std::vector<DlssNr::Worker::FeatureTuning>(1), 0x24480451ull);
    {
        ComPtr<ID3D12CommandAllocator> wrongAllocator;
        auto wrongList = Commands(secondary.device.Get(), wrongAllocator);
        DlssNr::MultiGpu::OwnedCommandChain wrongChain;
        if (wrongChain.Initialize(primary.Get(), session.Queue(), wrongList.Get()) != E_INVALIDARG ||
            wrongChain.Current() || wrongChain.SubmittedCount())
            throw std::runtime_error("Owned command chain accepted a foreign-device list");
    }
    std::cout << "Primary model initialized; starting isolated secondary.\n" << std::flush;
    Wire wire = frameTransfer.MakeWire(secondaryPasses);
    wire.enableDebug = 1;
    if (mainPreset)
        for (unsigned i = 0; i < secondaryPasses; ++i) wire.passes[i].preset = 1;
    {
        auto invalid = wire;
        invalid.secondary = invalid.primary;
        DlssNr::Worker::WorkerClient rejected;
        if (rejected.Start(primary.Get(), workerExe, core, forwarder, secondaryModel, workerData, log, frameTransfer.Buffer(),
                           invalid) != E_INVALIDARG)
            throw std::runtime_error("Accepted invalid worker adapter");
    }
    {
        auto invalid = wire;
        ++invalid.bytes;
        DlssNr::Worker::WorkerClient rejected;
        if (rejected.Start(primary.Get(), workerExe, core, forwarder, secondaryModel, workerData, log, frameTransfer.Buffer(),
                           invalid) != E_INVALIDARG)
            throw std::runtime_error("Accepted mismatched transfer size");
    }
    DlssNr::Worker::WorkerGeneration generation(std::move(frameTransfer));
    const auto& transferLayout = generation.Transfer();
    const auto& transfer = transferLayout.Buffer();
    Check(generation.Start(primary.Get(), workerExe, core, forwarder, secondaryModel, workerData, log, wire),
          "Start worker generation");
    const HRESULT immediate = generation.Poll(0);
    if (immediate != S_OK && immediate != E_PENDING)
        throw std::runtime_error("Worker initialization failed");
    if (immediate == E_PENDING)
        Check(generation.Poll(60000), "Wait for worker initialization");
    if (verifyIsolation)
        CheckWorkerDxgi(generation.ProcessHandle());
    auto& worker = generation.TestingClient();
    bool submitted = false;
    ComPtr<ID3D12Fence> asyncGate;
    ComPtr<ID3D12Resource> independentUpload, independentReadback;
    try
    {
        uint64_t previousChecksum = 0;
        std::vector<double> handoffTimes;
        const uint64_t frameCount = benchmark ? 30 : 2;
        for (uint64_t frameNumber = 1; frameNumber <= frameCount; ++frameNumber)
        {
            DlssNr::Worker::FrameCommand frameCommand {};
            frameCommand.frameNumber = frameNumber;
            frameCommand.guideWidth = frame.guideWidth;
            frameCommand.guideHeight = frame.guideHeight;
            frameCommand.motionWidth = frame.guideWidth;
            frameCommand.motionHeight = frame.guideHeight;
            frameCommand.depthX = frameCommand.motionX = frame.guideX;
            frameCommand.depthY = frameCommand.motionY = frame.guideY;
            if (invalidGuides)
                frameCommand.motionX = wire.motionWidth;
            frameCommand.resetMask = frameNumber == 1 ? ((1u << secondaryPasses) - 1u) : 0u;
            if (resetSecondFrame && frameNumber == 2)
                generation.MarkSkipped();
            if (frameNumber > 1)
                Fill(frame.color, 4);
            ComPtr<ID3D12CommandAllocator> allocator;
            auto commands = Commands(primary.Get(), allocator);
            if (transferLayout.RecordOutbound(commands.Get(), frame.motion.texture.Get(), frame.depth.texture.Get(),
                                              frame.motion.texture.Get()) != E_INVALIDARG)
                throw std::runtime_error("Accepted mismatched color format");
            DlssNr::MultiGpu::OwnedCommandChain chain;
            Check(chain.Initialize(primary.Get(), session.Queue(), commands.Get()), "Initialize owned command chain");
            if (frameNumber > 1)
            {
                NrFixture::Barrier(commands.Get(), frame.color.texture.Get(),
                                   primaryPass ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                               : D3D12_RESOURCE_STATE_COPY_SOURCE,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
                for (auto* input : { &frame.depth, &frame.motion })
                    if (input->texture)
                        NrFixture::Barrier(commands.Get(), input->texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                           D3D12_RESOURCE_STATE_COPY_DEST);
                NrFixture::Barrier(commands.Get(), frame.output.texture.Get(),
                                   primaryPass ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                               : D3D12_RESOURCE_STATE_COPY_SOURCE,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
                if (primaryPass)
                    NrFixture::Barrier(commands.Get(), primaryClamped.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                       D3D12_RESOURCE_STATE_COPY_DEST);
            }
            CopyUpload(commands.Get(), frame.color,
                       primaryPass ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (frame.depth.texture)
                CopyUpload(commands.Get(), frame.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            CopyUpload(commands.Get(), frame.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            CopyUpload(commands.Get(), frame.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (primaryPass)
            {
                CopyUpload(commands.Get(), primaryClamped, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Evaluate(session, commands.Get(), frame, frameNumber == 1);
                NrFixture::Barrier(commands.Get(), frame.output.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                primaryClamp.Record(commands.Get(), frame.output.texture.Get(), frame.color.texture.Get(),
                                    primaryClamped.texture.Get());
                NrFixture::Barrier(commands.Get(), primaryClamped.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
            }
            else
                NrFixture::Barrier(commands.Get(), frame.output.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (frame.depth.texture)
                NrFixture::Barrier(commands.Get(), frame.depth.texture.Get(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            NrFixture::Barrier(commands.Get(), frame.motion.texture.Get(),
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            auto* outboundColor = primaryPass ? primaryClamped.texture.Get() : frame.color.texture.Get();
            D3D12_COMMAND_QUEUE_DESC otherDesc {};
            otherDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ComPtr<ID3D12CommandQueue> otherQueue;
            Check(primary->CreateCommandQueue(&otherDesc, IID_PPV_ARGS(&otherQueue)), "Create alternate queue");
            const auto rejectedHandoff = generation.Submit(chain, otherQueue.Get(), frameCommand, outboundColor,
                                                           frame.depth.texture.Get(), frame.motion.texture.Get(), 0);
            if (rejectedHandoff.result != E_INVALIDARG ||
                rejectedHandoff.phase != DlssNr::Worker::HandoffPhase::Validate)
                throw std::runtime_error("Accepted a different primary queue");
            submitted = true;
            auto beginAsync = [&]()
            {
                auto require = [&](bool condition, const char* detail)
                {
                    if (condition) return;
                    std::cerr << detail << '\n' << std::flush;
                    // Release only this fixture's local queue gate before terminating.
                    if (asyncGate) asyncGate->Signal(1);
                    ExitProcess(3);
                };
                require(generation.PollReturn() == E_INVALIDARG && generation.WaitReturn(0) == E_INVALIDARG,
                        "Accepted a return before a frame was submitted");
                independentUpload = Buffer(primary.Get(), D3D12_HEAP_TYPE_UPLOAD,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, sizeof(uint32_t));
                independentReadback = Buffer(primary.Get(), D3D12_HEAP_TYPE_READBACK,
                                             D3D12_RESOURCE_STATE_COPY_DEST, sizeof(uint32_t));
                const auto marker = static_cast<uint32_t>(frameNumber);
                void* bytes = nullptr;
                D3D12_RANGE noRead { 0, 0 };
                Check(independentUpload->Map(0, &noRead, &bytes), "Map independent primary marker");
                std::memcpy(bytes, &marker, sizeof(marker));
                independentUpload->Unmap(0, nullptr);
                Check(primary->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&asyncGate)), "Create local async gate");
                // This fence is released by this CPU, never controlled by the worker.
                // It makes the not-ready polling checks independent of GPU timing.
                Check(session.Queue()->Wait(asyncGate.Get(), 1), "Gate async primary submission");
                auto sent = generation.SubmitAsync(chain, session.Queue(), frameCommand, outboundColor,
                                                   frame.depth.texture.Get(), frame.motion.texture.Get());
                require(sent.result == S_OK && sent.phase == DlssNr::Worker::HandoffPhase::OutboundSignalled,
                        "Async submission did not return before worker completion");
                require((invalidGuides || (generation.PollReturn() == E_PENDING && generation.WaitReturn(0) == E_PENDING)) &&
                            generation.WaitReturn(INFINITE) == E_INVALIDARG &&
                            worker.PollReturn(frameNumber + 1) == E_INVALIDARG &&
                            generation.Poll() == E_PENDING && generation.Stop(0) == E_PENDING,
                        "Pending async polling changed or poisoned the frame state");
                require(generation.RecordReturn(chain.Current(), frame.output.texture.Get()) == E_INVALIDARG,
                        "Read worker output before completion");
                const auto duplicate = generation.SubmitAsync(chain, session.Queue(), frameCommand, outboundColor,
                                                              frame.depth.texture.Get(), frame.motion.texture.Get());
                require(duplicate.result == E_PENDING && duplicate.phase == DlssNr::Worker::HandoffPhase::Validate,
                        "Accepted a second async frame before return");
                chain.Current()->CopyBufferRegion(independentReadback.Get(), 0, independentUpload.Get(), 0,
                                                 sizeof(marker));
                require(SUCCEEDED(chain.Split()) && chain.SubmittedCount() == 2,
                        "Could not submit independent primary work before waiting");
                require(SUCCEEDED(asyncGate->Signal(1)), "Could not release local async gate");
                return sent;
            };
            if (failureTest)
            {
                if (asyncHandoff)
                    beginAsync();
                else
                {
                    Check(transferLayout.RecordOutbound(commands.Get(), outboundColor, frame.depth.texture.Get(),
                                                        frame.motion.texture.Get()),
                          "Record failure-test outbound");
                    Check(worker.SendFrame(frameCommand), "Send failure-test frame");
                    if (FAILED(chain.Split()) || FAILED(session.Queue()->Signal(transfer.PrimaryReady(), frameNumber)))
                        ExitProcess(3);
                }
                Retire(primary.Get(), session.Queue());
                submitted = false;
                // Only this test's own child is terminated; the primary queue never waits on its fence.
                if (!TerminateProcess(worker.ProcessHandle(), 42) ||
                    WaitForSingleObject(worker.ProcessHandle(), 30000) != WAIT_OBJECT_0)
                    throw std::runtime_error("Could not stop test worker");
                if (asyncHandoff ? generation.PollReturn() != E_ABORT || generation.WaitReturn(1000) != E_ABORT
                                 : worker.WaitForReturn(frameNumber, 1000) != DlssNr::MultiGpu::WorkerFenceWaitResult::PeerExited)
                    throw std::runtime_error("Did not detect test worker exit");
                Retire(primary.Get(), session.Queue());
                Check(primary->GetDeviceRemovedReason(), "Primary device after worker exit");
                std::cout << "Worker exit detected after primary submission; primary queue remains usable.\n"
                          << std::flush;
                return 0;
            }
            const auto handoffStart = std::chrono::steady_clock::now();
            auto handoff = asyncHandoff ? beginAsync()
                : generation.Submit(chain, session.Queue(), frameCommand, outboundColor,
                                    frame.depth.texture.Get(), frame.motion.texture.Get(), 60000);
            if (asyncHandoff && SUCCEEDED(handoff.result))
            {
                handoff.result = generation.WaitReturn(60000);
                if (handoff.result == S_OK) handoff.phase = DlssNr::Worker::HandoffPhase::WorkerReturned;
            }
            if (benchmark && frameNumber > 1)
                handoffTimes.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - handoffStart).count());
            if (invalidGuides)
            {
                if (handoff.result != E_ABORT || handoff.phase != DlssNr::Worker::HandoffPhase::OutboundSignalled)
                    ExitProcess(3);
                Retire(primary.Get(), session.Queue());
                submitted = false;
                Check(primary->GetDeviceRemovedReason(), "Primary device after rejected guide");
                std::cout << "Invalid guide rejected after submission; primary queue remains usable.\n" << std::flush;
                return 0;
            }
            if (FAILED(handoff.result) || handoff.phase != DlssNr::Worker::HandoffPhase::WorkerReturned)
            {
                std::cerr << "Worker handoff failed; terminating isolated fixture.\n" << std::flush;
                ExitProcess(3);
            }
            if (asyncHandoff)
            {
                const auto duplicate = generation.SubmitAsync(chain, session.Queue(), frameCommand, outboundColor,
                                                              frame.depth.texture.Get(), frame.motion.texture.Get());
                if (generation.PollReturn() != S_OK || generation.WaitReturn(0) != S_OK ||
                    duplicate.result != E_PENDING || duplicate.phase != DlssNr::Worker::HandoffPhase::Validate)
                    ExitProcess(3);
            }
            ComPtr<ID3D12Fence> pendingRetirement;
            Check(primary->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pendingRetirement)),
                  "Create pending fence");
            auto readback =
                Buffer(primary.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, frame.color.bytes);
            NrFixture::Surface returnedColor;
            if (!NrFixture::MakeSurface(primary.Get(), frame.colorFormat, false, returnedColor, width, height))
                throw std::runtime_error("Create returned primary texture failed");
            auto* inbound = chain.Current();
            Check(generation.RecordReturn(inbound, returnedColor.texture.Get()), "Record frame transfer inbound");
            Transition(inbound, returnedColor.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            auto fromTexture = NrFixture::Texture(returnedColor.texture.Get());
            auto toReadback = NrFixture::Buffer(readback.Get(), returnedColor.footprint);
            inbound->CopyTextureRegion(&toReadback, 0, 0, 0, &fromTexture, nullptr);
            Check(generation.TrackRetirement(pendingRetirement.Get(), 1), "Track primary retirement");
            if (generation.Poll() != E_PENDING)
                throw std::runtime_error("Reused a frame before primary retirement");
            if (asyncHandoff && (generation.PollReturn() != E_INVALIDARG ||
                generation.SubmitAsync(chain, session.Queue(), frameCommand, outboundColor,
                                      frame.depth.texture.Get(), frame.motion.texture.Get()).result != E_PENDING))
                ExitProcess(3);
            if (FAILED(chain.SubmitFinal()) || chain.SubmittedCount() != (asyncHandoff ? 3u : 2u) || chain.Current() ||
                chain.Split() != E_ABORT || chain.SubmitFinal() != E_ABORT ||
                FAILED(session.Queue()->Signal(pendingRetirement.Get(), 1)))
                ExitProcess(3);
            Retire(primary.Get(), session.Queue());
            submitted = false;
            Check(generation.Poll(), "Acknowledge returned frame");
            if (asyncHandoff)
            {
                D3D12_RANGE markerRange { 0, sizeof(uint32_t) }, markerNoWrite { 0, 0 };
                uint32_t* marker = nullptr;
                Check(independentReadback->Map(0, &markerRange, reinterpret_cast<void**>(&marker)),
                      "Map independent primary result");
                const bool matched = *marker == static_cast<uint32_t>(frameNumber);
                independentReadback->Unmap(0, &markerNoWrite);
                if (!matched) throw std::runtime_error("Independent primary work produced the wrong marker");
            }
            CheckDebug(primary.Get());
            D3D12_RANGE range { 0, static_cast<SIZE_T>(frame.color.bytes) }, noWrite { 0, 0 };
            uint8_t* mapped = nullptr;
            Check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "Map returned color");
            bool finite = true, nonzero = false;
            uint64_t checksum = 1469598103934665603ull;
            for (UINT y = 0; y < frame.color.rows; ++y)
            {
                auto* row = mapped + y * frame.color.footprint.Footprint.RowPitch;
                for (unsigned x = 0; x < width; ++x)
                    for (unsigned c = 0; c < 3; ++c)
                    {
                        const uint16_t value =
                            rgba8 ? row[x * 4 + c] : reinterpret_cast<const uint16_t*>(row)[x * 4 + c];
                        if (!rgba8)
                            finite = finite && (value & 0x7c00u) != 0x7c00u;
                        nonzero = nonzero || (rgba8 ? value != 0 : (value & 0x7fffu) != 0);
                        checksum = (checksum ^ value) * 1099511628211ull;
                    }
            }
            readback->Unmap(0, &noWrite);
            if (!finite || !nonzero)
                throw std::runtime_error("Returned neural output was nonfinite or empty");
            if (frameNumber == 2 && checksum == previousChecksum)
                throw std::runtime_error("Distinct frames returned identical output");
            previousChecksum = checksum;
            std::cout << "Frame " << frameNumber << " returned to a primary GPU texture; checksum " << std::hex
                      << checksum << std::dec << "\n"
                      << std::flush;
        }
        if (!handoffTimes.empty())
        {
            std::sort(handoffTimes.begin(), handoffTimes.end());
            std::cout << "Handoff latency median=" << handoffTimes[handoffTimes.size() / 2]
                      << " ms p95=" << handoffTimes[(handoffTimes.size() * 95) / 100] << " ms\n";
            if (asyncHandoff)
                std::cout << "Async fixture timings include local validation gates and independent copies.\n";
        }
        if (asyncHandoff)
            CheckQueuedReturn(generation, primary.Get(), session.Queue(), frame,
                              primaryPass ? primaryClamped.texture.Get() : frame.color.texture.Get(),
                              width, height, rgba8);
        Check(generation.Stop(30000), "Stop worker");
        if (asyncHandoff)
            std::cout << "Async polling, independent primary work and return-retirement guards passed.\n";
        std::cout << (primaryPass ? "One primary pass and " : "") << secondaryPasses
                  << " secondary passes evaluated " << frameCount << " sequential frames; worker exited cleanly.\n"
                  << std::flush;
    }
    catch (...)
    {
        if (submitted)
        {
            if (asyncGate) asyncGate->Signal(1);
            Retire(primary.Get(), session.Queue());
        }
        throw;
    }
    return 0;
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc >= 11 && argc <= 27)
        {
            const wchar_t* secondaryModel = argv[3];
            bool secondaryModelSet = false;
            bool primaryPass = true, failureTest = false, resetSecondFrame = false, noDepth = false, halfGuides = false,
                 rgba8 = false, offsetGuides = false, invalidGuides = false, depthStencil = false, benchmark = false,
                 verifyIsolation = false, mainPreset = false, swapAdapters = false, asyncHandoff = false;
            for (int i = 11; i < argc; ++i)
            {
                if (std::wcscmp(argv[i], L"--no-primary") == 0 && primaryPass)
                    primaryPass = false;
                else if (std::wcscmp(argv[i], L"--failure-test") == 0 && !failureTest)
                    failureTest = true;
                else if (std::wcscmp(argv[i], L"--reset-second-frame") == 0 && !resetSecondFrame)
                    resetSecondFrame = true;
                else if (std::wcscmp(argv[i], L"--no-depth") == 0 && !noDepth)
                    noDepth = true;
                else if (std::wcscmp(argv[i], L"--half-guides") == 0 && !halfGuides)
                    halfGuides = true;
                else if (std::wcscmp(argv[i], L"--rgba8") == 0 && !rgba8)
                    rgba8 = true;
                else if (std::wcscmp(argv[i], L"--offset-guides") == 0 && !offsetGuides)
                    offsetGuides = true;
                else if (std::wcscmp(argv[i], L"--invalid-guides") == 0 && !invalidGuides)
                    invalidGuides = true;
                else if (std::wcscmp(argv[i], L"--depth-stencil") == 0 && !depthStencil)
                    depthStencil = true;
                else if (std::wcscmp(argv[i], L"--benchmark") == 0 && !benchmark)
                    benchmark = true;
                else if (std::wcscmp(argv[i], L"--verify-isolation") == 0 && !verifyIsolation)
                    verifyIsolation = true;
                else if (std::wcscmp(argv[i], L"--main-preset") == 0 && !mainPreset)
                    mainPreset = true;
                else if (std::wcscmp(argv[i], L"--swap-adapters") == 0 && !swapAdapters)
                    swapAdapters = true;
                else if (std::wcscmp(argv[i], L"--async-handoff") == 0 && !asyncHandoff)
                    asyncHandoff = true;
                else if (std::wcscmp(argv[i], L"--secondary-model") == 0 && !secondaryModelSet && i + 1 < argc)
                {
                    secondaryModel = argv[++i];
                    secondaryModelSet = true;
                }
                else
                    return 2;
            }
            wchar_t* ends[3] {};
            const unsigned long passes = std::wcstoul(argv[8], &ends[0], 10);
            const unsigned long width = std::wcstoul(argv[9], &ends[1], 10);
            const unsigned long height = std::wcstoul(argv[10], &ends[2], 10);
            if (!ends[0] || *ends[0] || !ends[1] || *ends[1] || !ends[2] || *ends[2] || passes == 0 ||
                passes > DlssNr::Worker::MaxWorkerPasses || width == 0 || height == 0 || width > 16384 ||
                height > 16384)
                return 2;
            return Parent(argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], primaryPass,
                          static_cast<unsigned>(passes), static_cast<unsigned>(width), static_cast<unsigned>(height),
                          failureTest, resetSecondFrame, noDepth, halfGuides, rgba8, offsetGuides, invalidGuides,
                          depthStencil, benchmark, verifyIsolation, mainPreset, swapAdapters, secondaryModel, asyncHandoff);
        }
        std::cerr << "Expected core, forwarder, model, primary data, worker data, worker log, executable, pass count "
                     "and dimensions.\n";
        return 2;
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
