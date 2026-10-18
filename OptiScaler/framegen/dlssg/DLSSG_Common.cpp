#include "pch.h"

#include "DLSSG_Common.h"

#include <hooks/Reflex_Hooks.h>
#include <low_latency/input/input_common.h>
#include <menu/menu_common.h>

#include <magic_enum.hpp>

#include <DirectXMath.h>
#include <chrono>

using namespace DirectX;

feature_version DLSSG_Common::Version()
{
    if (StreamlineProxy::LoadStreamline())
        return StreamlineProxy::Version();

    return { 0, 0, 0 };
}

void DLSSG_Common::Activate()
{
    LOG_DEBUG("");

    if (!_isActive)
    {
        UpdateTarget();
        _isActive = true;
    }
}

void DLSSG_Common::Deactivate()
{
    LOG_DEBUG("");

    if (_isActive)
    {
        sl::DLSSGOptions options {};
        options.mode = sl::DLSSGMode::eOff;
        options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;

        if (StreamlineProxy::DLSSGSetOptions() != nullptr)
            StreamlineProxy::DLSSGSetOptions()(_viewport, options); // Potential crash point on exit

        sl::ReflexOptions reflexConst = {};
        reflexConst.mode = sl::ReflexMode::eOff;
        reflexConst.useMarkersToOptimize = false;
        ScopedOptiScalerReflex optiScalerCall {};

        if (StreamlineProxy::ReflexSetOptions() != nullptr)
            StreamlineProxy::ReflexSetOptions()(reflexConst);

        _isActive = false;
    }
}

void DLSSG_Common::PauseAfterError()
{
    State::Instance().fgChanged = true;
    UpdateTarget();
    Deactivate();
}

void DLSSG_Common::QueryState()
{
    sl::DLSSGState dlssgState {};
    sl::DLSSGOptions dlssgOptions {};
    if (StreamlineProxy::DLSSGGetState()(_viewport, dlssgState, &dlssgOptions) == sl::Result::eOk)
    {
        State::Instance().dlssgMfgMax = dlssgState.numFramesToGenerateMax;
        _maxInterpolationCount = dlssgState.numFramesToGenerateMax;
        LOG_INFO("Max supported interpolations: {}", dlssgState.numFramesToGenerateMax);

        _supportsDMFG = dlssgState.bIsDynamicMFGSupported == sl::Boolean::eTrue;
    }
}

void DLSSG_Common::SetOptions(bool gameMarkersReachStreamline)
{
    auto config = Config::Instance();

    if (config->FGDLSSGInterpolationCount.value_or_default() > _maxInterpolationCount)
    {
        config->FGDLSSGInterpolationCount = _maxInterpolationCount;
        LOG_WARN("Requested interpolation count is higher than max supported, setting to max: {}",
                 _maxInterpolationCount);
    }

    int targetCount = std::clamp(config->FGInterpolationCount(config->FGDLSSGInterpolationCount.value_or_default()), 1,
                                 std::max(_maxInterpolationCount, 1));

    if (UseOptiDynamicMfg())
        targetCount = UpdateOptiDynamicMfg(targetCount);
    else
        StopOptiDynamicMfg();

    if (_framesToInterpolate != targetCount)
    {
        LOG_INFO("Interpolation count changed {} -> {}", _framesToInterpolate, targetCount);

        _framesToInterpolate = targetCount;
    }

    sl::DLSSGOptions options {};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = _framesToInterpolate;
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;

    // Streamline refuses eDynamic where it doesn't support it (Vulkan), OptiScaler's own Dynamic MFG runs there
    if (config->FGDLSSGForceDMFG.value_or_default() && _supportsDMFG)
    {
        options.mode = sl::DLSSGMode::eDynamic;
        options.dynamicTargetFrameRate = config->FGDLSSGFramerateTargetDMFG.value_or_default();
    }

    auto dlssgSetOptionsResult = StreamlineProxy::DLSSGSetOptions()(_viewport, options);

    if (dlssgSetOptionsResult != sl::Result::eOk)
        LOG_ERROR("Couldn't set DLSSG options, error: {}", magic_enum::enum_name(dlssgSetOptionsResult));

    sl::ReflexOptions reflexConst = {};
    reflexConst.mode = sl::ReflexMode::eLowLatency;
    reflexConst.useMarkersToOptimize = gameMarkersReachStreamline && ReflexHooks::gameIsSendingMarkers();

    ScopedOptiScalerReflex optiScalerCall {};
    auto reflexSetOptionsResult = StreamlineProxy::ReflexSetOptions()(reflexConst);

    if (reflexSetOptionsResult != sl::Result::eOk)
        LOG_ERROR("Couldn't set Reflex options, error: {}", magic_enum::enum_name(reflexSetOptionsResult));
}

bool DLSSG_Common::UseOptiDynamicMfg() const
{
    // Only where Streamline has no eDynamic of its own, and only with more than one multiplier to pick from
    return Config::Instance()->FGDLSSGForceDMFG.value_or_default() && !_supportsDMFG && _maxInterpolationCount >= 2;
}

float DLSSG_Common::DynamicMfgTarget(double nowMs, bool& autoTarget)
{
    autoTarget = false;

    if (auto target = Config::Instance()->FGDLSSGFramerateTargetDMFG.value_or_default(); target > 0.0f)
        return target;

    // Auto: Reflex's VRR-safe cap below the refresh rate, refresh - refresh^2 / 3600 (210.9 fps at 225 Hz)
    if (_refreshCheckMs < 0.0 || nowMs - _refreshCheckMs > 2000.0)
    {
        _refreshCheckMs = nowMs;

        if (auto hwnd = MenuCommon::Handle(); hwnd != nullptr)
            _refreshRate = Util::GetActiveRefreshRate(hwnd);
    }

    autoTarget = true;

    if (_refreshRate <= 1)
        return 0.0f;

    const float refresh = (float) _refreshRate;
    return refresh - refresh * refresh / 3600.0f;
}

int DLSSG_Common::UpdateOptiDynamicMfg(int fixedCount)
{
    auto config = Config::Instance();
    const double nowMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();

    // A pause (menus, loading) leaves stale timings behind
    if (!_dynamicMfgRunning || (_dynamicMfgLastFrameMs >= 0.0 && nowMs - _dynamicMfgLastFrameMs > 500.0))
    {
        if (!_dynamicMfgRunning)
            LOG_INFO("Dynamic MFG (OptiScaler) started, up to {}x", _maxInterpolationCount + 1);

        _dynamicMfg.Reset();
        _dynamicMfgRunning = true;
    }

    _dynamicMfgLastFrameMs = nowMs;

    bool autoTarget = false;
    const float target = DynamicMfgTarget(nowMs, autoTarget);

    // The output cap holds real frames at target / multiplier. Dynamic MFG sets it unless the user has one.
    auto& limit = config->FramerateLimit;

    if (_dynamicMfgOwnsCap && limit.value_or_default() != _dynamicMfgCap)
    {
        LOG_INFO("Dynamic MFG: the FPS limit was changed to {}, leaving it to the user", limit.value_or_default());
        _dynamicMfgOwnsCap = false;
        _capBeforeDynamicMfg.reset();
    }

    const bool wantCap = config->FGDLSSGDynamicCapOutput.value_or_default() && target > 0.0f;

    if (wantCap && !_dynamicMfgOwnsCap && limit.value_or_default() == 0.0f)
    {
        _capBeforeDynamicMfg = limit.has_value() ? std::optional<float>(limit.value()) : std::nullopt;
        _dynamicMfgOwnsCap = true;
        _dynamicMfgCap = -1.0f;
    }
    else if (!wantCap)
    {
        ReleaseDynamicMfgCap();
    }

    if (_dynamicMfgOwnsCap && _dynamicMfgCap != target)
    {
        _dynamicMfgCap = target;
        limit.set_volatile_value(target); // Never saved over the user's own limit
        LOG_INFO("Dynamic MFG: output capped at {:.1f} fps", target);
    }

    // A lower FPS limit of the user's is the real target
    float effectiveTarget = target;
    const float cap = limit.value_or_default();

    if (cap > 0.0f && (effectiveTarget <= 0.0f || cap < effectiveTarget))
        effectiveTarget = cap;

    double gpuMs = 0.0;
    double cpuMs = 0.0;
    const bool costs = InputCommon::get_render_costs(gpuMs, cpuMs);

    const int maxMultiplier = _maxInterpolationCount + 1;
    DynamicMfgController::Settings settings;
    settings.targetFps = effectiveTarget;
    settings.minMultiplier = std::clamp(config->FGDLSSGDynamicMinMultiplier.value_or_default(), 2, maxMultiplier);
    settings.maxMultiplier =
        std::clamp(config->FGDLSSGDynamicMaxMultiplier.value_or(maxMultiplier), settings.minMultiplier, maxMultiplier);

    DynamicMfgController::Sample sample;
    sample.nowMs = nowMs;
    sample.gpuRenderMs = gpuMs;
    sample.cpuFrameMs = cpuMs;
    sample.capped = cap > 0.0f && std::abs(cap - effectiveTarget) < 0.5f;

    const int previous = _dynamicMfg.GetStatus().multiplier;
    const int multiplier = _dynamicMfg.Update(sample, settings);
    const auto& status = _dynamicMfg.GetStatus();

    if (multiplier != previous || nowMs - _dynamicMfgLastLogMs > 5000.0)
    {
        _dynamicMfgLastLogMs = nowMs;
        LOG_INFO("Dynamic MFG {}x -> {}x ({}): target {:.1f} fps{}, base {:.1f} fps, capacity {:.1f} fps from {}, "
                 "GPU {:.2f} ms, CPU {:.2f} ms, cost factor {:.2f}, cap {}, range {}x-{}x",
                 previous, multiplier, magic_enum::enum_name(status.reason), effectiveTarget,
                 autoTarget ? " (auto)" : "", status.baseFps, status.capacityFps,
                 status.fromCosts ? "costs" : "frame rate", status.gpuRenderMs, status.cpuFrameMs, status.costFactor,
                 status.capBinding ? "binding" : (sample.capped ? "set" : "off"), settings.minMultiplier,
                 settings.maxMultiplier);
    }

    PublishDynamicMfg(effectiveTarget, autoTarget, costs);

    return multiplier > 0 ? multiplier - 1 : fixedCount;
}

void DLSSG_Common::ReleaseDynamicMfgCap()
{
    if (!_dynamicMfgOwnsCap)
        return;

    // Only undo the limit while it's still the one Dynamic MFG set
    auto& limit = Config::Instance()->FramerateLimit;

    if (limit.value_or_default() == _dynamicMfgCap)
    {
        if (_capBeforeDynamicMfg.has_value())
            limit.set_volatile_value(_capBeforeDynamicMfg.value());
        else
            limit.reset();

        LOG_INFO("Dynamic MFG: output cap removed");
    }

    _dynamicMfgOwnsCap = false;
    _capBeforeDynamicMfg.reset();
}

void DLSSG_Common::StopOptiDynamicMfg()
{
    if (!_dynamicMfgRunning && !_dynamicMfgOwnsCap)
        return;

    ReleaseDynamicMfgCap();
    _dynamicMfg.Reset();
    _dynamicMfgRunning = false;
    _dynamicMfgLastFrameMs = -1.0;

    {
        auto& state = State::Instance();
        std::lock_guard lock(state.optiDynamicMfgMutex);
        state.optiDynamicMfg = {};
    }

    LOG_INFO("Dynamic MFG (OptiScaler) stopped");
}

void DLSSG_Common::PublishDynamicMfg(float targetFps, bool autoTarget, bool costsAvailable)
{
    auto& state = State::Instance();
    std::lock_guard lock(state.optiDynamicMfgMutex);
    state.optiDynamicMfg.running = _dynamicMfgRunning;
    state.optiDynamicMfg.targetFps = targetFps;
    state.optiDynamicMfg.autoTarget = autoTarget;
    state.optiDynamicMfg.ownsCap = _dynamicMfgOwnsCap;
    state.optiDynamicMfg.costsAvailable = costsAvailable;
    state.optiDynamicMfg.status = _dynamicMfg.GetStatus();
}

bool DLSSG_Common::SetConstants(int fIndex, uint32_t frameId, uint32_t mvWidth, uint32_t mvHeight)
{
    sl::Constants constData = {};

    // Streamline wants near < far, the upscaler input has them swapped for inverted depth like FFX wants
    float cameraNear = std::min(_cameraNear[fIndex], _cameraFar[fIndex]);
    float cameraFar = std::max(_cameraNear[fIndex], _cameraFar[fIndex]);

    if (IsInfiniteDepth())
        cameraFar = std::numeric_limits<float>::infinity();

    if (_cameraPosition[fIndex][0] != 0.0f || _cameraPosition[fIndex][1] != 0.0f || _cameraPosition[fIndex][2] != 0.0f)
    {
        constData.cameraPos.x = _cameraPosition[fIndex][0];
        constData.cameraPos.y = _cameraPosition[fIndex][1];
        constData.cameraPos.z = _cameraPosition[fIndex][2];

        constData.cameraUp.x = _cameraUp[fIndex][0];
        constData.cameraUp.y = _cameraUp[fIndex][1];
        constData.cameraUp.z = _cameraUp[fIndex][2];

        constData.cameraFwd.x = _cameraForward[fIndex][0];
        constData.cameraFwd.y = _cameraForward[fIndex][1];
        constData.cameraFwd.z = _cameraForward[fIndex][2];

        constData.cameraRight.x = _cameraRight[fIndex][0];
        constData.cameraRight.y = _cameraRight[fIndex][1];
        constData.cameraRight.z = _cameraRight[fIndex][2];
    }
    else
    {
        constData.cameraPos = { 0.0f, 0.0f, 0.0f };
        constData.cameraUp = { 0.0f, 0.0f, 1.0f };
        constData.cameraRight = { 0.0f, 1.0f, 0.0f };
        constData.cameraFwd = { 1.0f, 0.0f, 0.0f };
        constData.cameraPinholeOffset = { 0.0f, 0.0f };

        auto prev = XMMatrixIdentity();

        XMFLOAT4X4 temp;
        XMStoreFloat4x4(&temp, prev);
        memcpy(&constData.clipToLensClip, &temp, sizeof(sl::float4x4));
        memcpy(&constData.clipToPrevClip, &temp, sizeof(sl::float4x4));
        memcpy(&constData.prevClipToClip, &temp, sizeof(sl::float4x4));
    }

    {
        XMMATRIX cameraViewToClip {};
        XMMATRIX clipToCameraView {};

        float vFov = _cameraVFov[fIndex];

        // XMMatrixPerspectiveFovRH will fail if input values are incorrect
        if (cameraNear > 0.f && std::isfinite(cameraFar) && !XMScalarNearEqual(vFov, 0.0f, 0.00001f) &&
            !XMScalarNearEqual(_cameraAspectRatio[fIndex], 0.0f, 0.00001f))
        {
            if (XMScalarNearEqual(cameraNear, cameraFar, 0.00001f))
                cameraFar++;

            // A reverse-Z projection for inverted depth, like the game's. DLSSG replacements like Nukem's read near
            // and far back out of it, a regular one loses the far plane to float precision.
            auto projectionNear = IsInvertedDepth() ? cameraFar : cameraNear;
            auto projectionFar = IsInvertedDepth() ? cameraNear : cameraFar;

            cameraViewToClip =
                XMMatrixPerspectiveFovRH(vFov, _cameraAspectRatio[fIndex], projectionNear, projectionFar);
            clipToCameraView = XMMatrixInverse(nullptr, cameraViewToClip);
        }
        else
        {
            LOG_WARN("Can't calculate projectionMatrix");
        }

        // Convert to sl::float4x4 for Streamline
        XMFLOAT4X4 temp;
        XMStoreFloat4x4(&temp, cameraViewToClip);
        memcpy(&constData.cameraViewToClip, &temp, sizeof(sl::float4x4));
        XMStoreFloat4x4(&temp, clipToCameraView);
        memcpy(&constData.clipToCameraView, &temp, sizeof(sl::float4x4));
    }

    constData.cameraAspectRatio = _cameraAspectRatio[fIndex];
    constData.cameraFOV = _cameraVFov[fIndex];
    constData.cameraNear = cameraNear;
    constData.cameraFar = cameraFar;

    constData.jitterOffset.x = _jitterX[fIndex];
    constData.jitterOffset.y = _jitterY[fIndex];

    constData.mvecScale.x = _mvScaleX[fIndex] / (float) mvWidth;
    constData.mvecScale.y = _mvScaleY[fIndex] / (float) mvHeight;

    if (!Config::Instance()->FGSkipReset.value_or_default())
        constData.reset = _reset[fIndex] != 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    else
        constData.reset = sl::Boolean::eFalse;

    constData.depthInverted = IsInvertedDepth() ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constData.cameraMotionIncluded = sl::Boolean::eTrue;
    constData.motionVectors3D = sl::Boolean::eFalse;
    constData.orthographicProjection = sl::Boolean::eFalse;
    constData.motionVectorsDilated = IsLowResMV() ? sl::Boolean::eFalse : sl::Boolean::eTrue;
    constData.motionVectorsJittered = IsJitteredMVs() ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    auto tokenResult = StreamlineProxy::GetNewFrameToken()(_frameToken, &frameId);
    if (tokenResult != sl::Result::eOk)
    {
        LOG_ERROR("GetNewFrameToken error: {} ({})", magic_enum::enum_name(tokenResult), (UINT) tokenResult);
        PauseAfterError();
        return false;
    }

    auto result = StreamlineProxy::SetConstants()(constData, *_frameToken, _viewport);
    if (result != sl::Result::eOk)
    {
        LOG_ERROR("SetConstants error: {} ({})", magic_enum::enum_name(result), (UINT) result);
        PauseAfterError();
        return false;
    }

    return true;
}
