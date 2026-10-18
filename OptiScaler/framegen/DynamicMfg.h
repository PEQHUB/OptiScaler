#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// OptiScaler's own Dynamic Multi Frame Generation, for outputs where Streamline's DLSSGMode::eDynamic is
// unavailable. Streamline limits eDynamic to D3D12 ("support is currently limited to D3D12 only",
// ProgrammingGuideDLSS_G 6.3), so Vulkan games pick the multiplier here and send it as numFramesToGenerate.
//
// Picks the smallest multiplier whose output reaches the target frame rate. While nothing holds the game back, the
// measured rate of real frames is the truth. While a frame cap holds it at target / multiplier, that rate only shows
// the cap, so the GPU and CPU frame costs from Reflex latency reports estimate what the game could do instead. Those
// costs leave out work that isn't the game's (frame generation itself), so whenever the cap isn't binding the ratio
// between the real frame time and the reported cost is learned and applied to later estimates.
//
// More generated frames are added quickly when the game falls short, fewer only after the headroom has held for a
// while and one step at a time. A step down that has to be undone soon after doubles the wait before the next one.
//
// Pure logic without API calls so it can be tested on its own.
class DynamicMfgController
{
  public:
    enum class Reason : uint8_t
    {
        None,        // No decision yet
        Startup,     // First decision after a reset
        BelowTarget, // The game can't reach the target with the current multiplier
        Headroom,    // The game reaches the target with one generated frame fewer
        Range,       // The allowed multiplier range changed
    };

    struct Settings
    {
        double targetFps = 0.0;     // Resolved output target, must be > 0
        int minMultiplier = 2;      // 2 = 2x (1 generated frame)
        int maxMultiplier = 2;      // Up to the output's maximum, 6 = 6x
        double upHoldMs = 250.0;    // Shortfall must last this long before adding generated frames
        double downHoldMs = 1500.0; // Headroom must last this long before removing one
        double minDwellMs = 750.0;  // Between any two changes
        double downMargin = 1.08;   // Capacity must beat what one multiplier fewer needs by this factor
        double upTolerance = 0.97;  // Shortfall when capacity * multiplier < target * this
        double emaTimeMs = 250.0;   // Smoothing time constant for the estimates
    };

    struct Sample
    {
        double nowMs = 0.0;       // Monotonic time of this real frame
        double gpuRenderMs = 0.0; // GPU render time of a recent frame, 0 = unknown
        double cpuFrameMs = 0.0;  // CPU frame time (simulation start to render submit end), 0 = unknown
        bool capped = false;      // An output frame cap of targetFps is active
    };

    struct Status
    {
        int multiplier = 0; // 0 until the first decision
        int previousMultiplier = 0;
        Reason reason = Reason::None;
        double lastChangeMs = -1.0;
        double baseFps = 0.0;     // Measured real frames per second
        double capacityFps = 0.0; // Estimated uncapped real frames per second, 0 = unknown
        double gpuRenderMs = 0.0; // Smoothed costs, 0 = unknown
        double cpuFrameMs = 0.0;
        double costFactor = 0.0; // Learned real frame time / reported cost, 0 = not learned yet
        bool capBinding = false; // The cap is what holds the game back right now
        bool fromCosts = false;  // capacityFps came from the frame costs
        double downHoldMs = 0.0; // Current wait before a step down, grows after reverted steps
    };

    void Reset()
    {
        const double costFactor = _status.costFactor; // Still true for the same game, kept across pauses
        _status = {};
        _status.costFactor = costFactor;
        _lastFrameMs = -1.0;
        _baseIntervalMs = 0.0;
        _upSinceMs = -1.0;
        _downSinceMs = -1.0;
        _settleUntilMs = -1.0;
        _firstFrameMs = -1.0;
        _frames = 0;
        _backoff = 1.0;
    }

    // After a pause (menus, loading, frame generation off for a while): measure and warm up again, but keep the
    // multiplier, the learned cost factor and the back-off
    void ResetMeasurements()
    {
        _status.baseFps = 0.0;
        _status.capacityFps = 0.0;
        _status.gpuRenderMs = 0.0;
        _status.cpuFrameMs = 0.0;
        _lastFrameMs = -1.0;
        _baseIntervalMs = 0.0;
        _upSinceMs = -1.0;
        _downSinceMs = -1.0;
        _settleUntilMs = -1.0;
        _firstFrameMs = -1.0;
        _frames = 0;
    }

    const Status& GetStatus() const { return _status; }

    // Once per real (rendered) frame. Returns the multiplier to use, or 0 while there isn't enough data yet.
    int Update(const Sample& sample, const Settings& settings)
    {
        const int minMultiplier = std::max(2, settings.minMultiplier);
        const int maxMultiplier = std::max(minMultiplier, settings.maxMultiplier);
        auto& current = _status.multiplier;
        const double now = sample.nowMs;

        UpdateEstimates(sample, settings);
        _status.downHoldMs = settings.downHoldMs * _backoff;

        if (settings.targetFps <= 0.0 || _status.baseFps <= 0.0)
            return current;

        // Runs during warmup too, so the cost factor is learned before the first decision
        UpdateCapacity(sample, settings);

        // Right after a start or a pause the first frames are often slow: they're discarded (kWarmupMs), then the
        // rates need kMeasureMs of clean frames before deciding anything
        if (_frames < kWarmupFrames || now - _firstFrameMs < kWarmupMs + kMeasureMs)
            return current;

        // The smallest multiplier that reaches the target, with slack so that 2.0001x doesn't become 3x
        const double ratio = settings.targetFps / _status.capacityFps;
        const int needed = std::clamp((int) std::ceil(ratio - kRoundingSlack), minMultiplier, maxMultiplier);

        if (current == 0)
        {
            Change(needed, Reason::Startup, now);
            return current;
        }

        if (current < minMultiplier || current > maxMultiplier)
        {
            Change(std::clamp(current, minMultiplier, maxMultiplier), Reason::Range, now);
            return current;
        }

        const bool dwellOver = _status.lastChangeMs < 0.0 || now - _status.lastChangeMs >= settings.minDwellMs;

        // Short of the target: jump straight to what's needed
        const bool shortfall =
            needed > current && _status.capacityFps * current < settings.targetFps * settings.upTolerance;

        if (Held(shortfall, _upSinceMs, now, settings.upHoldMs) && dwellOver)
        {
            // Undoing a recent step down: the estimate was too hopeful, wait longer next time
            if (_status.reason == Reason::Headroom && now - _status.lastChangeMs < kRevertWindowMs)
                _backoff = std::min(_backoff * 2.0, kMaxBackoff);

            Change(needed, Reason::BelowTarget, now);
            return current;
        }

        // Headroom: one generated frame fewer still reaches the target with margin
        const bool headroom =
            needed < current && _status.capacityFps * (current - 1) >= settings.targetFps * settings.downMargin;

        if (Held(headroom, _downSinceMs, now, _status.downHoldMs) && dwellOver)
            Change(current - 1, Reason::Headroom, now);

        return current;
    }

  private:
    static constexpr int kWarmupFrames = 8;
    static constexpr double kWarmupMs = 500.0;  // Frames discarded after a start or pause
    static constexpr double kMeasureMs = 300.0; // Then measured before the first decision
    static constexpr double kRoundingSlack = 0.02;
    static constexpr double kBindingTolerance = 0.95; // Base within 5 % of target / multiplier = held by the cap
    static constexpr double kRevertWindowMs = 5000.0;
    static constexpr double kMaxBackoff = 16.0;
    static constexpr double kSettleMs = 400.0;         // After a change the pipeline and the cap adjust
    static constexpr double kDefaultCostFactor = 1.15; // Until one is learned

    Status _status {};
    double _lastFrameMs = -1.0;
    double _baseIntervalMs = 0.0;
    double _upSinceMs = -1.0;
    double _downSinceMs = -1.0;
    double _settleUntilMs = -1.0;
    double _firstFrameMs = -1.0;
    uint32_t _frames = 0;
    double _backoff = 1.0;

    static double Smooth(double previous, double value, double dtMs, double timeMs)
    {
        if (previous <= 0.0 || dtMs <= 0.0)
            return value;

        const double alpha = 1.0 - std::exp(-dtMs / std::max(timeMs, 1.0));
        return previous + (value - previous) * alpha;
    }

    // True once condition has held continuously for holdMs
    static bool Held(bool condition, double& sinceMs, double nowMs, double holdMs)
    {
        if (!condition)
        {
            sinceMs = -1.0;
            return false;
        }

        if (sinceMs < 0.0)
            sinceMs = nowMs;

        return nowMs - sinceMs >= holdMs;
    }

    void Change(int multiplier, Reason reason, double nowMs)
    {
        if (multiplier == _status.multiplier)
            return;

        _status.previousMultiplier = _status.multiplier;
        _status.multiplier = multiplier;
        _status.reason = reason;
        _status.lastChangeMs = nowMs;
        _upSinceMs = -1.0;
        _downSinceMs = -1.0;

        // The old rate says nothing about the new multiplier's cap: measure afresh, and don't learn from the
        // frames in between
        _baseIntervalMs = 0.0;
        _settleUntilMs = nowMs + kSettleMs;
    }

    void UpdateEstimates(const Sample& sample, const Settings& settings)
    {
        double dt = 0.0;

        if (_frames++ == 0)
            _firstFrameMs = sample.nowMs;

        if (_lastFrameMs >= 0.0)
            dt = sample.nowMs - _lastFrameMs;

        _lastFrameMs = sample.nowMs;

        // Warmup frames don't count
        if (sample.nowMs - _firstFrameMs < kWarmupMs)
            return;

        // A long gap is a pause (menu, loading), not a frame
        if (dt > 0.0 && dt < 250.0)
            _baseIntervalMs = Smooth(_baseIntervalMs, dt, dt, settings.emaTimeMs);

        if (sample.gpuRenderMs > 0.0)
            _status.gpuRenderMs = Smooth(_status.gpuRenderMs, sample.gpuRenderMs, dt, settings.emaTimeMs);

        if (sample.cpuFrameMs > 0.0)
            _status.cpuFrameMs = Smooth(_status.cpuFrameMs, sample.cpuFrameMs, dt, settings.emaTimeMs);

        _status.baseFps = _baseIntervalMs > 0.0 ? 1000.0 / _baseIntervalMs : 0.0;
    }

    void UpdateCapacity(const Sample& sample, const Settings& settings)
    {
        const double costMs = std::max(_status.gpuRenderMs, _status.cpuFrameMs);
        const int current = _status.multiplier;

        _status.capBinding =
            sample.capped && current > 0 && _status.baseFps >= settings.targetFps / current * kBindingTolerance;

        if (!_status.capBinding)
        {
            // Nothing holds the game back: the real rate is the truth, and shows how far the costs undershoot
            if (costMs > 0.0 && sample.nowMs >= _settleUntilMs)
            {
                const double factor = std::clamp(_baseIntervalMs / costMs, 1.0, 3.0);
                _status.costFactor = _status.costFactor > 0.0
                                         ? Smooth(_status.costFactor, factor, 1000.0 / _status.baseFps, 2000.0)
                                         : factor;
            }

            _status.capacityFps = _status.baseFps;
            _status.fromCosts = false;
            return;
        }

        // Held by the cap: the game renders at least what it renders now, the costs say how much more it could
        _status.fromCosts = costMs > 0.0;
        const double factor = _status.costFactor > 0.0 ? _status.costFactor : kDefaultCostFactor;
        const double costFps = _status.fromCosts ? 1000.0 / (costMs * factor) : 0.0;
        _status.capacityFps = std::max(_status.baseFps, costFps);
    }
};
