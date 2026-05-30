#include "pch.h"
#include "FrameWarp.h"
#include "FrameWarp_Common.h"
#include "FrameWarp_DLSSGPolicy.h"
#include <Config.h>
#include <State.h>
#include <framegen/dlssg/DLSSG_Native.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>
#include <cstring>
#include <limits>

// Local helper: TranslateTypelessFormats is protected, so we provide
// a local copy (same pattern as Bias_Dx11, RCAS_Dx11, OS_Dx11, etc.)
inline static DXGI_FORMAT TranslateTypelessFormats(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS:   return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UINT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:       return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:     return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:  return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return format;
    }
}

inline static DXGI_FORMAT FrameWarpDepthCopyFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

inline static DXGI_FORMAT FrameWarpDepthSrvFormat(DXGI_FORMAT format)
{
    switch (FrameWarpDepthCopyFormat(format))
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

inline static UINT FrameWarpMotionVectorBytesPerPixel(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_UNORM:
        return 4;
    case DXGI_FORMAT_R32G32_FLOAT:
        return 8;
    default:
        return 0;
    }
}

inline static UINT FrameWarpAlignUInt(UINT value, UINT alignment)
{
    return alignment > 0 ? ((value + alignment - 1) / alignment) * alignment : value;
}

inline static float FrameWarpHalfToFloat(uint16_t h)
{
    const uint32_t sign = (h & 0x8000u) << 16;
    int exp = static_cast<int>((h >> 10) & 0x1Fu);
    uint32_t mant = h & 0x03FFu;
    uint32_t out = 0;

    if (exp == 0)
    {
        if (mant == 0)
        {
            out = sign;
        }
        else
        {
            exp = 1;
            while ((mant & 0x0400u) == 0)
            {
                mant <<= 1;
                exp--;
            }
            mant &= 0x03FFu;
            out = sign | (static_cast<uint32_t>(exp + (127 - 15)) << 23) | (mant << 13);
        }
    }
    else if (exp == 31)
    {
        out = sign | 0x7F800000u | (mant << 13);
    }
    else
    {
        out = sign | (static_cast<uint32_t>(exp + (127 - 15)) << 23) | (mant << 13);
    }

    float f = 0.0f;
    memcpy(&f, &out, sizeof(f));
    return f;
}

inline static bool FrameWarpReadMotionVectorPixel(
    const uint8_t* pixel,
    DXGI_FORMAT format,
    float& x,
    float& y)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16_FLOAT:
    {
        const uint16_t* v = reinterpret_cast<const uint16_t*>(pixel);
        x = FrameWarpHalfToFloat(v[0]);
        y = FrameWarpHalfToFloat(v[1]);
        return true;
    }
    case DXGI_FORMAT_R16G16_SNORM:
    {
        const int16_t* v = reinterpret_cast<const int16_t*>(pixel);
        x = std::max(-1.0f, static_cast<float>(v[0]) / 32767.0f);
        y = std::max(-1.0f, static_cast<float>(v[1]) / 32767.0f);
        return true;
    }
    case DXGI_FORMAT_R16G16_UNORM:
    {
        const uint16_t* v = reinterpret_cast<const uint16_t*>(pixel);
        x = static_cast<float>(v[0]) / 65535.0f;
        y = static_cast<float>(v[1]) / 65535.0f;
        return true;
    }
    case DXGI_FORMAT_R32G32_FLOAT:
    {
        const float* v = reinterpret_cast<const float*>(pixel);
        x = v[0];
        y = v[1];
        return true;
    }
    default:
        x = 0.0f;
        y = 0.0f;
        return false;
    }
}

inline static bool FrameWarpStateNeedsTransition(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    return before != after && before != D3D12_RESOURCE_STATE_COMMON;
}

inline static DXGI_FORMAT FrameWarpFallbackUavFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return TranslateTypelessFormats(format);
    }
}

constexpr UINT FRAMEWARP_HEAP_SRV_COUNT = 3;
constexpr UINT FRAMEWARP_HEAP_UAV_COUNT = 2;
constexpr UINT FRAMEWARP_HEAP_CBV_COUNT = 1;
constexpr float FRAMEWARP_PI = 3.14159265358979323846f;
constexpr float FRAMEWARP_DEFAULT_VFOV = 1.0471975512f;
constexpr float FRAMEWARP_MIN_WARP_MAGNITUDE = 0.00001f;

static int64_t FrameWarpQueryQpc()
{
    LARGE_INTEGER counter {};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

static double FrameWarpQpcToSeconds(int64_t qpcDelta)
{
    static const double frequency = []()
    {
        LARGE_INTEGER freq {};
        QueryPerformanceFrequency(&freq);
        return static_cast<double>(freq.QuadPart);
    }();

    return frequency > 0.0 ? static_cast<double>(qpcDelta) / frequency : 0.0;
}

static double FrameWarpQpcToMilliseconds(int64_t qpcDelta)
{
    return FrameWarpQpcToSeconds(qpcDelta) * 1000.0;
}

static void FrameWarpSetSkipReason(const char* reason)
{
    auto& status = State::Instance().frameWarpStatus;
    status.lastWarpApplied = false;
    if (reason != nullptr)
        strncpy_s(status.lastSkipReason, reason, _TRUNCATE);
}

static void FrameWarpSetApplied(const CameraPredictor::WarpResult& warpResult)
{
    static bool hasLastDisplayedPose = false;
    static float lastDisplayedYaw = 0.0f;
    static float lastDisplayedPitch = 0.0f;

    auto& status = State::Instance().frameWarpStatus;
    status.lastWarpApplied = true;
    status.lastPoseJitterYaw = hasLastDisplayedPose ? warpResult.deltaYaw - lastDisplayedYaw : 0.0f;
    status.lastPoseJitterPitch = hasLastDisplayedPose ? warpResult.deltaPitch - lastDisplayedPitch : 0.0f;
    status.lastDeltaYaw = warpResult.deltaYaw;
    status.lastDeltaPitch = warpResult.deltaPitch;
    status.lastAppliedDeltaYaw = warpResult.deltaYaw;
    status.lastAppliedDeltaPitch = warpResult.deltaPitch;
    status.lastAppliedPixelShift = status.lastPreparedPixelShift;
    status.lastSkipReason[0] = '\0';

    lastDisplayedYaw = warpResult.deltaYaw;
    lastDisplayedPitch = warpResult.deltaPitch;
    hasLastDisplayedPose = true;
}

static void FrameWarpSetDepthDiagnostics(
    ID3D12Resource* depthInput,
    bool configEnabled,
    bool psoAvailable,
    bool depthUsed,
    const char* overrideReason = nullptr)
{
    auto& status = State::Instance().frameWarpStatus;
    status.lastDepthAwareEnabled = configEnabled;
    status.lastDepthUsed = depthUsed;
    status.lastDepthResourceValid = false;
    status.lastDepthWidth = 0;
    status.lastDepthHeight = 0;
    status.lastDepthFormat = 0;

    const char* reason = "missing depth";
    if (!configEnabled)
        reason = "config disabled";
    else if (!psoAvailable)
        reason = "shader unavailable";

    if (depthInput != nullptr)
    {
        auto desc = depthInput->GetDesc();
        status.lastDepthResourceValid =
            desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            desc.Width > 0 &&
            desc.Height > 0;
        status.lastDepthWidth = static_cast<uint32_t>(desc.Width);
        status.lastDepthHeight = desc.Height;
        status.lastDepthFormat = static_cast<uint32_t>(desc.Format);

        if (configEnabled && psoAvailable)
            reason = status.lastDepthResourceValid ? (depthUsed ? "active" : "invalid depth") : "invalid depth";
    }

    strncpy_s(status.lastDepthReason, overrideReason != nullptr ? overrideReason : reason, _TRUNCATE);

    static uint64_t depthLogCount = 0;
    depthLogCount++;
    if (Config::Instance()->FrameWarpDebug.value_or_default() &&
        (depthLogCount <= 30 || depthLogCount % 300 == 0))
    {
        LOG_DEBUG("FrameWarp depth: enabled={} resource={} used={} size={}x{} fmt={} reason={}",
            status.lastDepthAwareEnabled,
            status.lastDepthResourceValid,
            status.lastDepthUsed,
            status.lastDepthWidth,
            status.lastDepthHeight,
            status.lastDepthFormat,
            status.lastDepthReason);
    }
}

static void FrameWarpLogDepthInfillDiagnostics(const char* phase)
{
    auto& status = State::Instance().frameWarpStatus;
    static uint64_t logCount = 0;
    logCount++;
    if (logCount <= 10 || logCount % 60 == 0)
    {
        LOG_INFO("VF2Depth #{} phase={} copy={} src={} copyFmt={} srvFmt={} state={:#x} size={}x{} ageFrames={:.0f} inverted={} historyAvail={} historyUsed={} maskCoveragePct={:.3f} reason={} source={}",
            logCount,
            phase != nullptr ? phase : "unknown",
            status.depthInfillCopySuccess,
            status.depthInfillSourceFormat,
            status.depthInfillCopyFormat,
            status.depthInfillSrvFormat,
            status.depthInfillSourceState,
            status.depthInfillWidth,
            status.depthInfillHeight,
            status.depthInfillAgeFrames,
            status.depthInfillInverted,
            status.depthInfillHistoryAvailable,
            status.depthInfillHistoryUsed,
            status.depthInfillMaskCoveragePct,
            status.depthInfillReason[0] ? status.depthInfillReason : "none",
            status.depthInfillSource[0] ? status.depthInfillSource : "none");
    }
}

static bool FrameWarpTimingAuditShouldLog(uint64_t count)
{
    return Config::Instance()->FrameWarpTimingAuditLog.value_or_default() &&
        (count <= 600 || count % 60 == 0);
}

static void FrameWarpResourceBarrier(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    if (cmdList == nullptr || resource == nullptr || before == after)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

static bool FrameWarpValidVFov(float vFovRadians)
{
    return std::isfinite(vFovRadians) && vFovRadians > 0.1f && vFovRadians < 3.05f;
}

static bool FrameWarpValidAspect(float aspectRatio)
{
    return std::isfinite(aspectRatio) && aspectRatio > 0.2f && aspectRatio < 8.0f;
}

// ============================================================================
// MouseTracker Implementation
// ============================================================================

int64_t MouseTracker::GetTimestampQpc()
{
    return FrameWarpQueryQpc();
}

double MouseTracker::QpcToSeconds(int64_t qpcDelta)
{
    return FrameWarpQpcToSeconds(qpcDelta);
}

double MouseTracker::QpcToMilliseconds(int64_t qpcDelta)
{
    return FrameWarpQpcToMilliseconds(qpcDelta);
}

void MouseTracker::OnRawMouseInput(LONG dx, LONG dy, const char* source, int64_t timestampQpc)
{
    std::scoped_lock lock(_mutex);
    if (timestampQpc == 0)
        timestampQpc = GetTimestampQpc();

    uint64_t sequence = ++_sampleSequence;
    double accumulatedDx = _accumulatedDx.load(std::memory_order_relaxed) + static_cast<double>(dx);
    double accumulatedDy = _accumulatedDy.load(std::memory_order_relaxed) + static_cast<double>(dy);
    _accumulatedDx.store(accumulatedDx, std::memory_order_relaxed);
    _accumulatedDy.store(accumulatedDy, std::memory_order_relaxed);

    _sampleHistory[_sampleIndex].dx = dx;
    _sampleHistory[_sampleIndex].dy = dy;
    _sampleHistory[_sampleIndex].timestampQpc = timestampQpc;
    _sampleHistory[_sampleIndex].sequence = sequence;
    _sampleHistory[_sampleIndex].accumulatedDx = accumulatedDx;
    _sampleHistory[_sampleIndex].accumulatedDy = accumulatedDy;
    strncpy_s(_sampleHistory[_sampleIndex].source,
        source != nullptr && source[0] != '\0' ? source : "unknown",
        _TRUNCATE);
    _sampleIndex = (_sampleIndex + 1) % SAMPLE_HISTORY;
    _sampleCount = std::min(_sampleCount + 1, SAMPLE_HISTORY);

    auto& status = State::Instance().frameWarpStatus;
    status.rawInputSampleCount++;
    status.rawInputSequence = sequence;
    status.lastAccumDx = static_cast<float>(accumulatedDx);
    status.lastAccumDy = static_cast<float>(accumulatedDy);

    if (_sampleCount >= 2)
    {
        size_t newestIndex = (_sampleIndex + SAMPLE_HISTORY - 1) % SAMPLE_HISTORY;
        size_t oldestIndex = (_sampleIndex + SAMPLE_HISTORY - _sampleCount) % SAMPLE_HISTORY;
        int64_t elapsedQpc = _sampleHistory[newestIndex].timestampQpc - _sampleHistory[oldestIndex].timestampQpc;
        const double elapsedSec = QpcToSeconds(elapsedQpc);
        if (elapsedSec > 0.0)
            status.rawInputSampleHz = static_cast<float>(static_cast<double>(_sampleCount - 1) / elapsedSec);

        double intervalMinMs = std::numeric_limits<double>::max();
        double intervalMaxMs = 0.0;
        double intervalSumMs = 0.0;
        double intervalSqSumMs = 0.0;
        size_t intervalCount = 0;

        for (size_t i = 1; i < _sampleCount; ++i)
        {
            size_t idx = (_sampleIndex + SAMPLE_HISTORY - _sampleCount + i) % SAMPLE_HISTORY;
            size_t prevIdx = (_sampleIndex + SAMPLE_HISTORY - _sampleCount + i - 1) % SAMPLE_HISTORY;
            const double intervalMs = QpcToMilliseconds(
                _sampleHistory[idx].timestampQpc - _sampleHistory[prevIdx].timestampQpc);
            if (intervalMs < 0.0)
                continue;

            intervalMinMs = std::min(intervalMinMs, intervalMs);
            intervalMaxMs = std::max(intervalMaxMs, intervalMs);
            intervalSumMs += intervalMs;
            intervalSqSumMs += intervalMs * intervalMs;
            intervalCount++;
        }

        if (intervalCount > 0)
        {
            const double meanMs = intervalSumMs / static_cast<double>(intervalCount);
            const double varianceMs =
                std::max(0.0, intervalSqSumMs / static_cast<double>(intervalCount) - meanMs * meanMs);
            status.rawInputIntervalMinMs = static_cast<float>(intervalMinMs);
            status.rawInputIntervalMaxMs = static_cast<float>(intervalMaxMs);
            status.rawInputIntervalMeanMs = static_cast<float>(meanMs);
            status.rawInputIntervalStdDevMs = static_cast<float>(std::sqrt(varianceMs));
        }
    }
}

void MouseTracker::SnapshotRenderState()
{
    std::scoped_lock lock(_mutex);

    const int64_t nowQpc = GetTimestampQpc();
    double snapshotDx = _accumulatedDx.load(std::memory_order_relaxed);
    double snapshotDy = _accumulatedDy.load(std::memory_order_relaxed);
    uint64_t snapshotSequence = _sampleSequence;
    const bool hadRenderSnapshot = _hasRenderSnapshot;
    const double previousSnapshotDx = _renderMouseState.accumulatedDx;
    const double previousSnapshotDy = _renderMouseState.accumulatedDy;

    _renderMouseState.accumulatedDx = snapshotDx;
    _renderMouseState.accumulatedDy = snapshotDy;
    _renderMouseState.timestampQpc = nowQpc;
    _renderMouseState.sequence = snapshotSequence;

    if (_sampleCount >= 2)
    {
        float totalDx = 0.0f;
        float totalDy = 0.0f;
        int64_t totalTimeNs = 0;

        for (size_t i = 0; i < _sampleCount - 1; i++)
        {
            size_t idx = (_sampleIndex + SAMPLE_HISTORY - 1 - i) % SAMPLE_HISTORY;
            size_t prevIdx = (idx + SAMPLE_HISTORY - 1) % SAMPLE_HISTORY;

            totalDx += static_cast<float>(_sampleHistory[idx].dx);
            totalDy += static_cast<float>(_sampleHistory[idx].dy);
            totalTimeNs += _sampleHistory[idx].timestampQpc - _sampleHistory[prevIdx].timestampQpc;
        }

        const double timeSec = QpcToSeconds(totalTimeNs);
        if (timeSec > 0.0)
        {
            _renderMouseState.velocityX = totalDx / static_cast<float>(timeSec);
            _renderMouseState.velocityY = totalDy / static_cast<float>(timeSec);
        }
    }

    _hasRenderSnapshot = true;

    auto& status = State::Instance().frameWarpStatus;
    status.renderSnapshotCount++;
    status.lastSnapshotDx = static_cast<float>(_renderMouseState.accumulatedDx);
    status.lastSnapshotDy = static_cast<float>(_renderMouseState.accumulatedDy);
    status.lastRenderMouseStepDx = hadRenderSnapshot
        ? static_cast<float>(snapshotDx - previousSnapshotDx)
        : 0.0f;
    status.lastRenderMouseStepDy = hadRenderSnapshot
        ? static_cast<float>(snapshotDy - previousSnapshotDy)
        : 0.0f;
    status.rawInputSnapshotSequence = _renderMouseState.sequence;
    status.rawInputSamplesSinceSnapshot =
        _sampleSequence >= _renderMouseState.sequence
            ? static_cast<uint32_t>(std::min<uint64_t>(
                _sampleSequence - _renderMouseState.sequence,
                static_cast<uint64_t>(UINT32_MAX)))
            : 0;
    status.lastSnapshotAgeMs = 0.0f;
}

void MouseTracker::SnapshotDLSSGResourcePendingAnchor()
{
    std::scoped_lock lock(_mutex);

    const int64_t nowQpc = GetTimestampQpc();
    MouseState pending {};
    pending.accumulatedDx = _accumulatedDx.load(std::memory_order_relaxed);
    pending.accumulatedDy = _accumulatedDy.load(std::memory_order_relaxed);
    pending.timestampQpc = nowQpc;
    pending.sequence = _sampleSequence;
    pending.velocityX = _renderMouseState.velocityX;
    pending.velocityY = _renderMouseState.velocityY;

    _dlssgResourcePendingAnchorMouseState = pending;
    _hasDLSSGResourcePendingAnchor = true;

    if (!_hasDLSSGResourceAnchor)
    {
        _dlssgResourceAnchorMouseState = pending;
        _hasDLSSGResourceAnchor = true;
    }
}

void MouseTracker::PromoteDLSSGResourcePendingAnchor()
{
    std::scoped_lock lock(_mutex);
    if (!_hasDLSSGResourcePendingAnchor)
        return;

    _dlssgResourceAnchorMouseState = _dlssgResourcePendingAnchorMouseState;
    _hasDLSSGResourceAnchor = true;
    _hasDLSSGResourcePendingAnchor = false;
}

MouseTracker::MouseState MouseTracker::GetCurrentState() const
{
    MouseState state;
    state.accumulatedDx = _accumulatedDx.load(std::memory_order_relaxed);
    state.accumulatedDy = _accumulatedDy.load(std::memory_order_relaxed);
    state.timestampQpc = GetTimestampQpc();

    std::scoped_lock lock(_mutex);
    state.velocityX = _renderMouseState.velocityX;
    state.velocityY = _renderMouseState.velocityY;
    return state;
}

bool MouseTracker::GetWarpDelta(float& outDeltaYaw, float& outDeltaPitch) const
{
    // Lock mutex for consistent read of _renderMouseState and _hasRenderSnapshot.
    // Previously this read _renderMouseState without the lock, which was a data race
    // since SnapshotRenderState() writes it under _mutex.
    std::scoped_lock lock(_mutex);

    if (!_hasRenderSnapshot)
        return false;

    // Snapshot current accumulated values inside the lock to avoid TOCTOU.
    double currentDx = _accumulatedDx.load(std::memory_order_relaxed);
    double currentDy = _accumulatedDy.load(std::memory_order_relaxed);

    double deltaDx = currentDx - _renderMouseState.accumulatedDx;
    double deltaDy = currentDy - _renderMouseState.accumulatedDy;
    const double rawDeltaDx = deltaDx;
    const double rawDeltaDy = deltaDy;
    float predictedDx = 0.0f;
    float predictedDy = 0.0f;
    bool predictionApplied = false;
    uint32_t samplesSinceSnapshot = 0;
    const int64_t nowQpc = GetTimestampQpc();

    if (_sampleSequence >= _renderMouseState.sequence)
        samplesSinceSnapshot = static_cast<uint32_t>(std::min<uint64_t>(
            _sampleSequence - _renderMouseState.sequence,
            static_cast<uint64_t>(UINT32_MAX)));

    if (Config::Instance()->FrameWarpInputPrediction.value_or_default() &&
        _sampleCount >= 2 &&
        samplesSinceSnapshot > 0)
    {
        size_t newestIndex = (_sampleIndex + SAMPLE_HISTORY - 1) % SAMPLE_HISTORY;
        const auto& newest = _sampleHistory[newestIndex];
        const double latestAgeSec = QpcToSeconds(nowQpc - newest.timestampQpc);

        float recentDx = 0.0f;
        float recentDy = 0.0f;
        int64_t oldestRecentTimestampQpc = newest.timestampQpc;
        size_t recentCount = 0;
        int signX = 0;
        int signY = 0;
        bool consistentX = true;
        bool consistentY = true;

        for (size_t i = 0; i < std::min<size_t>(_sampleCount, 8); ++i)
        {
            size_t idx = (_sampleIndex + SAMPLE_HISTORY - 1 - i) % SAMPLE_HISTORY;
            const auto& sample = _sampleHistory[idx];
            if (sample.sequence <= _renderMouseState.sequence)
                break;

            recentDx += static_cast<float>(sample.dx);
            recentDy += static_cast<float>(sample.dy);
            oldestRecentTimestampQpc = sample.timestampQpc;
            recentCount++;

            if (sample.dx != 0)
            {
                int sampleSign = sample.dx > 0 ? 1 : -1;
                if (signX == 0)
                    signX = sampleSign;
                else if (signX != sampleSign)
                    consistentX = false;
            }

            if (sample.dy != 0)
            {
                int sampleSign = sample.dy > 0 ? 1 : -1;
                if (signY == 0)
                    signY = sampleSign;
                else if (signY != sampleSign)
                    consistentY = false;
            }
        }

        const double recentTimeSec = QpcToSeconds(newest.timestampQpc - oldestRecentTimestampQpc);
        if (recentCount >= 2 &&
            recentTimeSec > 0.0001 &&
            latestAgeSec > 0.0 &&
            latestAgeSec <= 0.004 &&
            consistentX &&
            consistentY)
        {
            const double predictSec = std::min(latestAgeSec, 0.002);
            predictedDx = static_cast<float>((recentDx / static_cast<float>(recentTimeSec)) * predictSec);
            predictedDy = static_cast<float>((recentDy / static_cast<float>(recentTimeSec)) * predictSec);

            const float maxPredictX = std::max(2.0f, std::abs(recentDx) * 0.75f);
            const float maxPredictY = std::max(2.0f, std::abs(recentDy) * 0.75f);
            predictedDx = std::clamp(predictedDx, -maxPredictX, maxPredictX);
            predictedDy = std::clamp(predictedDy, -maxPredictY, maxPredictY);

            deltaDx += static_cast<double>(predictedDx);
            deltaDy += static_cast<double>(predictedDy);
            predictionApplied = std::abs(predictedDx) > 0.001f || std::abs(predictedDy) > 0.001f;
        }
    }

    outDeltaYaw = static_cast<float>(deltaDx) * _sensitivityX;
    outDeltaPitch = static_cast<float>(deltaDy) * _sensitivityY;

    auto& status = State::Instance().frameWarpStatus;
    static bool hasLastFinalDelta = false;
    static double lastFinalDeltaDx = 0.0;
    static double lastFinalDeltaDy = 0.0;

    status.lastAccumDx = static_cast<float>(currentDx);
    status.lastAccumDy = static_cast<float>(currentDy);
    status.lastSnapshotDx = static_cast<float>(_renderMouseState.accumulatedDx);
    status.lastSnapshotDy = static_cast<float>(_renderMouseState.accumulatedDy);
    status.lastMouseDeltaDx = static_cast<float>(rawDeltaDx);
    status.lastMouseDeltaDy = static_cast<float>(rawDeltaDy);
    status.lastPredictedMouseDx = predictedDx;
    status.lastPredictedMouseDy = predictedDy;
    status.lastFinalMouseDeltaDx = static_cast<float>(deltaDx);
    status.lastFinalMouseDeltaDy = static_cast<float>(deltaDy);
    status.lastFinalMouseDeltaStepDx =
        hasLastFinalDelta ? static_cast<float>(deltaDx - lastFinalDeltaDx) : 0.0f;
    status.lastFinalMouseDeltaStepDy =
        hasLastFinalDelta ? static_cast<float>(deltaDy - lastFinalDeltaDy) : 0.0f;
    status.rawInputSnapshotSequence = _renderMouseState.sequence;
    status.rawInputSamplesSinceSnapshot = samplesSinceSnapshot;
    status.lastInputPredictionApplied = predictionApplied;
    status.lastSnapshotAgeMs = static_cast<float>(
        QpcToMilliseconds(nowQpc - _renderMouseState.timestampQpc));
    if (_sampleCount > 0)
    {
        size_t newestIndex = (_sampleIndex + SAMPLE_HISTORY - 1) % SAMPLE_HISTORY;
        status.lastInputSampleAgeMs = static_cast<float>(
            QpcToMilliseconds(nowQpc - _sampleHistory[newestIndex].timestampQpc));
    }

    lastFinalDeltaDx = deltaDx;
    lastFinalDeltaDy = deltaDy;
    hasLastFinalDelta = true;

    return true;
}

bool MouseTracker::GetWarpDeltaFromDLSSGResourceAnchor(float& outDeltaYaw, float& outDeltaPitch) const
{
    std::scoped_lock lock(_mutex);

    if (!_hasDLSSGResourceAnchor)
        return false;

    const double currentDx = _accumulatedDx.load(std::memory_order_relaxed);
    const double currentDy = _accumulatedDy.load(std::memory_order_relaxed);
    const double deltaDx = currentDx - _dlssgResourceAnchorMouseState.accumulatedDx;
    const double deltaDy = currentDy - _dlssgResourceAnchorMouseState.accumulatedDy;
    const uint32_t samplesSinceAnchor = _sampleSequence >= _dlssgResourceAnchorMouseState.sequence
        ? static_cast<uint32_t>(std::min<uint64_t>(
            _sampleSequence - _dlssgResourceAnchorMouseState.sequence,
            static_cast<uint64_t>(UINT32_MAX)))
        : 0;
    const int64_t nowQpc = GetTimestampQpc();

    outDeltaYaw = static_cast<float>(deltaDx) * _sensitivityX;
    outDeltaPitch = static_cast<float>(deltaDy) * _sensitivityY;

    auto& status = State::Instance().frameWarpStatus;
    status.lastAccumDx = static_cast<float>(currentDx);
    status.lastAccumDy = static_cast<float>(currentDy);
    status.lastSnapshotDx = static_cast<float>(_dlssgResourceAnchorMouseState.accumulatedDx);
    status.lastSnapshotDy = static_cast<float>(_dlssgResourceAnchorMouseState.accumulatedDy);
    status.lastMouseDeltaDx = static_cast<float>(deltaDx);
    status.lastMouseDeltaDy = static_cast<float>(deltaDy);
    status.lastPredictedMouseDx = 0.0f;
    status.lastPredictedMouseDy = 0.0f;
    status.lastFinalMouseDeltaDx = static_cast<float>(deltaDx);
    status.lastFinalMouseDeltaDy = static_cast<float>(deltaDy);
    status.lastFinalMouseDeltaStepDx = 0.0f;
    status.lastFinalMouseDeltaStepDy = 0.0f;
    status.rawInputSnapshotSequence = _dlssgResourceAnchorMouseState.sequence;
    status.rawInputSamplesSinceSnapshot = samplesSinceAnchor;
    status.lastInputPredictionApplied = false;
    status.lastSnapshotAgeMs = static_cast<float>(
        QpcToMilliseconds(nowQpc - _dlssgResourceAnchorMouseState.timestampQpc));
    if (_sampleCount > 0)
    {
        size_t newestIndex = (_sampleIndex + SAMPLE_HISTORY - 1) % SAMPLE_HISTORY;
        status.lastInputSampleAgeMs = static_cast<float>(
            QpcToMilliseconds(nowQpc - _sampleHistory[newestIndex].timestampQpc));
    }

    return true;
}

void MouseTracker::ResetAccumulator()
{
    _accumulatedDx.store(0.0, std::memory_order_relaxed);
    _accumulatedDy.store(0.0, std::memory_order_relaxed);
}

void MouseTracker::AutoDetectSensitivity(float cameraDeltaYaw, float cameraDeltaPitch)
{
    std::scoped_lock lock(_mutex);

    if (_sampleCount < 4)
        return;

    float totalMouseDx = 0.0f;
    float totalMouseDy = 0.0f;
    for (size_t i = 0; i < _sampleCount; i++)
    {
        totalMouseDx += static_cast<float>(_sampleHistory[i].dx);
        totalMouseDy += static_cast<float>(_sampleHistory[i].dy);
    }

    const float MIN_MOUSE_DELTA = 5.0f;
    const float MIN_CAMERA_DELTA = 0.001f;

    if (std::abs(totalMouseDx) > MIN_MOUSE_DELTA && std::abs(cameraDeltaYaw) > MIN_CAMERA_DELTA)
    {
        float estimatedSensX = cameraDeltaYaw / totalMouseDx;
        _sensitivityX = _sensitivityX * 0.95f + estimatedSensX * 0.05f;
    }

    if (std::abs(totalMouseDy) > MIN_MOUSE_DELTA && std::abs(cameraDeltaPitch) > MIN_CAMERA_DELTA)
    {
        float estimatedSensY = cameraDeltaPitch / totalMouseDy;
        _sensitivityY = _sensitivityY * 0.95f + estimatedSensY * 0.05f;
    }
}

// ============================================================================
// CameraPredictor Implementation
// ============================================================================

void CameraPredictor::RecordRenderCamera(const XMMATRIX& viewProj)
{
    _prevViewProj = _renderViewProj;
    _hasPrevViewProj = _hasRenderViewProj;
    _renderViewProj = viewProj;
    _hasRenderViewProj = true;
}

CameraPredictor::WarpResult CameraPredictor::PredictFromMouseDelta(
    float mouseDeltaYaw,
    float mouseDeltaPitch,
    const XMMATRIX& renderViewProj,
    float warpStrength)
{
    WarpResult result;
    // If no render camera has been recorded (OnFrameRenderStart never called),
    // fall back to identity viewProj. The warp rotation is applied in clip space
    // relative to the viewProj, so identity = pure clip-space rotation.
    // This is correct for mouse-driven warp when we don't know the game's camera.
    XMMATRIX effectiveViewProj = _hasRenderViewProj ? _renderViewProj : XMMatrixIdentity();

    // Apply warp strength and max angle clamping to the INPUT (mouse delta)
    // BEFORE building the warp matrix. This produces valid intermediate
    // transforms (proper rotations at reduced angle), unlike post-hoc
    // matrix element interpolation which produces invalid sheared matrices.
    float clampedYaw = mouseDeltaYaw * warpStrength;
    float clampedPitch = mouseDeltaPitch * warpStrength;

    // Clamp the total rotation angle to maxWarpAngle.
    // This is correct because we're clamping the input rotation angle,
    // not trying to extract an angle from a clip-space matrix (which
    // includes projection and is not a pure rotation).
    float totalAngle = std::sqrt(clampedYaw * clampedYaw + clampedPitch * clampedPitch);
    bool clamped = false;
    if (totalAngle > _maxWarpAngle && totalAngle > 0.0f)
    {
        float scale = _maxWarpAngle / totalAngle;
        clampedYaw *= scale;
        clampedPitch *= scale;
        clamped = true;
    }
    State::Instance().frameWarpStatus.lastPoseClampApplied = clamped;
    if (clamped)
    {
        static uint64_t clampLogCount = 0;
        clampLogCount++;
        if (clampLogCount <= 20 || clampLogCount % 300 == 0)
        {
            LOG_DEBUG("FrameWarp: pose clamp applied inputYaw={:.6f} inputPitch={:.6f} maxAngle={:.6f}",
                mouseDeltaYaw * warpStrength,
                mouseDeltaPitch * warpStrength,
                _maxWarpAngle);
        }
    }

    XMMATRIX deltaYawMatrix = XMMatrixRotationY(clampedYaw);
    XMMATRIX deltaPitchMatrix = XMMatrixRotationX(clampedPitch);
    XMMATRIX deltaRotation = XMMatrixMultiply(deltaYawMatrix, deltaPitchMatrix);

    // predictedViewProj = deltaRotation * renderView * proj = deltaRotation * renderViewProj
    XMMATRIX predictedViewProj = XMMatrixMultiply(deltaRotation, effectiveViewProj);

    result = ComputeWarpMatrix(effectiveViewProj, predictedViewProj);
    result.deltaYaw = clampedYaw;
    result.deltaPitch = clampedPitch;
    return result;
}

CameraPredictor::WarpResult CameraPredictor::PredictFromVelocity(
    float deltaTimeSeconds,
    const XMMATRIX& renderViewProj,
    float warpStrength)
{
    WarpResult result;
    if (!_hasRenderViewProj || !_hasPrevViewProj || deltaTimeSeconds <= 0.0f)
    {
        result.isValid = false;
        return result;
    }

    XMMATRIX viewProjInv = XMMatrixInverse(nullptr, renderViewProj);
    XMMATRIX clipToPrevClip = XMMatrixMultiply(viewProjInv, _prevViewProj);

    // Apply warp strength to extrapolation factor (input-side scaling)
    float extrapolationFactor = deltaTimeSeconds * 60.0f * warpStrength;
    extrapolationFactor = std::min(extrapolationFactor, 2.0f);

    // Clamp extrapolation to maxWarpAngle equivalent
    float maxExtrapolation = _maxWarpAngle * 60.0f;
    if (extrapolationFactor > maxExtrapolation && maxExtrapolation > 0.0f)
        extrapolationFactor = maxExtrapolation;

    XMMATRIX identity = XMMatrixIdentity();
    XMMATRIX predictedClipToClip = {};
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++)
        {
            float deltaVal = clipToPrevClip.r[i].m128_f32[j] - identity.r[i].m128_f32[j];
            predictedClipToClip.r[i].m128_f32[j] = identity.r[i].m128_f32[j] + deltaVal * extrapolationFactor;
        }
    }

    XMMATRIX predictedViewProj = XMMatrixMultiply(predictedClipToClip, renderViewProj);
    result = ComputeWarpMatrix(renderViewProj, predictedViewProj);
    return result;
}

CameraPredictor::WarpResult CameraPredictor::ComputeWarpMatrix(
    const XMMATRIX& renderViewProj,
    const XMMATRIX& predictedViewProj)
{
    WarpResult result;

    // Pure matrix computation — no strength/angle clamping.
    // Strength and angle clamping must be applied to the inputs BEFORE calling.
    XMMATRIX renderViewProjInv = XMMatrixInverse(nullptr, renderViewProj);
    result.clipToClip = XMMatrixMultiply(predictedViewProj, renderViewProjInv);
    result.clipToClipInverse = XMMatrixInverse(nullptr, result.clipToClip);

    result.isValid = true;
    return result;
}

// ============================================================================
// FrameWarp_Dx12 Implementation
// ============================================================================

FrameWarp_Dx12::~FrameWarp_Dx12()
{
    Shutdown();
}

bool FrameWarp_Dx12::Initialize(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format)
{
    if (_initialized) return true;
    if (device == nullptr) return false;

    _device = device;
    _device->AddRef();
    _outputWidth = width;
    _outputHeight = height;
    _outputFormat = format;
    _sourceFormat = format;

    LOG_INFO("FrameWarp_Dx12: Initializing ({}x{}, format={})", width, height, (UINT)format);

    if (!CreatePipelineStates())
    {
        LOG_ERROR("FrameWarp_Dx12: Failed to create pipeline states");
        Shutdown();
        return false;
    }

    if (!CreateConstantBuffers())
    {
        LOG_ERROR("FrameWarp_Dx12: Failed to create constant buffers");
        Shutdown();
        return false;
    }

    if (!CreateOutputResource(width, height, format))
    {
        LOG_ERROR("FrameWarp_Dx12: Failed to create output resource");
        Shutdown();
        return false;
    }

    for (size_t i = 0; i < NUM_HEAPS; i++)
    {
        if (!_frameHeaps[i].Initialize(device,
            FRAMEWARP_HEAP_SRV_COUNT,
            FRAMEWARP_HEAP_UAV_COUNT,
            FRAMEWARP_HEAP_CBV_COUNT))
        {
            LOG_ERROR("FrameWarp_Dx12: Failed to init heap {}", i);
            Shutdown();
            return false;
        }
    }

    _initialized = true;
    State::Instance().frameWarpStatus.available = true;

    LOG_INFO("FrameWarp_Dx12: Initialized successfully");
    return true;
}

bool FrameWarp_Dx12::CreateConstantBuffers()
{
    HRESULT hr = S_OK;

    for (size_t i = 0; i < NUM_HEAPS; i++)
    {
        {
            auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(FrameWarpShaderConstants));
            auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            hr = _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
                &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&_constantBuffers[i]));
            if (FAILED(hr))
            {
                LOG_ERROR("FrameWarp_Dx12: CreateCB[{}] error {:X}", i, hr);
                return false;
            }
        }

        {
            auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(FrameWarpMVConstants));
            auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            hr = _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
                &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&_mvConstantBuffers[i]));
            if (FAILED(hr))
            {
                LOG_ERROR("FrameWarp_Dx12: CreateMV CB[{}] error {:X}", i, hr);
                return false;
            }
        }
    }

    return true;
}

bool FrameWarp_Dx12::CreatePipelineStates()
{
    HRESULT hr = S_OK;

    // ---- Root signature for color warp (SRV[0..1] + UAV[0..1] + CBV[0]) ----
    {
        CD3DX12_DESCRIPTOR_RANGE1 descriptorRanges[] = {
            CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, FRAMEWARP_HEAP_SRV_COUNT, 0, 0),
            // FrameDescriptorHeap reserves two UAV descriptors so it can also
            // serve combined/debug paths. Keep this range width in sync with
            // that heap layout; otherwise the CBV is read from the wrong slot.
            CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, FRAMEWARP_HEAP_UAV_COUNT, 0, 0),
            CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, FRAMEWARP_HEAP_CBV_COUNT, 0, 0),
        };
        CD3DX12_ROOT_PARAMETER1 rootParameter {};
        rootParameter.InitAsDescriptorTable(std::size(descriptorRanges), descriptorRanges);

        CD3DX12_STATIC_SAMPLER_DESC samplerDesc(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc;
        rootSigDesc.Init_1_1(1, &rootParameter, 1, &samplerDesc);

        ID3DBlob* errorBlob = nullptr;
        ID3DBlob* signatureBlob = nullptr;
        hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signatureBlob, &errorBlob);
        if (FAILED(hr))
        {
            LOG_ERROR("FrameWarp_Dx12: SerializeRootSignature error {:X}", hr);
            if (errorBlob) errorBlob->Release();
            return false;
        }

        hr = _device->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(), IID_PPV_ARGS(&_rootSignature));
        if (signatureBlob) signatureBlob->Release();
        if (errorBlob) errorBlob->Release();
        if (FAILED(hr))
        {
            LOG_ERROR("FrameWarp_Dx12: CreateRootSignature error {:X}", hr);
            return false;
        }
    }

    // ---- Root signature for MV correction (SRV[0..1] + UAV[0..1] + CBV[0]) ----
    {
        CD3DX12_DESCRIPTOR_RANGE1 descriptorRanges[] = {
            CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, FRAMEWARP_HEAP_SRV_COUNT, 0, 0),
            CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, FRAMEWARP_HEAP_UAV_COUNT, 0, 0),
            CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, FRAMEWARP_HEAP_CBV_COUNT, 0, 0),
        };
        CD3DX12_ROOT_PARAMETER1 rootParameter {};
        rootParameter.InitAsDescriptorTable(std::size(descriptorRanges), descriptorRanges);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc;
        rootSigDesc.Init_1_1(1, &rootParameter, 0, nullptr);

        ID3DBlob* errorBlob = nullptr;
        ID3DBlob* signatureBlob = nullptr;
        hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signatureBlob, &errorBlob);
        if (FAILED(hr))
        {
            LOG_ERROR("FrameWarp_Dx12: SerializeRootSignature (MV) error {:X}", hr);
            if (errorBlob) errorBlob->Release();
            return false;
        }

        hr = _device->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(), IID_PPV_ARGS(&_rootSignatureMV));
        if (signatureBlob) signatureBlob->Release();
        if (errorBlob) errorBlob->Release();
        if (FAILED(hr))
        {
            LOG_ERROR("FrameWarp_Dx12: CreateRootSignature (MV) error {:X}", hr);
            return false;
        }
    }

    // ---- Root signature for combined warp + MV (SRV[0..1] + UAV[0..1] + CBV[0]) ----
    {
        CD3DX12_DESCRIPTOR_RANGE1 descriptorRanges[] = {
            CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, FRAMEWARP_HEAP_SRV_COUNT, 0, 0),
            CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, FRAMEWARP_HEAP_UAV_COUNT, 0, 0),
            CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, FRAMEWARP_HEAP_CBV_COUNT, 0, 0),
        };
        CD3DX12_ROOT_PARAMETER1 rootParameter {};
        rootParameter.InitAsDescriptorTable(std::size(descriptorRanges), descriptorRanges);

        CD3DX12_STATIC_SAMPLER_DESC samplerDesc(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc;
        rootSigDesc.Init_1_1(1, &rootParameter, 1, &samplerDesc);

        ID3DBlob* errorBlob = nullptr;
        ID3DBlob* signatureBlob = nullptr;
        hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signatureBlob, &errorBlob);
        if (FAILED(hr))
        {
            LOG_ERROR("FrameWarp_Dx12: SerializeRootSignature (combined) error {:X}", hr);
            if (errorBlob) errorBlob->Release();
            return false;
        }

        hr = _device->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(), IID_PPV_ARGS(&_rootSignatureCombined));
        if (signatureBlob) signatureBlob->Release();
        if (errorBlob) errorBlob->Release();
        if (FAILED(hr))
        {
            LOG_ERROR("FrameWarp_Dx12: CreateRootSignature (combined) error {:X}", hr);
            return false;
        }
    }

    // ---- Compile and create PSOs ----

    ID3DBlob* simpleBlob = FrameWarp_CompileShader(frameWarpSimpleCode.c_str(), "CSMain", "cs_5_0");
    if (!simpleBlob)
    {
        LOG_ERROR("FrameWarp_Dx12: Failed to compile simple warp shader");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC simplePso = {};
    simplePso.pRootSignature = _rootSignature;
    simplePso.CS = CD3DX12_SHADER_BYTECODE(simpleBlob);
    hr = _device->CreateComputePipelineState(&simplePso, IID_PPV_ARGS(&_pipelineStateSimple));
    simpleBlob->Release();
    if (FAILED(hr))
    {
        LOG_ERROR("FrameWarp_Dx12: CreatePSO (simple) error {:X}", hr);
        return false;
    }

    ID3DBlob* depthBlob = FrameWarp_CompileShader(frameWarpDepthInfillCode.c_str(), "CSMain", "cs_5_0");
    if (depthBlob)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC depthPso = {};
        depthPso.pRootSignature = _rootSignature;
        depthPso.CS = CD3DX12_SHADER_BYTECODE(depthBlob);
        hr = _device->CreateComputePipelineState(&depthPso, IID_PPV_ARGS(&_pipelineStateDepthAware));
        depthBlob->Release();
        if (FAILED(hr))
            LOG_WARN("FrameWarp_Dx12: CreatePSO (depth-aware) error {:X}", hr);
    }
    else
        LOG_WARN("FrameWarp_Dx12: Failed to compile depth-aware warp shader");

    ID3DBlob* stableUiBlob = FrameWarp_CompileShader(frameWarpStableUiCompositeCode.c_str(), "CSMain", "cs_5_0");
    if (stableUiBlob)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC stableUiPso = {};
        stableUiPso.pRootSignature = _rootSignature;
        stableUiPso.CS = CD3DX12_SHADER_BYTECODE(stableUiBlob);
        hr = _device->CreateComputePipelineState(&stableUiPso, IID_PPV_ARGS(&_pipelineStateStableUiComposite));
        stableUiBlob->Release();
        if (FAILED(hr))
            LOG_WARN("FrameWarp_Dx12: CreatePSO (stable UI composite) error {:X}", hr);
    }
    else
        LOG_WARN("FrameWarp_Dx12: Failed to compile stable UI composite shader");

    ID3DBlob* uiExtractBlob = FrameWarp_CompileShader(frameWarpUiLayerExtractCode.c_str(), "CSMain", "cs_5_0");
    if (uiExtractBlob)
    {
        strncpy_s(_uiLayerExtractPipelineReason, "create pending", _TRUNCATE);
        D3D12_COMPUTE_PIPELINE_STATE_DESC uiExtractPso = {};
        uiExtractPso.pRootSignature = _rootSignature;
        uiExtractPso.CS = CD3DX12_SHADER_BYTECODE(uiExtractBlob);
        hr = _device->CreateComputePipelineState(&uiExtractPso, IID_PPV_ARGS(&_pipelineStateUiLayerExtract));
        uiExtractBlob->Release();
        if (FAILED(hr))
        {
            strncpy_s(_uiLayerExtractPipelineReason, "create failed", _TRUNCATE);
            LOG_WARN("FrameWarp_Dx12: CreatePSO (UI layer extract) error {:X}", hr);
        }
        else
        {
            strncpy_s(_uiLayerExtractPipelineReason, "ready", _TRUNCATE);
        }
    }
    else
    {
        strncpy_s(_uiLayerExtractPipelineReason, "compile failed", _TRUNCATE);
        LOG_WARN("FrameWarp_Dx12: Failed to compile UI layer extract shader");
    }

    if (_pipelineStateUiLayerExtract == nullptr &&
        Config::Instance()->FrameWarpDLSSGMode.value_or_default() == 4 &&
        Config::Instance()->FGHUDFix.value_or_default())
    {
        LOG_ERROR("FrameWarp_Dx12: UI layer extract pipeline is mandatory for DLSSGMode=4 + HUDFix; reason={}",
            _uiLayerExtractPipelineReason);
    }

    ID3DBlob* uiCompositeBlob = FrameWarp_CompileShader(frameWarpUiLayerCompositeCode.c_str(), "CSMain", "cs_5_0");
    if (uiCompositeBlob)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC uiCompositePso = {};
        uiCompositePso.pRootSignature = _rootSignature;
        uiCompositePso.CS = CD3DX12_SHADER_BYTECODE(uiCompositeBlob);
        hr = _device->CreateComputePipelineState(&uiCompositePso, IID_PPV_ARGS(&_pipelineStateUiLayerComposite));
        uiCompositeBlob->Release();
        if (FAILED(hr))
            LOG_WARN("FrameWarp_Dx12: CreatePSO (UI layer composite) error {:X}", hr);
    }
    else
        LOG_WARN("FrameWarp_Dx12: Failed to compile UI layer composite shader");

    ID3DBlob* mvBlob = FrameWarp_CompileShader(frameWarpMVCorrectCode.c_str(), "CSMain", "cs_5_0");
    if (mvBlob)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC mvPso = {};
        mvPso.pRootSignature = _rootSignatureMV;
        mvPso.CS = CD3DX12_SHADER_BYTECODE(mvBlob);
        hr = _device->CreateComputePipelineState(&mvPso, IID_PPV_ARGS(&_pipelineStateMVCorrect));
        mvBlob->Release();
        if (FAILED(hr))
            LOG_WARN("FrameWarp_Dx12: CreatePSO (MV correct) error {:X}", hr);
    }
    else
        LOG_WARN("FrameWarp_Dx12: Failed to compile MV correction shader");

    ID3DBlob* combinedBlob = FrameWarp_CompileShader(frameWarpCombinedCode.c_str(), "CSMain", "cs_5_0");
    if (combinedBlob)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC combinedPso = {};
        combinedPso.pRootSignature = _rootSignatureCombined;
        combinedPso.CS = CD3DX12_SHADER_BYTECODE(combinedBlob);
        hr = _device->CreateComputePipelineState(&combinedPso, IID_PPV_ARGS(&_pipelineStateCombined));
        combinedBlob->Release();
        if (FAILED(hr))
            LOG_WARN("FrameWarp_Dx12: CreatePSO (combined) error {:X}", hr);
    }
    else
        LOG_WARN("FrameWarp_Dx12: Failed to compile combined warp+MV shader");

    return true;
}

bool FrameWarp_Dx12::CreateOutputResource(UINT width, UINT height, DXGI_FORMAT format)
{
    if (_warpedOutput)
    {
        _warpedOutput->Release();
        _warpedOutput = nullptr;
    }
    if (_warpedPresentOutput)
    {
        _warpedPresentOutput->Release();
        _warpedPresentOutput = nullptr;
    }
    if (_stableUiOutput)
    {
        _stableUiOutput->Release();
        _stableUiOutput = nullptr;
    }
    for (size_t i = 0; i < NUM_HEAPS; ++i)
    {
        if (_uiLayerCache[i])
        {
            _uiLayerCache[i]->Release();
            _uiLayerCache[i] = nullptr;
        }
        _uiLayerCacheStates[i] = D3D12_RESOURCE_STATE_COMMON;
        if (_uiCleanSceneCache[i])
        {
            _uiCleanSceneCache[i]->Release();
            _uiCleanSceneCache[i] = nullptr;
        }
        _uiCleanSceneCacheStates[i] = D3D12_RESOURCE_STATE_COMMON;
        _cachedUiLayers[i] = {};
    }
    for (auto& history : _depthInfillHistory)
    {
        if (history.color) { history.color->Release(); history.color = nullptr; }
        if (history.depth) { history.depth->Release(); history.depth = nullptr; }
        history = {};
    }
    _depthInfillHistoryIndex = 0;
    _lastHistoryHudlessSerial = 0;
    _lastHistoryDepthSerial = 0;

    DXGI_FORMAT sourceCompatibleFormat = TranslateTypelessFormats(format);
    DXGI_FORMAT requestedFormat = FrameWarpFallbackUavFormat(format);
    DXGI_FORMAT candidateFormats[] = {
        sourceCompatibleFormat,
        requestedFormat,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = E_FAIL;
    DXGI_FORMAT createdFormat = DXGI_FORMAT_UNKNOWN;

    for (DXGI_FORMAT candidate : candidateFormats)
    {
        if (candidate == DXGI_FORMAT_UNKNOWN)
            continue;
        if (_warpedOutput != nullptr)
            break;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = candidate;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        hr = _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
            &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&_warpedOutput));
        if (SUCCEEDED(hr))
        {
            createdFormat = candidate;
            break;
        }

        LOG_WARN("FrameWarp_Dx12: CreateOutput failed for format {} (hr={:X})", (UINT)candidate, hr);
    }

    if (_warpedOutput == nullptr)
    {
        LOG_ERROR("FrameWarp_Dx12: CreateOutput error {:X}", hr);
        return false;
    }

    D3D12_RESOURCE_DESC stableDesc = _warpedOutput->GetDesc();
    hr = _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &stableDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&_warpedPresentOutput));
    if (FAILED(hr) || _warpedPresentOutput == nullptr)
    {
        LOG_WARN("FrameWarp_Dx12: CreatePresentOutput error {:X}; FG presentColor warp disabled", hr);
    }
    else
    {
        _warpedPresentOutput->SetName(L"FrameWarp_WarpedPresentOutput");
    }

    hr = _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &stableDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&_stableUiOutput));
    if (FAILED(hr) || _stableUiOutput == nullptr)
    {
        LOG_WARN("FrameWarp_Dx12: CreateStableUiOutput error {:X}; UI-stable composite disabled", hr);
    }
    else
    {
        _stableUiOutput->SetName(L"FrameWarp_StableUiOutput");
    }

    _warpedOutput->SetName(L"FrameWarp_WarpedOutput");
    _outputWidth = width;
    _outputHeight = height;
    _sourceFormat = format;
    _outputFormat = createdFormat;
    if (createdFormat != format)
        LOG_WARN("FrameWarp_Dx12: Using fallback UAV output format {} for source format {}",
            (UINT)createdFormat, (UINT)format);
    return true;
}

bool FrameWarp_Dx12::CreateMVOutputResource(UINT mvWidth, UINT mvHeight, DXGI_FORMAT mvFormat)
{
    if (_correctedMVOutput)
    {
        _correctedMVOutput->Release();
        _correctedMVOutput = nullptr;
    }
    for (size_t i = 0; i < NUM_HEAPS; i++)
    {
        if (_distortionFieldOutputs[i])
        {
            _distortionFieldOutputs[i]->Release();
            _distortionFieldOutputs[i] = nullptr;
        }
        _distortionFieldStates[i] = D3D12_RESOURCE_STATE_COMMON;
        if (_streamlineDistortionFieldOutputs[i])
        {
            _streamlineDistortionFieldOutputs[i]->Release();
            _streamlineDistortionFieldOutputs[i] = nullptr;
        }
        _streamlineDistortionFieldStates[i] = D3D12_RESOURCE_STATE_COMMON;
    }
    _distortionFieldOutput = nullptr;
    _lastDistortionFieldState = D3D12_RESOURCE_STATE_COMMON;
    _streamlineDistortionFieldOutput = nullptr;
    _lastStreamlineDistortionFieldState = D3D12_RESOURCE_STATE_COMMON;
    if (_pipelineStateDistortionField)
    {
        _pipelineStateDistortionField->Release();
        _pipelineStateDistortionField = nullptr;
    }
    if (_pipelineStateStreamlineDistortionField)
    {
        _pipelineStateStreamlineDistortionField->Release();
        _pipelineStateStreamlineDistortionField = nullptr;
    }

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = mvWidth;
    texDesc.Height = mvHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = mvFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&_correctedMVOutput));
    if (FAILED(hr))
    {
        LOG_ERROR("FrameWarp_Dx12: CreateMVOutput error {:X}", hr);
        return false;
    }

    _correctedMVOutput->SetName(L"FrameWarp_CorrectedMV");
    _mvWidth = mvWidth;
    _mvHeight = mvHeight;
    _mvFormat = mvFormat;
    return true;
}

void FrameWarp_Dx12::AddInputBarriers(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* colorInput,
    ID3D12Resource* depthInput,
    ID3D12Resource* mvInput,
    D3D12_RESOURCE_STATES colorInputState,
    D3D12_RESOURCE_STATES depthInputState)
{
    // Transition input resources to NON_PIXEL_SHADER_RESOURCE for compute shader reads.
    // colorInputState: the actual state of the color input resource.
    //   - RENDER_TARGET: when called from FG path (color is the FG hudless buffer)
    //   - COMMON: when called from standalone path (backbuffer after implicit decay)
    //   - PRESENT: when called from standalone path (backbuffer before Present)
    // Using the wrong StateBefore causes a D3D12 debug layer error and can cause
    // device removal on strict drivers.
    std::vector<D3D12_RESOURCE_BARRIER> barriers;

    if (colorInput != nullptr)
    {
        // Skip barrier if the resource is already in a compatible state
        // COMMON implicitly promotes to NON_PIXEL_SHADER_RESOURCE on read access
        if (colorInputState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
            colorInputState != D3D12_RESOURCE_STATE_COMMON)
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = colorInput;
            barrier.Transition.StateBefore = colorInputState;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers.push_back(barrier);
        }
    }

    if (depthInput != nullptr)
    {
        if (depthInputState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
            depthInputState != D3D12_RESOURCE_STATE_COMMON)
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = depthInput;
            barrier.Transition.StateBefore = depthInputState;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers.push_back(barrier);
        }
    }

    if (mvInput != nullptr)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = mvInput;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers.push_back(barrier);
    }

    if (!barriers.empty())
        cmdList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
}

void FrameWarp_Dx12::Shutdown()
{
    if (_initialized)
        LOG_INFO("FrameWarp_Dx12: Shutting down");

    if (_pipelineStateSimple) { _pipelineStateSimple->Release(); _pipelineStateSimple = nullptr; }
    if (_pipelineStateDepthAware) { _pipelineStateDepthAware->Release(); _pipelineStateDepthAware = nullptr; }
    if (_pipelineStateMVCorrect) { _pipelineStateMVCorrect->Release(); _pipelineStateMVCorrect = nullptr; }
    if (_pipelineStateCombined) { _pipelineStateCombined->Release(); _pipelineStateCombined = nullptr; }
    if (_pipelineStateDistortionField) { _pipelineStateDistortionField->Release(); _pipelineStateDistortionField = nullptr; }
    if (_pipelineStateStreamlineDistortionField) { _pipelineStateStreamlineDistortionField->Release(); _pipelineStateStreamlineDistortionField = nullptr; }
    if (_pipelineStateDebugDisplacement) { _pipelineStateDebugDisplacement->Release(); _pipelineStateDebugDisplacement = nullptr; }
    if (_pipelineStateDistortionVis) { _pipelineStateDistortionVis->Release(); _pipelineStateDistortionVis = nullptr; }
    if (_pipelineStateStableUiComposite) { _pipelineStateStableUiComposite->Release(); _pipelineStateStableUiComposite = nullptr; }
    if (_pipelineStateUiLayerExtract) { _pipelineStateUiLayerExtract->Release(); _pipelineStateUiLayerExtract = nullptr; }
    if (_pipelineStateUiLayerComposite) { _pipelineStateUiLayerComposite->Release(); _pipelineStateUiLayerComposite = nullptr; }

    if (_rootSignature) { _rootSignature->Release(); _rootSignature = nullptr; }
    if (_rootSignatureMV) { _rootSignatureMV->Release(); _rootSignatureMV = nullptr; }
    if (_rootSignatureCombined) { _rootSignatureCombined->Release(); _rootSignatureCombined = nullptr; }

    for (size_t i = 0; i < NUM_HEAPS; i++)
    {
        if (_constantBuffers[i]) { _constantBuffers[i]->Release(); _constantBuffers[i] = nullptr; }
        if (_mvConstantBuffers[i]) { _mvConstantBuffers[i]->Release(); _mvConstantBuffers[i] = nullptr; }
    }

    if (_warpedOutput) { _warpedOutput->Release(); _warpedOutput = nullptr; }
    if (_warpedPresentOutput) { _warpedPresentOutput->Release(); _warpedPresentOutput = nullptr; }
    if (_stableUiOutput) { _stableUiOutput->Release(); _stableUiOutput = nullptr; }
    for (size_t i = 0; i < NUM_HEAPS; ++i)
    {
        if (_uiLayerCache[i]) { _uiLayerCache[i]->Release(); _uiLayerCache[i] = nullptr; }
        _uiLayerCacheStates[i] = D3D12_RESOURCE_STATE_COMMON;
        if (_uiCleanSceneCache[i]) { _uiCleanSceneCache[i]->Release(); _uiCleanSceneCache[i] = nullptr; }
        _uiCleanSceneCacheStates[i] = D3D12_RESOURCE_STATE_COMMON;
        _cachedUiLayers[i] = {};
    }
    if (_correctedMVOutput) { _correctedMVOutput->Release(); _correctedMVOutput = nullptr; }
    if (_standaloneInputCopy) { _standaloneInputCopy->Release(); _standaloneInputCopy = nullptr; }
    _standaloneInputCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
    if (_stableUiPresentCopy) { _stableUiPresentCopy->Release(); _stableUiPresentCopy = nullptr; }
    _stableUiPresentCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
    if (_hudlessInputCopy) { _hudlessInputCopy->Release(); _hudlessInputCopy = nullptr; }
    _hudlessInputCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
    _hudlessInputCopyValid = false;
    _hudlessInputCopyWidth = 0;
    _hudlessInputCopyHeight = 0;
    _hudlessInputCopyFormat = DXGI_FORMAT_UNKNOWN;
    if (_trackedDepthInput) { _trackedDepthInput->Release(); _trackedDepthInput = nullptr; }
    _trackedDepthInputState = D3D12_RESOURCE_STATE_COMMON;
    _trackedDepthInputValid = false;
    _trackedDepthInputWidth = 0;
    _trackedDepthInputHeight = 0;
    _trackedDepthInputFormat = DXGI_FORMAT_UNKNOWN;
    if (_trackedMotionVectorInput) { _trackedMotionVectorInput->Release(); _trackedMotionVectorInput = nullptr; }
    _trackedMotionVectorInputState = D3D12_RESOURCE_STATE_COMMON;
    _trackedMotionVectorInputValid = false;
    _trackedMotionVectorInputWidth = 0;
    _trackedMotionVectorInputHeight = 0;
    _trackedMotionVectorInputFormat = DXGI_FORMAT_UNKNOWN;
    _trackedMotionVectorScaleX = 0.0f;
    _trackedMotionVectorScaleY = 0.0f;
    _trackedMotionVectorScalePreMultiplied = false;
    _trackedMotionVectorSource[0] = '\0';
    if (_mvCalibrationReadback) { _mvCalibrationReadback->Release(); _mvCalibrationReadback = nullptr; }
    _mvCalibrationReadbackSize = 0;
    _mvCalibrationRowPitch = 0;
    _mvCalibrationWidth = 0;
    _mvCalibrationHeight = 0;
    _mvCalibrationFormat = DXGI_FORMAT_UNKNOWN;
    _mvCalibrationPending = false;
    _mvCalibrationCopyScheduled = false;
    _mvCalibrationFenceValue = 0;
    _mvCalibrationFrame = 0;
    if (_depthSnapshot) { _depthSnapshot->Release(); _depthSnapshot = nullptr; }
    _depthSnapshotState = D3D12_RESOURCE_STATE_COMMON;
    _depthSnapshotValid = false;
    _depthSnapshotInverted = false;
    _depthSnapshotWidth = 0;
    _depthSnapshotHeight = 0;
    _depthSnapshotSourceFormat = DXGI_FORMAT_UNKNOWN;
    _depthSnapshotCopyFormat = DXGI_FORMAT_UNKNOWN;
    _depthSnapshotSrvFormat = DXGI_FORMAT_UNKNOWN;
    _depthSnapshotSourceState = D3D12_RESOURCE_STATE_COMMON;
    _depthSnapshotSerial = 0;
    _depthSnapshotFrame = 0;
    _depthSnapshotSource[0] = '\0';
    _depthSnapshotReason[0] = '\0';
    for (auto& history : _depthInfillHistory)
    {
        if (history.color) { history.color->Release(); history.color = nullptr; }
        if (history.depth) { history.depth->Release(); history.depth = nullptr; }
        history = {};
    }
    _depthInfillHistoryIndex = 0;
    _hudlessInputCopySerial = 0;
    _lastHistoryHudlessSerial = 0;
    _lastHistoryDepthSerial = 0;
    _dlssgHudlessSubmitUiValid = false;
    _dlssgHudlessSubmitUiFrameID = 0;
    _dlssgHudlessSubmitUiFrame = 0;
    State::Instance().frameWarpStatus.dlssgHudlessSubmitReady = false;
    State::Instance().frameWarpStatus.dlssgHudlessSubmitFrameID = 0;
    State::Instance().frameWarpStatus.dlssgHudlessSubmitFrame = 0;
    State::Instance().frameWarpStatus.dlssgHudlessSubmitLastReason[0] = '\0';
    _pendingStableUi = {};
    _stableUiSerial = 0;
    for (size_t i = 0; i < NUM_HEAPS; i++)
    {
        if (_distortionFieldOutputs[i]) { _distortionFieldOutputs[i]->Release(); _distortionFieldOutputs[i] = nullptr; }
        _distortionFieldStates[i] = D3D12_RESOURCE_STATE_COMMON;
        if (_streamlineDistortionFieldOutputs[i]) { _streamlineDistortionFieldOutputs[i]->Release(); _streamlineDistortionFieldOutputs[i] = nullptr; }
        _streamlineDistortionFieldStates[i] = D3D12_RESOURCE_STATE_COMMON;
    }
    _distortionFieldOutput = nullptr;
    _lastDistortionFieldState = D3D12_RESOURCE_STATE_COMMON;
    _streamlineDistortionFieldOutput = nullptr;
    _lastStreamlineDistortionFieldState = D3D12_RESOURCE_STATE_COMMON;
    if (_distortionVisOutput) { _distortionVisOutput->Release(); _distortionVisOutput = nullptr; }

    for (size_t i = 0; i < NUM_HEAPS; i++)
        _frameHeaps[i].ReleaseHeaps();

    if (_standaloneCommandList) { _standaloneCommandList->Release(); _standaloneCommandList = nullptr; }
    if (_standaloneCommandAllocator) { _standaloneCommandAllocator->Release(); _standaloneCommandAllocator = nullptr; }
    if (_standaloneFence) { _standaloneFence->Release(); _standaloneFence = nullptr; }
    if (_standaloneFenceEvent) { CloseHandle(_standaloneFenceEvent); _standaloneFenceEvent = nullptr; }
    _standaloneFenceValue = 0;

    if (_device) { _device->Release(); _device = nullptr; }

    _initialized = false;
    State::Instance().frameWarpStatus.available = false;
    State::Instance().frameWarpStatus.lastWarpApplied = false;
    State::Instance().frameWarpDebugMaskReady = false;
    State::Instance().frameWarpDebugMaskResource = nullptr;
}

bool FrameWarp_Dx12::Resize(UINT width, UINT height, DXGI_FORMAT format)
{
    if (!_initialized || !_device) return false;
    WaitStandaloneFence();
    return CreateOutputResource(width, height, format);
}

bool FrameWarp_Dx12::ResizeMV(UINT mvWidth, UINT mvHeight, DXGI_FORMAT mvFormat)
{
    if (!_initialized || !_device) return false;
    return CreateMVOutputResource(mvWidth, mvHeight, mvFormat);
}

void FrameWarp_Dx12::OnFrameRenderStart(const XMMATRIX& viewProj)
{
    if (!_enabled) return;
    _mouseTracker.SnapshotRenderState();
    _cameraPredictor.RecordRenderCamera(viewProj);
}

FrameWarp_Dx12::PreparedWarp FrameWarp_Dx12::PrepareWarpForPresent(
    const char* contextLabel,
    bool reuseLastWarpResult)
{
    PreparedWarp prepared {};
    const char* logContext = (contextLabel != nullptr && contextLabel[0] != '\0') ? contextLabel : "unknown";

    _warpApplied = false;
    _mvCorrected = false;
    State::Instance().frameWarpStatus.lastWarpApplied = false;
    State::Instance().frameWarpStatus.lastPreparedPixelShift = 0.0f;
    State::Instance().frameWarpStatus.lastPreparedDeltaYaw = 0.0f;
    State::Instance().frameWarpStatus.lastPreparedDeltaPitch = 0.0f;
    State::Instance().frameWarpStatus.lastPreparedStatus[0] = '\0';
    State::Instance().frameWarpStatus.lastPreparedReason[0] = '\0';

    auto setReason = [&](PreparedWarpStatus status, const char* reason) {
        prepared.status = status;
        strncpy_s(prepared.reason, sizeof(prepared.reason), reason != nullptr ? reason : "unknown", _TRUNCATE);
        const char* statusName = status == PreparedWarpStatus::Ready
            ? "Ready"
            : (status == PreparedWarpStatus::ZeroPose ? "ZeroPose" : "Invalid");
        auto& frameWarpStatus = State::Instance().frameWarpStatus;
        strncpy_s(frameWarpStatus.lastPreparedStatus, statusName, _TRUNCATE);
        strncpy_s(frameWarpStatus.lastPreparedReason, prepared.reason, _TRUNCATE);
        FrameWarpSetSkipReason(prepared.reason);
    };

    auto logAudit = [&](const char* statusName) {
        static uint64_t auditCount = 0;
        auditCount++;
        if (!FrameWarpTimingAuditShouldLog(auditCount))
            return;

        auto& status = State::Instance().frameWarpStatus;
        LOG_INFO("VF2AuditPrepare #{} frame={} context={} status={} reason={} rawSeq={} snapSeq={} sinceSnap={} snapshots={} snapSrc={} rawSrc={} activeSrc={} rawSamples={} rawHz={:.1f} rawIntMs={:.3f}/{:.3f}/{:.3f} snapAgeMs={:.3f} sampleAgeMs={:.3f} mouseRaw=({:.1f},{:.1f}) mouseFinal=({:.1f},{:.1f}) step=({:.1f},{:.1f}) yaw={:.6f} pitch={:.6f} mag={:.6f} px={:.3f} reuse={} pred={} depthAge={:.0f} depthHist={} ui={} skip={}",
            auditCount,
            State::Instance().frameCount,
            logContext,
            statusName != nullptr ? statusName : "unknown",
            prepared.reason[0] ? prepared.reason : "none",
            status.rawInputSequence,
            status.rawInputSnapshotSequence,
            status.rawInputSamplesSinceSnapshot,
            status.renderSnapshotCount,
            status.lastSnapshotSource[0] ? status.lastSnapshotSource : "none",
            status.lastRawInputSource[0] ? status.lastRawInputSource : "none",
            status.activeRawInputSource[0] ? status.activeRawInputSource : "none",
            status.rawInputSampleCount,
            status.rawInputSampleHz,
            status.rawInputIntervalMeanMs,
            status.rawInputIntervalStdDevMs,
            status.rawInputIntervalMaxMs,
            status.lastSnapshotAgeMs,
            status.lastInputSampleAgeMs,
            status.lastMouseDeltaDx,
            status.lastMouseDeltaDy,
            status.lastFinalMouseDeltaDx,
            status.lastFinalMouseDeltaDy,
            status.lastFinalMouseDeltaStepDx,
            status.lastFinalMouseDeltaStepDy,
            prepared.warpResult.deltaYaw,
            prepared.warpResult.deltaPitch,
            prepared.magnitude,
            prepared.approxPixelShift,
            prepared.reusedLastResult,
            status.lastInputPredictionApplied,
            status.depthInfillAgeFrames,
            status.depthInfillHistoryUsed,
            status.lastStableUiSource[0] ? status.lastStableUiSource : "none",
            status.lastSkipReason[0] ? status.lastSkipReason : "none");
    };

    if (!_enabled || !_initialized)
    {
        setReason(PreparedWarpStatus::Invalid, !_enabled ? "disabled" : "not initialized");
        return prepared;
    }

    float deltaYaw = 0.0f;
    float deltaPitch = 0.0f;
    const bool useDLSSGResourceAnchor = strcmp(logContext, "dlssg-resource-copy") == 0;
    bool hasMouseDelta = useDLSSGResourceAnchor
        ? _mouseTracker.GetWarpDeltaFromDLSSGResourceAnchor(deltaYaw, deltaPitch)
        : _mouseTracker.GetWarpDelta(deltaYaw, deltaPitch);

    CameraPredictor::WarpResult warpResult {};
    bool reusedLast = false;

    if (reuseLastWarpResult && _lastWarpResult.isValid)
    {
        warpResult = _lastWarpResult;
        hasMouseDelta = true;
        reusedLast = true;
    }
    else if (hasMouseDelta)
    {
        warpResult = _cameraPredictor.PredictFromMouseDelta(
            deltaYaw, deltaPitch,
            _cameraPredictor._renderViewProj,
            _cameraPredictor._warpStrength);
    }
    else
    {
        auto& status = State::Instance().frameWarpStatus;
        static uint64_t noDeltaLogCount = 0;
        noDeltaLogCount++;
        if (Config::Instance()->FrameWarpPacingLog.value_or_default() ||
            noDeltaLogCount <= 10 || noDeltaLogCount % 120 == 0)
        {
            LOG_DEBUG("FrameWarp: {} - no mouse delta (snapshots={}, rawSamples={}, accum=({:.1f},{:.1f}), snapshot=({:.1f},{:.1f}), age={:.3f}ms, source={})",
                logContext, status.renderSnapshotCount, status.rawInputSampleCount,
                status.lastAccumDx, status.lastAccumDy,
                status.lastSnapshotDx, status.lastSnapshotDy,
                status.lastSnapshotAgeMs, status.lastSnapshotSource);
        }
        setReason(PreparedWarpStatus::ZeroPose, "no mouse delta");
        logAudit("ZeroPose");
        return prepared;
    }

    if (!warpResult.isValid)
    {
        static uint64_t invalidLogCount = 0;
        invalidLogCount++;
        if (Config::Instance()->FrameWarpPacingLog.value_or_default() ||
            invalidLogCount <= 10 || invalidLogCount % 120 == 0)
        {
            LOG_DEBUG("FrameWarp: {} - warp result invalid (hasRenderViewProj={}, hasMouseDelta={})",
                logContext, _cameraPredictor._hasRenderViewProj, hasMouseDelta);
        }
        setReason(PreparedWarpStatus::Invalid, "zero pose: invalid warp");
        logAudit("Invalid");
        return prepared;
    }

    _lastWarpResult = warpResult;

    const float warpMagnitude = std::abs(warpResult.deltaYaw) + std::abs(warpResult.deltaPitch);
    {
        auto& status = State::Instance().frameWarpStatus;
        const CameraContext camera = ResolveCameraContext();
        const float tanHalfFovY = std::max(std::tan(camera.vFovRadians * 0.5f), 0.0001f);
        const float tanHalfFovX = std::max(tanHalfFovY * camera.aspectRatio, 0.0001f);
        const float outputWidth = static_cast<float>(std::max<UINT>(_outputWidth, 1));
        const float outputHeight = static_cast<float>(std::max<UINT>(_outputHeight, 1));
        float shiftX = std::abs(std::tan(warpResult.deltaYaw) / tanHalfFovX) * outputWidth * 0.5f;
        float shiftY = std::abs(std::tan(warpResult.deltaPitch) / tanHalfFovY) * outputHeight * 0.5f;
        if (!std::isfinite(shiftX))
            shiftX = 0.0f;
        if (!std::isfinite(shiftY))
            shiftY = 0.0f;
        prepared.approxPixelShift = std::max(shiftX, shiftY);
        status.lastApproxPixelShift = prepared.approxPixelShift;
        status.lastPreparedPixelShift = prepared.approxPixelShift;
        status.lastPreparedDeltaYaw = warpResult.deltaYaw;
        status.lastPreparedDeltaPitch = warpResult.deltaPitch;
        status.lastDisocclusionPixels = prepared.approxPixelShift;
    }

    if (warpMagnitude < FRAMEWARP_MIN_WARP_MAGNITUDE)
    {
        auto& status = State::Instance().frameWarpStatus;
        static uint64_t tinyDeltaLogCount = 0;
        tinyDeltaLogCount++;
        if (Config::Instance()->FrameWarpPacingLog.value_or_default() ||
            tinyDeltaLogCount <= 10 || tinyDeltaLogCount % 120 == 0)
        {
            LOG_DEBUG("FrameWarp: {} - warp magnitude too small ({:.6f}), yaw={:.6f}, pitch={:.6f}, mouseDelta=({:.1f},{:.1f}), accum=({:.1f},{:.1f}), snapshot=({:.1f},{:.1f}), age={:.3f}ms, source={}, rawSamples={}, snapshots={}, reuse={}",
                logContext, warpMagnitude, warpResult.deltaYaw, warpResult.deltaPitch,
                status.lastMouseDeltaDx, status.lastMouseDeltaDy,
                status.lastAccumDx, status.lastAccumDy,
                status.lastSnapshotDx, status.lastSnapshotDy,
                status.lastSnapshotAgeMs, status.lastSnapshotSource,
                status.rawInputSampleCount, status.renderSnapshotCount,
                reuseLastWarpResult);
        }
        prepared.warpResult = warpResult;
        prepared.magnitude = warpMagnitude;
        prepared.approxPixelShift = State::Instance().frameWarpStatus.lastApproxPixelShift;
        prepared.reusedLastResult = reusedLast;
        setReason(PreparedWarpStatus::ZeroPose, "delta too small");
        logAudit("ZeroPose");
        return prepared;
    }

    const float minPixelShift = Config::Instance()->FrameWarpMinPixelShift.value_or_default();
    if (minPixelShift > 0.0f &&
        State::Instance().frameWarpStatus.lastApproxPixelShift < minPixelShift)
    {
        prepared.warpResult = warpResult;
        prepared.magnitude = warpMagnitude;
        prepared.approxPixelShift = State::Instance().frameWarpStatus.lastApproxPixelShift;
        prepared.reusedLastResult = reusedLast;
        setReason(PreparedWarpStatus::ZeroPose, "pixel shift too small");
        logAudit("ZeroPose");
        return prepared;
    }

    prepared.status = PreparedWarpStatus::Ready;
    prepared.warpResult = warpResult;
    prepared.magnitude = warpMagnitude;
    prepared.approxPixelShift = State::Instance().frameWarpStatus.lastApproxPixelShift;
    prepared.reusedLastResult = reusedLast;
    strncpy_s(prepared.reason, sizeof(prepared.reason), "ready", _TRUNCATE);
    strncpy_s(State::Instance().frameWarpStatus.lastPreparedStatus, "Ready", _TRUNCATE);
    strncpy_s(State::Instance().frameWarpStatus.lastPreparedReason, prepared.reason, _TRUNCATE);
    State::Instance().frameWarpStatus.lastSkipReason[0] = '\0';
    logAudit("Ready");
    return prepared;
}

FrameWarp_Dx12::PresentResult FrameWarp_Dx12::OnPrePresentEx(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* colorInput,
    ID3D12Resource* depthInput,
    ID3D12Resource* uiInput,
    ID3D12Resource** output,
    D3D12_RESOURCE_STATES colorInputState,
    ID3D12Resource* outputOverride,
    D3D12_RESOURCE_STATES depthInputState,
    const char* contextLabel,
    bool reuseLastWarpResult)
{
    const char* logContext = (contextLabel != nullptr && contextLabel[0] != '\0') ? contextLabel : "unknown";

    if (!_enabled || !_initialized || cmdList == nullptr || colorInput == nullptr)
    {
        _warpApplied = false;
        _mvCorrected = false;
        State::Instance().frameWarpStatus.lastWarpApplied = false;
        FrameWarpSetSkipReason(!_enabled ? "disabled" : (!_initialized ? "not initialized" : "missing input"));
        if (output) *output = colorInput;
        return PresentResult::Invalid;
    }

    const PreparedWarp prepared = PrepareWarpForPresent(logContext, reuseLastWarpResult);
    return OnPrePresentPrepared(
        cmdList, colorInput, depthInput, uiInput, output,
        colorInputState, outputOverride, depthInputState, logContext, prepared);
}

FrameWarp_Dx12::PresentResult FrameWarp_Dx12::OnPrePresentPrepared(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* colorInput,
    ID3D12Resource* depthInput,
    ID3D12Resource* uiInput,
    ID3D12Resource** output,
    D3D12_RESOURCE_STATES colorInputState,
    ID3D12Resource* outputOverride,
    D3D12_RESOURCE_STATES depthInputState,
    const char* contextLabel,
    const PreparedWarp& preparedWarp)
{
    _warpApplied = false;
    _mvCorrected = false;
    State::Instance().frameWarpStatus.lastWarpApplied = false;
    const char* logContext = (contextLabel != nullptr && contextLabel[0] != '\0') ? contextLabel : "unknown";

    if (!_enabled || !_initialized || cmdList == nullptr || colorInput == nullptr)
    {
        FrameWarpSetSkipReason(!_enabled ? "disabled" : (!_initialized ? "not initialized" : "missing input"));
        if (output) *output = colorInput;
        return PresentResult::Invalid;
    }

    if (preparedWarp.status != PreparedWarpStatus::Ready)
    {
        if (output) *output = colorInput;
        return preparedWarp.status == PreparedWarpStatus::Invalid
            ? PresentResult::Invalid
            : PresentResult::ZeroPose;
    }

    const CameraPredictor::WarpResult& warpResult = preparedWarp.warpResult;

    // ---- Dispatch warp shader ----
    ID3D12Resource* warpOutput = outputOverride != nullptr ? outputOverride : _warpedOutput;
    if (warpOutput == nullptr)
    {
        FrameWarpSetSkipReason("missing warp output");
        if (output) *output = colorInput;
        return PresentResult::Invalid;
    }

    _heapIndex = (_heapIndex + 1) % NUM_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_heapIndex];

    auto config = Config::Instance();
    auto& state = State::Instance();
    const bool depthConfigEnabledRaw = config->FrameWarpDepthAware.value_or_default();
    const bool depthConfigEnabled = depthConfigEnabledRaw;
    bool depthResourceValid = false;
    if (depthInput != nullptr)
    {
        auto depthDesc = depthInput->GetDesc();
        depthResourceValid =
            depthDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            depthDesc.Width > 0 &&
            depthDesc.Height > 0;
    }
    bool useDepthAware = depthResourceValid && _pipelineStateDepthAware != nullptr && depthConfigEnabled;
    FrameWarpSetDepthDiagnostics(
        depthInput,
        depthConfigEnabledRaw,
        _pipelineStateDepthAware != nullptr,
        useDepthAware);

    // Add input resource state transitions for compute shader reads.
    ID3D12Resource* depthInputForShader = useDepthAware ? depthInput : nullptr;
    const DXGI_FORMAT depthSrvFormat =
        (depthInputForShader != nullptr && depthInputForShader == _depthSnapshot)
            ? _depthSnapshotSrvFormat
            : (depthInputForShader != nullptr ? FrameWarpDepthSrvFormat(depthInputForShader->GetDesc().Format) : DXGI_FORMAT_UNKNOWN);
    if (useDepthAware && depthSrvFormat == DXGI_FORMAT_UNKNOWN)
    {
        useDepthAware = false;
        depthInputForShader = nullptr;
        FrameWarpSetDepthDiagnostics(depthInput, depthConfigEnabledRaw, _pipelineStateDepthAware != nullptr, false,
            "unsupported depth SRV");
    }
    const int historyIndex = useDepthAware ? FindDepthInfillHistory(_outputWidth, _outputHeight, colorInput->GetDesc().Format) : -1;
    ID3D12Resource* historyColor = historyIndex >= 0 ? _depthInfillHistory[historyIndex].color : nullptr;
    const bool historyAvailable = historyColor != nullptr;

    AddInputBarriers(cmdList, colorInput, depthInputForShader, nullptr, colorInputState, depthInputState);
    D3D12_SHADER_RESOURCE_VIEW_DESC colorSrvDesc = {};
    colorSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    colorSrvDesc.Format = TranslateTypelessFormats(colorInput->GetDesc().Format);
    colorSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    colorSrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(colorInput, &colorSrvDesc, currentHeap.GetSrvCPU(0));

    if (depthInputForShader != nullptr)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Format = depthSrvFormat;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Texture2D.MipLevels = 1;
        _device->CreateShaderResourceView(depthInputForShader, &depthSrvDesc, currentHeap.GetSrvCPU(1));
    }
    else
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC dummyDepthSrvDesc = {};
        dummyDepthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dummyDepthSrvDesc.Format = colorSrvDesc.Format;
        dummyDepthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        dummyDepthSrvDesc.Texture2D.MipLevels = 1;
        _device->CreateShaderResourceView(colorInput, &dummyDepthSrvDesc, currentHeap.GetSrvCPU(1));
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC historySrvDesc = {};
    historySrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    historySrvDesc.Format = historyAvailable ? TranslateTypelessFormats(historyColor->GetDesc().Format) : colorSrvDesc.Format;
    historySrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    historySrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(historyAvailable ? historyColor : colorInput, &historySrvDesc, currentHeap.GetSrvCPU(2));

    // UAV for warped output
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = TranslateTypelessFormats(warpOutput->GetDesc().Format);
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    _device->CreateUnorderedAccessView(warpOutput, nullptr, &uavDesc, currentHeap.GetUavCPU(0));

    // Update constant buffer (ring-buffered by _heapIndex)
    FrameWarpShaderConstants constants {};
    FillShaderConstants(constants, warpResult);
    constants.depthAware = useDepthAware ? 1 : 0;
    constants.depthInverted = (useDepthAware && _depthSnapshotInverted) ? 1 : 0;
    constants.infillHistoryAvailable = historyAvailable ? 1 : 0;
    const float approximateCoverage = _outputWidth > 0 && _outputHeight > 0
        ? std::clamp(
            ((State::Instance().frameWarpStatus.lastDisocclusionPixels * static_cast<float>(_outputHeight) +
              State::Instance().frameWarpStatus.lastDisocclusionPixels * static_cast<float>(_outputWidth)) /
             std::max(1.0f, static_cast<float>(_outputWidth * _outputHeight))) * 100.0f,
            0.0f, 100.0f)
        : 0.0f;
    constants.infillMaskCoverageEstimate = useDepthAware ? approximateCoverage : 0.0f;

    auto& depthStatus = State::Instance().frameWarpStatus;
    depthStatus.depthInfillHistoryAvailable = historyAvailable;
    depthStatus.depthInfillHistoryUsed = useDepthAware && historyAvailable;
    depthStatus.depthInfillMaskCoveragePct = constants.infillMaskCoverageEstimate;
    depthStatus.depthInfillAgeFrames =
        (_depthSnapshotValid && _depthSnapshotFrame <= State::Instance().frameCount)
            ? static_cast<float>(State::Instance().frameCount - _depthSnapshotFrame)
            : 0.0f;
    if (useDepthAware)
    {
        depthStatus.depthInfillCopySuccess = depthInputForShader == _depthSnapshot;
        depthStatus.depthInfillInverted = _depthSnapshotInverted;
        depthStatus.depthInfillSourceFormat = static_cast<uint32_t>(_depthSnapshotSourceFormat);
        depthStatus.depthInfillCopyFormat = static_cast<uint32_t>(_depthSnapshotCopyFormat);
        depthStatus.depthInfillSrvFormat = static_cast<uint32_t>(_depthSnapshotSrvFormat);
        depthStatus.depthInfillSourceState = static_cast<uint32_t>(_depthSnapshotSourceState);
        depthStatus.depthInfillWidth = _depthSnapshotWidth;
        depthStatus.depthInfillHeight = _depthSnapshotHeight;
        strncpy_s(depthStatus.depthInfillSource, _depthSnapshotSource[0] ? _depthSnapshotSource : logContext, _TRUNCATE);
        strncpy_s(depthStatus.depthInfillReason, historyAvailable ? "active+history" : "active", _TRUNCATE);
    }
    else if (depthConfigEnabledRaw)
    {
        depthStatus.depthInfillHistoryUsed = false;
        strncpy_s(depthStatus.depthInfillReason,
            depthInput == nullptr ? "no private depth snapshot" : "depth inactive",
            _TRUNCATE);
    }
    FrameWarpLogDepthInfillDiagnostics(logContext);

    BYTE* pCBDataBegin = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    auto result = _constantBuffers[_heapIndex]->Map(0, &readRange, reinterpret_cast<void**>(&pCBDataBegin));
    if (result != S_OK || pCBDataBegin == nullptr)
    {
        LOG_ERROR("FrameWarp_Dx12: Failed to map constant buffer");
        FrameWarpSetSkipReason("constant buffer map failed");
        if (output) *output = colorInput;
        return PresentResult::Invalid;
    }
    memcpy(pCBDataBegin, &constants, sizeof(constants));
    _constantBuffers[_heapIndex]->Unmap(0, nullptr);

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = _constantBuffers[_heapIndex]->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = sizeof(constants);
    _device->CreateConstantBufferView(&cbvDesc, currentHeap.GetCbvCPU(0));

    ID3D12PipelineState* pso = useDepthAware ? _pipelineStateDepthAware : _pipelineStateSimple;
    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(pso);
    cmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    UINT dispatchWidth = (_outputWidth + THREAD_GROUP_X - 1) / THREAD_GROUP_X;
    UINT dispatchHeight = (_outputHeight + THREAD_GROUP_Y - 1) / THREAD_GROUP_Y;
    cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    // UAV barrier
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = warpOutput;
    cmdList->ResourceBarrier(1, &uavBarrier);

    _warpApplied = true;
    _warpFrameCount++;
    FrameWarpSetApplied(warpResult);
    if (config->FrameWarpDebugViewMode.value_or_default() >= Config::FrameWarpDebugView_Mask)
    {
        ID3D12Resource* debugOutput = nullptr;
        GenerateDebugDisplacement(
            cmdList,
            colorInput,
            depthInputForShader,
            &debugOutput,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (output) *output = warpOutput;

    LOG_TRACE("FrameWarp applied: yaw={:.4f}, pitch={:.4f}, frames={}",
        warpResult.deltaYaw, warpResult.deltaPitch, _warpFrameCount);

    return PresentResult::Warped;
}

bool FrameWarp_Dx12::OnPrePresent(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* colorInput,
    ID3D12Resource* depthInput,
    ID3D12Resource* uiInput,
    ID3D12Resource** output,
    D3D12_RESOURCE_STATES colorInputState,
    ID3D12Resource* outputOverride,
    D3D12_RESOURCE_STATES depthInputState,
    const char* contextLabel,
    bool reuseLastWarpResult)
{
    return OnPrePresentEx(
        cmdList,
        colorInput,
        depthInput,
        uiInput,
        output,
        colorInputState,
        outputOverride,
        depthInputState,
        contextLabel,
        reuseLastWarpResult) == PresentResult::Warped;
}

bool FrameWarp_Dx12::CorrectMotionVectors(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* mvInput,
    ID3D12Resource* depthInput,
    float mvScaleX,
    float mvScaleY,
    UINT depthWidth,
    UINT depthHeight,
    ID3D12Resource** output)
{
    _mvCorrected = false;

    if (!_warpApplied || !_initialized || !_pipelineStateMVCorrect ||
        cmdList == nullptr || mvInput == nullptr)
    {
        if (output) *output = mvInput;
        return false;
    }

    auto mvDesc = mvInput->GetDesc();

    // Create or resize MV output if needed
    if (_correctedMVOutput == nullptr ||
        _mvWidth != (UINT)mvDesc.Width || _mvHeight != (UINT)mvDesc.Height ||
        _mvFormat != mvDesc.Format)
    {
        if (!CreateMVOutputResource((UINT)mvDesc.Width, (UINT)mvDesc.Height, mvDesc.Format))
        {
            if (output) *output = mvInput;
            return false;
        }
    }

    // Check if combined path is valid (MV and color at same resolution)
    _canUseCombinedPath = (_mvWidth == _outputWidth && _mvHeight == _outputHeight);

    // Reuse the same heap index from OnPrePresent within this frame.
    // _heapIndex is only incremented once per frame (in OnPrePresent) to ensure
    // proper double-buffering between frames. If we incremented here too,
    // with NUM_HEAPS=4 we get 2 frames of buffering (4 heaps / 2 increments
 // per frame = 2 frames), preventing CPU from overwriting CB while GPU reads.
    FrameDescriptorHeap& currentHeap = _frameHeaps[_heapIndex];

    // Add input resource state transitions for MV and depth
    AddInputBarriers(cmdList, nullptr, depthInput, mvInput);

    // SRV for MV input
    D3D12_SHADER_RESOURCE_VIEW_DESC mvSrvDesc = {};
    mvSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    mvSrvDesc.Format = TranslateTypelessFormats(mvDesc.Format);
    mvSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    mvSrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(mvInput, &mvSrvDesc, currentHeap.GetSrvCPU(0));

    // SRV for depth (if depth-aware mode)
    bool useDepthAware = (depthInput != nullptr &&
        Config::Instance()->FrameWarpDepthAware.value_or_default());

    if (useDepthAware && depthInput != nullptr)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Format = TranslateTypelessFormats(depthInput->GetDesc().Format);
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Texture2D.MipLevels = 1;
        _device->CreateShaderResourceView(depthInput, &depthSrvDesc, currentHeap.GetSrvCPU(1));
    }

    // UAV for corrected MV output
    D3D12_UNORDERED_ACCESS_VIEW_DESC mvUavDesc = {};
    mvUavDesc.Format = TranslateTypelessFormats(_mvFormat);
    mvUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    mvUavDesc.Texture2D.MipSlice = 0;
    _device->CreateUnorderedAccessView(_correctedMVOutput, nullptr, &mvUavDesc, currentHeap.GetUavCPU(0));

    // Update MV constant buffer (ring-buffered by _heapIndex)
    FrameWarpMVConstants mvConstants {};
    mvConstants.clipToClipWarp = _lastWarpResult.clipToClip;
    mvConstants.clipToClipWarpInv = _lastWarpResult.clipToClipInverse;
    mvConstants.mvWidth = _mvWidth;
    mvConstants.mvHeight = _mvHeight;
    mvConstants.depthWidth = depthWidth;
    mvConstants.depthHeight = depthHeight;
    mvConstants.mvScaleX = mvScaleX;
    mvConstants.mvScaleY = mvScaleY;
    mvConstants.depthAware = useDepthAware ? 1 : 0;

    BYTE* pMVDataBegin = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    auto result = _mvConstantBuffers[_heapIndex]->Map(0, &readRange,
        reinterpret_cast<void**>(&pMVDataBegin));
    if (result != S_OK || pMVDataBegin == nullptr)
    {
        LOG_ERROR("FrameWarp_Dx12: Failed to map MV constant buffer");
        if (output) *output = mvInput;
        return false;
    }
    memcpy(pMVDataBegin, &mvConstants, sizeof(mvConstants));
    _mvConstantBuffers[_heapIndex]->Unmap(0, nullptr);

    D3D12_CONSTANT_BUFFER_VIEW_DESC mvCbvDesc = {};
    mvCbvDesc.BufferLocation = _mvConstantBuffers[_heapIndex]->GetGPUVirtualAddress();
    mvCbvDesc.SizeInBytes = sizeof(mvConstants);
    _device->CreateConstantBufferView(&mvCbvDesc, currentHeap.GetCbvCPU(0));

    // Set MV correction pipeline
    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_rootSignatureMV);
    cmdList->SetPipelineState(_pipelineStateMVCorrect);
    cmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    // Dispatch
    UINT dispatchWidth = (_mvWidth + THREAD_GROUP_X - 1) / THREAD_GROUP_X;
    UINT dispatchHeight = (_mvHeight + THREAD_GROUP_Y - 1) / THREAD_GROUP_Y;
    cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    // UAV barrier
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = _correctedMVOutput;
    cmdList->ResourceBarrier(1, &uavBarrier);

    _mvCorrected = true;

    if (output) *output = _correctedMVOutput;

    LOG_TRACE("FrameWarp MV corrected: mvScale=({:.4f}, {:.4f}), depthSize={}x{}, mvSize={}x{}",
        mvScaleX, mvScaleY, depthWidth, depthHeight, _mvWidth, _mvHeight);

    return true;
}

void FrameWarp_Dx12::OnRawMouseInput(LONG dx, LONG dy, const char* source, int64_t timestampQpc)
{
    _mouseTracker.OnRawMouseInput(dx, dy, source, timestampQpc);
}

void FrameWarp_Dx12::SetWarpStrength(float strength)
{
    _cameraPredictor._warpStrength = std::clamp(strength, 0.0f, 1.0f);
    _cameraPredictor.SetWarpStrength(_cameraPredictor._warpStrength);
}

void FrameWarp_Dx12::SetSensitivity(float sensX, float sensY)
{
    _mouseTracker.SetSensitivity(sensX, sensY);
}

void FrameWarp_Dx12::SetDepthAware(bool depthAware)
{
    // Config is read at dispatch time from Config::Instance()->FrameWarpDepthAware
}

void FrameWarp_Dx12::SetCameraContext(float vFovRadians, float aspectRatio, const char* source)
{
    if (!FrameWarpValidVFov(vFovRadians) || !FrameWarpValidAspect(aspectRatio))
        return;

    _cameraContext.vFovRadians = vFovRadians;
    _cameraContext.aspectRatio = aspectRatio;
    _cameraContext.valid = true;
    strncpy_s(_cameraContext.source,
        source != nullptr && source[0] != '\0' ? source : "runtime",
        _TRUNCATE);
}

FrameWarp_Dx12::CameraContext FrameWarp_Dx12::ResolveCameraContext() const
{
    CameraContext camera = _cameraContext;
    if (!FrameWarpValidAspect(camera.aspectRatio))
    {
        camera.aspectRatio = _outputHeight > 0
            ? static_cast<float>(_outputWidth) / static_cast<float>(_outputHeight)
            : 16.0f / 9.0f;
    }

    if (camera.valid && FrameWarpValidVFov(camera.vFovRadians))
        return camera;

    auto config = Config::Instance();
    camera.valid = false;

    if (config->FsrVerticalFov.has_value() && config->FsrVerticalFov.value() > 0.0f)
    {
        camera.vFovRadians = std::clamp(config->FsrVerticalFov.value(), 1.0f, 179.0f) * (FRAMEWARP_PI / 180.0f);
        strncpy_s(camera.source, "config", _TRUNCATE);
    }
    else if (config->FsrHorizontalFov.value_or_default() > 0.0f)
    {
        float hFov = std::clamp(config->FsrHorizontalFov.value_or_default(), 1.0f, 179.0f) * (FRAMEWARP_PI / 180.0f);
        camera.vFovRadians = 2.0f * std::atan(std::tan(hFov * 0.5f) / std::max(camera.aspectRatio, 0.2f));
        strncpy_s(camera.source, "config", _TRUNCATE);
    }
    else
    {
        camera.vFovRadians = FRAMEWARP_DEFAULT_VFOV;
        strncpy_s(camera.source, "default", _TRUNCATE);
    }

    if (!FrameWarpValidVFov(camera.vFovRadians))
    {
        camera.vFovRadians = FRAMEWARP_DEFAULT_VFOV;
        strncpy_s(camera.source, "default", _TRUNCATE);
    }

    return camera;
}

void FrameWarp_Dx12::FillShaderConstants(
    FrameWarpShaderConstants& constants,
    const CameraPredictor::WarpResult& warpResult) const
{
    auto camera = ResolveCameraContext();
    float tanHalfFovY = std::tan(camera.vFovRadians * 0.5f);
    float tanHalfFovX = tanHalfFovY * camera.aspectRatio;
    if (!std::isfinite(tanHalfFovX) || tanHalfFovX <= 0.0001f)
        tanHalfFovX = std::tan(FRAMEWARP_DEFAULT_VFOV * 0.5f) * (16.0f / 9.0f);
    if (!std::isfinite(tanHalfFovY) || tanHalfFovY <= 0.0001f)
        tanHalfFovY = std::tan(FRAMEWARP_DEFAULT_VFOV * 0.5f);

    constants = {};
    constants.clipToClipWarp = warpResult.clipToClip;
    constants.clipToClipWarpInv = warpResult.clipToClipInverse;
    constants.width = _outputWidth;
    constants.height = _outputHeight;
    constants.warpStrength = 1.0f; // Strength is already baked into deltaYaw/deltaPitch.
    constants.depthAware = 0;
    constants.tanHalfFovX = tanHalfFovX;
    constants.tanHalfFovY = tanHalfFovY;
    constants.deltaYaw = warpResult.deltaYaw;
    constants.deltaPitch = warpResult.deltaPitch;
    constants.stableUiSceneHasUnderlay = 0;
    constants.depthInverted = _depthSnapshotInverted ? 1 : 0;
    constants.infillHistoryAvailable = 0;
    constants.infillMaskCoverageEstimate = 0.0f;

    UpdateFrameWarpDiagnostics(camera, warpResult);
}

void FrameWarp_Dx12::UpdateFrameWarpDiagnostics(
    const CameraContext& camera,
    const CameraPredictor::WarpResult& warpResult) const
{
    auto& status = State::Instance().frameWarpStatus;
    status.lastVFovRadians = camera.vFovRadians;
    status.lastAspectRatio = camera.aspectRatio;
    strncpy_s(status.lastFovSource, camera.source, _TRUNCATE);
    strncpy_s(status.lastCalibrationSource,
        Config::Instance()->FrameWarpAutoCalibration.value_or_default()
            ? "manual fallback"
            : "manual",
        _TRUNCATE);

    float tanHalfFovY = std::max(std::tan(camera.vFovRadians * 0.5f), 0.0001f);
    float tanHalfFovX = std::max(tanHalfFovY * camera.aspectRatio, 0.0001f);
    float shiftX = std::abs(std::tan(warpResult.deltaYaw) / tanHalfFovX) * static_cast<float>(_outputWidth) * 0.5f;
    float shiftY = std::abs(std::tan(warpResult.deltaPitch) / tanHalfFovY) * static_cast<float>(_outputHeight) * 0.5f;
    status.lastApproxPixelShift = std::max(shiftX, shiftY);
    status.lastDisocclusionPixels = status.lastApproxPixelShift;
}

// ============================================================================
// Distortion Field Generation
// ============================================================================

bool FrameWarp_Dx12::CreateDistortionFieldResource(UINT width, UINT height)
{
    for (size_t i = 0; i < NUM_HEAPS; i++)
    {
        if (_distortionFieldOutputs[i])
        {
            _distortionFieldOutputs[i]->Release();
            _distortionFieldOutputs[i] = nullptr;
        }
        _distortionFieldStates[i] = D3D12_RESOURCE_STATE_COMMON;
    }
    _distortionFieldOutput = nullptr;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = _distortionFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = S_OK;
    for (size_t i = 0; i < NUM_HEAPS; i++)
    {
        hr = _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
            &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&_distortionFieldOutputs[i]));

        if (FAILED(hr))
        {
            LOG_ERROR("FrameWarp_Dx12: CreateDistortionFieldResource[{}] error {:X}", i, hr);
            for (size_t j = 0; j < NUM_HEAPS; j++)
            {
                if (_distortionFieldOutputs[j])
                {
                    _distortionFieldOutputs[j]->Release();
                    _distortionFieldOutputs[j] = nullptr;
                }
                _distortionFieldStates[j] = D3D12_RESOURCE_STATE_COMMON;
            }
            return false;
        }

        wchar_t name[64] = {};
        swprintf_s(name, L"FrameWarp_DistortionField_%zu", i);
        _distortionFieldOutputs[i]->SetName(name);
        _distortionFieldStates[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    _currentDistortionFieldIndex = 0;
    _distortionFieldOutput = _distortionFieldOutputs[_currentDistortionFieldIndex];
    _lastDistortionFieldState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}

bool FrameWarp_Dx12::CreateStreamlineDistortionFieldResource(UINT width, UINT height)
{
    for (size_t i = 0; i < NUM_HEAPS; i++)
    {
        if (_streamlineDistortionFieldOutputs[i])
        {
            _streamlineDistortionFieldOutputs[i]->Release();
            _streamlineDistortionFieldOutputs[i] = nullptr;
        }
        _streamlineDistortionFieldStates[i] = D3D12_RESOURCE_STATE_COMMON;
    }
    _streamlineDistortionFieldOutput = nullptr;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = _streamlineDistortionFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = S_OK;
    for (size_t i = 0; i < NUM_HEAPS; i++)
    {
        hr = _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
            &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&_streamlineDistortionFieldOutputs[i]));

        if (FAILED(hr))
        {
            LOG_ERROR("FrameWarp_Dx12: CreateStreamlineDistortionFieldResource[{}] error {:X}", i, hr);
            for (size_t j = 0; j < NUM_HEAPS; j++)
            {
                if (_streamlineDistortionFieldOutputs[j])
                {
                    _streamlineDistortionFieldOutputs[j]->Release();
                    _streamlineDistortionFieldOutputs[j] = nullptr;
                }
                _streamlineDistortionFieldStates[j] = D3D12_RESOURCE_STATE_COMMON;
            }
            return false;
        }

        wchar_t name[80] = {};
        swprintf_s(name, L"FrameWarp_SL_DistortionField_%zu", i);
        _streamlineDistortionFieldOutputs[i]->SetName(name);
        _streamlineDistortionFieldStates[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    _currentStreamlineDistortionFieldIndex = 0;
    _streamlineDistortionFieldOutput = _streamlineDistortionFieldOutputs[_currentStreamlineDistortionFieldIndex];
    _lastStreamlineDistortionFieldState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}


// ============================================================================
// Debug Displacement Visualization
// ============================================================================

bool FrameWarp_Dx12::GenerateDebugDisplacement(
	ID3D12GraphicsCommandList* cmdList,
	ID3D12Resource* colorInput,
	ID3D12Resource* depthInput,
	ID3D12Resource** output,
	D3D12_RESOURCE_STATES colorInputState)
{
	if (!_enabled || !_initialized || cmdList == nullptr || colorInput == nullptr)
	{
		if (output) *output = colorInput;
		return false;
	}

	// Must have a valid warp result from this frame
	if (!_lastWarpResult.isValid)
	{
		if (output) *output = colorInput;
		return false;
	}

	// Compile debug displacement PSO if not yet created (deferred)
	if (_pipelineStateDebugDisplacement == nullptr)
		{
			ID3DBlob* debugBlob = FrameWarp_CompileShader(
			frameWarpDebugInfillMaskCode.c_str(), "CSMain", "cs_5_0");
		if (debugBlob)
		{
			D3D12_COMPUTE_PIPELINE_STATE_DESC debugPso = {};
			// Reuse the main warp root signature (same layout: SRV[0..1] + UAV[0] + CBV[0])
			debugPso.pRootSignature = _rootSignature;
			debugPso.CS = CD3DX12_SHADER_BYTECODE(debugBlob);
			HRESULT hr = _device->CreateComputePipelineState(&debugPso, IID_PPV_ARGS(&_pipelineStateDebugDisplacement));
			debugBlob->Release();
			if (FAILED(hr))
			{
				LOG_WARN("FrameWarp_Dx12: CreatePSO (debug displacement) error {:X}", hr);
				if (output) *output = colorInput;
				return false;
			}
			LOG_INFO("FrameWarp_Dx12: Debug displacement PSO created");
		}
		else
		{
			LOG_ERROR("FrameWarp_Dx12: Failed to compile debug displacement shader");
			if (output) *output = colorInput;
			return false;
		}
	}

	// Use the same heap index as the warp dispatch (same frame)
	// If called instead of OnPrePresent (debug mode replaces normal warp),
	// we need our own heap index increment.
	_heapIndex = (_heapIndex + 1) % NUM_HEAPS;
	FrameDescriptorHeap& currentHeap = _frameHeaps[_heapIndex];

	// Add input resource state transitions for compute shader reads.
	AddInputBarriers(cmdList, colorInput, depthInput, nullptr, colorInputState,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	// SRV[0]: color input
	D3D12_SHADER_RESOURCE_VIEW_DESC colorSrvDesc = {};
	colorSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	colorSrvDesc.Format = TranslateTypelessFormats(colorInput->GetDesc().Format);
	colorSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	colorSrvDesc.Texture2D.MipLevels = 1;
	_device->CreateShaderResourceView(colorInput, &colorSrvDesc, currentHeap.GetSrvCPU(0));

	// SRV[1]: depth input (or null if not available)
	if (depthInput != nullptr)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
		depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		DXGI_FORMAT depthSrvFormat = depthInput == _depthSnapshot
			? _depthSnapshotSrvFormat
			: FrameWarpDepthSrvFormat(depthInput->GetDesc().Format);
		depthSrvDesc.Format = depthSrvFormat != DXGI_FORMAT_UNKNOWN ? depthSrvFormat : DXGI_FORMAT_R32_FLOAT;
		depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		depthSrvDesc.Texture2D.MipLevels = 1;
		_device->CreateShaderResourceView(depthInput, &depthSrvDesc, currentHeap.GetSrvCPU(1));
	}
	else
	{
		// Null SRV for depth — shader won't read it when depthAware=0
		D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
		nullSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		nullSrvDesc.Texture2D.MipLevels = 1;
		// Point at color input as dummy — shader ignores t1 when depthAware=0
		_device->CreateShaderResourceView(colorInput, &nullSrvDesc, currentHeap.GetSrvCPU(1));
	}

	bool recreateDebugOutput = _distortionVisOutput == nullptr;
	if (_distortionVisOutput != nullptr)
	{
		auto debugDesc = _distortionVisOutput->GetDesc();
		recreateDebugOutput =
			debugDesc.Width != _outputWidth ||
			debugDesc.Height != _outputHeight ||
			debugDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM;
	}
	if (recreateDebugOutput)
	{
		if (_distortionVisOutput) { _distortionVisOutput->Release(); _distortionVisOutput = nullptr; }
		D3D12_RESOURCE_DESC debugDesc = {};
		debugDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		debugDesc.Width = _outputWidth;
		debugDesc.Height = _outputHeight;
		debugDesc.DepthOrArraySize = 1;
		debugDesc.MipLevels = 1;
		debugDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		debugDesc.SampleDesc.Count = 1;
		debugDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		debugDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		HRESULT hr = _device->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &debugDesc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_distortionVisOutput));
		if (FAILED(hr) || _distortionVisOutput == nullptr)
		{
			LOG_WARN("FrameWarp_Dx12: Create depth infill debug output failed {:X}", hr);
			if (output) *output = colorInput;
			return false;
		}
		_distortionVisOutput->SetName(L"FrameWarp_DepthInfillDebug");
	}

	// UAV[0]: standalone debug mask output
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Texture2D.MipSlice = 0;
	_device->CreateUnorderedAccessView(_distortionVisOutput, nullptr, &uavDesc, currentHeap.GetUavCPU(0));

	// Update constant buffer (reuse FrameWarpShaderConstants — same layout as WarpParams)
	FrameWarpShaderConstants constants {};
	FillShaderConstants(constants, _lastWarpResult);
	constants.depthAware = (depthInput != nullptr) ? 1 : 0;
	constants.depthInverted = _depthSnapshotInverted ? 1 : 0;

	BYTE* pCBDataBegin = nullptr;
	CD3DX12_RANGE readRange(0, 0);
	auto result = _constantBuffers[_heapIndex]->Map(0, &readRange, reinterpret_cast<void**>(&pCBDataBegin));
	if (result != S_OK || pCBDataBegin == nullptr)
	{
		LOG_ERROR("FrameWarp_Dx12: Failed to map debug displacement constant buffer");
		if (output) *output = colorInput;
		return false;
	}
	memcpy(pCBDataBegin, &constants, sizeof(constants));
	_constantBuffers[_heapIndex]->Unmap(0, nullptr);

	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	cbvDesc.BufferLocation = _constantBuffers[_heapIndex]->GetGPUVirtualAddress();
	cbvDesc.SizeInBytes = sizeof(constants);
	_device->CreateConstantBufferView(&cbvDesc, currentHeap.GetCbvCPU(0));

	// Dispatch debug displacement compute shader
	ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
	cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
	cmdList->SetComputeRootSignature(_rootSignature);
	cmdList->SetPipelineState(_pipelineStateDebugDisplacement);
	cmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

	UINT dispatchWidth = (_outputWidth + THREAD_GROUP_X - 1) / THREAD_GROUP_X;
	UINT dispatchHeight = (_outputHeight + THREAD_GROUP_Y - 1) / THREAD_GROUP_Y;
	cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

	// UAV barrier
	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = _distortionVisOutput;
	cmdList->ResourceBarrier(1, &uavBarrier);

	State::Instance().frameWarpDebugMaskReady = true;
	State::Instance().frameWarpDebugMaskResource = _distortionVisOutput;
	if (output) *output = _distortionVisOutput;
	LOG_TRACE("FrameWarp debug displacement rendered: yaw={:.4f}, pitch={:.4f}",
		_lastWarpResult.deltaYaw, _lastWarpResult.deltaPitch);
	return true;
}

// ============================================================================
// Distortion Field Generation
// ============================================================================

bool FrameWarp_Dx12::GenerateDistortionField(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource** output)
{
    if (!_enabled || !_initialized || cmdList == nullptr)
    {
        FrameWarpSetSkipReason(!_enabled ? "disabled" : (!_initialized ? "not initialized" : "missing command list"));
        if (output) *output = nullptr;
        return false;
    }

    // Sync sensitivity from config (in case user changed it via menu)
    float cfgSens = std::clamp(Config::Instance()->FrameWarpSensitivity.value_or_default(), 0.00005f, 0.005f);
    _mouseTracker.SetSensitivity(cfgSens, cfgSens);
    _cameraPredictor.SetWarpStrength(Config::Instance()->FrameWarpStrength.value_or_default());
    _cameraPredictor.SetMaxWarpAngle(
        std::clamp(Config::Instance()->FrameWarpMaxAngle.value_or_default(), 0.1f, 180.0f) * (FRAMEWARP_PI / 180.0f));

    // Compute warp matrix from mouse input (same as OnPrePresent)
    float deltaYaw, deltaPitch;
    bool hasMouseDelta = _mouseTracker.GetWarpDelta(deltaYaw, deltaPitch);

    CameraPredictor::WarpResult warpResult;
    if (hasMouseDelta)
    {
        warpResult = _cameraPredictor.PredictFromMouseDelta(
            deltaYaw, deltaPitch,
            _cameraPredictor._renderViewProj,
            _cameraPredictor._warpStrength);
    }
    else
    {
        // Fallback: velocity-based prediction
        std::scoped_lock lock(_mouseTracker._mutex);
        float deltaTime = static_cast<float>(
            MouseTracker::QpcToSeconds(
                MouseTracker::GetTimestampQpc() - _mouseTracker._renderMouseState.timestampQpc));
        if (deltaTime > 0.0f && deltaTime < 0.1f)
        {
            warpResult = _cameraPredictor.PredictFromVelocity(
                deltaTime, _cameraPredictor._renderViewProj, _cameraPredictor._warpStrength);
        }
        else
        {
            auto& status = State::Instance().frameWarpStatus;
            LOG_DEBUG("FrameWarp: Distortion field - no mouse delta and velocity prediction unavailable (snapshots={}, rawSamples={}, accum=({:.1f},{:.1f}), snapshot=({:.1f},{:.1f}), age={:.3f}ms, source={})",
                status.renderSnapshotCount, status.rawInputSampleCount,
                status.lastAccumDx, status.lastAccumDy,
                status.lastSnapshotDx, status.lastSnapshotDy,
                status.lastSnapshotAgeMs, status.lastSnapshotSource);
            FrameWarpSetSkipReason("no mouse delta");
            if (output) *output = nullptr;
            return false;
        }
    }

    if (!warpResult.isValid)
    {
        LOG_DEBUG("FrameWarp: Distortion field - warp result invalid");
        FrameWarpSetSkipReason("invalid warp");
        if (output) *output = nullptr;
        return false;
    }

    float warpMagnitude = std::abs(warpResult.deltaYaw) + std::abs(warpResult.deltaPitch);
    if (warpMagnitude < 0.00001f)
    {
        auto& status = State::Instance().frameWarpStatus;
        LOG_DEBUG("FrameWarp: Distortion field - magnitude too small ({:.6f}), yaw={:.6f}, pitch={:.6f}, mouseDelta=({:.1f},{:.1f}), accum=({:.1f},{:.1f}), snapshot=({:.1f},{:.1f}), age={:.3f}ms, source={}, rawSamples={}, snapshots={}",
            warpMagnitude, warpResult.deltaYaw, warpResult.deltaPitch,
            status.lastMouseDeltaDx, status.lastMouseDeltaDy,
            status.lastAccumDx, status.lastAccumDy,
            status.lastSnapshotDx, status.lastSnapshotDy,
            status.lastSnapshotAgeMs, status.lastSnapshotSource,
            status.rawInputSampleCount, status.renderSnapshotCount);
        FrameWarpSetSkipReason("delta too small");
        if (output) *output = nullptr;
        return false;
    }

    _lastWarpResult = warpResult;

    // Create or resize distortion field texture if needed
    if (_distortionFieldOutput == nullptr ||
        _outputWidth != _distortionFieldOutput->GetDesc().Width ||
        _outputHeight != _distortionFieldOutput->GetDesc().Height)
    {
        if (!CreateDistortionFieldResource(_outputWidth, _outputHeight))
        {
            FrameWarpSetSkipReason("distortion allocation failed");
            if (output) *output = nullptr;
            return false;
        }
    }

    // Compile distortion field PSO if not yet created
    // (deferred creation — only needed when FG is active)
    if (_pipelineStateDistortionField == nullptr)
    {
        ID3DBlob* distBlob = FrameWarp_CompileShader(
            frameWarpDistortionFieldCode.c_str(), "CSMain", "cs_5_0");
        if (distBlob)
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC distPso = {};
            // Reuse MV correction root signature (same layout: SRV[0..1] + UAV[0] + CBV[0])
            distPso.pRootSignature = _rootSignatureMV;
            distPso.CS = CD3DX12_SHADER_BYTECODE(distBlob);
            HRESULT hr = _device->CreateComputePipelineState(&distPso,
                IID_PPV_ARGS(&_pipelineStateDistortionField));
            distBlob->Release();
            if (FAILED(hr))
            {
                LOG_WARN("FrameWarp_Dx12: CreatePSO (distortion field) error {:X}", hr);
                FrameWarpSetSkipReason("distortion PSO failed");
                if (output) *output = nullptr;
                return false;
            }
        }
        else
        {
            LOG_ERROR("FrameWarp_Dx12: Failed to compile distortion field shader");
            FrameWarpSetSkipReason("distortion shader failed");
            if (output) *output = nullptr;
            return false;
        }
    }

    // Use the same heap index as OnPrePresent (ring-buffered)
    // Note: when GenerateDistortionField is called instead of OnPrePresent
    // (FG-active path), we still need to increment the heap index for
    // proper double-buffering.
    _heapIndex = (_heapIndex + 1) % NUM_HEAPS;
    _currentDistortionFieldIndex = _heapIndex;
    _distortionFieldOutput = _distortionFieldOutputs[_currentDistortionFieldIndex];
    _lastDistortionFieldState = _distortionFieldStates[_currentDistortionFieldIndex];
    FrameDescriptorHeap& currentHeap = _frameHeaps[_heapIndex];

    // SRV[0]: depth — not bound in V1 (homographic only), create null SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
    nullSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrvDesc.Texture2D.MipLevels = 1;
    // Create null SRV (point to a placeholder or just leave descriptor empty)
    // Actually, we need a valid descriptor. Use the depth SRV slot but with
    // a 1x1 dummy texture. For V1 (no depth), the shader won't read it.
    // The simplest approach: just create the SRV on the distortion field itself.
    // The shader only reads SourceDepth when depthAware != 0, which is always 0 in V1.
    D3D12_SHADER_RESOURCE_VIEW_DESC dummySrvDesc = {};
    dummySrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dummySrvDesc.Format = _distortionFormat;
    dummySrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    dummySrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(_distortionFieldOutput, &dummySrvDesc,
        currentHeap.GetSrvCPU(0));
    // SRV[1]: unused, same dummy
    _device->CreateShaderResourceView(_distortionFieldOutput, &dummySrvDesc,
        currentHeap.GetSrvCPU(1));

    // UAV[0]: distortion field output
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = _distortionFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    _device->CreateUnorderedAccessView(_distortionFieldOutput, nullptr, &uavDesc,
        currentHeap.GetUavCPU(0));

    // Update constant buffer (reuse FrameWarpShaderConstants layout)
    FrameWarpShaderConstants constants {};
    FillShaderConstants(constants, warpResult);
    constants.depthAware = 0;      // V1: homographic only

    BYTE* pCBDataBegin = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    auto result = _constantBuffers[_heapIndex]->Map(0, &readRange,
        reinterpret_cast<void**>(&pCBDataBegin));
    if (result != S_OK || pCBDataBegin == nullptr)
    {
        LOG_ERROR("FrameWarp_Dx12: Failed to map distortion field constant buffer");
        FrameWarpSetSkipReason("constant buffer map failed");
        if (output) *output = nullptr;
        return false;
    }
    memcpy(pCBDataBegin, &constants, sizeof(constants));
    _constantBuffers[_heapIndex]->Unmap(0, nullptr);

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = _constantBuffers[_heapIndex]->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = sizeof(constants);
    _device->CreateConstantBufferView(&cbvDesc, currentHeap.GetCbvCPU(0));

    // Dispatch distortion field compute shader
    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_rootSignatureMV);
    cmdList->SetPipelineState(_pipelineStateDistortionField);
    cmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    UINT dispatchWidth = (_outputWidth + THREAD_GROUP_X - 1) / THREAD_GROUP_X;
    UINT dispatchHeight = (_outputHeight + THREAD_GROUP_Y - 1) / THREAD_GROUP_Y;
    cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    // UAV barrier (ensure write completes before FG reads)
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = _distortionFieldOutput;
    cmdList->ResourceBarrier(1, &uavBarrier);

    // UAV barrier (ensure write completes before FG reads)
    // Leave resource in UAV. FSRFG's FfxApi backend manages compute-SRV
    // transitions internally during its dispatch.
    _lastDistortionFieldState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    _distortionFieldStates[_currentDistortionFieldIndex] = _lastDistortionFieldState;
    _warpFrameCount++;
    FrameWarpSetApplied(warpResult);

    if (output) *output = _distortionFieldOutput;

    LOG_TRACE("FrameWarp distortion field generated: yaw={:.4f}, pitch={:.4f}",
        warpResult.deltaYaw, warpResult.deltaPitch);

    return true;
}

bool FrameWarp_Dx12::GenerateStreamlineDistortionField(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource** output)
{
    if (!_enabled || !_initialized || cmdList == nullptr)
    {
        FrameWarpSetSkipReason(!_enabled ? "disabled" : (!_initialized ? "not initialized" : "missing command list"));
        if (output) *output = nullptr;
        return false;
    }

    float cfgSens = std::clamp(Config::Instance()->FrameWarpSensitivity.value_or_default(), 0.00005f, 0.005f);
    _mouseTracker.SetSensitivity(cfgSens, cfgSens);
    _cameraPredictor.SetWarpStrength(Config::Instance()->FrameWarpStrength.value_or_default());
    _cameraPredictor.SetMaxWarpAngle(
        std::clamp(Config::Instance()->FrameWarpMaxAngle.value_or_default(), 0.1f, 180.0f) * (FRAMEWARP_PI / 180.0f));

    float deltaYaw, deltaPitch;
    bool hasMouseDelta = _mouseTracker.GetWarpDelta(deltaYaw, deltaPitch);

    CameraPredictor::WarpResult warpResult;
    if (hasMouseDelta)
    {
        warpResult = _cameraPredictor.PredictFromMouseDelta(
            deltaYaw, deltaPitch,
            _cameraPredictor._renderViewProj,
            _cameraPredictor._warpStrength);
    }
    else
    {
        std::scoped_lock lock(_mouseTracker._mutex);
        float deltaTime = static_cast<float>(
            MouseTracker::QpcToSeconds(
                MouseTracker::GetTimestampQpc() - _mouseTracker._renderMouseState.timestampQpc));
        if (deltaTime > 0.0f && deltaTime < 0.1f)
        {
            warpResult = _cameraPredictor.PredictFromVelocity(
                deltaTime, _cameraPredictor._renderViewProj, _cameraPredictor._warpStrength);
        }
        else
        {
            auto& status = State::Instance().frameWarpStatus;
            LOG_DEBUG("FrameWarp: SL distortion field - no mouse delta and velocity prediction unavailable (snapshots={}, rawSamples={}, accum=({:.1f},{:.1f}), snapshot=({:.1f},{:.1f}), age={:.3f}ms, source={})",
                status.renderSnapshotCount, status.rawInputSampleCount,
                status.lastAccumDx, status.lastAccumDy,
                status.lastSnapshotDx, status.lastSnapshotDy,
                status.lastSnapshotAgeMs, status.lastSnapshotSource);
            FrameWarpSetSkipReason("no mouse delta");
            if (output) *output = nullptr;
            return false;
        }
    }

    if (!warpResult.isValid)
    {
        LOG_DEBUG("FrameWarp: SL distortion field - warp result invalid");
        FrameWarpSetSkipReason("invalid warp");
        if (output) *output = nullptr;
        return false;
    }

    float warpMagnitude = std::abs(warpResult.deltaYaw) + std::abs(warpResult.deltaPitch);
    if (warpMagnitude < 0.00001f)
    {
        auto& status = State::Instance().frameWarpStatus;
        LOG_DEBUG("FrameWarp: SL distortion field - magnitude too small ({:.6f}), yaw={:.6f}, pitch={:.6f}, mouseDelta=({:.1f},{:.1f}), accum=({:.1f},{:.1f}), snapshot=({:.1f},{:.1f}), age={:.3f}ms, source={}, rawSamples={}, snapshots={}",
            warpMagnitude, warpResult.deltaYaw, warpResult.deltaPitch,
            status.lastMouseDeltaDx, status.lastMouseDeltaDy,
            status.lastAccumDx, status.lastAccumDy,
            status.lastSnapshotDx, status.lastSnapshotDy,
            status.lastSnapshotAgeMs, status.lastSnapshotSource,
            status.rawInputSampleCount, status.renderSnapshotCount);
        FrameWarpSetSkipReason("delta too small");
        if (output) *output = nullptr;
        return false;
    }

    _lastWarpResult = warpResult;

    if (_streamlineDistortionFieldOutput == nullptr ||
        _outputWidth != _streamlineDistortionFieldOutput->GetDesc().Width ||
        _outputHeight != _streamlineDistortionFieldOutput->GetDesc().Height)
    {
        if (!CreateStreamlineDistortionFieldResource(_outputWidth, _outputHeight))
        {
            FrameWarpSetSkipReason("SL distortion allocation failed");
            if (output) *output = nullptr;
            return false;
        }
    }

    if (_pipelineStateStreamlineDistortionField == nullptr)
    {
        ID3DBlob* distBlob = FrameWarp_CompileShader(
            frameWarpStreamlineDistortionFieldCode.c_str(), "CSMain", "cs_5_0");
        if (distBlob)
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC distPso = {};
            distPso.pRootSignature = _rootSignatureMV;
            distPso.CS = CD3DX12_SHADER_BYTECODE(distBlob);
            HRESULT hr = _device->CreateComputePipelineState(&distPso,
                IID_PPV_ARGS(&_pipelineStateStreamlineDistortionField));
            distBlob->Release();
            if (FAILED(hr))
            {
                LOG_WARN("FrameWarp_Dx12: CreatePSO (SL distortion field) error {:X}", hr);
                FrameWarpSetSkipReason("SL distortion PSO failed");
                if (output) *output = nullptr;
                return false;
            }
        }
        else
        {
            LOG_ERROR("FrameWarp_Dx12: Failed to compile SL distortion field shader");
            FrameWarpSetSkipReason("SL distortion shader failed");
            if (output) *output = nullptr;
            return false;
        }
    }

    _heapIndex = (_heapIndex + 1) % NUM_HEAPS;
    _currentStreamlineDistortionFieldIndex = _heapIndex;
    _streamlineDistortionFieldOutput = _streamlineDistortionFieldOutputs[_currentStreamlineDistortionFieldIndex];
    _lastStreamlineDistortionFieldState = _streamlineDistortionFieldStates[_currentStreamlineDistortionFieldIndex];
    FrameDescriptorHeap& currentHeap = _frameHeaps[_heapIndex];

    D3D12_SHADER_RESOURCE_VIEW_DESC dummySrvDesc = {};
    dummySrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dummySrvDesc.Format = _streamlineDistortionFormat;
    dummySrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    dummySrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(_streamlineDistortionFieldOutput, &dummySrvDesc, currentHeap.GetSrvCPU(0));
    _device->CreateShaderResourceView(_streamlineDistortionFieldOutput, &dummySrvDesc, currentHeap.GetSrvCPU(1));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = _streamlineDistortionFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    _device->CreateUnorderedAccessView(_streamlineDistortionFieldOutput, nullptr, &uavDesc, currentHeap.GetUavCPU(0));

    FrameWarpShaderConstants constants {};
    FillShaderConstants(constants, warpResult);
    constants.depthAware = 0;

    BYTE* pCBDataBegin = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    auto result = _constantBuffers[_heapIndex]->Map(0, &readRange,
        reinterpret_cast<void**>(&pCBDataBegin));
    if (result != S_OK || pCBDataBegin == nullptr)
    {
        LOG_ERROR("FrameWarp_Dx12: Failed to map SL distortion field constant buffer");
        FrameWarpSetSkipReason("SL constant buffer map failed");
        if (output) *output = nullptr;
        return false;
    }
    memcpy(pCBDataBegin, &constants, sizeof(constants));
    _constantBuffers[_heapIndex]->Unmap(0, nullptr);

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = _constantBuffers[_heapIndex]->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = sizeof(constants);
    _device->CreateConstantBufferView(&cbvDesc, currentHeap.GetCbvCPU(0));

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_rootSignatureMV);
    cmdList->SetPipelineState(_pipelineStateStreamlineDistortionField);
    cmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    UINT dispatchWidth = (_outputWidth + THREAD_GROUP_X - 1) / THREAD_GROUP_X;
    UINT dispatchHeight = (_outputHeight + THREAD_GROUP_Y - 1) / THREAD_GROUP_Y;
    cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = _streamlineDistortionFieldOutput;
    cmdList->ResourceBarrier(1, &uavBarrier);

    _lastStreamlineDistortionFieldState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    _streamlineDistortionFieldStates[_currentStreamlineDistortionFieldIndex] = _lastStreamlineDistortionFieldState;
    _warpFrameCount++;
    FrameWarpSetApplied(warpResult);

    if (output) *output = _streamlineDistortionFieldOutput;

    LOG_TRACE("FrameWarp SL bidirectional distortion field generated: yaw={:.4f}, pitch={:.4f}",
        warpResult.deltaYaw, warpResult.deltaPitch);

    return true;
}


bool FrameWarp_Dx12::GenerateDistortionVis(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource** output)
{
    if (!_enabled || !_initialized || cmdList == nullptr)
    {
        if (output) *output = nullptr;
        return false;
    }

    // Must have a distortion field output from this frame
    if (_distortionFieldOutput == nullptr)
    {
        if (output) *output = nullptr;
        return false;
    }

    // Create the vis output texture if not yet created
    if (_distortionVisOutput == nullptr)
    {
        D3D12_RESOURCE_DESC visDesc = {};
        visDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        visDesc.Width = _outputWidth;
        visDesc.Height = _outputHeight;
        visDesc.DepthOrArraySize = 1;
        visDesc.MipLevels = 1;
        visDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        visDesc.SampleDesc.Count = 1;
        visDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        visDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        HRESULT hr = _device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &visDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, IID_PPV_ARGS(&_distortionVisOutput));

        if (FAILED(hr))
        {
            LOG_ERROR("FrameWarp_Dx12: Failed to create distortion vis output (hr={:X})", hr);
            if (output) *output = nullptr;
            return false;
        }
        LOG_INFO("FrameWarp_Dx12: Created distortion vis output ({}x{})", _outputWidth, _outputHeight);
    }

    // Compile the vis PSO if not yet created (deferred)
    if (_pipelineStateDistortionVis == nullptr)
    {
        ID3DBlob* visBlob = FrameWarp_CompileShader(
            frameWarpDistortionVisCode.c_str(), "CSMain", "cs_5_0");
        if (visBlob)
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC visPso = {};
            visPso.pRootSignature = _rootSignature;
            visPso.CS = CD3DX12_SHADER_BYTECODE(visBlob);
            HRESULT hr = _device->CreateComputePipelineState(&visPso, IID_PPV_ARGS(&_pipelineStateDistortionVis));
            visBlob->Release();
            if (FAILED(hr))
            {
                LOG_WARN("FrameWarp_Dx12: CreatePSO (distortion vis) error {:X}", hr);
                if (output) *output = nullptr;
                return false;
            }
            LOG_INFO("FrameWarp_Dx12: Distortion vis PSO created");
        }
        else
        {
            LOG_ERROR("FrameWarp_Dx12: Failed to compile distortion vis shader");
            if (output) *output = nullptr;
            return false;
        }
    }

    // Distortion vis uses a separate heap from the distortion field dispatch
    // because both write to the same descriptor slots (SRV[0], UAV[0], CBV[0]).
    // If they shared a heap, the second dispatch would overwrite the first's
    // descriptors, and the GPU would read the wrong descriptors at execution time.
    _heapIndex = (_heapIndex + 1) % NUM_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_heapIndex];

    // Transition distortion field to SRV for reading
    D3D12_RESOURCE_BARRIER distBarrier = {};
    distBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    distBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    distBarrier.Transition.pResource = _distortionFieldOutput;
    distBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    distBarrier.Transition.StateBefore = _lastDistortionFieldState;
    distBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmdList->ResourceBarrier(1, &distBarrier);
    _lastDistortionFieldState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    _distortionFieldStates[_currentDistortionFieldIndex] = _lastDistortionFieldState;

    // SRV[0]: distortion field input
    D3D12_SHADER_RESOURCE_VIEW_DESC distSrvDesc = {};
    distSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    distSrvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    distSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    distSrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(_distortionFieldOutput, &distSrvDesc, currentHeap.GetSrvCPU(0));

    // SRV[1]: null (not used by this shader)
    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
    nullSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(_distortionFieldOutput, &nullSrvDesc, currentHeap.GetSrvCPU(1));

    // UAV[0]: vis output
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    _device->CreateUnorderedAccessView(_distortionVisOutput, nullptr, &uavDesc, currentHeap.GetUavCPU(0));

    // Update constant buffer for distortion vis shader.
    // The shader's cbuffer (DistortionVisParams) has a compact layout:
    //   uint width, uint height, float maxDisplacement, uint pad
    // We must match this layout exactly - NOT reuse FrameWarpShaderConstants
    // which has XMMATRIX fields at offset 0.
    struct DistortionVisConstants {
        uint32_t width; // offset 0
        uint32_t height; // offset 4
        float maxDisplacement; // offset 8
        uint32_t pad; // offset 12
    };
    static_assert(sizeof(DistortionVisConstants) <= sizeof(FrameWarpShaderConstants));

    DistortionVisConstants constants {};
    constants.width = _outputWidth;
    constants.height = _outputHeight;
    constants.maxDisplacement = 20.0f; // Max pixels for visualization scale
    constants.pad = 0;

    BYTE* pCBDataBegin = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    auto result = _constantBuffers[_heapIndex]->Map(0, &readRange, reinterpret_cast<void**>(&pCBDataBegin));
    if (result != S_OK || pCBDataBegin == nullptr)
    {
        LOG_ERROR("FrameWarp_Dx12: Failed to map distortion vis constant buffer");
        if (output) *output = nullptr;
        return false;
    }
    memcpy(pCBDataBegin, &constants, sizeof(constants));
    _constantBuffers[_heapIndex]->Unmap(0, nullptr);

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = _constantBuffers[_heapIndex]->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = sizeof(FrameWarpShaderConstants); // Must be 256-byte aligned, same size
    _device->CreateConstantBufferView(&cbvDesc, currentHeap.GetCbvCPU(0));

    // Dispatch
    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_pipelineStateDistortionVis);
    cmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    UINT dispatchWidth = (_outputWidth + THREAD_GROUP_X - 1) / THREAD_GROUP_X;
    UINT dispatchHeight = (_outputHeight + THREAD_GROUP_Y - 1) / THREAD_GROUP_Y;
    cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    // UAV barrier
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = _distortionVisOutput;
    cmdList->ResourceBarrier(1, &uavBarrier);

    // Transition distortion field back to UAV so it's ready for next frame
    D3D12_RESOURCE_BARRIER distBarrierBack = {};
    distBarrierBack.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    distBarrierBack.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    distBarrierBack.Transition.pResource = _distortionFieldOutput;
    distBarrierBack.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    distBarrierBack.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    distBarrierBack.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmdList->ResourceBarrier(1, &distBarrierBack);
    _lastDistortionFieldState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    _distortionFieldStates[_currentDistortionFieldIndex] = _lastDistortionFieldState;

    if (output) *output = _distortionVisOutput;
    LOG_TRACE("FrameWarp distortion vis rendered");
    return true;
}

bool FrameWarp_Dx12::EnsureStableUiPresentCopy(const D3D12_RESOURCE_DESC& desc)
{
    bool recreate = (_stableUiPresentCopy == nullptr);
    if (_stableUiPresentCopy != nullptr)
    {
        auto copyDesc = _stableUiPresentCopy->GetDesc();
        recreate =
            copyDesc.Width != desc.Width ||
            copyDesc.Height != desc.Height ||
            copyDesc.Format != desc.Format ||
            copyDesc.SampleDesc.Count != desc.SampleDesc.Count;
    }

    if (!recreate)
        return true;

    if (_stableUiPresentCopy != nullptr)
    {
        _stableUiPresentCopy->Release();
        _stableUiPresentCopy = nullptr;
    }

    D3D12_RESOURCE_DESC copyDesc = desc;
    copyDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &copyDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&_stableUiPresentCopy));
    if (FAILED(hr) || _stableUiPresentCopy == nullptr)
    {
        LOG_ERROR("FrameWarp_Dx12: Create stable UI present copy failed {:X}", hr);
        _stableUiPresentCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
        return false;
    }

    _stableUiPresentCopy->SetName(L"FrameWarp_StableUiPresentCopy");
    _stableUiPresentCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
    return true;
}

bool FrameWarp_Dx12::EnsureHudlessInputCopy(const D3D12_RESOURCE_DESC& desc)
{
    bool recreate = (_hudlessInputCopy == nullptr);
    if (_hudlessInputCopy != nullptr)
    {
        auto copyDesc = _hudlessInputCopy->GetDesc();
        recreate =
            copyDesc.Width != desc.Width ||
            copyDesc.Height != desc.Height ||
            copyDesc.Format != desc.Format ||
            copyDesc.SampleDesc.Count != desc.SampleDesc.Count;
    }

    if (!recreate)
        return true;

    if (_hudlessInputCopy != nullptr)
    {
        _hudlessInputCopy->Release();
        _hudlessInputCopy = nullptr;
    }

    D3D12_RESOURCE_DESC copyDesc = desc;
    copyDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &copyDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&_hudlessInputCopy));
    if (FAILED(hr) || _hudlessInputCopy == nullptr)
    {
        LOG_ERROR("FrameWarp_Dx12: Create standalone hudless copy failed {:X}", hr);
        _hudlessInputCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
        _hudlessInputCopyValid = false;
        return false;
    }

    _hudlessInputCopy->SetName(L"FrameWarp_HudlessInputCopy");
    _hudlessInputCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
    _hudlessInputCopyValid = false;
    return true;
}

bool FrameWarp_Dx12::EnsureUiLayerCacheResources(const D3D12_RESOURCE_DESC& desc)
{
    if (_device == nullptr)
        return false;

    D3D12_RESOURCE_DESC layerDesc = {};
    layerDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    layerDesc.Width = desc.Width;
    layerDesc.Height = desc.Height;
    layerDesc.DepthOrArraySize = 1;
    layerDesc.MipLevels = 1;
    layerDesc.Format = _stableUiOutput != nullptr ? _stableUiOutput->GetDesc().Format : FrameWarpFallbackUavFormat(desc.Format);
    layerDesc.SampleDesc.Count = 1;
    layerDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    layerDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    for (size_t i = 0; i < NUM_HEAPS; ++i)
    {
        bool recreate = (_uiLayerCache[i] == nullptr);
        if (_uiLayerCache[i] != nullptr)
        {
            auto existing = _uiLayerCache[i]->GetDesc();
            recreate =
                existing.Width != layerDesc.Width ||
                existing.Height != layerDesc.Height ||
                existing.Format != layerDesc.Format ||
                existing.SampleDesc.Count != layerDesc.SampleDesc.Count;
        }

        if (!recreate)
            continue;

        if (_uiLayerCache[i] != nullptr)
        {
            _uiLayerCache[i]->Release();
            _uiLayerCache[i] = nullptr;
        }

        HRESULT hr = _device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &layerDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&_uiLayerCache[i]));
        if (FAILED(hr) || _uiLayerCache[i] == nullptr)
        {
            LOG_WARN("FrameWarp_Dx12: Create UI layer cache[{}] failed {:X}; stable UI cache unavailable", i, hr);
            _uiLayerCacheStates[i] = D3D12_RESOURCE_STATE_COMMON;
            _cachedUiLayers[i] = {};
            return false;
        }

        wchar_t name[64] = {};
        swprintf_s(name, L"FrameWarp_UiLayerCache_%zu", i);
        _uiLayerCache[i]->SetName(name);
        _uiLayerCacheStates[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        _cachedUiLayers[i] = {};
    }

    return true;
}

bool FrameWarp_Dx12::EnsureUiCleanSceneCacheResources(const D3D12_RESOURCE_DESC& desc)
{
    if (_device == nullptr)
        return false;

    D3D12_RESOURCE_DESC cleanDesc = desc;
    cleanDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    for (size_t i = 0; i < NUM_HEAPS; ++i)
    {
        bool recreate = (_uiCleanSceneCache[i] == nullptr);
        if (_uiCleanSceneCache[i] != nullptr)
        {
            auto existing = _uiCleanSceneCache[i]->GetDesc();
            recreate =
                existing.Width != cleanDesc.Width ||
                existing.Height != cleanDesc.Height ||
                existing.Format != cleanDesc.Format ||
                existing.SampleDesc.Count != cleanDesc.SampleDesc.Count;
        }

        if (!recreate)
            continue;

        if (_uiCleanSceneCache[i] != nullptr)
        {
            _uiCleanSceneCache[i]->Release();
            _uiCleanSceneCache[i] = nullptr;
        }

        HRESULT hr = _device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &cleanDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&_uiCleanSceneCache[i]));
        if (FAILED(hr) || _uiCleanSceneCache[i] == nullptr)
        {
            LOG_WARN("FrameWarp_Dx12: Create UI clean-scene cache[{}] failed {:X}; generated UI repair unavailable", i, hr);
            _uiCleanSceneCacheStates[i] = D3D12_RESOURCE_STATE_COMMON;
            _cachedUiLayers[i].cleanSceneValid = false;
            return false;
        }

        wchar_t name[72] = {};
        swprintf_s(name, L"FrameWarp_UiCleanSceneCache_%zu", i);
        _uiCleanSceneCache[i]->SetName(name);
        _uiCleanSceneCacheStates[i] = D3D12_RESOURCE_STATE_COPY_DEST;
        _cachedUiLayers[i].cleanSceneValid = false;
    }

    return true;
}

int FrameWarp_Dx12::FindCachedUiLayer(UINT64 frameID, UINT64 maxFrameAge) const
{
    int bestIndex = -1;
    UINT64 bestAge = UINT64_MAX;

    for (int i = 0; i < static_cast<int>(NUM_HEAPS); ++i)
    {
        if (!_cachedUiLayers[i].valid || _uiLayerCache[i] == nullptr)
            continue;
        if (_cachedUiLayers[i].frameID > frameID)
            continue;

        UINT64 age = frameID - _cachedUiLayers[i].frameID;
        if (age <= maxFrameAge && age < bestAge)
        {
            bestAge = age;
            bestIndex = i;
        }
    }

    return bestIndex;
}

bool FrameWarp_Dx12::ExtractUiLayerFromHudless(
    ID3D12GraphicsCommandList* cmdList,
    UINT64 frameID,
    ID3D12Resource* originalHudless,
    D3D12_RESOURCE_STATES originalHudlessState,
    ID3D12Resource* originalPresent,
    D3D12_RESOURCE_STATES originalPresentState,
    const char* source)
{
    auto& status = State::Instance().frameWarpStatus;
    auto fail = [&](const char* reason) {
        const char* safeReason = reason != nullptr ? reason : "UI extract failed";
        strncpy_s(status.dlssgHudlessSubmitLastReason, safeReason, _TRUNCATE);
        FrameWarpSetSkipReason(safeReason);
        return false;
    };

    if (!_enabled)
        return fail("UI extract unavailable: FrameWarp disabled");
    if (!_initialized)
        return fail("UI extract unavailable: FrameWarp not initialized");
    if (cmdList == nullptr)
        return fail("UI extract unavailable: command list missing");
    if (originalHudless == nullptr)
        return fail("UI extract unavailable: HUD-less missing");
    if (originalPresent == nullptr)
        return fail("UI extract unavailable: final-with-UI missing");
    if (_pipelineStateUiLayerExtract == nullptr)
    {
        char reason[96] = {};
        sprintf_s(reason, "UI extract pipeline missing: %s", _uiLayerExtractPipelineReason);
        static uint64_t pipelineMissingLogCount = 0;
        pipelineMissingLogCount++;
        if (pipelineMissingLogCount <= 20 || pipelineMissingLogCount % 120 == 0)
        {
            LOG_ERROR("FrameWarp UI extract unavailable source={} reason={}",
                source != nullptr ? source : "unknown",
                reason);
        }
        return fail(reason);
    }

    auto hudlessDesc = originalHudless->GetDesc();
    auto presentDesc = originalPresent->GetDesc();

    if (hudlessDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        presentDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        hudlessDesc.SampleDesc.Count != 1 ||
        presentDesc.SampleDesc.Count != 1 ||
        hudlessDesc.Width != presentDesc.Width ||
        hudlessDesc.Height != presentDesc.Height ||
        hudlessDesc.Format != presentDesc.Format)
    {
        LOG_DEBUG("FrameWarp UI extract skipped: hudless {}x{} fmt={} present {}x{} fmt={}",
            hudlessDesc.Width,
            hudlessDesc.Height,
            (UINT)hudlessDesc.Format,
            presentDesc.Width,
            presentDesc.Height,
            (UINT)presentDesc.Format);
        return fail("UI extract source mismatch");
    }

    if (!EnsureStableUiPresentCopy(presentDesc) ||
        !EnsureUiLayerCacheResources(presentDesc) ||
        !EnsureUiCleanSceneCacheResources(hudlessDesc))
        return fail("UI extract resource allocation failed");

    const int cacheIndex = static_cast<int>(frameID % NUM_HEAPS);
    ID3D12Resource* uiLayer = _uiLayerCache[cacheIndex];
    ID3D12Resource* cleanScene = _uiCleanSceneCache[cacheIndex];
    if (uiLayer == nullptr || cleanScene == nullptr)
        return fail("UI extract cache missing");

    _heapIndex = (_heapIndex + 1) % NUM_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_heapIndex];

    FrameWarpResourceBarrier(cmdList, originalPresent, originalPresentState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    FrameWarpResourceBarrier(cmdList, _stableUiPresentCopy, _stableUiPresentCopyState,
        D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(_stableUiPresentCopy, originalPresent);
    FrameWarpResourceBarrier(cmdList, originalPresent, D3D12_RESOURCE_STATE_COPY_SOURCE, originalPresentState);
    FrameWarpResourceBarrier(cmdList, _stableUiPresentCopy, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    _stableUiPresentCopyState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    FrameWarpResourceBarrier(cmdList, originalHudless, originalHudlessState,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    FrameWarpResourceBarrier(cmdList, cleanScene, _uiCleanSceneCacheStates[cacheIndex],
        D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(cleanScene, originalHudless);
    FrameWarpResourceBarrier(cmdList, cleanScene, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    _uiCleanSceneCacheStates[cacheIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    FrameWarpResourceBarrier(cmdList, originalHudless, D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    FrameWarpResourceBarrier(cmdList, uiLayer, _uiLayerCacheStates[cacheIndex],
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    _uiLayerCacheStates[cacheIndex] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    D3D12_SHADER_RESOURCE_VIEW_DESC hudlessSrvDesc = {};
    hudlessSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hudlessSrvDesc.Format = TranslateTypelessFormats(hudlessDesc.Format);
    hudlessSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    hudlessSrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(originalHudless, &hudlessSrvDesc, currentHeap.GetSrvCPU(0));

    D3D12_SHADER_RESOURCE_VIEW_DESC presentSrvDesc = {};
    presentSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    presentSrvDesc.Format = TranslateTypelessFormats(presentDesc.Format);
    presentSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    presentSrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(_stableUiPresentCopy, &presentSrvDesc, currentHeap.GetSrvCPU(1));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = TranslateTypelessFormats(uiLayer->GetDesc().Format);
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    _device->CreateUnorderedAccessView(uiLayer, nullptr, &uavDesc, currentHeap.GetUavCPU(0));

    const bool dlssgStrictUiMask =
        source != nullptr && std::strstr(source, "dlssg-resource") != nullptr;

    FrameWarpShaderConstants constants {};
    constants.width = static_cast<UINT>(presentDesc.Width);
    constants.height = presentDesc.Height;
    constants.stableUiSceneHasUnderlay = dlssgStrictUiMask ? 3u : 0u;

    BYTE* pCBDataBegin = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    auto mapResult = _constantBuffers[_heapIndex]->Map(0, &readRange, reinterpret_cast<void**>(&pCBDataBegin));
    if (mapResult != S_OK || pCBDataBegin == nullptr)
    {
        LOG_ERROR("FrameWarp_Dx12: Failed to map UI extract constant buffer");
        FrameWarpResourceBarrier(cmdList, originalHudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            originalHudlessState);
        return fail("UI extract constant map failed");
    }
    memcpy(pCBDataBegin, &constants, sizeof(constants));
    _constantBuffers[_heapIndex]->Unmap(0, nullptr);

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = _constantBuffers[_heapIndex]->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = sizeof(constants);
    _device->CreateConstantBufferView(&cbvDesc, currentHeap.GetCbvCPU(0));

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_pipelineStateUiLayerExtract);
    cmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    UINT dispatchWidth = (constants.width + THREAD_GROUP_X - 1) / THREAD_GROUP_X;
    UINT dispatchHeight = (constants.height + THREAD_GROUP_Y - 1) / THREAD_GROUP_Y;
    cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = uiLayer;
    cmdList->ResourceBarrier(1, &uavBarrier);

    FrameWarpResourceBarrier(cmdList, uiLayer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    _uiLayerCacheStates[cacheIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    FrameWarpResourceBarrier(cmdList, originalHudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        originalHudlessState);

    _cachedUiLayers[cacheIndex].valid = true;
    _cachedUiLayers[cacheIndex].frameID = frameID;
    _cachedUiLayers[cacheIndex].width = static_cast<UINT>(presentDesc.Width);
    _cachedUiLayers[cacheIndex].height = presentDesc.Height;
    _cachedUiLayers[cacheIndex].format = presentDesc.Format;
    _cachedUiLayers[cacheIndex].cleanSceneValid = true;
    strncpy_s(_cachedUiLayers[cacheIndex].source, source != nullptr ? source : "hudfix-real", _TRUNCATE);

    strncpy_s(status.lastStableUiSource, _cachedUiLayers[cacheIndex].source, _TRUNCATE);
    status.lastStableUiFrameID = frameID;
    status.lastStableUiAge = 0;
    status.lastStableUiValid = true;

    static uint64_t uiExtractLogCount = 0;
    uiExtractLogCount++;
    if (uiExtractLogCount <= 40 || uiExtractLogCount % 120 == 0)
    {
        LOG_INFO("FrameWarp UI extract phase=real source={} frameID={} cache={} size={}x{} fmt={} strictMask={} uiCleanScene=valid valid=true",
            _cachedUiLayers[cacheIndex].source,
            frameID,
            cacheIndex,
            _cachedUiLayers[cacheIndex].width,
            _cachedUiLayers[cacheIndex].height,
            (UINT)_cachedUiLayers[cacheIndex].format,
            dlssgStrictUiMask);
    }

    return true;
}

bool FrameWarp_Dx12::PrepareDLSSGHudlessSubmit(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* hudlessInput,
    D3D12_RESOURCE_STATES hudlessInputState,
    ID3D12Resource* finalWithUi,
    D3D12_RESOURCE_STATES finalWithUiState)
{
    if (!_enabled || !_initialized || cmdList == nullptr || hudlessInput == nullptr || finalWithUi == nullptr)
    {
        InvalidateDLSSGHudlessSubmit("prepare unavailable");
        return false;
    }

    if (_pipelineStateUiLayerExtract == nullptr)
    {
        char reason[96] = {};
        sprintf_s(reason, "UI extract pipeline missing: %s", _uiLayerExtractPipelineReason);
        LOG_ERROR("VF2DLSSGHudlessSubmitUI frame={} prepared=false reason={}",
            State::Instance().frameCount,
            reason);
        InvalidateDLSSGHudlessSubmit(reason);
        return false;
    }

    const UINT64 uiFrameID = ++_stableUiSerial;
    if (!ExtractUiLayerFromHudless(
            cmdList,
            uiFrameID,
            hudlessInput,
            hudlessInputState,
            finalWithUi,
            finalWithUiState,
            "dlssg-hudless-submit"))
    {
        const auto& status = State::Instance().frameWarpStatus;
        InvalidateDLSSGHudlessSubmit(status.dlssgHudlessSubmitLastReason[0]
            ? status.dlssgHudlessSubmitLastReason
            : "UI extract failed");
        return false;
    }

    _dlssgHudlessSubmitUiValid = true;
    _dlssgHudlessSubmitUiFrameID = uiFrameID;
    _dlssgHudlessSubmitUiFrame = State::Instance().frameCount;
    auto& status = State::Instance().frameWarpStatus;
    status.dlssgHudlessSubmitReady = true;
    status.dlssgHudlessSubmitFrameID = uiFrameID;
    status.dlssgHudlessSubmitFrame = _dlssgHudlessSubmitUiFrame;
    strncpy_s(status.dlssgHudlessSubmitLastReason, "ready", _TRUNCATE);

    static uint64_t prepareLogCount = 0;
    prepareLogCount++;
    if (prepareLogCount <= 40 || prepareLogCount % 120 == 0)
    {
        auto hudlessDesc = hudlessInput->GetDesc();
        LOG_INFO("VF2DLSSGHudlessSubmitUI frame={} prepared=true frameID={} hudless={:X} final={:X} size={}x{} fmt={}",
            State::Instance().frameCount,
            uiFrameID,
            reinterpret_cast<uintptr_t>(hudlessInput),
            reinterpret_cast<uintptr_t>(finalWithUi),
            static_cast<UINT>(hudlessDesc.Width),
            hudlessDesc.Height,
            static_cast<uint32_t>(hudlessDesc.Format));
    }

    return true;
}

void FrameWarp_Dx12::InvalidateDLSSGHudlessSubmit(const char* reason)
{
    auto& status = State::Instance().frameWarpStatus;
    status.dlssgHudlessSubmitReady = false;
    status.dlssgHudlessSubmitFrameID = 0;
    status.dlssgHudlessSubmitFrame = 0;
    strncpy_s(status.dlssgHudlessSubmitLastReason,
        reason != nullptr ? reason : "invalidated",
        _TRUNCATE);

    if (_dlssgHudlessSubmitUiValid)
    {
        static uint64_t invalidateLogCount = 0;
        invalidateLogCount++;
        if (invalidateLogCount <= 20 || invalidateLogCount % 120 == 0)
        {
            LOG_INFO("VF2DLSSGHudlessSubmitUI frame={} prepared=false reason={}",
                State::Instance().frameCount,
                reason != nullptr ? reason : "invalidated");
        }
    }

    _dlssgHudlessSubmitUiValid = false;
    _dlssgHudlessSubmitUiFrameID = 0;
    _dlssgHudlessSubmitUiFrame = 0;
}

bool FrameWarp_Dx12::CompositeUiLayerToPresent(
    ID3D12GraphicsCommandList* cmdList,
    int cacheIndex,
    ID3D12Resource* warpedScene,
    D3D12_RESOURCE_STATES warpedSceneState,
    ID3D12Resource* outputPresent,
    D3D12_RESOURCE_STATES outputPresentState,
    const char* phase,
    bool suppressWarpedUiUnderlay,
    bool useWarpTransformForUnderlay)
{
    if (!_enabled || !_initialized || cmdList == nullptr || cacheIndex < 0 ||
        cacheIndex >= static_cast<int>(NUM_HEAPS) || warpedScene == nullptr ||
        outputPresent == nullptr || _stableUiOutput == nullptr ||
        _pipelineStateUiLayerComposite == nullptr || _uiLayerCache[cacheIndex] == nullptr ||
        !_cachedUiLayers[cacheIndex].valid)
    {
        FrameWarpSetSkipReason("UI composite unavailable");
        return false;
    }

    auto outputDesc = outputPresent->GetDesc();
    auto warpedDesc = warpedScene->GetDesc();
    auto uiDesc = _uiLayerCache[cacheIndex]->GetDesc();
    auto stableDesc = _stableUiOutput->GetDesc();
    bool cleanSceneValid =
        _cachedUiLayers[cacheIndex].cleanSceneValid &&
        _uiCleanSceneCache[cacheIndex] != nullptr;
    D3D12_RESOURCE_DESC cleanDesc = {};
    if (cleanSceneValid)
    {
        cleanDesc = _uiCleanSceneCache[cacheIndex]->GetDesc();
        cleanSceneValid =
            cleanDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            cleanDesc.Width == outputDesc.Width &&
            cleanDesc.Height == outputDesc.Height &&
            cleanDesc.Format == outputDesc.Format &&
            cleanDesc.SampleDesc.Count == outputDesc.SampleDesc.Count;
    }

    if (outputDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        warpedDesc.Width != outputDesc.Width ||
        warpedDesc.Height != outputDesc.Height ||
        uiDesc.Width != outputDesc.Width ||
        uiDesc.Height != outputDesc.Height ||
        stableDesc.Width != outputDesc.Width ||
        stableDesc.Height != outputDesc.Height)
    {
        LOG_DEBUG("FrameWarp UI composite skipped: warped {}x{} ui {}x{} output {}x{}",
            warpedDesc.Width,
            warpedDesc.Height,
            uiDesc.Width,
            uiDesc.Height,
            outputDesc.Width,
            outputDesc.Height);
        FrameWarpSetSkipReason("UI composite size mismatch");
        return false;
    }

    DXGI_FORMAT stableCopyFormat = TranslateTypelessFormats(stableDesc.Format);
    DXGI_FORMAT outputCopyFormat = TranslateTypelessFormats(outputDesc.Format);
    if (stableDesc.Format != outputDesc.Format && stableCopyFormat != outputCopyFormat)
    {
        LOG_DEBUG("FrameWarp UI composite skipped: output format mismatch stable={} output={}",
            (UINT)stableDesc.Format,
            (UINT)outputDesc.Format);
        FrameWarpSetSkipReason("UI composite format mismatch");
        return false;
    }

    auto& status = State::Instance().frameWarpStatus;
    status.lastStableUiCleanSceneValid = cleanSceneValid;
    status.lastStableUiSuppressUnderlay = suppressWarpedUiUnderlay;
    status.lastStableUiUseWarpTransform = useWarpTransformForUnderlay;
    strncpy_s(status.lastStableUiRepairSource,
        suppressWarpedUiUnderlay
            ? (cleanSceneValid ? "hudless-cache" : "spatial-fallback")
            : "none",
        _TRUNCATE);

    _heapIndex = (_heapIndex + 1) % NUM_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_heapIndex];

    FrameWarpResourceBarrier(cmdList, warpedScene, warpedSceneState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    FrameWarpResourceBarrier(cmdList, _uiLayerCache[cacheIndex], _uiLayerCacheStates[cacheIndex],
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    _uiLayerCacheStates[cacheIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (cleanSceneValid)
    {
        FrameWarpResourceBarrier(cmdList, _uiCleanSceneCache[cacheIndex],
            _uiCleanSceneCacheStates[cacheIndex],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        _uiCleanSceneCacheStates[cacheIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC warpedSrvDesc = {};
    warpedSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    warpedSrvDesc.Format = TranslateTypelessFormats(warpedDesc.Format);
    warpedSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    warpedSrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(warpedScene, &warpedSrvDesc, currentHeap.GetSrvCPU(0));

    D3D12_SHADER_RESOURCE_VIEW_DESC uiSrvDesc = {};
    uiSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    uiSrvDesc.Format = TranslateTypelessFormats(uiDesc.Format);
    uiSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    uiSrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(_uiLayerCache[cacheIndex], &uiSrvDesc, currentHeap.GetSrvCPU(1));

    ID3D12Resource* cleanScene = cleanSceneValid ? _uiCleanSceneCache[cacheIndex] : warpedScene;
    auto cleanSrvFormat = cleanSceneValid ? cleanDesc.Format : warpedDesc.Format;
    D3D12_SHADER_RESOURCE_VIEW_DESC cleanSrvDesc = {};
    cleanSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    cleanSrvDesc.Format = TranslateTypelessFormats(cleanSrvFormat);
    cleanSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    cleanSrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(cleanScene, &cleanSrvDesc, currentHeap.GetSrvCPU(2));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = TranslateTypelessFormats(stableDesc.Format);
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    _device->CreateUnorderedAccessView(_stableUiOutput, nullptr, &uavDesc, currentHeap.GetUavCPU(0));

    FrameWarpShaderConstants constants {};
    if (suppressWarpedUiUnderlay && useWarpTransformForUnderlay && _lastWarpResult.isValid)
        FillShaderConstants(constants, _lastWarpResult);
    else
    {
        CameraPredictor::WarpResult identityWarp {};
        identityWarp.clipToClip = XMMatrixIdentity();
        identityWarp.clipToClipInverse = XMMatrixIdentity();
        identityWarp.isValid = true;
        FillShaderConstants(constants, identityWarp);
    }
    constants.width = static_cast<UINT>(outputDesc.Width);
    constants.height = outputDesc.Height;
    constants.stableUiSceneHasUnderlay =
        suppressWarpedUiUnderlay ? (cleanSceneValid ? 2u : 1u) : 0u;

    BYTE* pCBDataBegin = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    auto mapResult = _constantBuffers[_heapIndex]->Map(0, &readRange, reinterpret_cast<void**>(&pCBDataBegin));
    if (mapResult != S_OK || pCBDataBegin == nullptr)
    {
        LOG_ERROR("FrameWarp_Dx12: Failed to map UI composite constant buffer");
        FrameWarpSetSkipReason("UI composite constant map failed");
        FrameWarpResourceBarrier(cmdList, warpedScene, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            warpedSceneState);
        return false;
    }
    memcpy(pCBDataBegin, &constants, sizeof(constants));
    _constantBuffers[_heapIndex]->Unmap(0, nullptr);

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = _constantBuffers[_heapIndex]->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = sizeof(constants);
    _device->CreateConstantBufferView(&cbvDesc, currentHeap.GetCbvCPU(0));

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_pipelineStateUiLayerComposite);
    cmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    UINT dispatchWidth = (constants.width + THREAD_GROUP_X - 1) / THREAD_GROUP_X;
    UINT dispatchHeight = (constants.height + THREAD_GROUP_Y - 1) / THREAD_GROUP_Y;
    cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = _stableUiOutput;
    cmdList->ResourceBarrier(1, &uavBarrier);

    FrameWarpResourceBarrier(cmdList, _stableUiOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    FrameWarpResourceBarrier(cmdList, outputPresent, outputPresentState, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(outputPresent, _stableUiOutput);
    FrameWarpResourceBarrier(cmdList, outputPresent, D3D12_RESOURCE_STATE_COPY_DEST, outputPresentState);
    FrameWarpResourceBarrier(cmdList, _stableUiOutput, D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    FrameWarpResourceBarrier(cmdList, warpedScene, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        warpedSceneState);

    if (suppressWarpedUiUnderlay)
    {
        static uint64_t uiRepairLogCount = 0;
        uiRepairLogCount++;
        if (uiRepairLogCount <= 40 || uiRepairLogCount % 120 == 0)
        {
            LOG_DEBUG("FrameWarp UI repair phase={} cache={} uiCleanScene={} repairSource={} useWarpTransform={} valid=true",
                phase != nullptr ? phase : "unknown",
                cacheIndex,
                cleanSceneValid ? "valid" : "missing",
                cleanSceneValid ? "hudless-cache" : "spatial-fallback",
                useWarpTransformForUnderlay);
        }
    }

    return true;
}

bool FrameWarp_Dx12::CompositeCachedUiLayer(
    ID3D12GraphicsCommandList* cmdList,
    UINT64 frameID,
    UINT64 maxFrameAge,
    ID3D12Resource* warpedScene,
    D3D12_RESOURCE_STATES warpedSceneState,
    ID3D12Resource* outputPresent,
    D3D12_RESOURCE_STATES outputPresentState,
    const char* phase,
    bool suppressWarpedUiUnderlay,
    bool useWarpTransformForUnderlay)
{
    int cacheIndex = FindCachedUiLayer(frameID, maxFrameAge);
    if (cacheIndex < 0)
    {
        auto& status = State::Instance().frameWarpStatus;
        strncpy_s(status.lastStableUiSource, "none", _TRUNCATE);
        strncpy_s(status.lastStableUiRepairSource, "none", _TRUNCATE);
        status.lastStableUiValid = false;
        status.lastStableUiAge = UINT64_MAX;
        status.lastStableUiCleanSceneValid = false;
        status.lastStableUiSuppressUnderlay = false;
        status.lastStableUiUseWarpTransform = false;
        static uint64_t noCacheLogCount = 0;
        noCacheLogCount++;
        if (noCacheLogCount <= 40 || noCacheLogCount % 120 == 0)
        {
            LOG_DEBUG("FrameWarp UI composite skipped phase={} frameID={} source=none maxAge={}",
                phase != nullptr ? phase : "unknown",
                frameID,
                maxFrameAge);
        }
        return false;
    }

    UINT64 age = frameID - _cachedUiLayers[cacheIndex].frameID;
    if (!CompositeUiLayerToPresent(
            cmdList,
            cacheIndex,
            warpedScene,
            warpedSceneState,
            outputPresent,
            outputPresentState,
            phase,
            suppressWarpedUiUnderlay,
            useWarpTransformForUnderlay))
    {
        return false;
    }

    auto& status = State::Instance().frameWarpStatus;
    strncpy_s(status.lastStableUiSource, _cachedUiLayers[cacheIndex].source, _TRUNCATE);
    status.lastStableUiFrameID = _cachedUiLayers[cacheIndex].frameID;
    status.lastStableUiAge = age;
    status.lastStableUiValid = true;

    static uint64_t uiCompositeLogCount = 0;
    uiCompositeLogCount++;
    if (uiCompositeLogCount <= 40 || uiCompositeLogCount % 120 == 0)
    {
        LOG_DEBUG("FrameWarp UI composite phase={} source={} frameID={} cacheFrameID={} age={} cache={} suppressUnderlay={} uiCleanScene={} repairSource={} valid=true",
            phase != nullptr ? phase : "unknown",
            _cachedUiLayers[cacheIndex].source,
            frameID,
            _cachedUiLayers[cacheIndex].frameID,
            age,
            cacheIndex,
            suppressWarpedUiUnderlay,
            _cachedUiLayers[cacheIndex].cleanSceneValid ? "valid" : "missing",
            (suppressWarpedUiUnderlay && _cachedUiLayers[cacheIndex].cleanSceneValid) ? "hudless-cache" :
                (suppressWarpedUiUnderlay ? "spatial-fallback" : "none"));
    }

    return true;
}

bool FrameWarp_Dx12::CompositeDLSSGHudlessSubmitUi(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* sceneInput,
    D3D12_RESOURCE_STATES sceneInputState,
    ID3D12Resource* outputPresent,
    D3D12_RESOURCE_STATES outputPresentState)
{
    if (!_dlssgHudlessSubmitUiValid || _dlssgHudlessSubmitUiFrameID == 0)
    {
        auto& status = State::Instance().frameWarpStatus;
        status.dlssgHudlessSubmitReady = false;
        strncpy_s(status.dlssgHudlessSubmitLastReason, "DLSSG HUD-less UI cache unavailable", _TRUNCATE);
        FrameWarpSetSkipReason("DLSSG HUD-less UI cache unavailable");
        return false;
    }

    const UINT64 currentFrame = State::Instance().frameCount;
    if (_dlssgHudlessSubmitUiFrame > currentFrame ||
        (currentFrame - _dlssgHudlessSubmitUiFrame) > 8)
    {
        InvalidateDLSSGHudlessSubmit("UI cache stale");
        FrameWarpSetSkipReason("DLSSG HUD-less UI cache stale");
        return false;
    }

    const bool composited = CompositeCachedUiLayer(
        cmdList,
        _dlssgHudlessSubmitUiFrameID,
        0,
        sceneInput,
        sceneInputState,
        outputPresent,
        outputPresentState,
        "dlssg-hudless-submit",
        true,
        true);

    static uint64_t compositeLogCount = 0;
    compositeLogCount++;
    if (compositeLogCount <= 40 || compositeLogCount % 120 == 0 || !composited)
    {
        LOG_INFO("VF2DLSSGHudlessSubmitUI frame={} composited={} frameID={} scene={:X} dst={:X} reason={}",
            State::Instance().frameCount,
            composited,
            _dlssgHudlessSubmitUiFrameID,
            reinterpret_cast<uintptr_t>(sceneInput),
            reinterpret_cast<uintptr_t>(outputPresent),
            composited ? "restored-repaired" : (State::Instance().frameWarpStatus.lastSkipReason[0]
                ? State::Instance().frameWarpStatus.lastSkipReason
                : "composite failed"));
    }

    return composited;
}

bool FrameWarp_Dx12::CaptureHudlessSource(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* hudlessInput,
    D3D12_RESOURCE_STATES hudlessInputState,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    const char* source)
{
    if (!_enabled || !_initialized || cmdList == nullptr || hudlessInput == nullptr)
        return false;

    auto desc = hudlessInput->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.SampleDesc.Count != 1 ||
        desc.MipLevels != 1 ||
        desc.DepthOrArraySize != 1)
    {
        LOG_DEBUG("FrameWarp: HUD-less capture skipped due to unsupported shape");
        return false;
    }

    if (width != 0 && desc.Width != width)
        LOG_DEBUG("FrameWarp: HUD-less capture width mismatch metadata={} resource={}", width, desc.Width);
    if (height != 0 && desc.Height != height)
        LOG_DEBUG("FrameWarp: HUD-less capture height mismatch metadata={} resource={}", height, desc.Height);
    if (format != DXGI_FORMAT_UNKNOWN && desc.Format != format)
        LOG_DEBUG("FrameWarp: HUD-less capture format metadata={} resource={}", (UINT)format, (UINT)desc.Format);

    if (!EnsureHudlessInputCopy(desc))
        return false;

    FrameWarpResourceBarrier(cmdList, hudlessInput, hudlessInputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    FrameWarpResourceBarrier(cmdList, _hudlessInputCopy, _hudlessInputCopyState, D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyResource(_hudlessInputCopy, hudlessInput);

    FrameWarpResourceBarrier(cmdList, hudlessInput, D3D12_RESOURCE_STATE_COPY_SOURCE, hudlessInputState);
    FrameWarpResourceBarrier(cmdList, _hudlessInputCopy, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    _hudlessInputCopyState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    _hudlessInputCopyValid = true;
    _hudlessInputCopyWidth = static_cast<UINT>(desc.Width);
    _hudlessInputCopyHeight = desc.Height;
    _hudlessInputCopyFormat = desc.Format;
    _hudlessInputCopySerial++;

    static uint64_t captureLogCount = 0;
    captureLogCount++;
    if (captureLogCount <= 10 || captureLogCount % 300 == 0)
    {
        LOG_DEBUG("FrameWarp: captured HUD-less source #{} {} {:X} {}x{} fmt={}",
            captureLogCount,
            source != nullptr ? source : "unknown",
            (size_t)_hudlessInputCopy,
            _hudlessInputCopyWidth,
            _hudlessInputCopyHeight,
            (UINT)_hudlessInputCopyFormat);
    }

    StoreDepthInfillHistory(cmdList);
    return true;
}

bool FrameWarp_Dx12::StoreDepthInfillHistory(ID3D12GraphicsCommandList* cmdList)
{
    if (cmdList == nullptr || !_hudlessInputCopyValid || _hudlessInputCopy == nullptr ||
        !_depthSnapshotValid || _depthSnapshot == nullptr)
        return false;

    if (_lastHistoryHudlessSerial == _hudlessInputCopySerial &&
        _lastHistoryDepthSerial == _depthSnapshotSerial)
        return true;

    auto colorDesc = _hudlessInputCopy->GetDesc();
    auto depthDesc = _depthSnapshot->GetDesc();
    if (colorDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        depthDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        colorDesc.Width == 0 || colorDesc.Height == 0 ||
        depthDesc.Width == 0 || depthDesc.Height == 0 ||
        colorDesc.SampleDesc.Count != 1 || depthDesc.SampleDesc.Count != 1)
        return false;

    const size_t index = _depthInfillHistoryIndex % DEPTH_INFILL_HISTORY_COUNT;
    auto& entry = _depthInfillHistory[index];
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    bool recreateColor = entry.color == nullptr;
    if (entry.color != nullptr)
    {
        auto existing = entry.color->GetDesc();
        recreateColor =
            existing.Width != colorDesc.Width ||
            existing.Height != colorDesc.Height ||
            existing.Format != colorDesc.Format ||
            existing.SampleDesc.Count != colorDesc.SampleDesc.Count;
    }

    if (recreateColor)
    {
        if (entry.color) { entry.color->Release(); entry.color = nullptr; }
        D3D12_RESOURCE_DESC historyColorDesc = colorDesc;
        historyColorDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        HRESULT hr = _device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &historyColorDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&entry.color));
        if (FAILED(hr) || entry.color == nullptr)
        {
            LOG_WARN("FrameWarp: depth infill history color allocation failed {:X}", hr);
            entry.valid = false;
            return false;
        }
        wchar_t name[64] = {};
        swprintf_s(name, L"FrameWarp_DepthInfillColor_%zu", index);
        entry.color->SetName(name);
        entry.colorState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    bool recreateDepth = entry.depth == nullptr;
    if (entry.depth != nullptr)
    {
        auto existing = entry.depth->GetDesc();
        recreateDepth =
            existing.Width != depthDesc.Width ||
            existing.Height != depthDesc.Height ||
            existing.Format != depthDesc.Format ||
            existing.SampleDesc.Count != depthDesc.SampleDesc.Count;
    }

    if (recreateDepth)
    {
        if (entry.depth) { entry.depth->Release(); entry.depth = nullptr; }
        D3D12_RESOURCE_DESC historyDepthDesc = depthDesc;
        historyDepthDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        HRESULT hr = _device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &historyDepthDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&entry.depth));
        if (FAILED(hr) || entry.depth == nullptr)
        {
            LOG_WARN("FrameWarp: depth infill history depth allocation failed {:X}", hr);
            entry.valid = false;
            return false;
        }
        wchar_t name[64] = {};
        swprintf_s(name, L"FrameWarp_DepthInfillDepth_%zu", index);
        entry.depth->SetName(name);
        entry.depthState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    FrameWarpResourceBarrier(cmdList, entry.color, entry.colorState, D3D12_RESOURCE_STATE_COPY_DEST);
    const auto hudlessStateBefore = _hudlessInputCopyState;
    if (hudlessStateBefore != D3D12_RESOURCE_STATE_COPY_SOURCE)
        FrameWarpResourceBarrier(cmdList, _hudlessInputCopy, hudlessStateBefore, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(entry.color, _hudlessInputCopy);
    if (hudlessStateBefore != D3D12_RESOURCE_STATE_COPY_SOURCE)
        FrameWarpResourceBarrier(cmdList, _hudlessInputCopy, D3D12_RESOURCE_STATE_COPY_SOURCE, hudlessStateBefore);
    _hudlessInputCopyState = hudlessStateBefore;
    FrameWarpResourceBarrier(cmdList, entry.color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    entry.colorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    FrameWarpResourceBarrier(cmdList, entry.depth, entry.depthState, D3D12_RESOURCE_STATE_COPY_DEST);
    const auto depthStateBefore = _depthSnapshotState;
    if (depthStateBefore != D3D12_RESOURCE_STATE_COPY_SOURCE)
        FrameWarpResourceBarrier(cmdList, _depthSnapshot, depthStateBefore, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(entry.depth, _depthSnapshot);
    if (depthStateBefore != D3D12_RESOURCE_STATE_COPY_SOURCE)
        FrameWarpResourceBarrier(cmdList, _depthSnapshot, D3D12_RESOURCE_STATE_COPY_SOURCE, depthStateBefore);
    _depthSnapshotState = depthStateBefore;
    FrameWarpResourceBarrier(cmdList, entry.depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    entry.depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    entry.valid = true;
    entry.width = static_cast<UINT>(colorDesc.Width);
    entry.height = colorDesc.Height;
    entry.colorFormat = colorDesc.Format;
    entry.depthFormat = _depthSnapshotCopyFormat;
    entry.hudlessSerial = _hudlessInputCopySerial;
    entry.depthSerial = _depthSnapshotSerial;
    entry.frame = State::Instance().frameCount;
    entry.invertedDepth = _depthSnapshotInverted;

    _lastHistoryHudlessSerial = _hudlessInputCopySerial;
    _lastHistoryDepthSerial = _depthSnapshotSerial;
    _depthInfillHistoryIndex = (_depthInfillHistoryIndex + 1) % DEPTH_INFILL_HISTORY_COUNT;

    auto& status = State::Instance().frameWarpStatus;
    status.depthInfillHistoryAvailable = true;

    static uint64_t historyLogCount = 0;
    historyLogCount++;
    if (historyLogCount <= 10 || historyLogCount % 300 == 0)
    {
        LOG_DEBUG("FrameWarp: stored depth infill history #{} cache={} color={}x{} fmt={} depth={}x{} fmt={} inv={}",
            historyLogCount,
            index,
            entry.width,
            entry.height,
            (UINT)entry.colorFormat,
            _depthSnapshotWidth,
            _depthSnapshotHeight,
            (UINT)entry.depthFormat,
            entry.invertedDepth);
    }

    return true;
}

int FrameWarp_Dx12::FindDepthInfillHistory(UINT width, UINT height, DXGI_FORMAT colorFormat) const
{
    int bestIndex = -1;
    UINT64 bestFrame = 0;
    int fallbackIndex = -1;
    UINT64 fallbackFrame = 0;
    const UINT64 currentFrame = State::Instance().frameCount;
    for (size_t i = 0; i < DEPTH_INFILL_HISTORY_COUNT; ++i)
    {
        const auto& entry = _depthInfillHistory[i];
        if (!entry.valid || entry.color == nullptr || entry.depth == nullptr)
            continue;
        if (entry.width != width || entry.height != height || entry.colorFormat != colorFormat)
            continue;

        if (fallbackIndex < 0 || entry.frame > fallbackFrame)
        {
            fallbackIndex = static_cast<int>(i);
            fallbackFrame = entry.frame;
        }

        if (entry.frame >= currentFrame)
            continue;

        if (bestIndex < 0 || entry.frame > bestFrame)
        {
            bestIndex = static_cast<int>(i);
            bestFrame = entry.frame;
        }
    }
    return bestIndex >= 0 ? bestIndex : fallbackIndex;
}

bool FrameWarp_Dx12::CaptureDepthSource(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* depthInput,
    D3D12_RESOURCE_STATES depthInputState,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    bool invertedDepth,
    const char* source)
{
    auto& status = State::Instance().frameWarpStatus;
    status.depthInfillCopySuccess = false;
    status.depthInfillSourceState = static_cast<uint32_t>(depthInputState);
    strncpy_s(status.depthInfillSource, source != nullptr ? source : "unknown", _TRUNCATE);

    if (!_enabled || !_initialized || cmdList == nullptr || depthInput == nullptr)
    {
        strncpy_s(status.depthInfillReason, !_enabled ? "disabled" : (!_initialized ? "not initialized" : "missing depth"), _TRUNCATE);
        FrameWarpLogDepthInfillDiagnostics("capture");
        return false;
    }

    auto desc = depthInput->GetDesc();
    const DXGI_FORMAT sourceFormat = format != DXGI_FORMAT_UNKNOWN ? format : desc.Format;
    const DXGI_FORMAT copyFormat = FrameWarpDepthCopyFormat(sourceFormat);
    const DXGI_FORMAT srvFormat = FrameWarpDepthSrvFormat(sourceFormat);
    status.depthInfillSourceFormat = static_cast<uint32_t>(sourceFormat);
    status.depthInfillCopyFormat = static_cast<uint32_t>(copyFormat);
    status.depthInfillSrvFormat = static_cast<uint32_t>(srvFormat);
    status.depthInfillWidth = width != 0 ? width : static_cast<uint32_t>(desc.Width);
    status.depthInfillHeight = height != 0 ? height : desc.Height;
    status.depthInfillInverted = invertedDepth;

    const bool validShape =
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        desc.Width > 0 &&
        desc.Height > 0 &&
        desc.SampleDesc.Count == 1 &&
        desc.MipLevels == 1 &&
        desc.DepthOrArraySize == 1;

    if (!validShape || copyFormat == DXGI_FORMAT_UNKNOWN || srvFormat == DXGI_FORMAT_UNKNOWN)
    {
        _depthSnapshotValid = false;
        strncpy_s(status.depthInfillReason, !validShape ? "unsupported depth shape" : "unsupported depth format", _TRUNCATE);
        FrameWarpLogDepthInfillDiagnostics("capture");
        return false;
    }

    bool recreate = _depthSnapshot == nullptr;
    if (_depthSnapshot != nullptr)
    {
        auto existing = _depthSnapshot->GetDesc();
        recreate =
            existing.Width != desc.Width ||
            existing.Height != desc.Height ||
            existing.Format != copyFormat ||
            existing.SampleDesc.Count != desc.SampleDesc.Count;
    }

    if (recreate)
    {
        if (_depthSnapshot)
        {
            _depthSnapshot->Release();
            _depthSnapshot = nullptr;
        }

        D3D12_RESOURCE_DESC copyDesc = desc;
        copyDesc.Format = copyFormat;
        copyDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = _device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &copyDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_depthSnapshot));
        if (FAILED(hr) || _depthSnapshot == nullptr)
        {
            _depthSnapshotValid = false;
            strncpy_s(status.depthInfillReason, "depth copy allocation failed", _TRUNCATE);
            LOG_WARN("FrameWarp: depth snapshot allocation failed {:X}", hr);
            FrameWarpLogDepthInfillDiagnostics("capture");
            return false;
        }
        _depthSnapshot->SetName(L"FrameWarp_DepthSnapshot");
        _depthSnapshotState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    if (_depthSnapshotState != D3D12_RESOURCE_STATE_COPY_DEST)
        FrameWarpResourceBarrier(cmdList, _depthSnapshot, _depthSnapshotState, D3D12_RESOURCE_STATE_COPY_DEST);

    if (FrameWarpStateNeedsTransition(depthInputState, D3D12_RESOURCE_STATE_COPY_SOURCE))
        FrameWarpResourceBarrier(cmdList, depthInput, depthInputState, D3D12_RESOURCE_STATE_COPY_SOURCE);

    cmdList->CopyResource(_depthSnapshot, depthInput);

    if (FrameWarpStateNeedsTransition(depthInputState, D3D12_RESOURCE_STATE_COPY_SOURCE))
        FrameWarpResourceBarrier(cmdList, depthInput, D3D12_RESOURCE_STATE_COPY_SOURCE, depthInputState);

    FrameWarpResourceBarrier(cmdList, _depthSnapshot, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    _depthSnapshotState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    _depthSnapshotValid = true;
    _depthSnapshotInverted = invertedDepth;
    _depthSnapshotWidth = static_cast<UINT>(desc.Width);
    _depthSnapshotHeight = desc.Height;
    _depthSnapshotSourceFormat = sourceFormat;
    _depthSnapshotCopyFormat = copyFormat;
    _depthSnapshotSrvFormat = srvFormat;
    _depthSnapshotSourceState = depthInputState;
    _depthSnapshotSerial++;
    _depthSnapshotFrame = State::Instance().frameCount;
    strncpy_s(_depthSnapshotSource, source != nullptr ? source : "unknown", _TRUNCATE);
    strncpy_s(_depthSnapshotReason, "active", _TRUNCATE);

    status.depthInfillCopySuccess = true;
    status.depthInfillAgeFrames = 0.0f;
    strncpy_s(status.depthInfillReason, "active", _TRUNCATE);

    StoreDepthInfillHistory(cmdList);
    FrameWarpLogDepthInfillDiagnostics("capture");
    return true;
}

bool FrameWarp_Dx12::TrackDepthSource(
    ID3D12Resource* depthInput,
    D3D12_RESOURCE_STATES depthInputState,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    const char* source)
{
    if (!_enabled || !_initialized || depthInput == nullptr)
        return false;

    auto desc = depthInput->GetDesc();
    const bool validShape =
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        desc.Width > 0 &&
        desc.Height > 0 &&
        desc.SampleDesc.Count == 1 &&
        desc.MipLevels == 1 &&
        desc.DepthOrArraySize == 1;

    if (!validShape)
    {
        if (_trackedDepthInput != nullptr)
        {
            _trackedDepthInput->Release();
            _trackedDepthInput = nullptr;
        }
        _trackedDepthInputState = D3D12_RESOURCE_STATE_COMMON;
        _trackedDepthInputValid = false;
        _trackedDepthInputWidth = 0;
        _trackedDepthInputHeight = 0;
        _trackedDepthInputFormat = DXGI_FORMAT_UNKNOWN;
        LOG_DEBUG("FrameWarp: depth tracking skipped due to unsupported shape");
        return false;
    }

    if (width != 0 && desc.Width != width)
        LOG_DEBUG("FrameWarp: depth tracking width mismatch metadata={} resource={}", width, desc.Width);
    if (height != 0 && desc.Height != height)
        LOG_DEBUG("FrameWarp: depth tracking height mismatch metadata={} resource={}", height, desc.Height);
    if (format != DXGI_FORMAT_UNKNOWN && desc.Format != format)
        LOG_DEBUG("FrameWarp: depth tracking format metadata={} resource={}", (UINT)format, (UINT)desc.Format);

    if (_trackedDepthInput != depthInput)
    {
        depthInput->AddRef();
        if (_trackedDepthInput != nullptr)
            _trackedDepthInput->Release();
        _trackedDepthInput = depthInput;
    }

    _trackedDepthInputState = depthInputState;
    _trackedDepthInputValid = true;
    _trackedDepthInputWidth = static_cast<UINT>(desc.Width);
    _trackedDepthInputHeight = desc.Height;
    _trackedDepthInputFormat = desc.Format;

    static uint64_t depthTrackLogCount = 0;
    depthTrackLogCount++;
    if (depthTrackLogCount <= 10 || depthTrackLogCount % 300 == 0)
    {
        LOG_DEBUG("FrameWarp: tracked depth source #{} {} {:X} {}x{} fmt={} state={:#x}",
            depthTrackLogCount,
            source != nullptr ? source : "unknown",
            (size_t)_trackedDepthInput,
            _trackedDepthInputWidth,
            _trackedDepthInputHeight,
            (UINT)_trackedDepthInputFormat,
            (UINT)_trackedDepthInputState);
    }

    return true;
}

bool FrameWarp_Dx12::TrackMotionVectorSource(
    ID3D12Resource* motionVectorInput,
    D3D12_RESOURCE_STATES motionVectorInputState,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    float mvScaleX,
    float mvScaleY,
    bool scalePreMultiplied,
    const char* source)
{
    auto& status = State::Instance().frameWarpStatus;
    strncpy_s(status.sensitivityAuditSource, source != nullptr ? source : "unknown", _TRUNCATE);

    if (!_enabled || !_initialized || motionVectorInput == nullptr)
    {
        status.sensitivityAuditAvailable = false;
        strncpy_s(status.sensitivityAuditReason,
            !_enabled ? "disabled" : (!_initialized ? "not initialized" : "missing motion vectors"),
            _TRUNCATE);
        return false;
    }

    auto desc = motionVectorInput->GetDesc();
    const bool validShape =
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        desc.Width > 0 &&
        desc.Height > 0 &&
        desc.SampleDesc.Count == 1 &&
        desc.MipLevels == 1 &&
        desc.DepthOrArraySize == 1;

    if (!validShape)
    {
        if (_trackedMotionVectorInput != nullptr)
        {
            _trackedMotionVectorInput->Release();
            _trackedMotionVectorInput = nullptr;
        }
        _trackedMotionVectorInputState = D3D12_RESOURCE_STATE_COMMON;
        _trackedMotionVectorInputValid = false;
        status.sensitivityAuditAvailable = false;
        strncpy_s(status.sensitivityAuditReason, "unsupported motion vector shape", _TRUNCATE);
        return false;
    }

    if (_trackedMotionVectorInput != motionVectorInput)
    {
        motionVectorInput->AddRef();
        if (_trackedMotionVectorInput != nullptr)
            _trackedMotionVectorInput->Release();
        _trackedMotionVectorInput = motionVectorInput;
    }

    _trackedMotionVectorInputState = motionVectorInputState;
    _trackedMotionVectorInputValid = true;
    _trackedMotionVectorInputWidth = width != 0 ? width : static_cast<UINT>(desc.Width);
    _trackedMotionVectorInputHeight = height != 0 ? height : desc.Height;
    _trackedMotionVectorInputFormat = format != DXGI_FORMAT_UNKNOWN ? format : desc.Format;
    _trackedMotionVectorScaleX = mvScaleX;
    _trackedMotionVectorScaleY = mvScaleY;
    _trackedMotionVectorScalePreMultiplied = scalePreMultiplied;
    strncpy_s(_trackedMotionVectorSource, source != nullptr ? source : "unknown", _TRUNCATE);

    status.sensitivityAuditAvailable = true;
    status.sensitivityAuditMvWidth = _trackedMotionVectorInputWidth;
    status.sensitivityAuditMvHeight = _trackedMotionVectorInputHeight;
    status.sensitivityAuditMvFormat = static_cast<uint32_t>(_trackedMotionVectorInputFormat);
    status.sensitivityAuditMvScaleX = _trackedMotionVectorScaleX;
    status.sensitivityAuditMvScaleY = _trackedMotionVectorScaleY;
    strncpy_s(status.sensitivityAuditReason, "tracked", _TRUNCATE);

    static uint64_t mvTrackLogCount = 0;
    mvTrackLogCount++;
    if (Config::Instance()->FrameWarpSensitivityAuditLog.value_or_default() &&
        (mvTrackLogCount <= 10 || mvTrackLogCount % 300 == 0))
    {
        LOG_INFO("VF2SensitivityAuditTrack #{} source={} resource={:X} size={}x{} fmt={} state={:#x} scale=({:.6f},{:.6f}) preMult={}",
            mvTrackLogCount,
            _trackedMotionVectorSource,
            reinterpret_cast<size_t>(_trackedMotionVectorInput),
            _trackedMotionVectorInputWidth,
            _trackedMotionVectorInputHeight,
            static_cast<uint32_t>(_trackedMotionVectorInputFormat),
            static_cast<uint32_t>(_trackedMotionVectorInputState),
            _trackedMotionVectorScaleX,
            _trackedMotionVectorScaleY,
            _trackedMotionVectorScalePreMultiplied);
    }

    return true;
}

void FrameWarp_Dx12::ProcessMotionVectorCalibrationReadback()
{
    if (!_mvCalibrationPending || _mvCalibrationReadback == nullptr || _standaloneFence == nullptr)
        return;

    if (_standaloneFence->GetCompletedValue() < _mvCalibrationFenceValue)
        return;

    auto& status = State::Instance().frameWarpStatus;
    status.sensitivityAuditCopyPending = false;
    _mvCalibrationPending = false;

    const UINT bytesPerPixel = FrameWarpMotionVectorBytesPerPixel(_mvCalibrationFormat);
    if (bytesPerPixel == 0 || _mvCalibrationWidth == 0 || _mvCalibrationHeight == 0 ||
        _mvCalibrationRowPitch == 0 || _mvCalibrationReadbackSize == 0)
    {
        strncpy_s(status.sensitivityAuditReason, "unsupported readback format", _TRUNCATE);
        return;
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange { 0, static_cast<SIZE_T>(_mvCalibrationReadbackSize) };
    HRESULT hr = _mvCalibrationReadback->Map(0, &readRange, &mapped);
    if (FAILED(hr) || mapped == nullptr)
    {
        strncpy_s(status.sensitivityAuditReason, "readback map failed", _TRUNCATE);
        return;
    }

    std::vector<float> samplesX;
    std::vector<float> samplesY;
    samplesX.reserve(static_cast<size_t>(_mvCalibrationWidth) * _mvCalibrationHeight);
    samplesY.reserve(static_cast<size_t>(_mvCalibrationWidth) * _mvCalibrationHeight);

    const uint8_t* base = static_cast<const uint8_t*>(mapped);
    for (UINT y = 0; y < _mvCalibrationHeight; ++y)
    {
        const uint8_t* row = base + static_cast<size_t>(y) * _mvCalibrationRowPitch;
        for (UINT x = 0; x < _mvCalibrationWidth; ++x)
        {
            float mvX = 0.0f;
            float mvY = 0.0f;
            if (!FrameWarpReadMotionVectorPixel(row + static_cast<size_t>(x) * bytesPerPixel,
                _mvCalibrationFormat, mvX, mvY))
            {
                continue;
            }

            const float pxX = mvX * _mvCalibrationScaleX;
            const float pxY = mvY * _mvCalibrationScaleY;
            if (!std::isfinite(pxX) || !std::isfinite(pxY) ||
                std::abs(pxX) > 100000.0f || std::abs(pxY) > 100000.0f)
            {
                continue;
            }

            samplesX.push_back(pxX);
            samplesY.push_back(pxY);
        }
    }

    D3D12_RANGE writeRange { 0, 0 };
    _mvCalibrationReadback->Unmap(0, &writeRange);

    if (samplesX.size() < 16 || samplesY.size() < 16)
    {
        strncpy_s(status.sensitivityAuditReason, "too few valid MV samples", _TRUNCATE);
        return;
    }

    auto median = [](std::vector<float>& values) -> float {
        const size_t mid = values.size() / 2;
        std::nth_element(values.begin(), values.begin() + mid, values.end());
        float m = values[mid];
        if ((values.size() & 1) == 0 && mid > 0)
        {
            std::nth_element(values.begin(), values.begin() + mid - 1, values.end());
            m = 0.5f * (m + values[mid - 1]);
        }
        return m;
    };

    const float medianX = median(samplesX);
    const float medianY = median(samplesY);
    const float gameAbsPx = std::max(std::abs(medianX), std::abs(medianY));
    const float predAbsPx = std::max(std::abs(_mvCalibrationPredPxX), std::abs(_mvCalibrationPredPxY));
    const float ratio = predAbsPx > 0.05f ? gameAbsPx / predAbsPx : 0.0f;
    const float recommended = ratio > 0.0f && std::isfinite(ratio)
        ? _mvCalibrationSensitivity * ratio
        : 0.0f;
    const float sampleCoverage = static_cast<float>(samplesX.size()) /
        static_cast<float>(std::max<UINT>(1, _mvCalibrationWidth * _mvCalibrationHeight));
    const float confidence =
        std::clamp(sampleCoverage, 0.0f, 1.0f) *
        std::clamp(gameAbsPx / 2.0f, 0.0f, 1.0f) *
        (predAbsPx > 0.25f ? 1.0f : 0.25f);

    status.sensitivityAuditCount++;
    status.sensitivityAuditAvailable = true;
    status.sensitivityAuditMvWidth = _trackedMotionVectorInputWidth;
    status.sensitivityAuditMvHeight = _trackedMotionVectorInputHeight;
    status.sensitivityAuditMvFormat = static_cast<uint32_t>(_mvCalibrationFormat);
    status.sensitivityAuditMvScaleX = _mvCalibrationScaleX;
    status.sensitivityAuditMvScaleY = _mvCalibrationScaleY;
    status.sensitivityAuditCurrentRadPerCount = _mvCalibrationSensitivity;
    status.sensitivityAuditRenderMouseDx = _mvCalibrationRenderMouseDx;
    status.sensitivityAuditRenderMouseDy = _mvCalibrationRenderMouseDy;
    status.sensitivityAuditGameMedianPxX = medianX;
    status.sensitivityAuditGameMedianPxY = medianY;
    status.sensitivityAuditGameAbsPx = gameAbsPx;
    status.sensitivityAuditPredPxX = _mvCalibrationPredPxX;
    status.sensitivityAuditPredPxY = _mvCalibrationPredPxY;
    status.sensitivityAuditPredAbsPx = predAbsPx;
    status.sensitivityAuditRatio = ratio;
    status.sensitivityAuditRecommendedRadPerCount = recommended;
    status.sensitivityAuditConfidence = confidence;
    strncpy_s(status.sensitivityAuditSource, _mvCalibrationSource, _TRUNCATE);
    strncpy_s(status.sensitivityAuditReason, "readback ok", _TRUNCATE);

    if (status.sensitivityAuditCount <= 60 || status.sensitivityAuditCount % 60 == 0)
    {
        LOG_INFO("VF2SensitivityAudit #{} frame={} source={} mv={}x{} fmt={} scale=({:.6f},{:.6f}) roi={}x{} samples={} mouseStep=({:.1f},{:.1f}) sens={:.7f} predPx=({:.3f},{:.3f}) gameMedianPx=({:.3f},{:.3f}) ratio={:.3f} recommendedSens={:.7f} confidence={:.3f}",
            status.sensitivityAuditCount,
            _mvCalibrationFrame,
            _mvCalibrationSource,
            _trackedMotionVectorInputWidth,
            _trackedMotionVectorInputHeight,
            static_cast<uint32_t>(_mvCalibrationFormat),
            _mvCalibrationScaleX,
            _mvCalibrationScaleY,
            _mvCalibrationWidth,
            _mvCalibrationHeight,
            samplesX.size(),
            _mvCalibrationRenderMouseDx,
            _mvCalibrationRenderMouseDy,
            _mvCalibrationSensitivity,
            _mvCalibrationPredPxX,
            _mvCalibrationPredPxY,
            medianX,
            medianY,
            ratio,
            recommended,
            confidence);
    }
}

bool FrameWarp_Dx12::CaptureMotionVectorCalibrationSample(ID3D12GraphicsCommandList* cmdList)
{
    auto& status = State::Instance().frameWarpStatus;
    if (!Config::Instance()->FrameWarpSensitivityAuditLog.value_or_default())
        return false;

    if (!_trackedMotionVectorInputValid || _trackedMotionVectorInput == nullptr || cmdList == nullptr)
    {
        strncpy_s(status.sensitivityAuditReason, "missing tracked motion vectors", _TRUNCATE);
        return false;
    }

    const UINT bytesPerPixel = FrameWarpMotionVectorBytesPerPixel(_trackedMotionVectorInputFormat);
    if (bytesPerPixel == 0)
    {
        strncpy_s(status.sensitivityAuditReason, "unsupported motion vector format", _TRUNCATE);
        return false;
    }

    auto desc = _trackedMotionVectorInput->GetDesc();
    const UINT width = std::min<UINT>(_trackedMotionVectorInputWidth, static_cast<UINT>(desc.Width));
    const UINT height = std::min<UINT>(_trackedMotionVectorInputHeight, desc.Height);
    if (width == 0 || height == 0)
    {
        strncpy_s(status.sensitivityAuditReason, "invalid motion vector size", _TRUNCATE);
        return false;
    }

    const UINT roiWidth = std::min<UINT>(MV_CALIBRATION_ROI, width);
    const UINT roiHeight = std::min<UINT>(MV_CALIBRATION_ROI, height);
    const UINT rowPitch = FrameWarpAlignUInt(roiWidth * bytesPerPixel, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT64 requiredSize = static_cast<UINT64>(rowPitch) * roiHeight;

    bool recreate = _mvCalibrationReadback == nullptr || _mvCalibrationReadbackSize < requiredSize;
    if (recreate)
    {
        if (_mvCalibrationReadback != nullptr)
        {
            _mvCalibrationReadback->Release();
            _mvCalibrationReadback = nullptr;
        }

        auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(requiredSize);
        HRESULT hr = _device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_mvCalibrationReadback));
        if (FAILED(hr) || _mvCalibrationReadback == nullptr)
        {
            strncpy_s(status.sensitivityAuditReason, "MV readback allocation failed", _TRUNCATE);
            LOG_WARN("FrameWarp: MV calibration readback allocation failed {:X}", hr);
            return false;
        }
        _mvCalibrationReadback->SetName(L"FrameWarp_MVCalibrationReadback");
        _mvCalibrationReadbackSize = requiredSize;
    }

    const UINT srcX = (width - roiWidth) / 2;
    const UINT srcY = (height - roiHeight) / 2;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = _mvCalibrationReadback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = _trackedMotionVectorInputFormat;
    dst.PlacedFootprint.Footprint.Width = roiWidth;
    dst.PlacedFootprint.Footprint.Height = roiHeight;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = rowPitch;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = _trackedMotionVectorInput;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_BOX box {};
    box.left = srcX;
    box.top = srcY;
    box.front = 0;
    box.right = srcX + roiWidth;
    box.bottom = srcY + roiHeight;
    box.back = 1;

    if (FrameWarpStateNeedsTransition(_trackedMotionVectorInputState, D3D12_RESOURCE_STATE_COPY_SOURCE))
        FrameWarpResourceBarrier(cmdList, _trackedMotionVectorInput, _trackedMotionVectorInputState, D3D12_RESOURCE_STATE_COPY_SOURCE);

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    if (FrameWarpStateNeedsTransition(_trackedMotionVectorInputState, D3D12_RESOURCE_STATE_COPY_SOURCE))
        FrameWarpResourceBarrier(cmdList, _trackedMotionVectorInput, D3D12_RESOURCE_STATE_COPY_SOURCE, _trackedMotionVectorInputState);

    const float currentSensitivity = std::clamp(
        Config::Instance()->FrameWarpSensitivity.value_or_default(), 0.00005f, 0.005f);
    const CameraContext camera = ResolveCameraContext();
    const float tanHalfFovY = std::max(std::tan(camera.vFovRadians * 0.5f), 0.0001f);
    const float tanHalfFovX = std::max(tanHalfFovY * camera.aspectRatio, 0.0001f);
    const float renderYaw = status.lastRenderMouseStepDx * currentSensitivity;
    const float renderPitch = status.lastRenderMouseStepDy * currentSensitivity;

    _mvCalibrationWidth = roiWidth;
    _mvCalibrationHeight = roiHeight;
    _mvCalibrationRowPitch = rowPitch;
    _mvCalibrationFormat = _trackedMotionVectorInputFormat;
    _mvCalibrationPending = true;
    _mvCalibrationCopyScheduled = true;
    _mvCalibrationFrame = State::Instance().frameCount;
    _mvCalibrationScaleX = _trackedMotionVectorScaleX;
    _mvCalibrationScaleY = _trackedMotionVectorScaleY;
    _mvCalibrationSensitivity = currentSensitivity;
    _mvCalibrationRenderMouseDx = status.lastRenderMouseStepDx;
    _mvCalibrationRenderMouseDy = status.lastRenderMouseStepDy;
    _mvCalibrationPredPxX = std::tan(renderYaw) / tanHalfFovX * static_cast<float>(_outputWidth) * 0.5f;
    _mvCalibrationPredPxY = std::tan(renderPitch) / tanHalfFovY * static_cast<float>(_outputHeight) * 0.5f;
    strncpy_s(_mvCalibrationSource, _trackedMotionVectorSource, _TRUNCATE);

    status.sensitivityAuditCopyPending = true;
    strncpy_s(status.sensitivityAuditReason, "copy scheduled", _TRUNCATE);
    return true;
}

void FrameWarp_Dx12::RegisterStableUiFrame(
    int frameIndex,
    ID3D12Resource* originalHudless,
    D3D12_RESOURCE_STATES originalHudlessState,
    ID3D12Resource* warpedHudless,
    D3D12_RESOURCE_STATES warpedHudlessState,
    UINT width,
    UINT height,
    DXGI_FORMAT format)
{
    _pendingStableUi.valid = originalHudless != nullptr && warpedHudless != nullptr && width > 0 && height > 0;
    _pendingStableUi.frameIndex = frameIndex;
    _pendingStableUi.originalHudless = originalHudless;
    _pendingStableUi.originalHudlessState = originalHudlessState;
    _pendingStableUi.warpedHudless = warpedHudless;
    _pendingStableUi.warpedHudlessState = warpedHudlessState;
    _pendingStableUi.width = width;
    _pendingStableUi.height = height;
    _pendingStableUi.format = format;
}

bool FrameWarp_Dx12::CompositeStableUiToPresent(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* originalHudless,
    D3D12_RESOURCE_STATES originalHudlessState,
    ID3D12Resource* originalPresent,
    D3D12_RESOURCE_STATES originalPresentState,
    ID3D12Resource* warpedHudless,
    D3D12_RESOURCE_STATES warpedHudlessState,
    ID3D12Resource* outputPresent,
    D3D12_RESOURCE_STATES outputPresentState)
{
    if (!_enabled || !_initialized || cmdList == nullptr || originalHudless == nullptr ||
        originalPresent == nullptr || warpedHudless == nullptr || outputPresent == nullptr ||
        _stableUiOutput == nullptr || _pipelineStateStableUiComposite == nullptr)
    {
        FrameWarpSetSkipReason("stable UI unavailable");
        return false;
    }

    auto presentDesc = outputPresent->GetDesc();
    auto originalPresentDesc = originalPresent->GetDesc();
    auto hudlessDesc = originalHudless->GetDesc();
    auto warpedDesc = warpedHudless->GetDesc();
    auto stableDesc = _stableUiOutput->GetDesc();

    if (presentDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        presentDesc.SampleDesc.Count != 1 ||
        presentDesc.MipLevels != 1 ||
        presentDesc.DepthOrArraySize != 1 ||
        originalPresentDesc.Width != presentDesc.Width ||
        originalPresentDesc.Height != presentDesc.Height ||
        hudlessDesc.Width != presentDesc.Width ||
        hudlessDesc.Height != presentDesc.Height ||
        warpedDesc.Width != presentDesc.Width ||
        warpedDesc.Height != presentDesc.Height ||
        stableDesc.Width != presentDesc.Width ||
        stableDesc.Height != presentDesc.Height)
    {
        LOG_DEBUG("FrameWarp: stable UI composite skipped due to size/shape mismatch");
        FrameWarpSetSkipReason("stable UI size mismatch");
        return false;
    }

    if (originalPresentDesc.Format != presentDesc.Format ||
        hudlessDesc.Format != presentDesc.Format ||
        warpedDesc.Format != presentDesc.Format)
    {
        LOG_DEBUG("FrameWarp: stable UI composite skipped due to source format mismatch present={} presentCopy={} hudless={} warped={}",
            (UINT)presentDesc.Format, (UINT)originalPresentDesc.Format,
            (UINT)hudlessDesc.Format, (UINT)warpedDesc.Format);
        FrameWarpSetSkipReason("stable UI source format mismatch");
        return false;
    }

    DXGI_FORMAT stableCopyFormat = TranslateTypelessFormats(stableDesc.Format);
    DXGI_FORMAT presentCopyFormat = TranslateTypelessFormats(presentDesc.Format);
    if (stableDesc.Format != presentDesc.Format && stableCopyFormat != presentCopyFormat)
    {
        LOG_DEBUG("FrameWarp: stable UI composite skipped due to copy-incompatible formats {} -> {}",
            (UINT)stableDesc.Format, (UINT)presentDesc.Format);
        FrameWarpSetSkipReason("stable UI format mismatch");
        return false;
    }

    if (!EnsureStableUiPresentCopy(presentDesc))
        return false;

    _heapIndex = (_heapIndex + 1) % NUM_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_heapIndex];

    const bool originalIsOutput = (originalPresent == outputPresent);

    FrameWarpResourceBarrier(cmdList, originalPresent, originalPresentState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    FrameWarpResourceBarrier(cmdList, _stableUiPresentCopy, _stableUiPresentCopyState,
        D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyResource(_stableUiPresentCopy, originalPresent);

    if (originalIsOutput)
    {
        FrameWarpResourceBarrier(cmdList, outputPresent, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    }
    else
    {
        FrameWarpResourceBarrier(cmdList, originalPresent, D3D12_RESOURCE_STATE_COPY_SOURCE,
            originalPresentState);
        FrameWarpResourceBarrier(cmdList, outputPresent, outputPresentState,
            D3D12_RESOURCE_STATE_COPY_DEST);
    }

    FrameWarpResourceBarrier(cmdList, _stableUiPresentCopy, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    _stableUiPresentCopyState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    FrameWarpResourceBarrier(cmdList, originalHudless, originalHudlessState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    FrameWarpResourceBarrier(cmdList, warpedHudless, warpedHudlessState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    D3D12_SHADER_RESOURCE_VIEW_DESC hudlessSrvDesc = {};
    hudlessSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hudlessSrvDesc.Format = TranslateTypelessFormats(hudlessDesc.Format);
    hudlessSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    hudlessSrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(originalHudless, &hudlessSrvDesc, currentHeap.GetSrvCPU(0));

    D3D12_SHADER_RESOURCE_VIEW_DESC presentSrvDesc = {};
    presentSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    presentSrvDesc.Format = TranslateTypelessFormats(presentDesc.Format);
    presentSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    presentSrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(_stableUiPresentCopy, &presentSrvDesc, currentHeap.GetSrvCPU(1));

    D3D12_SHADER_RESOURCE_VIEW_DESC warpedSrvDesc = {};
    warpedSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    warpedSrvDesc.Format = TranslateTypelessFormats(warpedDesc.Format);
    warpedSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    warpedSrvDesc.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(warpedHudless, &warpedSrvDesc, currentHeap.GetSrvCPU(2));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = TranslateTypelessFormats(stableDesc.Format);
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    _device->CreateUnorderedAccessView(_stableUiOutput, nullptr, &uavDesc, currentHeap.GetUavCPU(0));

    FrameWarpShaderConstants constants {};
    constants.width = static_cast<UINT>(presentDesc.Width);
    constants.height = presentDesc.Height;

    BYTE* pCBDataBegin = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    auto mapResult = _constantBuffers[_heapIndex]->Map(0, &readRange, reinterpret_cast<void**>(&pCBDataBegin));
    if (mapResult != S_OK || pCBDataBegin == nullptr)
    {
        LOG_ERROR("FrameWarp_Dx12: Failed to map stable UI constant buffer");
        FrameWarpSetSkipReason("stable UI constant map failed");
        return false;
    }
    memcpy(pCBDataBegin, &constants, sizeof(constants));
    _constantBuffers[_heapIndex]->Unmap(0, nullptr);

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = _constantBuffers[_heapIndex]->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = sizeof(constants);
    _device->CreateConstantBufferView(&cbvDesc, currentHeap.GetCbvCPU(0));

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_pipelineStateStableUiComposite);
    cmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    UINT dispatchWidth = (constants.width + THREAD_GROUP_X - 1) / THREAD_GROUP_X;
    UINT dispatchHeight = (constants.height + THREAD_GROUP_Y - 1) / THREAD_GROUP_Y;
    cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = _stableUiOutput;
    cmdList->ResourceBarrier(1, &uavBarrier);

    FrameWarpResourceBarrier(cmdList, _stableUiOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(outputPresent, _stableUiOutput);
    FrameWarpResourceBarrier(cmdList, outputPresent, D3D12_RESOURCE_STATE_COPY_DEST, outputPresentState);
    FrameWarpResourceBarrier(cmdList, _stableUiOutput, D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    FrameWarpResourceBarrier(cmdList, originalHudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        originalHudlessState);
    FrameWarpResourceBarrier(cmdList, warpedHudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        warpedHudlessState);

    static uint64_t stableUiLogCount = 0;
    stableUiLogCount++;
    if (stableUiLogCount <= 10 || stableUiLogCount % 300 == 0)
        LOG_DEBUG("FrameWarp: stable UI composite applied #{}", stableUiLogCount);
    return true;
}

bool FrameWarp_Dx12::ApplyStableUiComposite(
    int frameIndex,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* presentWithUi,
    D3D12_RESOURCE_STATES presentState)
{
    if (!_pendingStableUi.valid || _pendingStableUi.frameIndex != frameIndex)
        return false;

    PendingStableUiFrame pending = _pendingStableUi;
    _pendingStableUi.valid = false;

    return CompositeStableUiToPresent(
        cmdList,
        pending.originalHudless,
        pending.originalHudlessState,
        presentWithUi,
        presentState,
        pending.warpedHudless,
        pending.warpedHudlessState,
        presentWithUi,
        presentState);
}

bool FrameWarp_Dx12::CompositeStableUiFrame(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* originalHudless,
    D3D12_RESOURCE_STATES originalHudlessState,
    ID3D12Resource* originalPresent,
    D3D12_RESOURCE_STATES originalPresentState,
    ID3D12Resource* warpedHudless,
    D3D12_RESOURCE_STATES warpedHudlessState,
    ID3D12Resource* outputPresent,
    D3D12_RESOURCE_STATES outputPresentState)
{
    return CompositeStableUiToPresent(
        cmdList,
        originalHudless,
        originalHudlessState,
        originalPresent,
        originalPresentState,
        warpedHudless,
        warpedHudlessState,
        outputPresent,
        outputPresentState);
}

bool FrameWarp_Dx12::EnsureStandaloneCommandObjects(ID3D12CommandQueue* commandQueue)
{
    if (commandQueue == nullptr || _device == nullptr)
        return false;

    if (_standaloneCommandAllocator != nullptr && _standaloneCommandList != nullptr &&
        _standaloneFence != nullptr && _standaloneFenceEvent != nullptr)
    {
        return true;
    }

    if (_standaloneCommandAllocator == nullptr)
    {
        HRESULT hr = _device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_standaloneCommandAllocator));
        if (FAILED(hr))
        {
            LOG_ERROR("FrameWarp_Dx12: Create standalone allocator failed {:X}", hr);
            return false;
        }
        _standaloneCommandAllocator->SetName(L"FrameWarp_StandaloneAllocator");
    }

    if (_standaloneCommandList == nullptr)
    {
        HRESULT hr = _device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, _standaloneCommandAllocator, nullptr,
            IID_PPV_ARGS(&_standaloneCommandList));
        if (FAILED(hr))
        {
            LOG_ERROR("FrameWarp_Dx12: Create standalone command list failed {:X}", hr);
            return false;
        }
        _standaloneCommandList->SetName(L"FrameWarp_StandaloneCommandList");
        _standaloneCommandList->Close();
    }

    if (_standaloneFence == nullptr)
    {
        HRESULT hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_standaloneFence));
        if (FAILED(hr))
        {
            LOG_ERROR("FrameWarp_Dx12: Create standalone fence failed {:X}", hr);
            return false;
        }
        _standaloneFence->SetName(L"FrameWarp_StandaloneFence");
    }

    if (_standaloneFenceEvent == nullptr)
    {
        _standaloneFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (_standaloneFenceEvent == nullptr)
        {
            LOG_ERROR("FrameWarp_Dx12: Create standalone fence event failed");
            return false;
        }
    }

    return true;
}

bool FrameWarp_Dx12::WaitStandaloneFence()
{
    if (_standaloneFence == nullptr || _standaloneFenceEvent == nullptr || _standaloneFenceValue == 0)
        return true;

    if (_standaloneFence->GetCompletedValue() >= _standaloneFenceValue)
        return true;

    HRESULT hr = _standaloneFence->SetEventOnCompletion(_standaloneFenceValue, _standaloneFenceEvent);
    if (FAILED(hr))
    {
        LOG_ERROR("FrameWarp_Dx12: SetEventOnCompletion failed {:X}", hr);
        return false;
    }

    DWORD waitResult = WaitForSingleObject(_standaloneFenceEvent, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("FrameWarp_Dx12: Standalone fence wait failed {:X}", waitResult);
        return false;
    }

    return true;
}

bool FrameWarp_Dx12::IsStandaloneFenceComplete() const
{
    if (_standaloneFence == nullptr || _standaloneFenceValue == 0)
        return true;

    return _standaloneFence->GetCompletedValue() >= _standaloneFenceValue;
}

bool FrameWarp_Dx12::ApplyStandaloneWarp(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue)
{
    if (!_enabled || !_initialized || swapChain == nullptr || commandQueue == nullptr)
    {
        FrameWarpSetSkipReason(!_enabled ? "disabled" : (!_initialized ? "not initialized" : "missing swapchain"));
        return false;
    }

    IDXGISwapChain3* swapChain3 = nullptr;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) || swapChain3 == nullptr)
    {
        FrameWarpSetSkipReason("swapchain3 unavailable");
        return false;
    }

    UINT backBufferIndex = swapChain3->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = nullptr;
    HRESULT hr = swapChain3->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer));
    swapChain3->Release();

    if (FAILED(hr) || backBuffer == nullptr)
    {
        FrameWarpSetSkipReason("backbuffer unavailable");
        return false;
    }

    auto desc = backBuffer->GetDesc();
    const UINT width = static_cast<UINT>(desc.Width);
    const UINT height = desc.Height;
    if (width == 0 || height == 0)
    {
        backBuffer->Release();
        FrameWarpSetSkipReason("invalid backbuffer size");
        return false;
    }

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.SampleDesc.Count != 1 ||
        desc.MipLevels != 1 ||
        desc.DepthOrArraySize != 1)
    {
        backBuffer->Release();
        FrameWarpSetSkipReason("unsupported backbuffer shape");
        return false;
    }

    if (_outputWidth != width || _outputHeight != height || _sourceFormat != desc.Format)
    {
        if (!Resize(width, height, desc.Format))
        {
            backBuffer->Release();
            FrameWarpSetSkipReason("resize failed");
            return false;
        }
    }

    if (_outputFormat != TranslateTypelessFormats(desc.Format) && _outputFormat != desc.Format)
    {
        LOG_DEBUG("FrameWarp standalone skipped: copy-incompatible output format {} for backbuffer format {}",
            (UINT)_outputFormat, (UINT)desc.Format);
        backBuffer->Release();
        FrameWarpSetSkipReason("copy format mismatch");
        return false;
    }

    ID3D12Resource* depthForWarp = nullptr;
    D3D12_RESOURCE_STATES depthForWarpState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const bool vibeflex2LatePresent =
        State::Instance().activeFgOutput == FGOutput::DLSSG &&
        Config::Instance()->FrameWarpDLSSGMode.value_or_default() == 3 &&
        !DLSSGNative::IsAttachActive();
    const uint32_t latePresentTestMode = vibeflex2LatePresent
        ? Config::Instance()->FrameWarpDLSSGLatePresentTestMode.value_or_default()
        : 0;
    const bool forceIdentityWarp =
        vibeflex2LatePresent && (latePresentTestMode == 2 || latePresentTestMode == 4);
    const bool disableStableUiForTest =
        vibeflex2LatePresent && (latePresentTestMode == 3 || latePresentTestMode == 4);
    if (Config::Instance()->FrameWarpDepthAware.value_or_default() &&
        vibeflex2LatePresent &&
        _depthSnapshotValid &&
        _depthSnapshot != nullptr)
    {
        depthForWarp = _depthSnapshot;
        depthForWarpState = _depthSnapshotState;
    }
    else if (Config::Instance()->FrameWarpDepthAware.value_or_default() &&
        _trackedDepthInputValid &&
        _trackedDepthInput != nullptr &&
        !vibeflex2LatePresent)
    {
        auto depthDesc = _trackedDepthInput->GetDesc();
        const bool depthShapeValid =
            depthDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            depthDesc.Width > 0 &&
            depthDesc.Height > 0 &&
            depthDesc.SampleDesc.Count == 1 &&
            depthDesc.MipLevels == 1 &&
            depthDesc.DepthOrArraySize == 1;

        if (depthShapeValid)
        {
            depthForWarp = _trackedDepthInput;
            depthForWarpState = _trackedDepthInputState;
        }
        else
        {
            _trackedDepthInputValid = false;
            LOG_DEBUG("FrameWarp standalone depth ignored: tracked depth shape became invalid");
        }
    }

    PreparedWarp preparedWarp = PrepareWarpForPresent(
        vibeflex2LatePresent ? "dlssg-late-preflight" : "standalone-preflight",
        false);

    if (forceIdentityWarp)
    {
        CameraPredictor::WarpResult identityWarp = _cameraPredictor.PredictFromMouseDelta(
            0.0f, 0.0f, _cameraPredictor._renderViewProj, 1.0f);
        if (identityWarp.isValid)
        {
            preparedWarp.status = PreparedWarpStatus::Ready;
            preparedWarp.warpResult = identityWarp;
            preparedWarp.magnitude = 0.0f;
            preparedWarp.approxPixelShift = 0.0f;
            preparedWarp.reusedLastResult = false;
            strncpy_s(preparedWarp.reason, sizeof(preparedWarp.reason), "identity diagnostic", _TRUNCATE);
            FrameWarpSetSkipReason("identity diagnostic");
            auto& status = State::Instance().frameWarpStatus;
            status.lastApproxPixelShift = 0.0f;
            status.lastPreparedPixelShift = 0.0f;
            status.lastPreparedDeltaYaw = 0.0f;
            status.lastPreparedDeltaPitch = 0.0f;
            strncpy_s(status.lastPreparedStatus, "Ready", _TRUNCATE);
            strncpy_s(status.lastPreparedReason, preparedWarp.reason, _TRUNCATE);
            status.lastDisocclusionPixels = 0.0f;
        }
    }

    auto preparedStatusName = [&]() -> const char* {
        switch (preparedWarp.status)
        {
        case PreparedWarpStatus::Ready: return "Ready";
        case PreparedWarpStatus::ZeroPose: return "ZeroPose";
        case PreparedWarpStatus::Invalid: return "Invalid";
        default: return "Unknown";
        }
    };

    auto auditStandalone = [&](const char* phase, const char* path, bool submitted) {
        static uint64_t auditCount = 0;
        auditCount++;
        if (!FrameWarpTimingAuditShouldLog(auditCount))
            return;

        auto& status = State::Instance().frameWarpStatus;
        LOG_INFO("VF2AuditStandalone #{} frame={} phase={} path={} late={} testMode={} submitted={} prepared={} reason={} backbuffer={} size={}x{} fmt={} rawSeq={} snapSeq={} sinceSnap={} snapshots={} snapSrc={} rawSrc={} activeSrc={} snapAgeMs={:.3f} sampleAgeMs={:.3f} finalMouse=({:.1f},{:.1f}) step=({:.1f},{:.1f}) yaw={:.6f} pitch={:.6f} mag={:.6f} px={:.3f} depth={} depthAge={:.0f} depthHist={} depthMask={:.3f} stableUi={} uiAge={} skip={}",
            auditCount,
            State::Instance().frameCount,
            phase != nullptr ? phase : "unknown",
            path != nullptr ? path : "unknown",
            vibeflex2LatePresent,
            latePresentTestMode,
            submitted,
            preparedStatusName(),
            preparedWarp.reason[0] ? preparedWarp.reason : "none",
            backBufferIndex,
            width,
            height,
            static_cast<uint32_t>(desc.Format),
            status.rawInputSequence,
            status.rawInputSnapshotSequence,
            status.rawInputSamplesSinceSnapshot,
            status.renderSnapshotCount,
            status.lastSnapshotSource[0] ? status.lastSnapshotSource : "none",
            status.lastRawInputSource[0] ? status.lastRawInputSource : "none",
            status.activeRawInputSource[0] ? status.activeRawInputSource : "none",
            status.lastSnapshotAgeMs,
            status.lastInputSampleAgeMs,
            status.lastFinalMouseDeltaDx,
            status.lastFinalMouseDeltaDy,
            status.lastFinalMouseDeltaStepDx,
            status.lastFinalMouseDeltaStepDy,
            preparedWarp.warpResult.deltaYaw,
            preparedWarp.warpResult.deltaPitch,
            preparedWarp.magnitude,
            preparedWarp.approxPixelShift,
            depthForWarp != nullptr,
            status.depthInfillAgeFrames,
            status.depthInfillHistoryUsed,
            status.depthInfillMaskCoveragePct,
            status.lastStableUiValid,
            status.lastStableUiAge,
            status.lastSkipReason[0] ? status.lastSkipReason : "none");
    };

    if (preparedWarp.status != PreparedWarpStatus::Ready)
    {
        if (vibeflex2LatePresent)
        {
            auto& status = State::Instance().frameWarpStatus;
            if (preparedWarp.status == PreparedWarpStatus::ZeroPose)
                status.dlssgLatePresentZeroPoseCount++;
            strncpy_s(status.lastStandaloneWarpPath, "preflight-skip", _TRUNCATE);
        }
        auditStandalone("skip", "preflight", false);
        backBuffer->Release();
        return false;
    }

    bool recreateInputCopy = (_standaloneInputCopy == nullptr);
    if (_standaloneInputCopy != nullptr)
    {
        auto copyDesc = _standaloneInputCopy->GetDesc();
        recreateInputCopy =
            copyDesc.Width != desc.Width ||
            copyDesc.Height != desc.Height ||
            copyDesc.Format != desc.Format ||
            copyDesc.SampleDesc.Count != desc.SampleDesc.Count;
    }

    if (recreateInputCopy)
    {
        if (_standaloneInputCopy != nullptr)
        {
            _standaloneInputCopy->Release();
            _standaloneInputCopy = nullptr;
        }

        D3D12_RESOURCE_DESC inputDesc = desc;
        inputDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        hr = _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
            &inputDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&_standaloneInputCopy));
        if (FAILED(hr) || _standaloneInputCopy == nullptr)
        {
            LOG_ERROR("FrameWarp_Dx12: Create standalone input copy failed {:X}", hr);
            backBuffer->Release();
            FrameWarpSetSkipReason("input copy allocation failed");
            return false;
        }

        _standaloneInputCopy->SetName(L"FrameWarp_StandaloneInputCopy");
        _standaloneInputCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    if (!EnsureStandaloneCommandObjects(commandQueue))
    {
        backBuffer->Release();
        FrameWarpSetSkipReason("standalone command setup failed");
        return false;
    }

    if (vibeflex2LatePresent)
    {
        auto& status = State::Instance().frameWarpStatus;
        const bool previousFenceComplete = IsStandaloneFenceComplete();
        status.lastVf2PreviousFenceComplete = previousFenceComplete;
        const int64_t fenceCheckQpc = FrameWarpQueryQpc();
        if (previousFenceComplete)
            status.lastVf2FenceCompleteQpc = fenceCheckQpc;
        status.lastVf2PreviousSubmitAgeMs = status.lastVf2SubmitQpc != 0
            ? static_cast<float>(FrameWarpQpcToMilliseconds(fenceCheckQpc - status.lastVf2SubmitQpc))
            : 0.0f;

        if (!previousFenceComplete)
        {
            status.dlssgLatePresentInFlightSkipCount++;
            FrameWarpSetSkipReason("previous warp in flight");
            strncpy_s(status.lastStandaloneWarpPath, "in-flight-skip", _TRUNCATE);
            auditStandalone("skip", "in-flight", false);
            backBuffer->Release();
            return false;
        }
        ProcessMotionVectorCalibrationReadback();
    }
    else if (!WaitStandaloneFence())
    {
        backBuffer->Release();
        FrameWarpSetSkipReason("standalone command setup failed");
        return false;
    }
    else
    {
        ProcessMotionVectorCalibrationReadback();
    }

    hr = _standaloneCommandAllocator->Reset();
    if (FAILED(hr))
    {
        backBuffer->Release();
        FrameWarpSetSkipReason("allocator reset failed");
        return false;
    }

    hr = _standaloneCommandList->Reset(_standaloneCommandAllocator, nullptr);
    if (FAILED(hr))
    {
        backBuffer->Release();
        FrameWarpSetSkipReason("command list reset failed");
        return false;
    }

    _mvCalibrationCopyScheduled = false;
    CaptureMotionVectorCalibrationSample(_standaloneCommandList);

    bool useHudlessSource = false;
    const bool stableHudfixEnabled =
        Config::Instance()->FGHUDFix.value_or_default() && !disableStableUiForTest;
    if (!stableHudfixEnabled)
        _hudlessInputCopyValid = false;

    if (stableHudfixEnabled && _hudlessInputCopyValid && _hudlessInputCopy != nullptr)
    {
        auto hudlessDesc = _hudlessInputCopy->GetDesc();
        useHudlessSource =
            hudlessDesc.Width == desc.Width &&
            hudlessDesc.Height == desc.Height &&
            hudlessDesc.Format == desc.Format &&
            hudlessDesc.SampleDesc.Count == desc.SampleDesc.Count;

        if (!useHudlessSource)
        {
            LOG_DEBUG("FrameWarp standalone stable UI skipped: hudless {}x{} fmt={} backbuffer {}x{} fmt={}",
                (UINT)hudlessDesc.Width, hudlessDesc.Height, (UINT)hudlessDesc.Format,
                width, height, (UINT)desc.Format);
        }
    }

    if (useHudlessSource)
    {
        UINT64 stableUiFrameID = ++_stableUiSerial;
        bool uiExtracted = ExtractUiLayerFromHudless(
            _standaloneCommandList,
            stableUiFrameID,
            _hudlessInputCopy,
            _hudlessInputCopyState,
            backBuffer,
            D3D12_RESOURCE_STATE_PRESENT,
            "standalone-hudfix");
        _hudlessInputCopyState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        if (!uiExtracted)
        {
            LOG_DEBUG("FrameWarp standalone stable UI skipped: UI layer extraction failed");
        }
        else
        {
            ID3D12Resource* warpedOutput = nullptr;
            PresentResult presentResult = OnPrePresentPrepared(
                _standaloneCommandList,
                _hudlessInputCopy,
                depthForWarp,
                nullptr,
                &warpedOutput,
                _hudlessInputCopyState,
                nullptr,
                depthForWarpState,
                "standalone-hudless",
                preparedWarp);
            _hudlessInputCopyState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if (!vibeflex2LatePresent && depthForWarp != nullptr && presentResult == PresentResult::Warped)
                _trackedDepthInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

            const bool warped = presentResult == PresentResult::Warped && warpedOutput == _warpedOutput;
            const bool zeroPose = presentResult == PresentResult::ZeroPose && warpedOutput == _hudlessInputCopy;
            if (!warped && !zeroPose)
            {
                _standaloneCommandList->Close();
                backBuffer->Release();
                return false;
            }

            ID3D12Resource* sceneForUi = warped ? _warpedOutput : _hudlessInputCopy;
            D3D12_RESOURCE_STATES sceneForUiState =
                warped ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : _hudlessInputCopyState;

            bool composited = CompositeCachedUiLayer(
                _standaloneCommandList,
                stableUiFrameID,
                0,
                sceneForUi,
                sceneForUiState,
                backBuffer,
                D3D12_RESOURCE_STATE_PRESENT,
                "standalone");

            if (!composited)
            {
                _standaloneCommandList->Close();
                backBuffer->Release();
                return false;
            }

            hr = _standaloneCommandList->Close();
            if (FAILED(hr))
            {
                backBuffer->Release();
                FrameWarpSetSkipReason("command list close failed");
                return false;
            }

            ID3D12CommandList* lists[] = { _standaloneCommandList };
            commandQueue->ExecuteCommandLists(1, lists);

            _standaloneFenceValue++;
            hr = commandQueue->Signal(_standaloneFence, _standaloneFenceValue);
            if (FAILED(hr))
                LOG_WARN("FrameWarp_Dx12: Standalone fence signal failed {:X}", hr);
            if (_mvCalibrationCopyScheduled)
            {
                _mvCalibrationFenceValue = _standaloneFenceValue;
                State::Instance().frameWarpStatus.sensitivityAuditCopyPending = true;
            }

            if (vibeflex2LatePresent)
            {
                auto& status = State::Instance().frameWarpStatus;
                status.dlssgLatePresentSubmittedCount++;
                status.lastVf2SubmitQpc = FrameWarpQueryQpc();
                strncpy_s(status.lastStandaloneWarpPath, "hudless-submit", _TRUNCATE);
            }

            auditStandalone("submit", "hudless", true);

            backBuffer->Release();
            return true;
        }
    }

    const D3D12_RESOURCE_STATES inputCopyStateBefore = _standaloneInputCopyState;
    FrameWarpResourceBarrier(
        _standaloneCommandList, backBuffer,
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    FrameWarpResourceBarrier(
        _standaloneCommandList, _standaloneInputCopy,
        inputCopyStateBefore, D3D12_RESOURCE_STATE_COPY_DEST);

    _standaloneCommandList->CopyResource(_standaloneInputCopy, backBuffer);

    FrameWarpResourceBarrier(
        _standaloneCommandList, backBuffer,
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    FrameWarpResourceBarrier(
        _standaloneCommandList, _standaloneInputCopy,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    _standaloneInputCopyState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    ID3D12Resource* warpedOutput = nullptr;
    PresentResult presentResult = OnPrePresentPrepared(
        _standaloneCommandList,
        _standaloneInputCopy,
        depthForWarp,
        nullptr,
        &warpedOutput,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        nullptr,
        depthForWarpState,
        "standalone-backbuffer",
        preparedWarp);
    bool warped = presentResult == PresentResult::Warped;
    if (!vibeflex2LatePresent && depthForWarp != nullptr && warped)
        _trackedDepthInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (!warped || warpedOutput != _warpedOutput)
    {
        _standaloneCommandList->Close();
        backBuffer->Release();
        return false;
    }

    FrameWarpResourceBarrier(
        _standaloneCommandList, _warpedOutput,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    FrameWarpResourceBarrier(
        _standaloneCommandList, backBuffer,
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

    _standaloneCommandList->CopyResource(backBuffer, _warpedOutput);

    FrameWarpResourceBarrier(
        _standaloneCommandList, backBuffer,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    FrameWarpResourceBarrier(
        _standaloneCommandList, _warpedOutput,
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    hr = _standaloneCommandList->Close();
    if (FAILED(hr))
    {
        backBuffer->Release();
        FrameWarpSetSkipReason("command list close failed");
        return false;
    }

    ID3D12CommandList* lists[] = { _standaloneCommandList };
    commandQueue->ExecuteCommandLists(1, lists);

    _standaloneFenceValue++;
    hr = commandQueue->Signal(_standaloneFence, _standaloneFenceValue);
    if (FAILED(hr))
        LOG_WARN("FrameWarp_Dx12: Standalone fence signal failed {:X}", hr);
    if (_mvCalibrationCopyScheduled)
    {
        _mvCalibrationFenceValue = _standaloneFenceValue;
        State::Instance().frameWarpStatus.sensitivityAuditCopyPending = true;
    }

    if (vibeflex2LatePresent)
    {
        auto& status = State::Instance().frameWarpStatus;
        status.dlssgLatePresentSubmittedCount++;
        status.lastVf2SubmitQpc = FrameWarpQueryQpc();
        strncpy_s(status.lastStandaloneWarpPath, "backbuffer-submit", _TRUNCATE);
    }

    auditStandalone("submit", "backbuffer", true);

    _standaloneInputCopyState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    backBuffer->Release();
    return true;
}

bool FrameWarp_Dx12::ApplyDLSSGResourceCopyWarp(
    ID3D12CommandQueue* commandQueue,
    ID3D12Resource* targetResource,
    D3D12_RESOURCE_STATES targetState,
    ID3D12Resource* sourceResource,
    const char* reason)
{
    auto& status = State::Instance().frameWarpStatus;
    const auto policy = FrameWarpDLSSG::GetResourceWarpPolicy();
    status.dlssgResourceWarpPolicyMode = policy.mode;
    strncpy_s(status.dlssgResourceWarpPolicyName, policy.modeName, _TRUNCATE);
    status.dlssgResourceWarpLastApplied = false;
    status.dlssgResourceWarpLastDst = reinterpret_cast<uint64_t>(targetResource);
    status.dlssgResourceWarpLastSrc = reinterpret_cast<uint64_t>(sourceResource);
    status.dlssgResourceWarpLastQueue = reinterpret_cast<uint64_t>(commandQueue);
    status.dlssgResourceWarpLastBeforeState = static_cast<uint32_t>(targetState);
    status.dlssgResourceWarpLastAfterState = static_cast<uint32_t>(targetState);
    strncpy_s(status.dlssgResourceWarpLastReason, reason != nullptr ? reason : "candidate", _TRUNCATE);

    auto skip = [&](const char* skipReason) {
        status.dlssgResourceWarpSkippedCount++;
        status.dlssgResourceWarpLastApplied = false;
        strncpy_s(status.dlssgResourceWarpLastReason, skipReason != nullptr ? skipReason : "skipped", _TRUNCATE);
        FrameWarpSetSkipReason(status.dlssgResourceWarpLastReason);
        _mouseTracker.PromoteDLSSGResourcePendingAnchor();
        if (FrameWarpTimingAuditShouldLog(status.dlssgResourceWarpSkippedCount))
        {
            LOG_INFO("VF2DLSSGResourceWarp frame={} applied=false submitted=false reason={} queue={:X} dst={:X} src={:X} size={}x{} fmt={} state={:X}",
                State::Instance().frameCount,
                status.dlssgResourceWarpLastReason,
                reinterpret_cast<uintptr_t>(commandQueue),
                reinterpret_cast<uintptr_t>(targetResource),
                reinterpret_cast<uintptr_t>(sourceResource),
                status.dlssgResourceWarpLastWidth,
                status.dlssgResourceWarpLastHeight,
                status.dlssgResourceWarpLastFormat,
                static_cast<uint32_t>(targetState));
        }
        return false;
    };

    if (!_enabled || !_initialized || commandQueue == nullptr || targetResource == nullptr)
        return skip(!_enabled ? "disabled" : (!_initialized ? "not initialized" : "missing target"));
    if (!policy.resourceCopyEnabled)
        return skip("resource-copy policy disabled");

    auto desc = targetResource->GetDesc();
    status.dlssgResourceWarpLastWidth = static_cast<uint32_t>(desc.Width);
    status.dlssgResourceWarpLastHeight = desc.Height;
    status.dlssgResourceWarpLastFormat = static_cast<uint32_t>(desc.Format);

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Width == 0 || desc.Height == 0 ||
        desc.SampleDesc.Count != 1 ||
        desc.MipLevels != 1 ||
        desc.DepthOrArraySize != 1)
    {
        return skip("unsupported target shape");
    }

    const UINT width = static_cast<UINT>(desc.Width);
    const UINT height = desc.Height;
    if (_outputWidth != width || _outputHeight != height || _sourceFormat != desc.Format)
    {
        if (!Resize(width, height, desc.Format))
            return skip("resize failed");
    }

    if (_outputFormat != TranslateTypelessFormats(desc.Format) && _outputFormat != desc.Format)
        return skip("copy format mismatch");

    PreparedWarp preparedWarp = PrepareWarpForPresent("dlssg-resource-copy", true);
    auto preparedStatusName = [&]() -> const char* {
        switch (preparedWarp.status)
        {
        case PreparedWarpStatus::Ready: return "Ready";
        case PreparedWarpStatus::ZeroPose: return "ZeroPose";
        case PreparedWarpStatus::Invalid: return "Invalid";
        default: return "Unknown";
        }
    };

    const bool strictMode4HudlessSubmit = policy.strictHudlessUi;
    const UINT64 currentFrame = State::Instance().frameCount;
    bool hudlessSubmitUiAvailable =
        _dlssgHudlessSubmitUiValid &&
        _dlssgHudlessSubmitUiFrameID != 0 &&
        _dlssgHudlessSubmitUiFrame <= currentFrame &&
        (currentFrame - _dlssgHudlessSubmitUiFrame) <= 8;
    if (_dlssgHudlessSubmitUiValid && !hudlessSubmitUiAvailable)
    {
        InvalidateDLSSGHudlessSubmit("UI cache stale before resource warp");
    }

    if (strictMode4HudlessSubmit && !hudlessSubmitUiAvailable)
    {
        status.dlssgResourceWarpLastApplied = false;
        strncpy_s(status.dlssgResourceWarpLastReason, "mode4-ui-cache-required", _TRUNCATE);
        status.dlssgResourceWarpSkippedCount++;
        FrameWarpSetSkipReason(status.dlssgResourceWarpLastReason);
        _mouseTracker.PromoteDLSSGResourcePendingAnchor();
        LOG_INFO("VF2DLSSGResourceWarp frame={} applied=false submitted=false reason=mode4-ui-cache-required prepared={} hudlessReady={} hudlessReason={} queue={:X} dst={:X} src={:X} size={}x{} fmt={} state={:X} rawSeq={} snapSeq={} snapshots={} px={:.3f}",
            State::Instance().frameCount,
            preparedStatusName(),
            State::Instance().frameWarpStatus.dlssgHudlessSubmitReady,
            State::Instance().frameWarpStatus.dlssgHudlessSubmitLastReason[0]
                ? State::Instance().frameWarpStatus.dlssgHudlessSubmitLastReason
                : "none",
            reinterpret_cast<uintptr_t>(commandQueue),
            reinterpret_cast<uintptr_t>(targetResource),
            reinterpret_cast<uintptr_t>(sourceResource),
            width,
            height,
            static_cast<uint32_t>(desc.Format),
            static_cast<uint32_t>(targetState),
            status.rawInputSequence,
            status.rawInputSnapshotSequence,
            status.renderSnapshotCount,
            preparedWarp.approxPixelShift);
        return false;
    }

    if (preparedWarp.status != PreparedWarpStatus::Ready && !hudlessSubmitUiAvailable)
    {
        status.dlssgResourceWarpLastApplied = false;
        strncpy_s(status.dlssgResourceWarpLastReason,
            preparedWarp.reason[0] ? preparedWarp.reason : "prepare skipped",
            _TRUNCATE);
        status.dlssgResourceWarpSkippedCount++;
        _mouseTracker.PromoteDLSSGResourcePendingAnchor();
        LOG_INFO("VF2DLSSGResourceWarp frame={} applied=false submitted=false reason={} prepared={} queue={:X} dst={:X} src={:X} size={}x{} fmt={} state={:X} rawSeq={} snapSeq={} snapshots={} px={:.3f}",
            State::Instance().frameCount,
            status.dlssgResourceWarpLastReason,
            preparedStatusName(),
            reinterpret_cast<uintptr_t>(commandQueue),
            reinterpret_cast<uintptr_t>(targetResource),
            reinterpret_cast<uintptr_t>(sourceResource),
            width,
            height,
            static_cast<uint32_t>(desc.Format),
            static_cast<uint32_t>(targetState),
            status.rawInputSequence,
            status.rawInputSnapshotSequence,
            status.renderSnapshotCount,
            preparedWarp.approxPixelShift);
        return false;
    }

    if (!EnsureStandaloneCommandObjects(commandQueue))
        return skip("command setup failed");

    if (!IsStandaloneFenceComplete())
    {
        status.dlssgResourceWarpInFlightSkipCount++;
        return skip("previous resource warp in flight");
    }

    bool recreateInputCopy = (_standaloneInputCopy == nullptr);
    if (_standaloneInputCopy != nullptr)
    {
        auto copyDesc = _standaloneInputCopy->GetDesc();
        recreateInputCopy =
            copyDesc.Width != desc.Width ||
            copyDesc.Height != desc.Height ||
            copyDesc.Format != desc.Format ||
            copyDesc.SampleDesc.Count != desc.SampleDesc.Count;
    }

    if (recreateInputCopy)
    {
        if (_standaloneInputCopy != nullptr)
        {
            _standaloneInputCopy->Release();
            _standaloneInputCopy = nullptr;
        }

        D3D12_RESOURCE_DESC inputDesc = desc;
        inputDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
            &inputDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&_standaloneInputCopy));
        if (FAILED(hr) || _standaloneInputCopy == nullptr)
            return skip("input copy allocation failed");

        _standaloneInputCopy->SetName(L"FrameWarp_DLSSGResourceInputCopy");
        _standaloneInputCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    HRESULT hr = _standaloneCommandAllocator->Reset();
    if (FAILED(hr))
        return skip("allocator reset failed");

    hr = _standaloneCommandList->Reset(_standaloneCommandAllocator, nullptr);
    if (FAILED(hr))
        return skip("command list reset failed");

    const bool stableHudfixConfigured = Config::Instance()->FGHUDFix.value_or_default();
    const bool stableHudfixEnabled = false;
    if (stableHudfixConfigured)
    {
        static uint64_t dlssgHudfixBypassLogCount = 0;
        dlssgHudfixBypassLogCount++;
        if (dlssgHudfixBypassLogCount <= 40 || dlssgHudfixBypassLogCount % 120 == 0)
        {
            LOG_INFO("VF2DLSSGResourceWarpUI frame={} stableUi=false reason=legacy hudfix bypassed; using DLSSG HUD-less submit UI restore when available",
                State::Instance().frameCount);
        }
    }

    bool useHudlessSource = false;
    if (stableHudfixEnabled && _hudlessInputCopyValid && _hudlessInputCopy != nullptr)
    {
        auto hudlessDesc = _hudlessInputCopy->GetDesc();
        useHudlessSource =
            hudlessDesc.Dimension == desc.Dimension &&
            hudlessDesc.Width == desc.Width &&
            hudlessDesc.Height == desc.Height &&
            hudlessDesc.Format == desc.Format &&
            hudlessDesc.SampleDesc.Count == desc.SampleDesc.Count &&
            hudlessDesc.MipLevels == desc.MipLevels &&
            hudlessDesc.DepthOrArraySize == desc.DepthOrArraySize;

        if (!useHudlessSource)
        {
            static uint64_t hudlessMismatchLogCount = 0;
            hudlessMismatchLogCount++;
            if (hudlessMismatchLogCount <= 20 || hudlessMismatchLogCount % 120 == 0)
            {
                LOG_INFO("VF2DLSSGResourceWarpUI frame={} stableUi=false reason=hudless mismatch hudless={}x{} fmt={} target={}x{} fmt={}",
                    State::Instance().frameCount,
                    static_cast<UINT>(hudlessDesc.Width),
                    hudlessDesc.Height,
                    static_cast<uint32_t>(hudlessDesc.Format),
                    width,
                    height,
                    static_cast<uint32_t>(desc.Format));
            }
        }
    }

    if (stableHudfixEnabled && !useHudlessSource)
    {
        static uint64_t noHudlessLogCount = 0;
        noHudlessLogCount++;
        if (noHudlessLogCount <= 20 || noHudlessLogCount % 120 == 0)
        {
            LOG_INFO("VF2DLSSGResourceWarpUI frame={} stableUi=false reason={} hudlessValid={} hudless={:X}",
                State::Instance().frameCount,
                _hudlessInputCopy == nullptr ? "no hudless copy" : "hudless not usable",
                _hudlessInputCopyValid,
                reinterpret_cast<uintptr_t>(_hudlessInputCopy));
        }
    }

    if (useHudlessSource)
    {
        UINT64 stableUiFrameID = ++_stableUiSerial;
        bool uiExtracted = ExtractUiLayerFromHudless(
            _standaloneCommandList,
            stableUiFrameID,
            _hudlessInputCopy,
            _hudlessInputCopyState,
            targetResource,
            targetState,
            "dlssg-resource-hudfix");
        _hudlessInputCopyState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        if (!uiExtracted)
        {
            _standaloneCommandList->Close();
            return skip("stable UI extract failed");
        }

        ID3D12Resource* warpedHudless = nullptr;
        PresentResult hudlessWarpResult = OnPrePresentPrepared(
            _standaloneCommandList,
            _hudlessInputCopy,
            nullptr,
            nullptr,
            &warpedHudless,
            _hudlessInputCopyState,
            nullptr,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            "dlssg-resource-hudless",
            preparedWarp);
        _hudlessInputCopyState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        if (hudlessWarpResult != PresentResult::Warped || warpedHudless != _warpedOutput)
        {
            _standaloneCommandList->Close();
            return skip("hudless warp shader skipped");
        }

        bool composited = CompositeCachedUiLayer(
            _standaloneCommandList,
            stableUiFrameID,
            0,
            _warpedOutput,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            targetResource,
            targetState,
            "dlssg-resource",
            false,
            false);

        if (!composited)
        {
            _standaloneCommandList->Close();
            return skip("stable UI composite failed");
        }

        hr = _standaloneCommandList->Close();
        if (FAILED(hr))
            return skip("command list close failed");

        ID3D12CommandList* lists[] = { _standaloneCommandList };
        commandQueue->ExecuteCommandLists(1, lists);

        _standaloneFenceValue++;
        hr = commandQueue->Signal(_standaloneFence, _standaloneFenceValue);
        if (FAILED(hr))
            LOG_WARN("FrameWarp_Dx12: DLSSG resource warp fence signal failed {:X}", hr);

        status.dlssgResourceWarpSubmittedCount++;
        status.dlssgResourceWarpAppliedCount++;
        status.dlssgResourceWarpLastApplied = true;
        status.lastVf2SubmitQpc = FrameWarpQueryQpc();
        strncpy_s(status.lastStandaloneWarpPath, "dlssg-resource-hudless", _TRUNCATE);
        strncpy_s(status.dlssgResourceWarpLastReason, "applied hudless-ui", _TRUNCATE);
        _mouseTracker.PromoteDLSSGResourcePendingAnchor();

        LOG_INFO("VF2DLSSGResourceWarp frame={} applied=true submitted=true reason=applied-hudless-ui prepared={} queue={:X} dst={:X} src={:X} hudless={:X} size={}x{} fmt={} state={:X} rawSeq={} snapSeq={} snapshots={} yaw={:.6f} pitch={:.6f} px={:.3f}",
            State::Instance().frameCount,
            preparedStatusName(),
            reinterpret_cast<uintptr_t>(commandQueue),
            reinterpret_cast<uintptr_t>(targetResource),
            reinterpret_cast<uintptr_t>(sourceResource),
            reinterpret_cast<uintptr_t>(_hudlessInputCopy),
            width,
            height,
            static_cast<uint32_t>(desc.Format),
            static_cast<uint32_t>(targetState),
            status.rawInputSequence,
            status.rawInputSnapshotSequence,
            status.renderSnapshotCount,
            preparedWarp.warpResult.deltaYaw,
            preparedWarp.warpResult.deltaPitch,
            preparedWarp.approxPixelShift);

        return true;
    }

    const D3D12_RESOURCE_STATES inputCopyStateBefore = _standaloneInputCopyState;
    FrameWarpResourceBarrier(
        _standaloneCommandList, targetResource,
        targetState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    FrameWarpResourceBarrier(
        _standaloneCommandList, _standaloneInputCopy,
        inputCopyStateBefore, D3D12_RESOURCE_STATE_COPY_DEST);

    _standaloneCommandList->CopyResource(_standaloneInputCopy, targetResource);

    FrameWarpResourceBarrier(
        _standaloneCommandList, targetResource,
        D3D12_RESOURCE_STATE_COPY_SOURCE, targetState);
    FrameWarpResourceBarrier(
        _standaloneCommandList, _standaloneInputCopy,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    _standaloneInputCopyState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (preparedWarp.status != PreparedWarpStatus::Ready)
    {
        bool composited = CompositeDLSSGHudlessSubmitUi(
            _standaloneCommandList,
            _standaloneInputCopy,
            _standaloneInputCopyState,
            targetResource,
            targetState);

        if (!composited)
        {
            _standaloneCommandList->Close();
            return skip("UI restore skipped");
        }

        hr = _standaloneCommandList->Close();
        if (FAILED(hr))
            return skip("command list close failed");

        ID3D12CommandList* lists[] = { _standaloneCommandList };
        commandQueue->ExecuteCommandLists(1, lists);

        _standaloneFenceValue++;
        hr = commandQueue->Signal(_standaloneFence, _standaloneFenceValue);
        if (FAILED(hr))
            LOG_WARN("FrameWarp_Dx12: DLSSG UI restore fence signal failed {:X}", hr);

        status.dlssgResourceWarpSubmittedCount++;
        status.dlssgResourceWarpLastApplied = false;
        status.lastVf2SubmitQpc = FrameWarpQueryQpc();
        strncpy_s(status.lastStandaloneWarpPath, "dlssg-hudless-ui-restore", _TRUNCATE);
        strncpy_s(status.dlssgResourceWarpLastReason, "UI restored no warp", _TRUNCATE);
        _mouseTracker.PromoteDLSSGResourcePendingAnchor();

        LOG_INFO("VF2DLSSGResourceWarp frame={} applied=false submitted=true reason=ui-restored-no-warp prepared={} queue={:X} dst={:X} src={:X} size={}x{} fmt={} state={:X} rawSeq={} snapSeq={} snapshots={} px={:.3f}",
            State::Instance().frameCount,
            preparedStatusName(),
            reinterpret_cast<uintptr_t>(commandQueue),
            reinterpret_cast<uintptr_t>(targetResource),
            reinterpret_cast<uintptr_t>(sourceResource),
            width,
            height,
            static_cast<uint32_t>(desc.Format),
            static_cast<uint32_t>(targetState),
            status.rawInputSequence,
            status.rawInputSnapshotSequence,
            status.renderSnapshotCount,
            preparedWarp.approxPixelShift);

        return true;
    }

    ID3D12Resource* warpedOutput = nullptr;
    PresentResult presentResult = OnPrePresentPrepared(
        _standaloneCommandList,
        _standaloneInputCopy,
        nullptr,
        nullptr,
        &warpedOutput,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        nullptr,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        "dlssg-resource-copy",
        preparedWarp);

    if (presentResult != PresentResult::Warped || warpedOutput != _warpedOutput)
    {
        _standaloneCommandList->Close();
        _standaloneInputCopyState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        return skip("warp shader skipped");
    }

    bool restoredHudlessSubmitUi = false;
    if (hudlessSubmitUiAvailable)
    {
        restoredHudlessSubmitUi = CompositeDLSSGHudlessSubmitUi(
            _standaloneCommandList,
            _warpedOutput,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            targetResource,
            targetState);
        if (!restoredHudlessSubmitUi)
        {
            _standaloneCommandList->Close();
            return skip("UI restore failed");
        }
    }
    else
    {
        FrameWarpResourceBarrier(
            _standaloneCommandList, _warpedOutput,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        FrameWarpResourceBarrier(
            _standaloneCommandList, targetResource,
            targetState, D3D12_RESOURCE_STATE_COPY_DEST);

        _standaloneCommandList->CopyResource(targetResource, _warpedOutput);

        FrameWarpResourceBarrier(
            _standaloneCommandList, targetResource,
            D3D12_RESOURCE_STATE_COPY_DEST, targetState);
        FrameWarpResourceBarrier(
            _standaloneCommandList, _warpedOutput,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    hr = _standaloneCommandList->Close();
    if (FAILED(hr))
        return skip("command list close failed");

    ID3D12CommandList* lists[] = { _standaloneCommandList };
    commandQueue->ExecuteCommandLists(1, lists);

    _standaloneFenceValue++;
    hr = commandQueue->Signal(_standaloneFence, _standaloneFenceValue);
    if (FAILED(hr))
        LOG_WARN("FrameWarp_Dx12: DLSSG resource warp fence signal failed {:X}", hr);

    status.dlssgResourceWarpSubmittedCount++;
    status.dlssgResourceWarpAppliedCount++;
    status.dlssgResourceWarpLastApplied = true;
    status.lastVf2SubmitQpc = FrameWarpQueryQpc();
    strncpy_s(status.lastStandaloneWarpPath,
        restoredHudlessSubmitUi ? "dlssg-hudless-submit-resource" : "dlssg-resource",
        _TRUNCATE);
    strncpy_s(status.dlssgResourceWarpLastReason,
        restoredHudlessSubmitUi ? "applied hudless-submit-ui" : "applied",
        _TRUNCATE);
    _mouseTracker.PromoteDLSSGResourcePendingAnchor();

    LOG_INFO("VF2DLSSGResourceWarp frame={} applied=true submitted=true reason={} prepared={} queue={:X} dst={:X} src={:X} size={}x{} fmt={} state={:X} rawSeq={} snapSeq={} snapshots={} yaw={:.6f} pitch={:.6f} px={:.3f}",
        State::Instance().frameCount,
        restoredHudlessSubmitUi ? "applied-hudless-submit-ui" : "applied",
        preparedStatusName(),
        reinterpret_cast<uintptr_t>(commandQueue),
        reinterpret_cast<uintptr_t>(targetResource),
        reinterpret_cast<uintptr_t>(sourceResource),
        width,
        height,
        static_cast<uint32_t>(desc.Format),
        static_cast<uint32_t>(targetState),
        status.rawInputSequence,
        status.rawInputSnapshotSequence,
        status.renderSnapshotCount,
        preparedWarp.warpResult.deltaYaw,
        preparedWarp.warpResult.deltaPitch,
        preparedWarp.approxPixelShift);

    return true;
}

bool FrameWarp_Dx12::LatchDLSSGResourceCopyWarpPose()
{
    auto& status = State::Instance().frameWarpStatus;
    PreparedWarp preparedWarp = PrepareWarpForPresent("dlssg-resource-latch", false);

    auto preparedStatusName = [&]() -> const char* {
        switch (preparedWarp.status)
        {
        case PreparedWarpStatus::Ready: return "Ready";
        case PreparedWarpStatus::ZeroPose: return "ZeroPose";
        case PreparedWarpStatus::Invalid: return "Invalid";
        default: return "Unknown";
        }
    };

    const bool latched = preparedWarp.status == PreparedWarpStatus::Ready;
    if (FrameWarpTimingAuditShouldLog(status.dlssgResourceWarpCandidateCount + status.dlssgResourceWarpSkippedCount + 1))
    {
        LOG_INFO("VF2DLSSGResourceWarpLatch frame={} latched={} prepared={} reason={} rawSeq={} snapSeq={} snapshots={} yaw={:.6f} pitch={:.6f} px={:.3f}",
            State::Instance().frameCount,
            latched,
            preparedStatusName(),
            preparedWarp.reason[0] ? preparedWarp.reason : "none",
            status.rawInputSequence,
            status.rawInputSnapshotSequence,
            status.renderSnapshotCount,
            preparedWarp.warpResult.deltaYaw,
            preparedWarp.warpResult.deltaPitch,
            preparedWarp.approxPixelShift);
    }

    if (latched)
        strncpy_s(status.dlssgResourceWarpLastReason, "latched late pose", _TRUNCATE);

    return latched;
}

void FrameWarp_Dx12::BeginDLSSGResourceFrameAnchor(float vFovRadians, float aspectRatio, const char* source)
{
    const char* anchorSource = source != nullptr && source[0] != '\0' ? source : "dlssg-constants";

    if (FrameWarpValidVFov(vFovRadians) && FrameWarpValidAspect(aspectRatio))
        SetCameraContext(vFovRadians, aspectRatio, anchorSource);

    _cameraPredictor.RecordRenderCamera(XMMatrixIdentity());
    _mouseTracker.SnapshotDLSSGResourcePendingAnchor();
    strncpy_s(State::Instance().frameWarpStatus.lastSnapshotSource, anchorSource, _TRUNCATE);

    static uint64_t anchorAuditCount = 0;
    anchorAuditCount++;
    if (FrameWarpTimingAuditShouldLog(anchorAuditCount))
    {
        auto& status = State::Instance().frameWarpStatus;
        LOG_INFO("VF2DLSSGFrameAnchor #{} frame={} source={} fovDeg={:.2f} aspect={:.3f} validCamera={} rawSeq={} snapSeq={} rawSamples={} accum=({:.1f},{:.1f}) snapshot=({:.1f},{:.1f})",
            anchorAuditCount,
            State::Instance().frameCount,
            anchorSource,
            vFovRadians * 57.29578f,
            aspectRatio,
            FrameWarpValidVFov(vFovRadians) && FrameWarpValidAspect(aspectRatio),
            status.rawInputSequence,
            status.rawInputSnapshotSequence,
            status.rawInputSampleCount,
            status.lastAccumDx,
            status.lastAccumDy,
            status.lastSnapshotDx,
            status.lastSnapshotDy);
    }
}

void FrameWarp_Dx12::SetMaxWarpAngle(float maxAngle)
{
    _cameraPredictor.SetMaxWarpAngle(maxAngle);
}

namespace
{
    FrameWarp_Dx12 g_frameWarpRuntime;
    std::mutex g_frameWarpRuntimeMutex;
    std::mutex g_frameWarpOwnerMutex;
    std::atomic<bool> g_fsrfgPresentCallbackConfigured { false };
    std::atomic<uint64_t> g_fsrfgPresentCallbackConfiguredFrame { 0 };
    std::atomic<uint64_t> g_fsrfgPresentCallbackSeenFrame { 0 };
    std::atomic<uint64_t> g_fsrfgPresentCallbackCount { 0 };
    std::atomic<uint64_t> g_fsrfgPresentCallbackFrameID { 0 };
    std::atomic<bool> g_fsrfgPresentCallbackGenerated { false };
    std::atomic<uint64_t> g_xefgPresentStatusCount { 0 };
    std::array<double, 128> g_fsrfgPresentCallbackIntervalsMs {};
    size_t g_fsrfgPresentCallbackIntervalIndex = 0;
    size_t g_fsrfgPresentCallbackIntervalCount = 0;
    int64_t g_fsrfgLastPresentCallbackQpc = 0;
    char g_fsrfgDispatchReason[64] = "not configured";
    UINT64 g_lastOwnerLogFrame = UINT64_MAX;
    FrameWarpPresentationOwner g_lastLoggedOwner = FrameWarpPresentationOwner::Disabled;
    char g_lastLoggedOwnerReason[64] = {};

    void FrameWarpRawMouseForwarder(LONG dx, LONG dy, const char* source, int64_t timestampQpc)
    {
        FrameWarpRuntime::OnRawMouseInput(dx, dy, source, timestampQpc);
    }

    void SetFsrfgDispatchReason(const char* reason)
    {
        std::scoped_lock lock(g_frameWarpOwnerMutex);
        strncpy_s(g_fsrfgDispatchReason, reason != nullptr ? reason : "unknown", _TRUNCATE);
    }

    void CopyFsrfgDispatchReason(char (&outReason)[64])
    {
        std::scoped_lock lock(g_frameWarpOwnerMutex);
        strncpy_s(outReason, g_fsrfgDispatchReason, _TRUNCATE);
    }

    struct FrameWarpWithFgDecision
    {
        bool configured = false;
        bool rawValue = false;
        bool resolved = false;
        const char* source = "auto";
    };

    FrameWarpWithFgDecision ResolveFrameWarpWithFgDecision(Config* config, State& state)
    {
        FrameWarpWithFgDecision decision {};
        decision.configured = config->FrameWarpWithFG.has_value();
        decision.rawValue = config->FrameWarpWithFG.value_or(false);

        if (decision.configured)
        {
            decision.resolved = decision.rawValue;
            decision.source = decision.rawValue ? "true" : "false";
        }
        else
        {
            decision.resolved =
                config->FrameWarpEnabled.value_or_default() &&
                (state.activeFgOutput == FGOutput::FSRFG ||
                    state.activeFgOutput == FGOutput::XeFG ||
                    (state.activeFgOutput == FGOutput::DLSSG &&
                        (config->FrameWarpDLSSGMode.value_or_default() == 2 ||
                         config->FrameWarpDLSSGMode.value_or_default() == 3 ||
                         config->FrameWarpDLSSGMode.value_or_default() == 4)));
            decision.source = "auto";
        }

        auto& status = state.frameWarpStatus;
        status.frameWarpWithFgConfigured = decision.configured;
        status.frameWarpWithFgRawValue = decision.rawValue;
        status.frameWarpWithFgResolved = decision.resolved;
        strncpy_s(status.frameWarpWithFgSource, decision.source, _TRUNCATE);
        return decision;
    }
}

const char* FrameWarpRuntime::PresentationOwnerName(FrameWarpPresentationOwner owner)
{
    switch (owner)
    {
    case FrameWarpPresentationOwner::Disabled: return "Disabled";
    case FrameWarpPresentationOwner::StandaloneNoFG: return "StandaloneNoFG";
    case FrameWarpPresentationOwner::FsrfgPending: return "FsrfgPending";
    case FrameWarpPresentationOwner::FsrfgCallback: return "FsrfgCallback";
    case FrameWarpPresentationOwner::FsrfgUnavailable: return "FsrfgUnavailable";
    case FrameWarpPresentationOwner::XeFGPending: return "XeFGPending";
    case FrameWarpPresentationOwner::XeFGInputFrame: return "XeFGInputFrame";
    case FrameWarpPresentationOwner::DLSSGDistortionField: return "DLSSGDistortionField";
    case FrameWarpPresentationOwner::DLSSGLatePresent: return "DLSSGLatePresent";
    case FrameWarpPresentationOwner::NativeDLSSGLatePresent: return "NativeDLSSGLatePresent";
    default: return "Unknown";
    }
}

FrameWarpPresentationOwner FrameWarpRuntime::ResolvePresentationOwner(const char* caller, bool logDecision)
{
    auto config = Config::Instance();
    auto& state = State::Instance();
    auto& status = state.frameWarpStatus;
    DLSSGNative::RefreshRuntimeMode();

    const bool frameWarpEnabled = config->FrameWarpEnabled.value_or_default();
    const bool fgConfigEnabled = config->FGEnabled.value_or_default();
    const bool fgOutputConfigured = state.activeFgOutput != FGOutput::NoFG;
    const bool hasCurrentFG = state.currentFG != nullptr;
    const auto withFgDecision = ResolveFrameWarpWithFgDecision(config, state);
    const bool frameWarpWithFG = withFgDecision.resolved;
    const bool fgActive = hasCurrentFG && state.currentFG->IsActive();
    const bool fgPaused = hasCurrentFG && state.currentFG->IsPaused();
    const bool fgProducingOutput = fgActive && !fgPaused;
    const bool nativeDLSSGRequested =
        state.activeFgOutput == FGOutput::DLSSG &&
        DLSSGNative::IsAttachActive();
    const bool fgRequested =
        fgOutputConfigured && (fgConfigEnabled || fgProducingOutput || state.FGchanged || nativeDLSSGRequested);
    const bool callbackConfigured = g_fsrfgPresentCallbackConfigured.load(std::memory_order_relaxed);

    FrameWarpPresentationOwner owner = FrameWarpPresentationOwner::Disabled;
    char reason[64] = {};

    if (!frameWarpEnabled)
    {
        owner = FrameWarpPresentationOwner::Disabled;
        strncpy_s(reason, "framewarp disabled", _TRUNCATE);
    }
    else if (!fgRequested)
    {
        owner = FrameWarpPresentationOwner::StandaloneNoFG;
        if (state.activeFgOutput == FGOutput::NoFG)
            strncpy_s(reason, "fg output NoFG", _TRUNCATE);
        else if (!fgConfigEnabled && !fgProducingOutput)
            strncpy_s(reason, "FG configured but disabled", _TRUNCATE);
        else
            strncpy_s(reason, "fg disabled", _TRUNCATE);
    }
    else if (!frameWarpWithFG)
    {
        owner = FrameWarpPresentationOwner::Disabled;
        strncpy_s(reason, "FrameWarp WithFG disabled", _TRUNCATE);
    }
    else if (state.activeFgOutput == FGOutput::XeFG)
    {
        if (state.FGchanged)
        {
            owner = FrameWarpPresentationOwner::XeFGPending;
            strncpy_s(reason, "FG changing", _TRUNCATE);
        }
        else if (!hasCurrentFG)
        {
            owner = FrameWarpPresentationOwner::XeFGPending;
            strncpy_s(reason, "currentFG missing", _TRUNCATE);
        }
        else if (!fgActive)
        {
            owner = FrameWarpPresentationOwner::XeFGPending;
            strncpy_s(reason, "XeFG inactive", _TRUNCATE);
        }
        else if (fgPaused)
        {
            owner = FrameWarpPresentationOwner::XeFGPending;
            strncpy_s(reason, "XeFG paused", _TRUNCATE);
        }
        else
        {
            owner = FrameWarpPresentationOwner::XeFGInputFrame;
            strncpy_s(reason, "XeFG proxy input frame", _TRUNCATE);
        }
    }
    else if (state.activeFgOutput == FGOutput::DLSSG)
    {
        const auto dlssgMode = config->FrameWarpDLSSGMode.value_or_default();
        const bool nativeAttach = DLSSGNative::IsAttachActive();
        const bool nativePassthrough = DLSSGNative::IsPassthroughActive();
        if (dlssgMode == 0)
        {
            owner = FrameWarpPresentationOwner::Disabled;
            strncpy_s(reason, "DLSSG FrameWarp mode off", _TRUNCATE);
        }
        else if (dlssgMode == 1)
        {
            owner = FrameWarpPresentationOwner::Disabled;
            strncpy_s(reason, "DLSSG telemetry only", _TRUNCATE);
        }
        else if (dlssgMode > 4)
        {
            owner = FrameWarpPresentationOwner::Disabled;
            strncpy_s(reason, "DLSSG FrameWarp mode invalid", _TRUNCATE);
        }
        else if (dlssgMode == 4)
        {
            owner = FrameWarpPresentationOwner::Disabled;
            strncpy_s(reason, "DLSSG resource-copy warp", _TRUNCATE);
        }
        else if (nativePassthrough)
        {
            owner = FrameWarpPresentationOwner::Disabled;
            strncpy_s(reason, "native DLSSG passthrough", _TRUNCATE);
        }
        else if (nativeAttach)
        {
            if (dlssgMode != 3)
            {
                owner = FrameWarpPresentationOwner::Disabled;
                strncpy_s(reason, "native DLSSG requires late-present mode", _TRUNCATE);
            }
            else if (!DLSSGNative::IsNativeEvaluateFresh(state.frameCount))
            {
                owner = FrameWarpPresentationOwner::Disabled;
                strncpy_s(reason, "native DLSSG evaluate not seen", _TRUNCATE);
            }
            else
            {
                owner = FrameWarpPresentationOwner::NativeDLSSGLatePresent;
                strncpy_s(reason, "native DLSSG late-present warp", _TRUNCATE);
            }
        }
        else if (state.FGchanged)
        {
            owner = FrameWarpPresentationOwner::Disabled;
            strncpy_s(reason, "FG changing", _TRUNCATE);
        }
        else if (!hasCurrentFG)
        {
            owner = FrameWarpPresentationOwner::Disabled;
            strncpy_s(reason, "currentFG missing", _TRUNCATE);
        }
        else if (!fgActive)
        {
            owner = FrameWarpPresentationOwner::Disabled;
            strncpy_s(reason, "DLSSG inactive", _TRUNCATE);
        }
        else if (fgPaused)
        {
            owner = FrameWarpPresentationOwner::Disabled;
            strncpy_s(reason, "DLSSG paused", _TRUNCATE);
        }
        else
        {
            if (dlssgMode == 3)
            {
                owner = FrameWarpPresentationOwner::DLSSGLatePresent;
                strncpy_s(reason, "DLSSG late-present warp", _TRUNCATE);
            }
            else
            {
                owner = FrameWarpPresentationOwner::DLSSGDistortionField;
                strncpy_s(reason, "DLSSG distortion field", _TRUNCATE);
            }
        }
    }
    else if (state.activeFgOutput != FGOutput::FSRFG)
    {
        owner = FrameWarpPresentationOwner::FsrfgUnavailable;
        strncpy_s(reason, "unsupported FG output", _TRUNCATE);
    }
    else if (state.FGchanged)
    {
        owner = FrameWarpPresentationOwner::FsrfgPending;
        strncpy_s(reason, "FG changing", _TRUNCATE);
    }
    else if (!hasCurrentFG)
    {
        owner = FrameWarpPresentationOwner::FsrfgPending;
        strncpy_s(reason, "currentFG missing", _TRUNCATE);
    }
    else if (!fgActive)
    {
        owner = FrameWarpPresentationOwner::FsrfgPending;
        strncpy_s(reason, "FSRFG inactive", _TRUNCATE);
    }
    else if (fgPaused)
    {
        owner = FrameWarpPresentationOwner::FsrfgPending;
        strncpy_s(reason, "FSRFG paused", _TRUNCATE);
    }
    else if (callbackConfigured)
    {
        owner = FrameWarpPresentationOwner::FsrfgCallback;
        strncpy_s(reason, "present callback configured", _TRUNCATE);
    }
    else
    {
        owner = FrameWarpPresentationOwner::FsrfgPending;
        CopyFsrfgDispatchReason(reason);
    }

    status.lastPresentationOwner = static_cast<uint32_t>(owner);
    status.lastOwnerFgRequested = fgRequested;
    status.lastOwnerHasCurrentFG = hasCurrentFG;
    status.lastOwnerFgActive = fgActive;
    status.lastOwnerFgPaused = fgPaused;
    status.fsrfgPresentCallbackConfigured = callbackConfigured;
    status.fsrfgPresentCallbackSeen = g_fsrfgPresentCallbackCount.load(std::memory_order_relaxed) > 0;
    status.fsrfgPresentCallbackCount = g_fsrfgPresentCallbackCount.load(std::memory_order_relaxed);
    status.fsrfgPresentCallbackFrameID = g_fsrfgPresentCallbackFrameID.load(std::memory_order_relaxed);
    status.fsrfgPresentCallbackGenerated = g_fsrfgPresentCallbackGenerated.load(std::memory_order_relaxed);
    strncpy_s(status.lastPresentationOwnerName, PresentationOwnerName(owner), _TRUNCATE);
    strncpy_s(status.lastPresentationOwnerReason, reason, _TRUNCATE);

    if (logDecision)
    {
        std::scoped_lock lock(g_frameWarpOwnerMutex);
        if (g_lastOwnerLogFrame != state.frameCount ||
            g_lastLoggedOwner != owner ||
            strcmp(g_lastLoggedOwnerReason, reason) != 0)
        {
            LOG_DEBUG("FrameWarp owner={} caller={} fgRequested={} activeOutput={} currentFG={} fgActive={} fgPaused={} callbackConfigured={} callbackSeen={} withFg={}({}) reason={}",
                PresentationOwnerName(owner),
                caller != nullptr ? caller : "unknown",
                fgRequested,
                (UINT) state.activeFgOutput,
                hasCurrentFG,
                fgActive,
                fgPaused,
                callbackConfigured,
                status.fsrfgPresentCallbackSeen,
                frameWarpWithFG,
                withFgDecision.source,
                reason);

            g_lastOwnerLogFrame = state.frameCount;
            g_lastLoggedOwner = owner;
            strncpy_s(g_lastLoggedOwnerReason, reason, _TRUNCATE);
        }
    }

    return owner;
}

bool FrameWarpRuntime::IsStandalonePresentationOwner(const char* caller, bool logDecision)
{
    return ResolvePresentationOwner(caller, logDecision) == FrameWarpPresentationOwner::StandaloneNoFG;
}

bool FrameWarpRuntime::IsFsrfgPresentCallbackRequested()
{
    auto config = Config::Instance();
    auto& state = State::Instance();
    if (!config->FrameWarpEnabled.value_or_default() || state.activeFgOutput != FGOutput::FSRFG)
        return false;

    return ResolveFrameWarpWithFgDecision(config, state).resolved;
}

void FrameWarpRuntime::ReportFsrfgDispatchSkipped(const char* reason)
{
    g_fsrfgPresentCallbackConfigured.store(false, std::memory_order_relaxed);
    SetFsrfgDispatchReason(reason != nullptr ? reason : "dispatch skipped");
}

void FrameWarpRuntime::ReportFsrfgPresentCallbackConfigured(bool configured, const char* reason)
{
    g_fsrfgPresentCallbackConfigured.store(configured, std::memory_order_relaxed);
    g_fsrfgPresentCallbackConfiguredFrame.store(State::Instance().frameCount, std::memory_order_relaxed);
    SetFsrfgDispatchReason(reason != nullptr ? reason : (configured ? "present callback configured" : "present callback disabled"));
}

void FrameWarpRuntime::ReportFsrfgPresentCallbackSeen(UINT64 frameID, bool generated)
{
    const int64_t nowQpc = FrameWarpQueryQpc();
    g_fsrfgPresentCallbackCount.fetch_add(1, std::memory_order_relaxed);
    g_fsrfgPresentCallbackSeenFrame.store(State::Instance().frameCount, std::memory_order_relaxed);
    g_fsrfgPresentCallbackFrameID.store(frameID, std::memory_order_relaxed);
    g_fsrfgPresentCallbackGenerated.store(generated, std::memory_order_relaxed);

    std::scoped_lock lock(g_frameWarpOwnerMutex);
    if (g_fsrfgLastPresentCallbackQpc != 0)
    {
        const double intervalMs = FrameWarpQpcToMilliseconds(nowQpc - g_fsrfgLastPresentCallbackQpc);
        g_fsrfgPresentCallbackIntervalsMs[g_fsrfgPresentCallbackIntervalIndex] = intervalMs;
        g_fsrfgPresentCallbackIntervalIndex =
            (g_fsrfgPresentCallbackIntervalIndex + 1) % g_fsrfgPresentCallbackIntervalsMs.size();
        g_fsrfgPresentCallbackIntervalCount =
            std::min(g_fsrfgPresentCallbackIntervalCount + 1, g_fsrfgPresentCallbackIntervalsMs.size());

        double sumMs = 0.0;
        double sqSumMs = 0.0;
        for (size_t i = 0; i < g_fsrfgPresentCallbackIntervalCount; ++i)
        {
            const double value = g_fsrfgPresentCallbackIntervalsMs[i];
            sumMs += value;
            sqSumMs += value * value;
        }

        auto& status = State::Instance().frameWarpStatus;
        const double meanMs = sumMs / static_cast<double>(g_fsrfgPresentCallbackIntervalCount);
        const double varianceMs =
            std::max(0.0, sqSumMs / static_cast<double>(g_fsrfgPresentCallbackIntervalCount) - meanMs * meanMs);
        status.fsrfgPresentIntervalMs = static_cast<float>(intervalMs);
        status.fsrfgPresentIntervalMeanMs = static_cast<float>(meanMs);
        status.fsrfgPresentIntervalStdDevMs = static_cast<float>(std::sqrt(varianceMs));
    }

    g_fsrfgLastPresentCallbackQpc = nowQpc;
}

void FrameWarpRuntime::ReportXeFGPresentStatus(uint32_t framesPresented, int32_t frameGenResult,
    bool frameGenEnabled, int32_t queryResult)
{
    auto& status = State::Instance().frameWarpStatus;
    status.xefgPresentStatusSeen = true;
    status.xefgPresentStatusCount = g_xefgPresentStatusCount.fetch_add(1, std::memory_order_relaxed) + 1;
    status.xefgLastFramesPresented = framesPresented;
    status.xefgLastFrameGenResult = frameGenResult;
    status.xefgLastFrameGenEnabled = frameGenEnabled;
    status.xefgLastPresentStatusResult = queryResult;
}

bool FrameWarpRuntime::EnsureInitialized(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format)
{
    if (device == nullptr || width == 0 || height == 0)
        return false;

    std::scoped_lock lock(g_frameWarpRuntimeMutex);

    if (g_frameWarpRuntime.IsInitialized() && g_frameWarpRuntime.GetDevice() != device)
    {
        LOG_INFO("FrameWarp_Dx12: D3D12 device changed; reinitializing runtime");
        g_frameWarpRuntime.Shutdown();
    }

    if (!g_frameWarpRuntime.IsInitialized())
    {
        if (!g_frameWarpRuntime.Initialize(device, width, height, format))
            return false;
    }
    else if (g_frameWarpRuntime.GetOutputWidth() != width ||
        g_frameWarpRuntime.GetOutputHeight() != height ||
        g_frameWarpRuntime.GetSourceFormat() != format)
    {
        LOG_INFO("FrameWarp_Dx12: resizing runtime {}x{} fmt {} -> {}x{} fmt {}",
            g_frameWarpRuntime.GetOutputWidth(), g_frameWarpRuntime.GetOutputHeight(),
            (UINT)g_frameWarpRuntime.GetSourceFormat(), width, height, (UINT)format);
        if (!g_frameWarpRuntime.Resize(width, height, format))
            return false;
    }

    State::Instance().frameWarpRawMouseCallback = FrameWarpRawMouseForwarder;
    FrameWarpRuntime::SyncConfig();
    return true;
}

void FrameWarpRuntime::Shutdown()
{
    std::scoped_lock lock(g_frameWarpRuntimeMutex);
    State::Instance().frameWarpRawMouseCallback = nullptr;
    ReportFsrfgPresentCallbackConfigured(false, "framewarp shutdown");
    g_frameWarpRuntime.Shutdown();
}

FrameWarp_Dx12* FrameWarpRuntime::Get()
{
    return g_frameWarpRuntime.IsInitialized() ? &g_frameWarpRuntime : nullptr;
}

void FrameWarpRuntime::SyncConfig()
{
    auto config = Config::Instance();
    bool enabled = config->FrameWarpEnabled.value_or_default();

    g_frameWarpRuntime.SetEnabled(enabled);
    g_frameWarpRuntime.SetWarpStrength(config->FrameWarpStrength.value_or_default());
    g_frameWarpRuntime.SetSensitivity(
        std::clamp(config->FrameWarpSensitivity.value_or_default(), 0.00005f, 0.005f),
        std::clamp(config->FrameWarpSensitivity.value_or_default(), 0.00005f, 0.005f));
    g_frameWarpRuntime.SetMaxWarpAngle(
        std::clamp(config->FrameWarpMaxAngle.value_or_default(), 0.1f, 180.0f) * (FRAMEWARP_PI / 180.0f));

    State::Instance().frameWarpStatus.enabled = enabled;
    State::Instance().frameWarpStatus.sensitivityAuditCurrentRadPerCount =
        std::clamp(config->FrameWarpSensitivity.value_or_default(), 0.00005f, 0.005f);
    strncpy_s(State::Instance().frameWarpStatus.lastCalibrationSource,
        config->FrameWarpAutoCalibration.value_or_default() ? "manual fallback" : "manual",
        _TRUNCATE);
    if (!enabled)
        FrameWarpSetSkipReason("disabled");
}

void FrameWarpRuntime::MarkFrameRenderStart(const char* source, float vFovRadians, float aspectRatio)
{
    std::scoped_lock lock(g_frameWarpRuntimeMutex);
    if (!g_frameWarpRuntime.IsInitialized())
        return;

    SyncConfig();
    if (!Config::Instance()->FrameWarpEnabled.value_or_default())
        return;

    auto& status = State::Instance().frameWarpStatus;
    strncpy_s(status.lastSnapshotSource, source != nullptr ? source : "unknown", _TRUNCATE);

    if (FrameWarpValidVFov(vFovRadians) && FrameWarpValidAspect(aspectRatio))
    {
        const char* fovSource = source != nullptr && source[0] != '\0' ? source : "runtime";
        if (strcmp(fovSource, "fsrfg-upscale") == 0 || strcmp(fovSource, "upscale") == 0)
            fovSource = "FSR input";
        g_frameWarpRuntime.SetCameraContext(vFovRadians, aspectRatio, fovSource);
    }

    if (State::Instance().activeFgOutput == FGOutput::DLSSG &&
        Config::Instance()->FrameWarpDLSSGMode.value_or_default() == 4)
    {
        g_frameWarpRuntime.GetMouseTracker().SnapshotDLSSGResourcePendingAnchor();
    }

    g_frameWarpRuntime.OnFrameRenderStart(XMMatrixIdentity());

    static uint64_t snapshotAuditCount = 0;
    snapshotAuditCount++;
    if (FrameWarpTimingAuditShouldLog(snapshotAuditCount))
    {
        LOG_INFO("VF2AuditSnapshot #{} frame={} source={} snapshots={} rawSeq={} snapSeq={} rawSamples={} rawSrc={} activeSrc={} accum=({:.1f},{:.1f}) snapshot=({:.1f},{:.1f}) rawHz={:.1f} rawIntMs={:.3f}/{:.3f}/{:.3f} fovDeg={:.2f} aspect={:.3f}",
            snapshotAuditCount,
            State::Instance().frameCount,
            status.lastSnapshotSource[0] ? status.lastSnapshotSource : "none",
            status.renderSnapshotCount,
            status.rawInputSequence,
            status.rawInputSnapshotSequence,
            status.rawInputSampleCount,
            status.lastRawInputSource[0] ? status.lastRawInputSource : "none",
            status.activeRawInputSource[0] ? status.activeRawInputSource : "none",
            status.lastAccumDx,
            status.lastAccumDy,
            status.lastSnapshotDx,
            status.lastSnapshotDy,
            status.rawInputSampleHz,
            status.rawInputIntervalMeanMs,
            status.rawInputIntervalStdDevMs,
            status.rawInputIntervalMaxMs,
            status.lastVFovRadians * 57.29578f,
            status.lastAspectRatio);
    }

    static uint64_t snapshotLogCount = 0;
    snapshotLogCount++;
    if (snapshotLogCount <= 10 || snapshotLogCount % 300 == 0)
    {
        LOG_DEBUG("FrameWarp: render snapshot #{} source={} accum=({:.1f},{:.1f}) rawSamples={}",
            status.renderSnapshotCount, status.lastSnapshotSource,
            status.lastSnapshotDx, status.lastSnapshotDy,
            status.rawInputSampleCount);
    }
}

void FrameWarpRuntime::BeginDLSSGResourceFrameAnchor(float vFovRadians, float aspectRatio, const char* source)
{
    std::scoped_lock lock(g_frameWarpRuntimeMutex);
    if (!g_frameWarpRuntime.IsInitialized())
        return;

    SyncConfig();
    if (!Config::Instance()->FrameWarpEnabled.value_or_default() ||
        Config::Instance()->FrameWarpDLSSGMode.value_or_default() != 4)
        return;

    g_frameWarpRuntime.BeginDLSSGResourceFrameAnchor(vFovRadians, aspectRatio, source);
}

void FrameWarpRuntime::OnRawMouseInput(LONG dx, LONG dy, const char* source, int64_t timestampQpc)
{
    // Raw input must not wait behind present/depth/resize work on the runtime
    // mutex. The runtime object is static for process lifetime and MouseTracker
    // has its own mutex, so accepting an in-flight packet here is safe even if
    // shutdown is concurrently clearing the callback.
    g_frameWarpRuntime.OnRawMouseInput(dx, dy, source, timestampQpc);
    State::Instance().frameWarpStatus.rawInputSeen = true;

    static uint64_t rawForwardLogCount = 0;
    rawForwardLogCount++;
    if (rawForwardLogCount <= 10 || rawForwardLogCount % 500 == 0)
    {
        auto& status = State::Instance().frameWarpStatus;
        LOG_DEBUG("FrameWarp: raw mouse forwarded #{} source={} dx={} dy={} accum=({:.1f},{:.1f})",
            rawForwardLogCount,
            source != nullptr && source[0] != '\0' ? source : "unknown",
            dx, dy, status.lastAccumDx, status.lastAccumDy);
    }
}

bool FrameWarpRuntime::ApplyStandalone(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue)
{
    std::scoped_lock lock(g_frameWarpRuntimeMutex);
    if (!g_frameWarpRuntime.IsInitialized())
        return false;

    SyncConfig();
    return g_frameWarpRuntime.ApplyStandaloneWarp(swapChain, commandQueue);
}

bool FrameWarpRuntime::LatchDLSSGResourceCopyWarpPose(IDXGISwapChain* swapChain)
{
    if (swapChain == nullptr)
        return false;

    std::scoped_lock lock(g_frameWarpRuntimeMutex);
    SyncConfig();
    if (!Config::Instance()->FrameWarpEnabled.value_or_default())
        return false;

    ID3D12Resource* backBuffer = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || backBuffer == nullptr)
        return false;

    auto desc = backBuffer->GetDesc();
    ID3D12Device* device = nullptr;
    hr = backBuffer->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr) || device == nullptr)
    {
        backBuffer->Release();
        return false;
    }

    bool initialized = true;
    if (!g_frameWarpRuntime.IsInitialized() || g_frameWarpRuntime.GetDevice() != device)
        initialized = g_frameWarpRuntime.Initialize(device, static_cast<UINT>(desc.Width), desc.Height, desc.Format);
    else if (g_frameWarpRuntime.GetOutputWidth() != static_cast<UINT>(desc.Width) ||
        g_frameWarpRuntime.GetOutputHeight() != desc.Height ||
        g_frameWarpRuntime.GetSourceFormat() != desc.Format)
        initialized = g_frameWarpRuntime.Resize(static_cast<UINT>(desc.Width), desc.Height, desc.Format);

    if (initialized)
        State::Instance().frameWarpRawMouseCallback = FrameWarpRawMouseForwarder;

    device->Release();
    backBuffer->Release();

    if (!initialized)
        return false;

    SyncConfig();
    return g_frameWarpRuntime.LatchDLSSGResourceCopyWarpPose();
}

bool FrameWarpRuntime::ApplyToResourceAfterDLSSGCopy(
    ID3D12CommandQueue* commandQueue,
    ID3D12Resource* targetResource,
    D3D12_RESOURCE_STATES targetState,
    ID3D12Resource* sourceResource,
    const char* reason)
{
    std::scoped_lock lock(g_frameWarpRuntimeMutex);
    if (!g_frameWarpRuntime.IsInitialized() && targetResource != nullptr)
    {
        ID3D12Device* device = nullptr;
        auto desc = targetResource->GetDesc();
        HRESULT hr = targetResource->GetDevice(IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr) && device != nullptr)
        {
            if (g_frameWarpRuntime.Initialize(device, static_cast<UINT>(desc.Width), desc.Height, desc.Format))
            {
                State::Instance().frameWarpRawMouseCallback = FrameWarpRawMouseForwarder;
                g_frameWarpRuntime.GetMouseTracker().SnapshotDLSSGResourcePendingAnchor();
                g_frameWarpRuntime.OnFrameRenderStart(XMMatrixIdentity());
                strncpy_s(State::Instance().frameWarpStatus.lastSnapshotSource,
                    "dlssg-resource-init",
                    _TRUNCATE);
            }
            device->Release();
        }
    }

    if (!g_frameWarpRuntime.IsInitialized())
        return false;

    SyncConfig();
    return g_frameWarpRuntime.ApplyDLSSGResourceCopyWarp(
        commandQueue, targetResource, targetState, sourceResource, reason);
}

bool FrameWarpRuntime::PrepareDLSSGHudlessSubmit(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* hudlessInput,
    D3D12_RESOURCE_STATES hudlessInputState,
    ID3D12Resource* finalWithUi,
    D3D12_RESOURCE_STATES finalWithUiState)
{
    std::scoped_lock lock(g_frameWarpRuntimeMutex);
    if (!g_frameWarpRuntime.IsInitialized())
        return false;

    SyncConfig();
    return g_frameWarpRuntime.PrepareDLSSGHudlessSubmit(
        cmdList,
        hudlessInput,
        hudlessInputState,
        finalWithUi,
        finalWithUiState);
}

void FrameWarpRuntime::InvalidateDLSSGHudlessSubmit(const char* reason)
{
    std::scoped_lock lock(g_frameWarpRuntimeMutex);
    if (!g_frameWarpRuntime.IsInitialized())
        return;

    g_frameWarpRuntime.InvalidateDLSSGHudlessSubmit(reason);
}

bool FrameWarpRuntime::CaptureHudlessSource(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* hudlessInput,
    D3D12_RESOURCE_STATES hudlessInputState,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    const char* source)
{
    std::scoped_lock lock(g_frameWarpRuntimeMutex);
    if (!g_frameWarpRuntime.IsInitialized())
        return false;

    SyncConfig();
    return g_frameWarpRuntime.CaptureHudlessSource(
        cmdList, hudlessInput, hudlessInputState, width, height, format, source);
}

bool FrameWarpRuntime::TrackDepthSource(
    ID3D12Resource* depthInput,
    D3D12_RESOURCE_STATES depthInputState,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    const char* source)
{
    std::scoped_lock lock(g_frameWarpRuntimeMutex);
    if (!g_frameWarpRuntime.IsInitialized())
        return false;

    SyncConfig();
    return g_frameWarpRuntime.TrackDepthSource(
        depthInput, depthInputState, width, height, format, source);
}

bool FrameWarpRuntime::TrackMotionVectorSource(
    ID3D12Resource* motionVectorInput,
    D3D12_RESOURCE_STATES motionVectorInputState,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    float mvScaleX,
    float mvScaleY,
    bool scalePreMultiplied,
    const char* source)
{
    std::scoped_lock lock(g_frameWarpRuntimeMutex);
    if (!g_frameWarpRuntime.IsInitialized())
        return false;

    SyncConfig();
    return g_frameWarpRuntime.TrackMotionVectorSource(
        motionVectorInput,
        motionVectorInputState,
        width,
        height,
        format,
        mvScaleX,
        mvScaleY,
        scalePreMultiplied,
        source);
}

bool FrameWarpRuntime::CaptureDepthSource(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* depthInput,
    D3D12_RESOURCE_STATES depthInputState,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    bool invertedDepth,
    const char* source)
{
    std::scoped_lock lock(g_frameWarpRuntimeMutex);
    if (!g_frameWarpRuntime.IsInitialized())
        return false;

    SyncConfig();
    return g_frameWarpRuntime.CaptureDepthSource(
        cmdList, depthInput, depthInputState, width, height, format, invertedDepth, source);
}

void FrameWarpRuntime::RegisterStableUiFrame(
    int frameIndex,
    ID3D12Resource* originalHudless,
    D3D12_RESOURCE_STATES originalHudlessState,
    ID3D12Resource* warpedHudless,
    D3D12_RESOURCE_STATES warpedHudlessState,
    UINT width,
    UINT height,
    DXGI_FORMAT format)
{
    std::scoped_lock lock(g_frameWarpRuntimeMutex);
    if (!g_frameWarpRuntime.IsInitialized())
        return;

    g_frameWarpRuntime.RegisterStableUiFrame(
        frameIndex,
        originalHudless,
        originalHudlessState,
        warpedHudless,
        warpedHudlessState,
        width,
        height,
        format);
}

bool FrameWarpRuntime::ApplyStableUiComposite(
    int frameIndex,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* presentWithUi,
    D3D12_RESOURCE_STATES presentState)
{
    std::scoped_lock lock(g_frameWarpRuntimeMutex);
    if (!g_frameWarpRuntime.IsInitialized())
        return false;

    SyncConfig();
    return g_frameWarpRuntime.ApplyStableUiComposite(frameIndex, cmdList, presentWithUi, presentState);
}
