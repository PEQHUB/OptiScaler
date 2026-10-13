// CPU regression tests for the lifecycle policies used by the graphics collectors.
#include "../OptiScaler/gpu_time/FgTiming.h"
#include "../OptiScaler/gpu_time/FgTimingLifecycle.h"

#include <cstdlib>
#include <iostream>

static void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

static void TestQueueMigration()
{
    FgTiming::SignalTimeline<uint64_t> timeline(7);
    Check(timeline.Observe(1, 8, true), "First GPU signal after an initial counter must be accepted");
    Check(timeline.Observe(1, 9, true), "One queue can advance its timeline without a host wait");
    Check(timeline.NeedsRetirement(2), "A different signaling queue requires retirement evidence");
    Check(timeline.Observe(2, 10, true, 9), "A completed GPU timeline must support queue migration");
    Check(timeline.Matches(2, 10), "The migrated queue must own the new completion token");
    Check(!timeline.Matches(1, 9), "Old queue metadata must not be selected for the new submission");
    Check(timeline.Observe(2, 11, true), "Timing must keep working after migration");
    Check(timeline.Observe(1, 12, true, 11), "A second drained migration must also work");

    FgTiming::SignalTimeline<uint64_t> pending;
    Check(pending.Observe(1, 3, true), "A pending GPU signal must be tracked");
    Check(!pending.Observe(2, 4, true, 2), "Another queue must not complete unfinished work by advancing the counter");
    Check(!pending.Matches(1, 3) && !pending.Trusted(), "Unsafe cross-queue signals invalidate pending tokens");

    FgTiming::SignalTimeline<uint64_t> unavailable;
    unavailable.Observe(1, 1, true);
    Check(!unavailable.Observe(2, 2, true), "Missing/failed counter reads must not authorize migration");
}

static void TestUntrustedSignals()
{
    FgTiming::SignalTimeline<uint64_t> host;
    host.Observe(1, 1, true);
    host.Invalidate();
    Check(!host.Matches(1, 1), "A host/unobserved signal must invalidate an existing GPU token");
    Check(!host.Observe(1, 2, true, 1), "A later GPU signal cannot repair lost host-signal provenance");

    FgTiming::SignalTimeline<uint64_t> early;
    early.Observe(1, 1, true);
    Check(!early.Observe(1, 2, false), "An early-stage signal must not certify the end timestamp");
    Check(!early.Matches(1, 1), "An early-stage advance must also invalidate older pending tokens");

    FgTiming::SignalTimeline<uint64_t> duplicate;
    duplicate.Observe(1, 2, true);
    Check(!duplicate.Observe(1, 2, true), "A duplicate signal value must be rejected");
    Check(!duplicate.Trusted(), "Nonmonotonic signals make completion attribution unavailable");

    FgTiming::SignalTimeline<uint64_t> recreated(20);
    Check(recreated.Observe(2, 21, true), "A newly created semaphore starts a distinct trusted lifecycle");
    Check(!recreated.Matches(1, 2), "A recreated semaphore must not inherit an older object's token");
}

static void TestCompletedOrdering()
{
    auto channel = std::make_shared<FgTiming::Channel>("test", "selected workload");
    auto other = std::make_shared<FgTiming::Channel>("other", "selected workload");
    const auto epoch = channel->Epoch();
    {
        FgTiming::CompletedSamples results;
        results.Add(channel, epoch, 2, 20.0, 3);
        results.Add(other, other->Epoch(), 1, 7.0);
        results.Add(channel, epoch, 1, 10.0, 1);
        results.Add(channel, epoch, 2, 99.0, 3);
        Check(!channel->Read().milliseconds, "Results must not publish while the collector still holds its lock");
    }
    const auto snapshot = channel->Read();
    Check(snapshot.sampleCount == 2 && snapshot.averageMilliseconds == 15.0 && snapshot.milliseconds == 20.0,
          "Reverse query/hash order must preserve both completed samples and their correct average");
    Check(snapshot.frameId == 2 && snapshot.generatedFrames == 3, "The newest sample supplies the displayed metadata");
    Check(other->Read().sampleCount == 1 && other->Read().milliseconds == 7.0,
          "Ordering a batch must not mix the histories of different contexts");

    {
        FgTiming::CompletedSamples results;
        results.Add(channel, epoch, 1, 100.0);
    }
    Check(channel->Read().sampleCount == 2, "Late results from an earlier batch must not replay old history");
}

static void TestRetirementAndEpochs()
{
    auto channel = std::make_shared<FgTiming::Channel>("test", "selected workload", FgTiming::Coverage::Partial);
    const auto retiredEpoch = channel->Epoch();
    {
        FgTiming::CompletedSamples results;
        results.Add(channel, retiredEpoch, 4, 10.0);
        channel->Reset(); // Context/mode retirement happens before the collector releases its lock.
        results.Add(channel, channel->Epoch(), 1, 2.0);
    }
    auto snapshot = channel->Read();
    Check(snapshot.sampleCount == 1 && snapshot.milliseconds == 2.0 && snapshot.frameId == 1,
          "A retired context must not repopulate the counter when delayed readback is published");
    Check(snapshot.coverage == FgTiming::Coverage::Partial, "Coverage metadata must survive a context reset");

    std::weak_ptr<FgTiming::Channel> retiredChannel = channel;
    {
        FgTiming::CompletedSamples results;
        results.Add(channel, channel->Epoch(), 2, 3.0);
        channel.reset();
        Check(!retiredChannel.expired(), "Completed CPU results retain their channel until publication finishes");
    }
    Check(retiredChannel.expired(), "Publication must release a retired channel instead of retaining it forever");

    FgTiming::Channel full("full", "wording includes (partial), coverage is explicit");
    Check(full.Read().coverage == FgTiming::Coverage::Full,
          "Tooltip wording must not control whether the UI marks a measurement partial");
}

int main()
{
    TestQueueMigration();
    TestUntrustedSignals();
    TestCompletedOrdering();
    TestRetirementAndEpochs();
    std::cout << "FG collector queue migration, completion ordering and retirement tests passed\n";
}
