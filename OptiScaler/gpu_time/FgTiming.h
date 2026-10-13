#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace FgTiming
{
using Clock = std::chrono::steady_clock;

enum class Coverage
{
    Full,
    Partial
};

struct Snapshot
{
    std::string backend;
    std::string scope;
    Coverage coverage = Coverage::Full;
    std::optional<double> milliseconds;
    std::optional<double> averageMilliseconds;
    uint64_t frameId = 0;
    uint32_t generatedFrames = 0;
    size_t sampleCount = 0;
    double ageMilliseconds = 0.0;
};

// A channel belongs to one FG context. Recording captures its epoch so delayed GPU results cannot
// repopulate the counter after a mode change, pause or context recreation.
class Channel
{
    mutable std::mutex _mutex;
    std::string _backend;
    std::string _scope;
    Coverage _coverage;
    uint64_t _epoch = 1;
    uint64_t _lastFrameId = 0;
    uint32_t _generatedFrames = 0;
    std::deque<double> _samples;
    Clock::time_point _updated {};

  public:
    static constexpr size_t HistorySize = 64;
    static constexpr auto MaxSampleAge = std::chrono::seconds(1);

    Channel(std::string backend, std::string scope, Coverage coverage = Coverage::Full)
        : _backend(std::move(backend)), _scope(std::move(scope)), _coverage(coverage)
    {
    }

    uint64_t Epoch() const
    {
        std::lock_guard lock(_mutex);
        return _epoch;
    }

    void Reset()
    {
        std::lock_guard lock(_mutex);
        ++_epoch;
        _lastFrameId = 0;
        _generatedFrames = 0;
        _samples.clear();
        _updated = {};
    }

    bool Publish(uint64_t epoch, uint64_t frameId, double milliseconds, uint32_t generatedFrames = 0,
                 Clock::time_point now = Clock::now())
    {
        if (frameId == 0 || !std::isfinite(milliseconds) || milliseconds < 0.0)
            return false;

        std::lock_guard lock(_mutex);
        if (epoch != _epoch || (!_samples.empty() && frameId <= _lastFrameId))
            return false;

        _lastFrameId = frameId;
        _generatedFrames = generatedFrames;
        _updated = now;
        _samples.push_back(milliseconds);
        if (_samples.size() > HistorySize)
            _samples.pop_front();
        return true;
    }

    Snapshot Read(Clock::time_point now = Clock::now()) const
    {
        std::lock_guard lock(_mutex);
        Snapshot result;
        result.backend = _backend;
        result.scope = _scope;
        result.coverage = _coverage;
        result.frameId = _lastFrameId;
        result.generatedFrames = _generatedFrames;
        result.sampleCount = _samples.size();

        if (_samples.empty())
            return result;

        const auto age = now - _updated;
        result.ageMilliseconds = std::chrono::duration<double, std::milli>(age).count();
        if (age > MaxSampleAge)
            return result;

        result.milliseconds = _samples.back();
        double sum = 0.0;
        for (auto sample : _samples)
            sum += sample;
        result.averageMilliseconds = sum / static_cast<double>(_samples.size());
        return result;
    }
};

// Declare a batch before collector locks. Its destructor publishes after those locks are released,
// in frame order regardless of query-slot or hash-table order. The channel still rejects old epochs,
// duplicates and results older than a sample published by an earlier batch.
class CompletedSamples
{
    struct Result
    {
        std::shared_ptr<Channel> channel;
        uint64_t epoch;
        uint64_t frameId;
        double milliseconds;
        uint32_t generatedFrames;
    };
    std::vector<Result> _results;

  public:
    CompletedSamples() = default;
    CompletedSamples(const CompletedSamples&) = delete;
    CompletedSamples& operator=(const CompletedSamples&) = delete;

    void Add(std::shared_ptr<Channel> channel, uint64_t epoch, uint64_t frameId, double milliseconds,
             uint32_t generatedFrames = 0)
    {
        if (channel)
            _results.push_back({ std::move(channel), epoch, frameId, milliseconds, generatedFrames });
    }

    ~CompletedSamples()
    {
        std::stable_sort(_results.begin(), _results.end(),
                         [](const Result& a, const Result& b) { return a.frameId < b.frameId; });
        for (auto& result : _results)
            result.channel->Publish(result.epoch, result.frameId, result.milliseconds, result.generatedFrames);
    }
};

inline std::optional<double> VulkanMilliseconds(uint64_t start, uint64_t end, uint32_t validBits,
                                                double timestampPeriod)
{
    if (validBits == 0 || validBits > 64 || !std::isfinite(timestampPeriod) || timestampPeriod <= 0.0)
        return std::nullopt;

    const uint64_t mask = validBits == 64 ? UINT64_MAX : (uint64_t { 1 } << validBits) - 1;
    return static_cast<double>((end - start) & mask) * timestampPeriod / 1e6;
}
} // namespace FgTiming
