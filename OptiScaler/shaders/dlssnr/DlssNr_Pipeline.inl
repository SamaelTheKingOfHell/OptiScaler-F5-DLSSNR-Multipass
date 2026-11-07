// Included inside namespace DlssNr after the finished-picture colour helpers.
namespace Late
{
Slot* CompletePipeline()
{
    if (g_dispatchQuarantined) return nullptr;
    auto* slot = pendingPipeline;
    if (!slot) return nullptr;
    auto& task = slot->workerContext.deferredTask;
    if (task && task->Pending()) task->Resume();
    if (!task || task->Pending() || task->Failed() || !slot->finishPipeline || !slot->finishPipeline())
    {
        slot->quarantined = true;
        g_dispatchQuarantined = true;
        Say("The previous pipelined picture could not complete. Restart the game to retry.");
        return nullptr;
    }
    pendingPipeline = nullptr;
    slot->finishPipeline = {};
    return slot;
}

void PresentCompleted(Slot* source, ID3D12Resource* target, ID3D12CommandQueue* queue,
                      DXGI_COLOR_SPACE_TYPE colorSpace)
{
    Late::presentedPrevious = false;
    if (!source || !source->pipelinePicture || !target || !queue || source->quarantined) return;
    const auto from = source->pipelinePicture->GetDesc(), to = target->GetDesc();
    if (from.Width != to.Width || from.Height != to.Height || from.Format != to.Format ||
        source->pipelineColorSpace != colorSpace)
    { RequestReset(); return; }
    auto* realQueue = RealQueue(queue);
    if (!presenter)
    {
        presenter = std::make_unique<Worker::FinishedPicturePresenter>();
        if (FAILED(presenter->Initialize(device.Get(), realQueue)))
        { presenter.reset(); Say("Could not initialize pipelined presentation."); return; }
    }
    if (!presenter->UsesQueue(realQueue)) return;
    const auto result = presenter->Submit(source->pipelinePicture.Get(), target);
    if (SUCCEEDED(result.result))
    {
        source->displayFence = result.fence;
        source->displayValue = result.value;
        Late::presentedPrevious = Late::presentingPrevious;
    }
    else if (result.result != E_PENDING)
    {
        source->quarantined = true;
        g_dispatchQuarantined = true;
        Say("The completed pipelined picture could not be displayed.");
    }
}

bool DrainPipeline()
{
    if (g_dispatchQuarantined) return false;
    if (pendingPipeline && !CompletePipeline()) return false;
    const auto started = GetTickCount64();
    for (auto& slot : slots)
    {
        if (slot.quarantined) return false;
        if (!slot.workerContext.pipelineRequested || !slot.ownedChain || slot.done <= slot.ready) continue;
        while (!Finished(slot))
        {
            if (slot.fence->GetCompletedValue() == UINT64_MAX || GetTickCount64() - started >= 5000) return false;
            Sleep(1);
        }
    }
    return !presenter || SUCCEEDED(presenter->Drain(5000));
}
} // namespace Late
