#pragma once

#include "../DlssNr_CrossAdapterIpc.h"
#include "../DlssNr_WorkerFenceWait.h"
#include "WorkerProtocol.h"
#include <windows.h>
#include <dxgi.h>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace DlssNr::Worker
{
// Starts exactly one child with a private inherited pipe and explicitly duplicated GPU handles.
// The owner must retire all of its submitted GPU work before destroying the client or shared
// resources. A worker failure never queues a GPU wait on its fence. Calls are externally serialized.
class WorkerClient
{
    struct Handle
    {
        HANDLE value = nullptr;
        Handle() = default;
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        ~Handle() { Reset(); }
        void Reset(HANDLE next = nullptr)
        {
            if (value && value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
            value = next;
        }
    };
    struct Attributes
    {
        std::vector<uint8_t> storage;
        PPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
        ~Attributes()
        {
            if (value)
                DeleteProcThreadAttributeList(value);
        }
    };
    enum class State
    {
        Empty,
        Initializing,
        Idle,
        Pending,
        Returned,
        Failed,
        Stopped
    };
    Handle process_, input_, initialized_;
    Microsoft::WRL::ComPtr<ID3D12Device> primary_;
    MultiGpu::CrossAdapterBuffer transfer_;
    State state_ = State::Empty;
    uint64_t nextFrame_ = 1;

    static std::wstring Quote(const wchar_t* argument)
    {
        std::wstring result = L"\"";
        size_t slashes = 0;
        for (const wchar_t* p = argument; *p; ++p)
        {
            if (*p == L'\\')
            {
                ++slashes;
                continue;
            }
            if (*p == L'\"')
            {
                result.append(slashes * 2 + 1, L'\\');
                result += L'\"';
            }
            else
            {
                result.append(slashes, L'\\');
                result += *p;
            }
            slashes = 0;
        }
        result.append(slashes * 2, L'\\');
        result += L'\"';
        return result;
    }
    bool Owns(ID3D12DeviceChild* child) const
    {
        if (!primary_ || !child)
            return false;
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        Microsoft::WRL::ComPtr<IUnknown> a, b;
        return SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&owner))) && owner && SUCCEEDED(primary_.As(&a)) &&
               SUCCEEDED(owner.As(&b)) && a.Get() == b.Get();
    }
    static HRESULT LastErrorResult()
    {
        const DWORD error = GetLastError();
        return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }
    static HRESULT WriteExact(HANDLE pipe, const void* data, DWORD bytes)
    {
        DWORD sent = 0;
        while (sent < bytes)
        {
            DWORD part = 0;
            if (!WriteFile(pipe, static_cast<const uint8_t*>(data) + sent, bytes - sent, &part, nullptr))
                return LastErrorResult();
            if (!part)
                return E_FAIL;
            sent += part;
        }
        return S_OK;
    }
    void AbortChild()
    {
        input_.Reset();
        initialized_.Reset();
        if (process_.value && WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT)
        {
            TerminateProcess(process_.value, 1);
            WaitForSingleObject(process_.value, 30000);
        }
        process_.Reset();
    }

  public:
    WorkerClient() = default;
    WorkerClient(const WorkerClient&) = delete;
    WorkerClient& operator=(const WorkerClient&) = delete;
    ~WorkerClient() { AbortChild(); }

    // All paths must refer to local, absolute paths. `wire` describes one resource generation;
    // this method overwrites its handle fields with handles valid only in the new child.
    HRESULT Start(ID3D12Device* primary, const wchar_t* executable, const wchar_t* core, const wchar_t* forwarder,
                  const wchar_t* model, const wchar_t* data, const wchar_t* log,
                  const MultiGpu::CrossAdapterBuffer& transfer, InitialFrameWire wire)
    {
        if (state_ != State::Empty || !primary || !executable || !*executable || !core || !*core || !forwarder ||
            !*forwarder || !model || !*model || !data || !*data || !log || !*log || !transfer.PrimaryBuffer() ||
            !transfer.SecondaryBuffer() || !transfer.PrimaryReady() || !transfer.PrimaryDone() ||
            wire.bytes != transfer.Size() || wire.magic != ProtocolMagic || wire.version != ProtocolVersion ||
            wire.size != sizeof(wire) || wire.frameCount != 0 || !wire.passCount || wire.passCount > MaxWorkerPasses)
            return E_INVALIDARG;
        const LUID actual = primary->GetAdapterLuid();
        if (actual.HighPart != wire.primary.HighPart || actual.LowPart != wire.primary.LowPart)
            return E_INVALIDARG;
        primary_ = primary;
        if (!Owns(transfer.PrimaryBuffer()) || !Owns(transfer.PrimaryReady()) || !Owns(transfer.PrimaryDone()))
            return E_INVALIDARG;
        Microsoft::WRL::ComPtr<ID3D12Device> secondary;
        if (FAILED(transfer.SecondaryBuffer()->GetDevice(IID_PPV_ARGS(&secondary))) || !secondary)
            return E_INVALIDARG;
        const LUID other = secondary->GetAdapterLuid();
        if ((other.HighPart == actual.HighPart && other.LowPart == actual.LowPart) ||
            other.HighPart != wire.secondary.HighPart || other.LowPart != wire.secondary.LowPart)
            return E_INVALIDARG;
        MultiGpu::CrossAdapterHandles handles;
        const HRESULT exported = MultiGpu::CrossAdapterHandles::Create(transfer, handles);
        if (FAILED(exported))
            return exported;
        transfer_ = transfer;
        Handle readPipe, writePipe, thread;
        try
        {
            SECURITY_ATTRIBUTES security { sizeof(security), nullptr, TRUE };
            if (!CreatePipe(&readPipe.value, &writePipe.value, &security, 0) ||
                !SetHandleInformation(writePipe.value, HANDLE_FLAG_INHERIT, 0))
                return LastErrorResult();
            SIZE_T size = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            if (!size)
                return LastErrorResult();
            Attributes attributes;
            attributes.storage.resize(size);
            auto* list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.storage.data());
            if (!InitializeProcThreadAttributeList(list, 1, 0, &size))
                return LastErrorResult();
            attributes.value = list;
            HANDLE inherited[] = { readPipe.value };
            if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                           nullptr, nullptr))
                return LastErrorResult();
            std::wstring command = Quote(executable) + L" --worker " +
                                   std::to_wstring(reinterpret_cast<uintptr_t>(readPipe.value)) + L" " + Quote(core) +
                                   L" " + Quote(forwarder) + L" " + Quote(model) + L" " + Quote(data) + L" " +
                                   Quote(log);
            STARTUPINFOEXW startup {};
            startup.StartupInfo.cb = sizeof(startup);
            startup.lpAttributeList = list;
            PROCESS_INFORMATION child {};
            if (!CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
                                EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, data,
                                &startup.StartupInfo, &child))
                return LastErrorResult();
            process_.value = child.hProcess;
            thread.value = child.hThread;
            readPipe.Reset();
            initialized_.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!initialized_.value)
            {
                HRESULT error = LastErrorResult();
                AbortChild();
                state_ = State::Failed;
                return error;
            }
            HANDLE remoteHeap = nullptr, remoteReady = nullptr, remoteDone = nullptr, remoteParent = nullptr,
                   remoteInitialized = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), handles.Heap(), process_.value, &remoteHeap, 0, FALSE,
                                 DUPLICATE_SAME_ACCESS) ||
                !DuplicateHandle(GetCurrentProcess(), handles.Ready(), process_.value, &remoteReady, 0, FALSE,
                                 DUPLICATE_SAME_ACCESS) ||
                !DuplicateHandle(GetCurrentProcess(), handles.Done(), process_.value, &remoteDone, 0, FALSE,
                                 DUPLICATE_SAME_ACCESS) ||
                !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), process_.value, &remoteParent, SYNCHRONIZE,
                                 FALSE, 0) ||
                !DuplicateHandle(GetCurrentProcess(), initialized_.value, process_.value, &remoteInitialized,
                                 EVENT_MODIFY_STATE, FALSE, 0))
            {
                HRESULT error = LastErrorResult();
                AbortChild();
                state_ = State::Failed;
                return error;
            }
            wire.heap = reinterpret_cast<uintptr_t>(remoteHeap);
            wire.ready = reinterpret_cast<uintptr_t>(remoteReady);
            wire.done = reinterpret_cast<uintptr_t>(remoteDone);
            wire.parent = reinterpret_cast<uintptr_t>(remoteParent);
            wire.initialized = reinterpret_cast<uintptr_t>(remoteInitialized);
            HRESULT hr = WriteExact(writePipe.value, &wire, static_cast<DWORD>(sizeof(wire)));
            if (FAILED(hr))
            {
                AbortChild();
                state_ = State::Failed;
                return hr;
            }
            if (ResumeThread(thread.value) == static_cast<DWORD>(-1))
            {
                HRESULT error = LastErrorResult();
                AbortChild();
                state_ = State::Failed;
                return error;
            }
            input_.value = writePipe.value;
            writePipe.value = nullptr;
            state_ = State::Initializing;
            return S_OK;
        }
        catch (const std::bad_alloc&)
        {
            AbortChild();
            state_ = State::Failed;
            return E_OUTOFMEMORY;
        }
    }
    // Use timeout 0 in a render path to poll without waiting for model loading.
    HRESULT WaitForReady(DWORD timeoutMs)
    {
        if (state_ == State::Idle)
            return S_OK;
        if (state_ != State::Initializing || timeoutMs == INFINITE)
            return E_INVALIDARG;
        HANDLE events[] = { process_.value, initialized_.value };
        const DWORD result = WaitForMultipleObjects(2, events, FALSE, timeoutMs);
        if (result == WAIT_TIMEOUT)
            return E_PENDING;
        if (result == WAIT_OBJECT_0 + 1)
        {
            state_ = State::Idle;
            initialized_.Reset();
            return S_OK;
        }
        state_ = State::Failed;
        return result == WAIT_OBJECT_0 ? E_FAIL : LastErrorResult();
    }
    HRESULT SendFrame(const FrameCommand& frame)
    {
        if (state_ != State::Idle || frame.size != sizeof(frame) || frame.kind != CommandKind::Frame ||
            frame.frameNumber != nextFrame_ || nextFrame_ == (std::numeric_limits<uint64_t>::max)())
            return E_INVALIDARG;
        const HRESULT hr = WriteExact(input_.value, &frame, static_cast<DWORD>(sizeof(frame)));
        if (FAILED(hr))
        {
            state_ = State::Failed;
            return hr;
        }
        state_ = State::Pending;
        return S_OK;
    }
    MultiGpu::WorkerFenceWaitResult WaitForReturn(uint64_t frame, DWORD timeoutMs)
    {
        if (state_ != State::Pending || frame != nextFrame_)
            return MultiGpu::WorkerFenceWaitResult::Failed;
        const auto result = MultiGpu::WaitForFenceOrPeer(transfer_.PrimaryDone(), frame, process_.value, timeoutMs);
        state_ = result == MultiGpu::WorkerFenceWaitResult::Ready ? State::Returned : State::Failed;
        return result;
    }
    // Confirm that the primary GPU has finished consuming this frame before permitting overwrite.
    HRESULT AcknowledgeReturn(uint64_t frame, ID3D12Fence* primaryRetirement, uint64_t value)
    {
        if (state_ != State::Returned || frame != nextFrame_ || !Owns(primaryRetirement) || !value)
            return E_INVALIDARG;
        const uint64_t completed = primaryRetirement->GetCompletedValue();
        if (completed == (std::numeric_limits<uint64_t>::max)())
        {
            state_ = State::Failed;
            return DXGI_ERROR_DEVICE_REMOVED;
        }
        if (completed < value)
            return E_PENDING;
        ++nextFrame_;
        state_ = State::Idle;
        return S_OK;
    }
    HRESULT Stop(DWORD timeoutMs)
    {
        if (state_ != State::Idle || timeoutMs == INFINITE)
            return E_INVALIDARG;
        FrameCommand shutdown {};
        shutdown.kind = CommandKind::Shutdown;
        const HRESULT hr = WriteExact(input_.value, &shutdown, static_cast<DWORD>(sizeof(shutdown)));
        if (FAILED(hr))
        {
            state_ = State::Failed;
            return hr;
        }
        input_.Reset();
        const DWORD waited = WaitForSingleObject(process_.value, timeoutMs);
        if (waited != WAIT_OBJECT_0)
        {
            state_ = State::Failed;
            return waited == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) : LastErrorResult();
        }
        DWORD exitCode = 1;
        if (!GetExitCodeProcess(process_.value, &exitCode) || exitCode != 0)
        {
            state_ = State::Failed;
            return E_FAIL;
        }
        process_.Reset();
        state_ = State::Stopped;
        return S_OK;
    }
    bool UsesTransfer(const MultiGpu::CrossAdapterBuffer& transfer) const
    {
        return transfer_.PrimaryBuffer() && transfer_.PrimaryBuffer() == transfer.PrimaryBuffer() &&
               transfer_.PrimaryReady() == transfer.PrimaryReady() && transfer_.PrimaryDone() == transfer.PrimaryDone();
    }
    HANDLE ProcessHandle() const { return process_.value; }
};
} // namespace DlssNr::Worker
