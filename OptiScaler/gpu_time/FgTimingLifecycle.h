#pragma once

#include <cstdint>
#include <optional>

namespace FgTiming
{
// A GPU-only timeline has one observed signaling queue until its last signal completes. Changing
// queue after that retirement is safe; allowing an unrelated higher signal while work is pending
// would falsely complete old samples. Host, early-stage and unobserved signals invalidate the proof.
template <typename Queue> class SignalTimeline
{
    Queue _queue {};
    uint64_t _lastSignal = 0;
    bool _trusted = true;

  public:
    explicit SignalTimeline(uint64_t initialValue = 0) : _lastSignal(initialValue) {}

    bool Trusted() const { return _trusted; }
    void Invalidate() { _trusted = false; }

    bool NeedsRetirement(Queue queue) const { return _trusted && _queue != Queue {} && _queue != queue; }

    bool Observe(Queue queue, uint64_t value, bool allCommands, std::optional<uint64_t> completed = std::nullopt)
    {
        if (!_trusted)
            return false;
        if (queue == Queue {} || !allCommands || value <= _lastSignal ||
            (NeedsRetirement(queue) && (!completed || *completed < _lastSignal)))
        {
            Invalidate();
            return false;
        }
        _queue = queue;
        _lastSignal = value;
        return true;
    }

    bool Matches(Queue queue, uint64_t value) const { return _trusted && _queue == queue && _lastSignal == value; }
};
} // namespace FgTiming
