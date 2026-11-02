#pragma once

#include "WorkerModelSession.h"
#include <limits>

namespace DlssNr::Worker
{
// One frame is submitted at a time. Reuse recording storage only after the
// shared return fence proves that all its commands have finished on the GPU.
class WorkerCommandContext
{
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> completion_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> commands_;
    HANDLE event_ = nullptr;
    uint64_t retired_ = 0;

    [[noreturn]] static void Unretired()
    {
        std::cerr << "Worker submission did not retire; terminating without releasing GPU resources.\n"
                  << std::flush;
        ExitProcess(3);
    }

  public:
    WorkerCommandContext(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Fence* completion)
        : device_(device), queue_(queue), completion_(completion)
    {
        if (!device || !queue || !completion)
            throw std::runtime_error("Missing worker recording device, queue or completion fence");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)),
              "Create worker allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                                        IID_PPV_ARGS(&commands_)), "Create worker list");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_)
            throw std::runtime_error("Create worker completion event failed");
    }
    WorkerCommandContext(const WorkerCommandContext&) = delete;
    WorkerCommandContext& operator=(const WorkerCommandContext&) = delete;
    ~WorkerCommandContext() { CloseHandle(event_); }

    // The first list is open at construction; later lists reuse retired storage.
    ID3D12GraphicsCommandList* Begin()
    {
        if (retired_)
        {
            Check(allocator_->Reset(), "Reset retired worker allocator");
            Check(commands_->Reset(allocator_.Get(), nullptr), "Reset worker list");
        }
        return commands_.Get();
    }

    // The existing cross-adapter return signal also retires the worker's list.
    // Never unwind after submission unless completion has been proved: even a
    // parent exit does not make outstanding GPU references safe to release.
    void SubmitAndRetire(uint64_t value)
    {
        if (value <= retired_ || value == (std::numeric_limits<uint64_t>::max)())
            throw std::runtime_error("Invalid worker completion value");
        Check(commands_->Close(), "Close worker list");
        ID3D12CommandList* lists[] = { commands_.Get() };
        queue_->ExecuteCommandLists(1, lists);
        if (FAILED(queue_->Signal(completion_.Get(), value)) ||
            FAILED(completion_->SetEventOnCompletion(value, event_)) ||
            WaitForSingleObject(event_, 30000) != WAIT_OBJECT_0)
            Unretired();
        // Always consume the auto-reset event above, including immediate
        // completion, so it cannot wake a subsequent frame prematurely.
        const auto completed = completion_->GetCompletedValue();
        if (completed < value || completed == (std::numeric_limits<uint64_t>::max)() ||
            FAILED(device_->GetDeviceRemovedReason()))
            Unretired();
        retired_ = value;
    }
};
} // namespace DlssNr::Worker
