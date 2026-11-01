#include "../OptiScaler/dlssnr/DlssNr_CrossAdapterProtocol.h"
#include <cassert>
#include <vector>

struct FakeOperations
{
    unsigned fail = 0;
    unsigned calls = 0;
    std::vector<char> events;
    std::vector<uint64_t> tickets;
    void SubmitPrimary() { events.push_back('A'); }
    HRESULT SignalPrimary(uint64_t value)
    {
        events.push_back('B');
        return Step(value);
    }
    HRESULT WaitSecondary(uint64_t value)
    {
        events.push_back('C');
        return Step(value);
    }
    void SubmitSecondary() { events.push_back('D'); }
    HRESULT SignalSecondary(uint64_t value)
    {
        events.push_back('E');
        return Step(value);
    }
    HRESULT WaitPrimary(uint64_t value)
    {
        events.push_back('F');
        return Step(value);
    }
    void SubmitPrimaryAfter() { events.push_back('G'); }
    HRESULT Step(uint64_t value)
    {
        tickets.push_back(value);
        return ++calls == fail ? E_FAIL : S_OK;
    }
};

int main()
{
    using namespace DlssNr::MultiGpu;
    CrossAdapterProtocol ok;
    FakeOperations sequence;
    auto first = ok.Submit(sequence);
    assert(first.result == S_OK && first.ticket == 1 && first.phase == SubmissionPhase::Complete);
    assert((sequence.events == std::vector<char> { 'A', 'B', 'C', 'D', 'E', 'F', 'G' }));
    assert((sequence.tickets == std::vector<uint64_t> { 1, 1, 1, 1 }));
    sequence.events.clear();
    sequence.tickets.clear();
    auto second = ok.Submit(sequence);
    assert(second.result == S_OK && second.ticket == 2 && second.phase == SubmissionPhase::Complete);
    assert((sequence.tickets == std::vector<uint64_t> { 2, 2, 2, 2 }));
    assert(!ok.Poisoned());
    const SubmissionPhase phases[] = { SubmissionPhase::PrimarySubmitted, SubmissionPhase::OutboundSignalled,
                                       SubmissionPhase::SecondarySubmitted, SubmissionPhase::ReturnSignalled };
    const size_t lengths[] = { 2, 3, 5, 6 };
    for (unsigned i = 0; i < 4; ++i)
    {
        CrossAdapterProtocol protocol;
        FakeOperations fail;
        fail.fail = i + 1;
        const auto result = protocol.Submit(fail);
        assert(result.result == E_FAIL && result.ticket == 1 && result.phase == phases[i]);
        assert(fail.events.size() == lengths[i] && protocol.Poisoned());
        const auto before = fail.events;
        const auto refused = protocol.Submit(fail);
        assert(refused.result == E_ABORT && refused.ticket == 0 && refused.phase == SubmissionPhase::Validate);
        assert(fail.events == before);
    }
}
