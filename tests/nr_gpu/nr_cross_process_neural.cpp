#include "pch.h"
#include "../../OptiScaler/dlssnr/DlssNr_CrossAdapterIpc.h"
#include "../../OptiScaler/dlssnr/DlssNr_CrossAdapterSelect.h"
#include "nr_ngx_session_fixture.h"
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
struct Wire
{
    uint64_t heap = 0, ready = 0, done = 0, bytes = 0, colorBytes = 0, depthBytes = 0, motionBytes = 0;
    UINT colorPitch = 0, depthPitch = 0, motionPitch = 0;
    LUID primary {}, secondary {};
};
struct Frame
{
    NrFixture::Surface color, depth, motion, output;
    explicit Frame(ID3D12Device* device)
    {
        if (!NrFixture::MakeSurface(device, DXGI_FORMAT_R16G16B16A16_FLOAT, false, color) ||
            !NrFixture::MakeSurface(device, DXGI_FORMAT_R32_FLOAT, false, depth) ||
            !NrFixture::MakeSurface(device, DXGI_FORMAT_R16G16_FLOAT, false, motion) ||
            !NrFixture::MakeSurface(device, DXGI_FORMAT_R16G16B16A16_FLOAT, true, output))
            throw std::runtime_error("Could not create neural textures");
        if (color.bytes % 512 || depth.bytes % 512 || motion.bytes % 512 || color.bytes != output.bytes)
            throw std::runtime_error("Unsupported copy footprint alignment");
    }
    uint64_t Bytes() const { return color.bytes + depth.bytes + motion.bytes; }
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
            for (unsigned x = 0; x < 256; ++x)
            {
                half[4 * x] = half[4 * x + 1] = half[4 * x + 2] = kind == 0 ? 0x3800 : 0x3400;
                half[4 * x + 3] = 0x3c00;
            }
        }
        else if (kind == 1)
        {
            auto* values = reinterpret_cast<float*>(row);
            for (unsigned x = 0; x < 256; ++x)
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
static void CopyFromShared(ID3D12GraphicsCommandList* commands, ID3D12Resource* shared, NrFixture::Surface& surface,
                           UINT64 offset)
{
    auto src = SharedLocation(shared, surface, offset);
    auto dst = NrFixture::Texture(surface.texture.Get());
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    NrFixture::Barrier(commands, surface.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
static void Evaluate(NrFixture::ModelSession& session, ID3D12GraphicsCommandList* commands, Frame& frame, bool reset)
{
    const int result = session.Record(commands, frame.color.texture.Get(), frame.depth.texture.Get(),
                                      frame.motion.texture.Get(), frame.output.texture.Get(), reset);
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
static int Worker(HANDLE pipe, const wchar_t* core, const wchar_t* forwarder, const wchar_t* model, const wchar_t* data,
                  const wchar_t* log)
{
    FILE* unused = nullptr;
    _wfreopen_s(&unused, log, L"w", stderr);
    Wire wire {};
    DWORD read = 0;
    if (!ReadFile(pipe, &wire, sizeof(wire), &read, nullptr) || read != sizeof(wire))
        return 10;
    CloseHandle(pipe);
    OwnedHandle heap { reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.heap)) };
    OwnedHandle ready { reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.ready)) };
    OwnedHandle done { reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.done)) };
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
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
    Frame frame(device.Get());
    if (frame.Bytes() != wire.bytes || frame.color.bytes != wire.colorBytes || frame.depth.bytes != wire.depthBytes ||
        frame.motion.bytes != wire.motionBytes || frame.color.footprint.Footprint.RowPitch != wire.colorPitch ||
        frame.depth.footprint.Footprint.RowPitch != wire.depthPitch ||
        frame.motion.footprint.Footprint.RowPitch != wire.motionPitch)
        throw std::runtime_error("Worker footprint mismatch");
    Fill(frame.output, 3);
    NrFixture::ModelSession session;
    session.Initialize(device.Get(), core, forwarder, model, data);
    for (uint64_t frameNumber = 1; frameNumber <= 2; ++frameNumber)
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        auto commands = Commands(device.Get(), allocator);
        if (frameNumber > 1)
        {
            for (auto* input : { &frame.color, &frame.depth, &frame.motion })
                NrFixture::Barrier(commands.Get(), input->texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
            NrFixture::Barrier(commands.Get(), frame.output.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
        }
        Transition(commands.Get(), peer.Buffer(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        CopyFromShared(commands.Get(), peer.Buffer(), frame.color, 0);
        CopyFromShared(commands.Get(), peer.Buffer(), frame.depth, frame.color.bytes);
        CopyFromShared(commands.Get(), peer.Buffer(), frame.motion, frame.color.bytes + frame.depth.bytes);
        CopyUpload(commands.Get(), frame.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Evaluate(session, commands.Get(), frame, frameNumber == 1);
        NrFixture::Barrier(commands.Get(), frame.output.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(commands.Get(), peer.Buffer(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        CopyToShared(commands.Get(), peer.Buffer(), frame.output, 0);
        Transition(commands.Get(), peer.Buffer(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Check(session.Queue()->Wait(peer.Ready(), frameNumber), "Worker wait for primary frame");
        Submit(session.Queue(), commands.Get());
        if (FAILED(session.Queue()->Signal(peer.Done(), frameNumber)))
        {
            std::cerr << "Cannot signal worker completion.\n" << std::flush;
            ExitProcess(3);
        }
        Retire(device.Get(), session.Queue());
    }
    CheckDebug(device.Get());
    std::cerr << "Worker neural pass retired successfully.\n" << std::flush;
    return 0;
}

static int Parent(const wchar_t* core, const wchar_t* forwarder, const wchar_t* model, const wchar_t* parentData,
                  const wchar_t* workerData, const wchar_t* log)
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
    Frame frame(primary.Get());
    Fill(frame.color, 0);
    Fill(frame.depth, 1);
    Fill(frame.motion, 2);
    Fill(frame.output, 3);
    DlssNr::MultiGpu::CrossAdapterBuffer transfer;
    Check(DlssNr::MultiGpu::CrossAdapterBuffer::Create(primary.Get(), secondary.device.Get(), frame.Bytes(), transfer),
          "Create frame transfer");
    DlssNr::MultiGpu::CrossAdapterHandles handles;
    Check(DlssNr::MultiGpu::CrossAdapterHandles::Create(transfer, handles), "Export frame handles");
    NrFixture::ModelSession session;
    session.Initialize(primary.Get(), core, forwarder, model, parentData);
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
    wchar_t executable[32768] {};
    DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!length || length >= std::size(executable))
    {
        DeleteProcThreadAttributeList(attributes);
        throw std::runtime_error("Get executable failed");
    }
    auto quote = [](const wchar_t* value) { return L"\"" + std::wstring(value) + L"\""; };
    std::wstring command = quote(executable) + L" --worker " +
                           std::to_wstring(reinterpret_cast<uintptr_t>(readPipe.value)) + L" " + quote(core) + L" " +
                           quote(forwarder) + L" " + quote(model) + L" " + quote(workerData) + L" " + quote(log);
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION child {};
    const BOOL created = CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
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
        wire.bytes = frame.Bytes();
        wire.colorBytes = frame.color.bytes;
        wire.depthBytes = frame.depth.bytes;
        wire.motionBytes = frame.motion.bytes;
        wire.colorPitch = frame.color.footprint.Footprint.RowPitch;
        wire.depthPitch = frame.depth.footprint.Footprint.RowPitch;
        wire.motionPitch = frame.motion.footprint.Footprint.RowPitch;
        wire.primary = primaryLuid;
        wire.secondary = secondary.device->GetAdapterLuid();
        HANDLE remoteHeap = nullptr, remoteReady = nullptr, remoteDone = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), handles.Heap(), process.value, &remoteHeap, 0, FALSE,
                             DUPLICATE_SAME_ACCESS) ||
            !DuplicateHandle(GetCurrentProcess(), handles.Ready(), process.value, &remoteReady, 0, FALSE,
                             DUPLICATE_SAME_ACCESS) ||
            !DuplicateHandle(GetCurrentProcess(), handles.Done(), process.value, &remoteDone, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
            throw std::runtime_error("Duplicate handles failed");
        wire.heap = reinterpret_cast<uintptr_t>(remoteHeap);
        wire.ready = reinterpret_cast<uintptr_t>(remoteReady);
        wire.done = reinterpret_cast<uintptr_t>(remoteDone);
        DWORD written = 0;
        if (!WriteFile(writePipe.value, &wire, sizeof(wire), &written, nullptr) || written != sizeof(wire))
            throw std::runtime_error("Write handshake failed");
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1))
            throw std::runtime_error("Resume worker failed");
        uint64_t previousChecksum = 0;
        for (uint64_t frameNumber = 1; frameNumber <= 2; ++frameNumber)
        {
            if (frameNumber > 1)
                Fill(frame.color, 4);
            ComPtr<ID3D12CommandAllocator> allocator;
            auto commands = Commands(primary.Get(), allocator);
            if (frameNumber > 1)
            {
                NrFixture::Barrier(commands.Get(), frame.color.texture.Get(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                for (auto* input : { &frame.depth, &frame.motion, &frame.output })
                    NrFixture::Barrier(commands.Get(), input->texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                       D3D12_RESOURCE_STATE_COPY_DEST);
            }
            CopyUpload(commands.Get(), frame.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            CopyUpload(commands.Get(), frame.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            CopyUpload(commands.Get(), frame.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            CopyUpload(commands.Get(), frame.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Evaluate(session, commands.Get(), frame, frameNumber == 1);
            NrFixture::Barrier(commands.Get(), frame.output.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
            NrFixture::Barrier(commands.Get(), frame.depth.texture.Get(),
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            NrFixture::Barrier(commands.Get(), frame.motion.texture.Get(),
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(commands.Get(), transfer.PrimaryBuffer(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            CopyToShared(commands.Get(), transfer.PrimaryBuffer(), frame.output, 0);
            CopyToShared(commands.Get(), transfer.PrimaryBuffer(), frame.depth, frame.color.bytes);
            CopyToShared(commands.Get(), transfer.PrimaryBuffer(), frame.motion, frame.color.bytes + frame.depth.bytes);
            Transition(commands.Get(), transfer.PrimaryBuffer(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_COMMON);
            Submit(session.Queue(), commands.Get());
            submitted = true;
            if (FAILED(session.Queue()->Signal(transfer.PrimaryReady(), frameNumber)))
                ExitProcess(3);
            Retire(primary.Get(), session.Queue());
            submitted = false;
            std::cout << "Primary neural pass retired and frame sent to secondary.\n" << std::flush;
            OwnedHandle doneEvent { CreateEventW(nullptr, FALSE, FALSE, nullptr) };
            if (!doneEvent.value)
                throw std::runtime_error("Create return event failed");
            Check(transfer.PrimaryDone()->SetEventOnCompletion(frameNumber, doneEvent.value), "Return event");
            HANDLE waits[] = { doneEvent.value, process.value };
            DWORD result = WaitForMultipleObjects(2, waits, FALSE, 60000);
            uint64_t returned = transfer.PrimaryDone()->GetCompletedValue();
            if (returned == (std::numeric_limits<uint64_t>::max)() ||
                (result != WAIT_OBJECT_0 && returned < frameNumber))
                throw std::runtime_error("Worker exited or timed out before returning its frame; see worker log");
            auto readback =
                Buffer(primary.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, frame.color.bytes);
            NrFixture::Surface returnedColor;
            if (!NrFixture::MakeSurface(primary.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, false, returnedColor))
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
                for (unsigned x = 0; x < 256; ++x)
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
        if (WaitForSingleObject(process.value, 30000) != WAIT_OBJECT_0)
            throw std::runtime_error("Worker did not exit");
        DWORD exitCode = 1;
        if (!GetExitCodeProcess(process.value, &exitCode) || exitCode != 0)
            throw std::runtime_error("Worker failed; see worker log");
        completed = true;
        std::cout << "Both persistent neural features evaluated two sequential frames; worker exited cleanly.\n"
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
        if (argc == 8 && std::wcscmp(argv[1], L"--worker") == 0)
            return Worker(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[2], nullptr, 10))), argv[3],
                          argv[4], argv[5], argv[6], argv[7]);
        if (argc == 7)
            return Parent(argv[1], argv[2], argv[3], argv[4], argv[5], argv[6]);
        std::cerr << "Expected core, forwarder, model, primary data, worker data, worker log.\n";
        return 2;
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
