#include "pch.h"
#include "../../OptiScaler/dlssnr/DlssNr_CrossAdapterIpc.h"
#include "../../OptiScaler/dlssnr/DlssNr_CrossAdapterSelect.h"
#include "nr_cross_adapter_test_helpers.h"
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
    uint64_t heap = 0, ready = 0, done = 0, bytes = 0;
    LUID primary {}, secondary {};
};

static void WaitFence(ID3D12Fence* fence, uint64_t value)
{
    OwnedHandle event { CreateEventW(nullptr, FALSE, FALSE, nullptr) };
    if (!event.value)
        throw std::runtime_error("CreateEvent failed");
    Check(fence->SetEventOnCompletion(value, event.value), "Set fence event");
    if (WaitForSingleObject(event.value, 30000) != WAIT_OBJECT_0)
        throw std::runtime_error("Fence timed out");
}

// Once work is submitted, an uncertain fence cannot safely release its resources in this fixture.
static void SafeWaitFence(ID3D12Fence* fence, uint64_t value)
{
    try
    {
        WaitFence(fence, value);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << "; terminating the isolated fixture.\n" << std::flush;
        ExitProcess(3);
    }
}

static int Worker(HANDLE pipe, bool failAfterHandshake)
{
    Wire wire {};
    DWORD read = 0;
    if (!ReadFile(pipe, &wire, sizeof(wire), &read, nullptr) || read != sizeof(wire))
        return 10;
    CloseHandle(pipe);
    OwnedHandle heap { reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.heap)) };
    OwnedHandle ready { reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.ready)) };
    OwnedHandle done { reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.done)) };
    if (failAfterHandshake)
        return 42;
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
    Queue queue(device.Get());
    auto local = Buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, wire.bytes);
    auto patch = Buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 4);
    D3D12_RANGE noRead { 0, 0 };
    uint8_t* mapped = nullptr;
    Check(patch->Map(0, &noRead, reinterpret_cast<void**>(&mapped)), "Worker map");
    const uint8_t marker[] = { 0xde, 0xad, 0xbe, 0xef };
    std::memcpy(mapped, marker, sizeof(marker));
    patch->Unmap(0, nullptr);
    auto commands = queue.List(device.Get());
    Transition(commands.Get(), peer.Buffer(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->CopyBufferRegion(local.Get(), 0, peer.Buffer(), 0, wire.bytes);
    Transition(commands.Get(), local.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Transition(commands.Get(), local.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    commands->CopyBufferRegion(local.Get(), 4, patch.Get(), 0, sizeof(marker));
    Transition(commands.Get(), local.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(commands.Get(), peer.Buffer(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    commands->CopyBufferRegion(peer.Buffer(), 0, local.Get(), 0, wire.bytes);
    Transition(commands.Get(), peer.Buffer(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Check(commands->Close(), "Worker close list");
    Check(queue.queue->Wait(peer.Ready(), 1), "Worker wait outbound");
    ID3D12CommandList* lists[] = { commands.Get() };
    queue.queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue.queue->Signal(peer.Done(), 1)))
        ExitProcess(3);
    SafeWaitFence(peer.Done(), 1);
    return FAILED(device->GetDeviceRemovedReason()) ? 11 : 0;
}

static int Parent(bool failWorker)
{
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
        Check(hr, "Parent enumerate adapters");
        for (UINT j = 0;; ++j)
        {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(j, &output);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            Check(hr, "Parent enumerate outputs");
            DXGI_OUTPUT_DESC outputDesc {};
            Check(output->GetDesc(&outputDesc), "Parent output description");
            if (outputDesc.AttachedToDesktop && outputDesc.Monitor == monitor)
            {
                DXGI_ADAPTER_DESC1 desc {};
                Check(adapter->GetDesc1(&desc), "Parent adapter description");
                primaryLuid = desc.AdapterLuid;
                primaryAdapter = adapter;
            }
        }
    }
    if (!primaryAdapter)
        throw std::runtime_error("Primary display adapter missing");
    DXGI_ADAPTER_DESC1 primaryDesc {};
    Check(primaryAdapter->GetDesc1(&primaryDesc), "Primary description");
    ComPtr<ID3D12Device> primary;
    Check(D3D12CreateDevice(primaryAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&primary)), "Parent device");
    DlssNr::MultiGpu::SelectedAdapter secondary;
    Check(DlssNr::MultiGpu::SelectSecondary(factory.Get(), primaryLuid, nullptr, 0x10de, &D3D12CreateDevice, secondary),
          "Select worker adapter");
    std::wcout << L"Primary: " << primaryDesc.Description << L"; secondary: " << secondary.description.Description
               << L"\n";
    constexpr uint64_t bytes = 4096;
    DlssNr::MultiGpu::CrossAdapterBuffer transfer;
    Check(DlssNr::MultiGpu::CrossAdapterBuffer::Create(primary.Get(), secondary.device.Get(), bytes, transfer),
          "Parent create transfer");
    DlssNr::MultiGpu::CrossAdapterHandles handles;
    Check(DlssNr::MultiGpu::CrossAdapterHandles::Create(transfer, handles), "Parent export handles");
    SECURITY_ATTRIBUTES security { sizeof(security), nullptr, TRUE };
    OwnedHandle readPipe, writePipe;
    if (!CreatePipe(&readPipe.value, &writePipe.value, &security, 0) ||
        !SetHandleInformation(writePipe.value, HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("Create handshake pipe failed");
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<uint8_t> storage(attributeBytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
        throw std::runtime_error("Initialize child attributes failed");
    HANDLE inherited[] = { readPipe.value };
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                   nullptr, nullptr))
    {
        DeleteProcThreadAttributeList(attributes);
        throw std::runtime_error("Restrict inherited handles failed");
    }
    wchar_t executable[32768] {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!length || length >= std::size(executable))
    {
        DeleteProcThreadAttributeList(attributes);
        throw std::runtime_error("Get executable path failed");
    }
    std::wstring command = L"\"" + std::wstring(executable) + (failWorker ? L"\" --worker-fail " : L"\" --worker ") +
                           std::to_wstring(reinterpret_cast<uintptr_t>(readPipe.value));
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
    bool childFinished = false;
    try
    {
        Wire wire {};
        wire.bytes = bytes;
        wire.primary = primaryLuid;
        wire.secondary = secondary.device->GetAdapterLuid();
        HANDLE remoteHeap = nullptr, remoteReady = nullptr, remoteDone = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), handles.Heap(), process.value, &remoteHeap, 0, FALSE,
                             DUPLICATE_SAME_ACCESS) ||
            !DuplicateHandle(GetCurrentProcess(), handles.Ready(), process.value, &remoteReady, 0, FALSE,
                             DUPLICATE_SAME_ACCESS) ||
            !DuplicateHandle(GetCurrentProcess(), handles.Done(), process.value, &remoteDone, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
            throw std::runtime_error("Duplicate worker handles failed");
        wire.heap = reinterpret_cast<uintptr_t>(remoteHeap);
        wire.ready = reinterpret_cast<uintptr_t>(remoteReady);
        wire.done = reinterpret_cast<uintptr_t>(remoteDone);
        DWORD written = 0;
        if (!WriteFile(writePipe.value, &wire, sizeof(wire), &written, nullptr) || written != sizeof(wire))
            throw std::runtime_error("Write handshake failed");
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1))
            throw std::runtime_error("Resume worker failed");
        Queue queue(primary.Get());
        auto upload = Buffer(primary.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, bytes);
        auto readback = Buffer(primary.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, bytes);
        uint8_t* data = nullptr;
        D3D12_RANGE noRead { 0, 0 };
        Check(upload->Map(0, &noRead, reinterpret_cast<void**>(&data)), "Parent map upload");
        for (uint64_t i = 0; i < bytes; ++i)
            data[i] = static_cast<uint8_t>((i * 13u + 7u) & 255u);
        upload->Unmap(0, nullptr);
        auto outbound = queue.List(primary.Get());
        Transition(outbound.Get(), transfer.PrimaryBuffer(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        outbound->CopyBufferRegion(transfer.PrimaryBuffer(), 0, upload.Get(), 0, bytes);
        Transition(outbound.Get(), transfer.PrimaryBuffer(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_COMMON);
        queue.Submit(outbound.Get());
        if (FAILED(queue.queue->Signal(transfer.PrimaryReady(), 1)))
            ExitProcess(3);
        SafeWaitFence(transfer.PrimaryReady(), 1);
        OwnedHandle doneEvent { CreateEventW(nullptr, FALSE, FALSE, nullptr) };
        if (!doneEvent.value)
            throw std::runtime_error("Create return event failed");
        Check(transfer.PrimaryDone()->SetEventOnCompletion(1, doneEvent.value), "Parent return event");
        HANDLE waits[] = { doneEvent.value, process.value };
        const DWORD result = WaitForMultipleObjects(2, waits, FALSE, 30000);
        const auto returned = transfer.PrimaryDone()->GetCompletedValue();
        if (returned == (std::numeric_limits<uint64_t>::max)())
            throw std::runtime_error("Primary device was removed");
        if (failWorker)
        {
            DWORD exitCode = 0;
            if (result != WAIT_OBJECT_0 + 1 || !GetExitCodeProcess(process.value, &exitCode) || exitCode != 42 ||
                returned != 0)
                throw std::runtime_error("Expected worker failure was not detected");
            ComPtr<ID3D12Fence> safeFence;
            Check(primary->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&safeFence)), "Failure-path fence");
            Check(queue.queue->Signal(safeFence.Get(), 1), "Failure-path queue signal");
            WaitFence(safeFence.Get(), 1);
            childFinished = true;
            std::cout << "Worker failure detected without blocking the primary GPU queue.\n";
            return 0;
        }
        if (result != WAIT_OBJECT_0 && returned < 1)
            throw std::runtime_error("Worker exited or timed out before GPU return");
        ComPtr<ID3D12Fence> localFence;
        Check(primary->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&localFence)), "Parent completion fence");
        auto inbound = queue.List(primary.Get());
        Transition(inbound.Get(), transfer.PrimaryBuffer(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        inbound->CopyBufferRegion(readback.Get(), 0, transfer.PrimaryBuffer(), 0, bytes);
        Transition(inbound.Get(), transfer.PrimaryBuffer(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
        queue.Submit(inbound.Get());
        if (FAILED(queue.queue->Signal(localFence.Get(), 1)))
            ExitProcess(3);
        SafeWaitFence(localFence.Get(), 1);
        D3D12_RANGE readRange { 0, static_cast<SIZE_T>(bytes) };
        Check(readback->Map(0, &readRange, reinterpret_cast<void**>(&data)), "Parent map result");
        const uint8_t marker[] = { 0xde, 0xad, 0xbe, 0xef };
        for (uint64_t i = 0; i < bytes; ++i)
        {
            const uint8_t expected = i >= 4 && i < 8 ? marker[i - 4] : static_cast<uint8_t>((i * 13u + 7u) & 255u);
            if (data[i] != expected)
            {
                readback->Unmap(0, &noRead);
                throw std::runtime_error("Worker result mismatch");
            }
        }
        readback->Unmap(0, &noRead);
        if (WaitForSingleObject(process.value, 30000) != WAIT_OBJECT_0)
            throw std::runtime_error("Worker did not exit");
        DWORD exitCode = 1;
        if (!GetExitCodeProcess(process.value, &exitCode) || exitCode != 0)
            throw std::runtime_error("Worker failed");
        childFinished = true;
        std::cout << "Cross-process shared-buffer round trip passed; only the private pipe was inherited.\n";
    }
    catch (...)
    {
        if (!childFinished && WaitForSingleObject(process.value, 0) != WAIT_OBJECT_0)
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
        if (argc == 3 && (std::wcscmp(argv[1], L"--worker") == 0 || std::wcscmp(argv[1], L"--worker-fail") == 0))
            return Worker(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[2], nullptr, 10))),
                          std::wcscmp(argv[1], L"--worker-fail") == 0);
        if (argc == 2 && std::wcscmp(argv[1], L"--failure-test") == 0)
            return Parent(true);
        if (argc != 1)
            return 2;
        return Parent(false);
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
