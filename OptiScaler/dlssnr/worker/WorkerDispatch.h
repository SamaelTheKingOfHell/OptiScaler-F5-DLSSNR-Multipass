#pragma once
#include <memory>
#include <d3d12.h>

namespace DlssNr::MultiGpu
{
class OwnedCommandChain;
}
namespace DlssNr::Worker
{
class WorkerGeneration;
class DispatchTask;
// Only a finished-picture caller that owns its list may supply this context.
struct DispatchContext
{
    MultiGpu::OwnedCommandChain* chain = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    std::shared_ptr<WorkerGeneration> generation;
    bool needsRetirement = false;
    bool unusable = false;
    bool pipelineRequested = false;
    unsigned adaptivePrimaryPasses = ~0u;
    bool balanceEligible = false;
    std::shared_ptr<DispatchTask> deferredTask;
};
} // namespace DlssNr::Worker
