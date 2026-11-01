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
#include <d3d12sdklayers.h>
#include <cwchar>
#include <limits>

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
    unsigned width, height;
    explicit Frame(ID3D12Device* device, unsigned requestedWidth, unsigned requestedHeight)
        : width(requestedWidth), height(requestedHeight)
    {
        if (!NrFixture::MakeSurface(device, DXGI_FORMAT_R16G16B16A16_FLOAT, false, color, width, height) ||
            !NrFixture::MakeSurface(device, DXGI_FORMAT_R32_FLOAT, false, depth, width, height) ||
            !NrFixture::MakeSurface(device, DXGI_FORMAT_R16G16_FLOAT, false, motion, width, height) ||
            !NrFixture::MakeSurface(device, DXGI_FORMAT_R16G16B16A16_FLOAT, true, output, width, height))
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
            auto* half = reinterpret_cast<uint16_t*>(row);
            for (unsigned x = 0; x < surface.texture->GetDesc().Width; ++x)
            {
                half[4 * x] = half[4 * x + 1] = half[4 * x + 2] = kind == 0 ? 0x3800 : 0x3400;
                half[4 * x + 3] = 0x3c00;
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
static D3D12_TEXTURE_COPY_LOCATION SharedLocation(ID3D12Resource* shared, const NrFixture::Surface& surface,
                                                  UINT64 offset)
{
    auto location = NrFixture::Buffer(shared, surface.footprint);
    location.PlacedFootprint.Offset = offset;
    return location;
}
static void CopyToShared(ID3D12GraphicsCommandList* commands, ID3D12Resource* shared, NrFixture::Surface& surface,
                         UINT64 offset)
{
    auto src = NrFixture::Texture(surface.texture.Get());
    auto dst = SharedLocation(shared, surface, offset);
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
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
static DlssNr::Worker::Evaluation MakeEvaluation(Frame& frame, bool reset)
{
    DlssNr::Worker::Evaluation request {};
    request.color = frame.color.texture.Get();
    request.depth = frame.depth.texture.Get();
    request.motion = frame.motion.texture.Get();
    request.output = frame.output.texture.Get();
    request.width = request.guideWidth = request.motionWidth = frame.width;
    request.height = request.guideHeight = request.motionHeight = frame.height;
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
static int Parent(const wchar_t* core, const wchar_t* forwarder, const wchar_t* model, const wchar_t* parentData,
                  const wchar_t* workerData, const wchar_t* log, const wchar_t* workerExe, bool primaryPass,
                  unsigned secondaryPasses, unsigned width, unsigned height)
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
    DXGI_ADAPTER_DESC1 primaryDesc {};
    Check(primaryAdapter->GetDesc1(&primaryDesc), "Primary adapter description");
    std::wcout << L"Primary: " << primaryDesc.Description << L"; secondary: " << secondary.description.Description
               << L"\n"
               << std::flush;
    Frame frame(primary.Get(), width, height);
    Fill(frame.color, 0);
    Fill(frame.depth, 1);
    Fill(frame.motion, 2);
    Fill(frame.output, 3);
    NrFixture::Surface primaryClamped;
    if (!NrFixture::MakeSurface(primary.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true, primaryClamped, width, height))
        throw std::runtime_error("Create primary clamp texture failed");
    Fill(primaryClamped, 3);
    InterpassClamp primaryClamp(primary.Get(), width, height);
    DlssNr::MultiGpu::CrossAdapterBuffer transfer;
    Check(DlssNr::MultiGpu::CrossAdapterBuffer::Create(primary.Get(), secondary.device.Get(), frame.Bytes(), transfer),
          "Create frame transfer");
    DlssNr::MultiGpu::CrossAdapterHandles handles;
    Check(DlssNr::MultiGpu::CrossAdapterHandles::Create(transfer, handles), "Export frame handles");
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
    SECURITY_ATTRIBUTES security { sizeof(security), nullptr, TRUE };
    OwnedHandle readPipe, writePipe;
    if (!CreatePipe(&readPipe.value, &writePipe.value, &security, 0) ||
        !SetHandleInformation(writePipe.value, HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("Create pipe failed");
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<uint8_t> storage(attributeBytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
        throw std::runtime_error("Initialize attributes failed");
    HANDLE inherited[] = { readPipe.value };
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                   nullptr, nullptr))
    {
        DeleteProcThreadAttributeList(attributes);
        throw std::runtime_error("Restrict inherited handles failed");
    }
    auto quote = [](const wchar_t* value) { return L"\"" + std::wstring(value) + L"\""; };
    std::wstring command = quote(workerExe) + L" --worker " +
                           std::to_wstring(reinterpret_cast<uintptr_t>(readPipe.value)) + L" " + quote(core) + L" " +
                           quote(forwarder) + L" " + quote(model) + L" " + quote(workerData) + L" " + quote(log);
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION child {};
    const BOOL created = CreateProcessW(workerExe, command.data(), nullptr, nullptr, TRUE,
                                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                                        nullptr, &startup.StartupInfo, &child);
    DeleteProcThreadAttributeList(attributes);
    if (!created)
        throw std::runtime_error("Create worker failed");
    OwnedHandle process { child.hProcess }, thread { child.hThread };
    bool completed = false;
    bool submitted = false;
    try
    {
        Wire wire {};
        wire.passCount = secondaryPasses;
        wire.frameCount = 0; // Exercise a persistent worker followed by an explicit shutdown command.
        wire.width = width;
        wire.height = height;
        wire.depthOffset = frame.DepthOffset();
        wire.motionOffset = frame.MotionOffset();
        wire.bytes = frame.Bytes();
        wire.colorBytes = frame.color.bytes;
        wire.depthBytes = frame.depth.bytes;
        wire.motionBytes = frame.motion.bytes;
        wire.colorPitch = frame.color.footprint.Footprint.RowPitch;
        wire.depthPitch = frame.depth.footprint.Footprint.RowPitch;
        wire.motionPitch = frame.motion.footprint.Footprint.RowPitch;
        wire.primary = primaryLuid;
        wire.secondary = secondary.device->GetAdapterLuid();
        HANDLE remoteHeap = nullptr, remoteReady = nullptr, remoteDone = nullptr, remoteParent = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), handles.Heap(), process.value, &remoteHeap, 0, FALSE,
                             DUPLICATE_SAME_ACCESS) ||
            !DuplicateHandle(GetCurrentProcess(), handles.Ready(), process.value, &remoteReady, 0, FALSE,
                             DUPLICATE_SAME_ACCESS) ||
            !DuplicateHandle(GetCurrentProcess(), handles.Done(), process.value, &remoteDone, 0, FALSE,
                             DUPLICATE_SAME_ACCESS) ||
            !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), process.value, &remoteParent, SYNCHRONIZE, FALSE,
                             0))
            throw std::runtime_error("Duplicate handles failed");
        wire.heap = reinterpret_cast<uintptr_t>(remoteHeap);
        wire.ready = reinterpret_cast<uintptr_t>(remoteReady);
        wire.done = reinterpret_cast<uintptr_t>(remoteDone);
        wire.parent = reinterpret_cast<uintptr_t>(remoteParent);
        DWORD written = 0;
        if (!WriteFile(writePipe.value, &wire, sizeof(wire), &written, nullptr) || written != sizeof(wire))
            throw std::runtime_error("Write handshake failed");
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1))
            throw std::runtime_error("Resume worker failed");
        uint64_t previousChecksum = 0;
        for (uint64_t frameNumber = 1; frameNumber <= 2; ++frameNumber)
        {
            DlssNr::Worker::FrameCommand frameCommand {};
            frameCommand.frameNumber = frameNumber;
            frameCommand.resetMask = frameNumber == 1 ? ((1u << secondaryPasses) - 1u) : 0u;
            DWORD controlWritten = 0;
            if (!WriteFile(writePipe.value, &frameCommand, sizeof(frameCommand), &controlWritten, nullptr) ||
                controlWritten != sizeof(frameCommand))
                throw std::runtime_error("Write frame control failed");
            if (frameNumber > 1)
                Fill(frame.color, 4);
            ComPtr<ID3D12CommandAllocator> allocator;
            auto commands = Commands(primary.Get(), allocator);
            DlssNr::MultiGpu::OwnedCommandChain chain;
            Check(chain.Initialize(primary.Get(), session.Queue(), commands.Get()), "Initialize owned command chain");
            if (frameNumber > 1)
            {
                NrFixture::Barrier(commands.Get(), frame.color.texture.Get(),
                                   primaryPass ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                               : D3D12_RESOURCE_STATE_COPY_SOURCE,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
                for (auto* input : { &frame.depth, &frame.motion })
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
            NrFixture::Barrier(commands.Get(), frame.depth.texture.Get(),
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            NrFixture::Barrier(commands.Get(), frame.motion.texture.Get(),
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            // This is an owned list: prove that recorded primary work can be submitted before a
            // separately owned continuation records the cross-process handoff.
            submitted = true;
            if (FAILED(chain.Split()))
                ExitProcess(3);
            auto* continuation = chain.Current();
            Transition(continuation, transfer.PrimaryBuffer(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            CopyToShared(continuation, transfer.PrimaryBuffer(), primaryPass ? primaryClamped : frame.color, 0);
            CopyToShared(continuation, transfer.PrimaryBuffer(), frame.depth, frame.DepthOffset());
            CopyToShared(continuation, transfer.PrimaryBuffer(), frame.motion, frame.MotionOffset());
            Transition(continuation, transfer.PrimaryBuffer(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_COMMON);
            if (FAILED(chain.SubmitFinal()) || chain.SubmittedCount() != 2 || chain.Current() ||
                chain.Split() != E_ABORT || chain.SubmitFinal() != E_ABORT)
                ExitProcess(3);
            if (FAILED(session.Queue()->Signal(transfer.PrimaryReady(), frameNumber)))
                ExitProcess(3);
            Retire(primary.Get(), session.Queue());
            submitted = false;
            std::cout << (primaryPass ? "Primary neural pass retired and frame sent to secondary.\n"
                                      : "Original primary frame sent to secondary.\n")
                      << std::flush;
            const auto result =
                DlssNr::MultiGpu::WaitForFenceOrPeer(transfer.PrimaryDone(), frameNumber, process.value, 60000);
            const uint64_t returned = transfer.PrimaryDone()->GetCompletedValue();
            // A worker may exit cleanly immediately after signaling. Accept that race only when
            // the result fence really completed; its process exit code is checked below.
            if ((result != DlssNr::MultiGpu::WorkerFenceWaitResult::Ready &&
                 result != DlssNr::MultiGpu::WorkerFenceWaitResult::PeerExited) ||
                returned == (std::numeric_limits<uint64_t>::max)() || returned < frameNumber)
                throw std::runtime_error("Worker exited or timed out before returning its frame; see worker log");
            auto readback =
                Buffer(primary.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, frame.color.bytes);
            NrFixture::Surface returnedColor;
            if (!NrFixture::MakeSurface(primary.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, false, returnedColor, width,
                                        height))
                throw std::runtime_error("Create returned primary texture failed");
            ComPtr<ID3D12CommandAllocator> inboundAllocator;
            auto inbound = Commands(primary.Get(), inboundAllocator);
            Transition(inbound.Get(), transfer.PrimaryBuffer(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            auto fromShared = SharedLocation(transfer.PrimaryBuffer(), returnedColor, 0);
            auto toTexture = NrFixture::Texture(returnedColor.texture.Get());
            inbound->CopyTextureRegion(&toTexture, 0, 0, 0, &fromShared, nullptr);
            NrFixture::Barrier(inbound.Get(), returnedColor.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
            auto fromTexture = NrFixture::Texture(returnedColor.texture.Get());
            auto toReadback = NrFixture::Buffer(readback.Get(), returnedColor.footprint);
            inbound->CopyTextureRegion(&toReadback, 0, 0, 0, &fromTexture, nullptr);
            Transition(inbound.Get(), transfer.PrimaryBuffer(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_COMMON);
            Submit(session.Queue(), inbound.Get());
            submitted = true;
            Retire(primary.Get(), session.Queue());
            submitted = false;
            CheckDebug(primary.Get());
            D3D12_RANGE range { 0, static_cast<SIZE_T>(frame.color.bytes) }, noWrite { 0, 0 };
            uint8_t* mapped = nullptr;
            Check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "Map returned color");
            bool finite = true, nonzero = false;
            uint64_t checksum = 1469598103934665603ull;
            for (UINT y = 0; y < frame.color.rows; ++y)
            {
                auto* row = reinterpret_cast<const uint16_t*>(mapped + y * frame.color.footprint.Footprint.RowPitch);
                for (unsigned x = 0; x < width; ++x)
                    for (unsigned c = 0; c < 3; ++c)
                    {
                        const uint16_t value = row[x * 4 + c];
                        finite = finite && (value & 0x7c00u) != 0x7c00u;
                        nonzero = nonzero || (value & 0x7fffu) != 0;
                        checksum = (checksum ^ value) * 1099511628211ull;
                    }
            }
            readback->Unmap(0, &noWrite);
            if (!finite || !nonzero)
                throw std::runtime_error("Returned neural output was nonfinite or empty");
            if (frameNumber > 1 && checksum == previousChecksum)
                throw std::runtime_error("Distinct frames returned identical output");
            previousChecksum = checksum;
            std::cout << "Frame " << frameNumber << " returned to a primary GPU texture; checksum " << std::hex
                      << checksum << std::dec << "\n"
                      << std::flush;
        }
        DlssNr::Worker::FrameCommand shutdown {};
        shutdown.kind = DlssNr::Worker::CommandKind::Shutdown;
        DWORD shutdownWritten = 0;
        if (!WriteFile(writePipe.value, &shutdown, sizeof(shutdown), &shutdownWritten, nullptr) ||
            shutdownWritten != sizeof(shutdown))
            throw std::runtime_error("Write worker shutdown failed");
        if (WaitForSingleObject(process.value, 30000) != WAIT_OBJECT_0)
            throw std::runtime_error("Worker did not exit");
        DWORD exitCode = 1;
        if (!GetExitCodeProcess(process.value, &exitCode) || exitCode != 0)
            throw std::runtime_error("Worker failed; see worker log");
        completed = true;
        std::cout << (primaryPass ? "One primary pass and " : "") << secondaryPasses
                  << " secondary passes evaluated two sequential frames; worker exited cleanly.\n"
                  << std::flush;
    }
    catch (...)
    {
        if (submitted)
            Retire(primary.Get(), session.Queue());
        if (!completed && WaitForSingleObject(process.value, 0) != WAIT_OBJECT_0)
        {
            TerminateProcess(process.value, 1);
            WaitForSingleObject(process.value, 30000);
        }
        throw;
    }
    return 0;
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 11 || (argc == 12 && std::wcscmp(argv[11], L"--no-primary") == 0))
        {
            wchar_t* ends[3] {};
            const unsigned long passes = std::wcstoul(argv[8], &ends[0], 10);
            const unsigned long width = std::wcstoul(argv[9], &ends[1], 10);
            const unsigned long height = std::wcstoul(argv[10], &ends[2], 10);
            if (!ends[0] || *ends[0] || !ends[1] || *ends[1] || !ends[2] || *ends[2] || passes == 0 ||
                passes > DlssNr::Worker::MaxWorkerPasses || width == 0 || height == 0 || width > 16384 ||
                height > 16384)
                return 2;
            return Parent(argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argc == 11,
                          static_cast<unsigned>(passes), static_cast<unsigned>(width), static_cast<unsigned>(height));
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
