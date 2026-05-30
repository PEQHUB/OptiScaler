#include "pch.h"
#include "DLSSG_Dx12.h"
#include <framewarp/FrameWarp.h>
#include <framewarp/FrameWarp_DLSSGPolicy.h>
#include <hudfix/Hudfix_Dx12.h>
#include <menu/menu_overlay_dx.h>
#include <resource_tracking/ResTrack_dx12.h>
#include <nvapi/fakenvapi.h>

#include <magic_enum.hpp>
#include <DirectXMath.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <vector>

using namespace DirectX;

namespace
{
    static const char* kDlssgUiExtractShader = R"(
Texture2D<float4> HudlessColor : register(t0);
Texture2D<float4> FinalColor : register(t1);
RWTexture2D<float4> UiColorAlpha : register(u0);

float PixelDelta(int2 p, int2 maxPixel)
{
    p = clamp(p, int2(0, 0), maxPixel);
    float3 finalColor = FinalColor.Load(int3(p, 0)).rgb;
    float3 hudless = HudlessColor.Load(int3(p, 0)).rgb;
    float3 d = abs(finalColor - hudless);
    return max(max(d.r, d.g), d.b);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width;
    uint height;
    UiColorAlpha.GetDimensions(width, height);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;

    int2 p = int2(dispatchThreadID.xy);
    int2 maxPixel = int2(width - 1, height - 1);
    float localMax = 0.0;

    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
            localMax = max(localMax, PixelDelta(p + int2(x, y), maxPixel));
    }

    float uiMask = smoothstep(0.025, 0.085, localMax);
    float4 finalColor = FinalColor.Load(int3(p, 0));
    UiColorAlpha[p] = float4(finalColor.rgb, uiMask);
}
)";

    static DXGI_FORMAT DLSSGTranslateTypelessFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32B32_TYPELESS: return DXGI_FORMAT_R32G32B32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UINT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return format;
        }
    }

    static void DLSSGResourceBarrier(ID3D12GraphicsCommandList* cmdList,
                                     ID3D12Resource* resource,
                                     D3D12_RESOURCE_STATES beforeState,
                                     D3D12_RESOURCE_STATES afterState)
    {
        if (cmdList == nullptr || resource == nullptr || beforeState == afterState)
            return;

        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = beforeState;
        barrier.Transition.StateAfter = afterState;
        cmdList->ResourceBarrier(1, &barrier);
    }
}

// --- SL Initialization & Teardown ---

bool DLSSG_Dx12::InitStreamline(ID3D12Device* device)
{
    if (_slInitialized)
        return true;

    if (!SLProxy::InitSL())
    {
        LOG_ERROR("Failed to load Streamline DLLs");
        return false;
    }

    // Configure SL Preferences
    auto dllPath = Util::DllPath();
    std::filesystem::path slPluginPath = dllPath.parent_path() / L"sl";
    static std::wstring slPluginPathStr = slPluginPath.wstring();
    static const wchar_t* pluginPaths[] = { slPluginPathStr.c_str() };

    const sl::Feature baseFeatures[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
    const sl::Feature latewarpProbeFeatures[] = {
        sl::kFeatureDLSS_G,
        sl::kFeatureReflex,
        sl::kFeaturePCL,
        sl::kFeatureLatewarp,
    };

    sl::Preferences prefs {};
    prefs.showConsole = false;
    prefs.pathsToPlugins = pluginPaths;
    prefs.numPathsToPlugins = 1;
    prefs.pathToLogsAndData = nullptr;
    prefs.renderAPI = sl::RenderAPI::eD3D12;
    prefs.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eBypassOSVersionCheck |
                   sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    prefs.engine = sl::EngineType::eCustom;
    prefs.engineVersion = "1.0.0";

    // applicationId is required for NGX initialization which DLSS-G depends on.
    // Use the game's app ID if already known, otherwise use a generic DLSS-G capable app ID.
    auto gameAppId = State::Instance().NVNGX_ApplicationId;
    prefs.applicationId = (gameAppId != 0 && gameAppId != 1337) ? (uint32_t) gameAppId : 231;
    LOG_INFO("Using SL applicationId: {} (game: {})", prefs.applicationId, gameAppId);

    prefs.logLevel = sl::LogLevel::eVerbose;
    prefs.logMessageCallback = [](sl::LogType type, const char* msg) {
        if (msg == nullptr)
            return;
        switch (type)
        {
        case sl::LogType::eInfo:
            LOG_INFO("[SL] {}", msg);
            break;
        case sl::LogType::eWarn:
            LOG_WARN("[SL] {}", msg);
            break;
        case sl::LogType::eError:
        case sl::LogType::eCount:
            LOG_ERROR("[SL] {}", msg);
            break;
        }
    };

    auto tryInit = [&](const sl::Feature* features, uint32_t featureCount, const char* label) {
        prefs.featuresToLoad = features;
        prefs.numFeaturesToLoad = featureCount;

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        // Do NOT use DisableChecks("") here - we need LoadLibrary hooks to remain active
        // so that hookDlssg() and hookCommon() can install on sl.dlss_g.dll and sl.common.dll
        // during slInit's internal plugin loading. These hooks patch the JSON config
        // (hws.required=false) and spoof systemCaps->hwsSupported=true.

        auto result = SLProxy::Init()(prefs, sl::kSDKVersion);

        if (result != sl::Result::eOk)
        {
            LOG_WARN("slInit({}) failed: {}", label, (int) result);
            return false;
        }

        LOG_INFO("slInit({}) succeeded", label);
        return true;
    };

    const bool latewarpFeatureProbe = Config::Instance()->FGDLSSGLatewarpFeatureProbe.value_or_default();
    if (latewarpFeatureProbe)
    {
        LOG_INFO("DLSS-G Latewarp feature probe enabled; requesting sl::kFeatureLatewarp during slInit");
        if (!tryInit(latewarpProbeFeatures, _countof(latewarpProbeFeatures), "DLSSG+Reflex+PCL+Latewarp"))
        {
            LOG_WARN("DLSS-G Latewarp feature probe failed; retrying slInit without Latewarp");
            SLProxy::Shutdown()();
            if (!tryInit(baseFeatures, _countof(baseFeatures), "DLSSG+Reflex+PCL"))
            {
                LOG_ERROR("slInit failed after Latewarp fallback");
                return false;
            }
        }
    }
    else if (!tryInit(baseFeatures, _countof(baseFeatures), "DLSSG+Reflex+PCL"))
    {
        LOG_ERROR("slInit failed");
        return false;
    }

    // Register device
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};

        auto result = SLProxy::SetD3DDevice()(device);

        if (result != sl::Result::eOk)
        {
            LOG_ERROR("slSetD3DDevice failed: {}", (int) result);
            SLProxy::Shutdown()();
            return false;
        }
    }

    _deviceRegistered = true;
    LOG_INFO("slSetD3DDevice succeeded");

    // Diagnostic: check feature support and loaded status
    if (SLProxy::IsFeatureSupported() != nullptr)
    {
        sl::AdapterInfo adapterInfo {};
        auto supportResult = SLProxy::IsFeatureSupported()(sl::kFeatureDLSS_G, adapterInfo);
        LOG_INFO("slIsFeatureSupported(DLSS_G): {} (0=OK)", (int) supportResult);
    }

    if (SLProxy::IsFeatureLoaded() != nullptr)
    {
        bool loaded = false;
        auto loadedResult = SLProxy::IsFeatureLoaded()(sl::kFeatureDLSS_G, loaded);
        LOG_INFO("slIsFeatureLoaded(DLSS_G): result={}, loaded={}", (int) loadedResult, loaded);
    }

    if (SLProxy::GetFeatureRequirements() != nullptr)
    {
        sl::FeatureRequirements reqs {};
        auto reqResult = SLProxy::GetFeatureRequirements()(sl::kFeatureDLSS_G, reqs);
        LOG_INFO("slGetFeatureRequirements(DLSS_G): result={}, flags={:#x}", (int) reqResult, (uint32_t) reqs.flags);
    }

    // Resolve DLSS-G feature functions (requires device to be set)
    if (!SLProxy::ResolveDLSSGFunctions())
    {
        LOG_ERROR("Failed to resolve DLSS-G feature functions");
        SLProxy::Shutdown()();
        return false;
    }

    _dlssgFeatureReady = true;

    // Resolve PCL feature functions (required for DLSS-G frame lifecycle)
    if (!SLProxy::ResolvePCLFunctions())
    {
        LOG_WARN("PCL feature functions not available - DLSS-G requires PCL markers!");
    }

    // Resolve Reflex feature functions (required for DLSS-G)
    if (!SLProxy::ResolveReflexFunctions())
    {
        LOG_WARN("Reflex feature functions not available - DLSS-G requires Reflex!");
    }
    ProbeLatewarpCapability(device);

    // Initialize Reflex - DLSS-G REQUIRES Reflex to be active
    // The programming guide states: "It is REQUIRED for sl.reflex to be integrated"
    if (SLProxy::ReflexSetOptions() != nullptr)
    {
        sl::ReflexOptions reflexOptions {};
        reflexOptions.mode = sl::ReflexMode::eLowLatency;
        reflexOptions.useMarkersToOptimize = false;
        reflexOptions.virtualKey = 0;
        reflexOptions.idThread = 0;
        // Set frame limit from config — lets SL Reflex handle pacing natively
        float fpsCap = Config::Instance()->FramerateLimit.value_or_default();
        if (fpsCap > 0.0f)
        {
            reflexOptions.frameLimitUs = static_cast<uint32_t>(1'000'000.0f / fpsCap);
            LOG_INFO("SL Reflex frameLimitUs set to {} ({}fps)", reflexOptions.frameLimitUs, fpsCap);
        }
        else
        {
            reflexOptions.frameLimitUs = 0;
        }

        auto reflexResult = SLProxy::ReflexSetOptions()(reflexOptions);
        LOG_INFO("slReflexSetOptions (eLowLatency) result: {} (0=OK)", (int) reflexResult);
        LogReflexState("init");
    }
    else
    {
        LOG_ERROR("Cannot initialize Reflex - slReflexSetOptions not available!");
    }

    // Query MFG capabilities
    sl::DLSSGState dlssgState {};
    sl::ViewportHandle viewport(0);
    auto stateResult = SLProxy::DLSSGGetState()(viewport, dlssgState, nullptr);
    if (stateResult == sl::Result::eOk)
    {
        _numFramesToGenerateMax = dlssgState.numFramesToGenerateMax;
        if (_numFramesToGenerateMax == 0)
            _numFramesToGenerateMax = 1;

        State::Instance().DLSSGMaxFramesToGenerate = _numFramesToGenerateMax;
        LOG_INFO("DLSS-G max frames to generate: {}", _numFramesToGenerateMax);
    }
    else
    {
        LOG_WARN("slDLSSGGetState failed during init: {}, defaulting max to 1", (int) stateResult);
        _numFramesToGenerateMax = 1;
    }

    // Clamp requested frame count
    if (_numFramesToGenerate > _numFramesToGenerateMax)
        _numFramesToGenerate = _numFramesToGenerateMax;

    // Set up fakenvapi for Reflex (LatencyFlex mode provides Reflex markers).
    // Skip in DX11 FG proxy mode: Streamline handles Reflex internally via
    // slReflexSetOptions/slReflexSleep, fakenvapi context setup fails with DX11
    // devices anyway, and setting the mode to LatencyFlex causes the menu to
    // display "LatencyFlex" instead of "Reflex".
    if (!State::Instance().dx11FGMode)
    {
        auto fnaResult = fakenvapi::setModeAndContext(nullptr, Mode::LatencyFlex);
        LOG_DEBUG("fakenvapi::setModeAndContext (LatencyFlex): {}", fnaResult);
    }
    else
    {
        LOG_DEBUG("DX11 FG mode: skipping fakenvapi LatencyFlex setup (Streamline handles Reflex)");
    }

    _slInitialized = true;
    LOG_INFO("Streamline initialized successfully for DLSS-G output");
    return true;
}

void DLSSG_Dx12::ShutdownStreamline()
{
    if (!_slInitialized)
        return;

    if (_dlssgFeatureReady)
    {
        // Turn off DLSS-G
        sl::DLSSGOptions options {};
        options.mode = sl::DLSSGMode::eOff;
        sl::ViewportHandle viewport(0);
        SLProxy::DLSSGSetOptions()(viewport, options);

        if (SLProxy::FreeResources() != nullptr)
        {
            sl::ViewportHandle vp(0);
            SLProxy::FreeResources()(sl::kFeatureDLSS_G, vp);
        }
    }

    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        State::DisableChecks(0x534C4F50, "");

        SLProxy::Shutdown()();

        State::EnableChecks(0x534C4F50);
    }

    _slInitialized = false;
    _deviceRegistered = false;
    _dlssgFeatureReady = false;
    _currentFrameToken = nullptr;

    LOG_INFO("Streamline shut down");
}

void DLSSG_Dx12::ProbeLatewarpCapability(ID3D12Device* device)
{
    static bool loggedOnce = false;
    if (loggedOnce)
        return;
    loggedOnce = true;

    LOG_INFO("SL probe: headers SDK version {}.{}.{}", SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);

    LUID adapterLuid {};
    sl::AdapterInfo adapterInfo {};
    if (device != nullptr)
    {
        adapterLuid = device->GetAdapterLuid();
        adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&adapterLuid);
        adapterInfo.deviceLUIDSizeInBytes = sizeof(adapterLuid);
    }

    auto probeFeature = [&](sl::Feature feature, const char* name) {
        if (SLProxy::IsFeatureSupported() != nullptr)
        {
            auto result = SLProxy::IsFeatureSupported()(feature, adapterInfo);
            LOG_INFO("SL probe {} support: result={} ({})", name, (int) result, magic_enum::enum_name(result));
        }

        if (SLProxy::IsFeatureLoaded() != nullptr)
        {
            bool loaded = false;
            auto result = SLProxy::IsFeatureLoaded()(feature, loaded);
            LOG_INFO("SL probe {} loaded: result={} ({}) loaded={}", name, (int) result,
                     magic_enum::enum_name(result), loaded);
        }

        if (SLProxy::GetFeatureRequirements() != nullptr)
        {
            sl::FeatureRequirements reqs {};
            auto result = SLProxy::GetFeatureRequirements()(feature, reqs);
            LOG_INFO("SL probe {} requirements: result={} ({}) flags={:#x} viewports={} cpuThreads={} os={} requiredOs={} driver={} requiredDriver={}",
                     name,
                     (int) result,
                     magic_enum::enum_name(result),
                     (uint32_t) reqs.flags,
                     reqs.maxNumViewports,
                     reqs.maxNumCPUThreads,
                     reqs.osVersionDetected.toStr(),
                     reqs.osVersionRequired.toStr(),
                     reqs.driverVersionDetected.toStr(),
                     reqs.driverVersionRequired.toStr());
        }

        if (SLProxy::GetFeatureVersion() != nullptr)
        {
            sl::FeatureVersion version {};
            auto result = SLProxy::GetFeatureVersion()(feature, version);
            LOG_INFO("SL probe {} version: result={} ({}) sl={} ngx={}",
                     name,
                     (int) result,
                     magic_enum::enum_name(result),
                     version.versionSL.toStr(),
                     version.versionNGX.toStr());
        }
    };

    probeFeature(sl::kFeatureReflex, "Reflex");
    probeFeature(sl::kFeaturePCL, "PCL");
    probeFeature(sl::kFeatureLatewarp, "Latewarp");
}

void DLSSG_Dx12::SubmitReflexCameraData(int fIndex, const XMMATRIX& worldToView, const XMMATRIX& viewToClip,
                                        bool haveView)
{
    if (_currentFrameToken == nullptr)
        return;

    static uint64_t callCount = 0;
    static sl::Result lastSetResult = sl::Result::eOk;
    static sl::Result lastPredictResult = sl::Result::eOk;
    callCount++;

    if (SLProxy::ReflexSetCameraData() == nullptr)
    {
        if (callCount == 1)
            LOG_INFO("SL Reflex camera probe skipped: slReflexSetCameraData is unavailable");
        return;
    }

    if (!haveView)
    {
        if (callCount <= 3 || callCount % 300 == 0)
            LOG_INFO("SL Reflex camera probe skipped: camera vectors unavailable for frame token {}",
                     (uint32_t) *_currentFrameToken);
        return;
    }

    const int prevIndex = (fIndex + BUFFER_COUNT - 1) % BUFFER_COUNT;
    const bool havePrevious = !_reset[fIndex] && _hasPrevReflexCamera[prevIndex];
    const XMMATRIX& prevWorldToView = havePrevious ? _prevReflexWorldToView[prevIndex] : worldToView;
    const XMMATRIX& prevViewToClip = havePrevious ? _prevReflexViewToClip[prevIndex] : viewToClip;

    sl::ReflexCameraData cameraData {};
    memcpy(&cameraData.worldToViewMatrix, worldToView.r, sizeof(sl::float4x4));
    memcpy(&cameraData.viewToClipMatrix, viewToClip.r, sizeof(sl::float4x4));
    memcpy(&cameraData.prevRenderedWorldToViewMatrix, prevWorldToView.r, sizeof(sl::float4x4));
    memcpy(&cameraData.prevRenderedViewToClipMatrix, prevViewToClip.r, sizeof(sl::float4x4));

    sl::ViewportHandle viewport(0);
    auto setResult = SLProxy::ReflexSetCameraData()(viewport, *_currentFrameToken, cameraData);
    const bool logSet = callCount <= 10 || callCount % 300 == 0 || setResult != lastSetResult;
    if (logSet)
    {
        LOG_INFO("SL Reflex camera data: frame={} result={} ({}) hadPrevious={}",
                 (uint32_t) *_currentFrameToken,
                 (int) setResult,
                 magic_enum::enum_name(setResult),
                 havePrevious);
    }
    lastSetResult = setResult;

    if (setResult == sl::Result::eOk)
        SetPCLCameraConstructed();

    if (SLProxy::ReflexGetPredictedCameraData() != nullptr)
    {
        sl::ReflexPredictedCameraData predicted {};
        auto predictResult = SLProxy::ReflexGetPredictedCameraData()(viewport, *_currentFrameToken, predicted);
        const bool logPredict = callCount <= 10 || callCount % 300 == 0 || predictResult != lastPredictResult;
        if (logPredict)
        {
            const auto& row = predicted.predictedWorldToViewMatrix[3];
            LOG_INFO("SL Reflex predicted camera: frame={} result={} ({}) posRow=({:.4f}, {:.4f}, {:.4f}, {:.4f})",
                     (uint32_t) *_currentFrameToken,
                     (int) predictResult,
                     magic_enum::enum_name(predictResult),
                     row.x,
                     row.y,
                     row.z,
                     row.w);
        }
        lastPredictResult = predictResult;
    }
    else if (callCount == 1)
    {
        LOG_INFO("SL Reflex camera prediction skipped: slReflexGetPredictedCameraData is unavailable");
    }

    _prevReflexWorldToView[fIndex] = worldToView;
    _prevReflexViewToClip[fIndex] = viewToClip;
    _hasPrevReflexCamera[fIndex] = true;
}

void DLSSG_Dx12::LogReflexState(const char* source)
{
    if (SLProxy::ReflexGetState() == nullptr)
        return;

    static uint64_t callCount = 0;
    static sl::Result lastResult = sl::Result::eOk;
    callCount++;

    const bool baseLog = callCount <= 5 || callCount % 300 == 0;
    sl::ReflexState state {};
    auto result = SLProxy::ReflexGetState()(state);
    if (!baseLog && result == lastResult)
        return;

    const sl::ReflexReport* latestReport = nullptr;
    const sl::ReflexReport2* latestReport2 = nullptr;
    for (int i = sl::kReflexFrameReportCount - 1; i >= 0; --i)
    {
        const auto& report = state.frameReport[i];
        if (report.frameID != 0 || report.presentEndTime != 0 || report.gpuRenderEndTime != 0)
        {
            latestReport = &report;
            latestReport2 = &state.frameReport2[i];
            break;
        }
    }

    if (latestReport != nullptr)
    {
        const uint64_t inputToPresent =
            latestReport->presentEndTime > latestReport->inputSampleTime
                ? latestReport->presentEndTime - latestReport->inputSampleTime
                : 0;
        LOG_INFO("SL Reflex state [{}]: result={} ({}) lowLatency={} latencyReport={} frame={} inputToPresentRaw={} gpuActiveUs={} gpuFrameUs={} cameraConstructed={} crossAdapterCopyUs={}",
                 source,
                 (int) result,
                 magic_enum::enum_name(result),
                 state.lowLatencyAvailable,
                 state.latencyReportAvailable,
                 latestReport->frameID,
                 inputToPresent,
                 latestReport->gpuActiveRenderTimeUs,
                 latestReport->gpuFrameTimeUs,
                 latestReport2 != nullptr ? latestReport2->cameraConstructedTime : 0,
                 latestReport2 != nullptr ? latestReport2->crossAdapterCopyTimeUs : 0);
    }
    else
    {
        LOG_INFO("SL Reflex state [{}]: result={} ({}) lowLatency={} latencyReport={} no frame report yet",
                 source,
                 (int) result,
                 magic_enum::enum_name(result),
                 state.lowLatencyAvailable,
                 state.latencyReportAvailable);
    }

    lastResult = result;
}

// --- SL Resource Helper ---

sl::Resource DLSSG_Dx12::MakeSLResource(ID3D12Resource* d3dResource, D3D12_RESOURCE_STATES state)
{
    auto desc = d3dResource->GetDesc();
    sl::Resource slRes(sl::ResourceType::eTex2d, d3dResource, (uint32_t) state);
    slRes.width = (uint32_t) desc.Width;
    slRes.height = (uint32_t) desc.Height;
    slRes.nativeFormat = (uint32_t) desc.Format;
    slRes.mipLevels = desc.MipLevels;
    slRes.arrayLayers = desc.DepthOrArraySize;
    return slRes;
}

bool DLSSG_Dx12::EnsureUiExtractionPipeline(ID3D12Device* device)
{
    if (_uiExtractPipelineState != nullptr && _uiExtractRootSignature != nullptr)
        return true;
    if (device == nullptr)
        return false;

    CD3DX12_DESCRIPTOR_RANGE1 descriptorRanges[] = {
        CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0),
        CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0),
    };

    CD3DX12_ROOT_PARAMETER1 rootParameter {};
    rootParameter.InitAsDescriptorTable(_countof(descriptorRanges), descriptorRanges);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc;
    rootSigDesc.Init_1_1(1, &rootParameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ID3DBlob* signatureBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signatureBlob, &errorBlob);
    if (FAILED(hr) || signatureBlob == nullptr)
    {
        LOG_ERROR("DLSSG UI extract root signature serialize failed {:X} {}",
                  hr,
                  errorBlob != nullptr ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "");
        if (errorBlob != nullptr)
            errorBlob->Release();
        return false;
    }

    hr = device->CreateRootSignature(
        0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
        IID_PPV_ARGS(&_uiExtractRootSignature));
    signatureBlob->Release();
    if (errorBlob != nullptr)
        errorBlob->Release();
    if (FAILED(hr) || _uiExtractRootSignature == nullptr)
    {
        LOG_ERROR("DLSSG UI extract root signature create failed {:X}", hr);
        return false;
    }

    ID3DBlob* shaderBlob = nullptr;
    errorBlob = nullptr;
    hr = D3DCompile(
        kDlssgUiExtractShader,
        strlen(kDlssgUiExtractShader),
        nullptr,
        nullptr,
        nullptr,
        "CSMain",
        "cs_5_0",
        0,
        0,
        &shaderBlob,
        &errorBlob);
    if (FAILED(hr) || shaderBlob == nullptr)
    {
        LOG_ERROR("DLSSG UI extract shader compile failed {:X} {}",
                  hr,
                  errorBlob != nullptr ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "");
        if (errorBlob != nullptr)
            errorBlob->Release();
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc {};
    psoDesc.pRootSignature = _uiExtractRootSignature;
    psoDesc.CS = CD3DX12_SHADER_BYTECODE(shaderBlob);
    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_uiExtractPipelineState));
    shaderBlob->Release();
    if (errorBlob != nullptr)
        errorBlob->Release();
    if (FAILED(hr) || _uiExtractPipelineState == nullptr)
    {
        LOG_ERROR("DLSSG UI extract pipeline create failed {:X}", hr);
        return false;
    }

    LOG_INFO("DLSSG UI extract pipeline ready");
    return true;
}

bool DLSSG_Dx12::EnsureSynthUiResource(int fIndex, const D3D12_RESOURCE_DESC& desc)
{
    if (_device == nullptr || fIndex < 0 || fIndex >= BUFFER_COUNT)
        return false;

    bool recreate = _synthUiResource[fIndex] == nullptr;
    if (_synthUiResource[fIndex] != nullptr)
    {
        auto existing = _synthUiResource[fIndex]->GetDesc();
        recreate =
            existing.Width != desc.Width ||
            existing.Height != desc.Height ||
            existing.Format != desc.Format ||
            existing.SampleDesc.Count != desc.SampleDesc.Count ||
            existing.MipLevels != desc.MipLevels ||
            existing.DepthOrArraySize != desc.DepthOrArraySize;
    }

    if (!recreate)
        return true;

    if (_synthUiResource[fIndex] != nullptr)
    {
        _synthUiResource[fIndex]->Release();
        _synthUiResource[fIndex] = nullptr;
    }

    D3D12_RESOURCE_DESC uiDesc = desc;
    uiDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = _device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &uiDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&_synthUiResource[fIndex]));
    if (FAILED(hr) || _synthUiResource[fIndex] == nullptr)
    {
        LOG_ERROR("DLSSG synthesized UI resource allocation failed {:X}", hr);
        return false;
    }

    _synthUiResource[fIndex]->SetName(std::format(L"DLSSG_SynthUIColorAlpha[{}]", fIndex).c_str());
    _synthUiResourceState[fIndex] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}

ID3D12Resource* DLSSG_Dx12::SynthesizeUiColorAlpha(
    int fIndex,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* hudless,
    D3D12_RESOURCE_STATES hudlessState,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_STATES* outState)
{
    if (outState != nullptr)
        *outState = D3D12_RESOURCE_STATE_COMMON;

    if (cmdList == nullptr || hudless == nullptr || _swapChain == nullptr || _device == nullptr ||
        fIndex < 0 || fIndex >= BUFFER_COUNT)
        return nullptr;
    if (Config::Instance()->FrameWarpDLSSGMode.value_or_default() != 4)
        return nullptr;
    if (!EnsureUiExtractionPipeline(_device))
        return nullptr;

    IDXGISwapChain3* sc3 = nullptr;
    if (((IDXGISwapChain*) _swapChain)->QueryInterface(IID_PPV_ARGS(&sc3)) != S_OK || sc3 == nullptr)
        return nullptr;

    ID3D12Resource* backBuffer = nullptr;
    UINT bbIndex = sc3->GetCurrentBackBufferIndex();
    HRESULT hr = sc3->GetBuffer(bbIndex, IID_PPV_ARGS(&backBuffer));
    sc3->Release();
    if (FAILED(hr) || backBuffer == nullptr)
        return nullptr;

    if (backBuffer == hudless)
    {
        backBuffer->Release();
        return nullptr;
    }

    auto presentDesc = backBuffer->GetDesc();
    auto hudlessDesc = hudless->GetDesc();
    if (presentDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        hudlessDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        presentDesc.Width != hudlessDesc.Width ||
        presentDesc.Height != hudlessDesc.Height ||
        presentDesc.Width != width ||
        presentDesc.Height != height ||
        presentDesc.Format != hudlessDesc.Format ||
        presentDesc.Format != format ||
        presentDesc.SampleDesc.Count != 1 ||
        hudlessDesc.SampleDesc.Count != 1)
    {
        static uint64_t mismatchLogCount = 0;
        mismatchLogCount++;
        if (mismatchLogCount <= 20 || mismatchLogCount % 120 == 0)
        {
            LOG_INFO("DLSSG synthesized UI skipped: mismatch present={}x{} fmt={} hudless={}x{} fmt={} tag={}x{} fmt={}",
                     presentDesc.Width,
                     presentDesc.Height,
                     static_cast<uint32_t>(presentDesc.Format),
                     hudlessDesc.Width,
                     hudlessDesc.Height,
                     static_cast<uint32_t>(hudlessDesc.Format),
                     width,
                     height,
                     static_cast<uint32_t>(format));
        }
        backBuffer->Release();
        return nullptr;
    }

    if (!EnsureSynthUiResource(fIndex, presentDesc))
    {
        backBuffer->Release();
        return nullptr;
    }

    FrameDescriptorHeap& heap = _uiExtractHeaps[fIndex];
    D3D12_SHADER_RESOURCE_VIEW_DESC hudlessSrv {};
    hudlessSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hudlessSrv.Format = DLSSGTranslateTypelessFormat(hudlessDesc.Format);
    hudlessSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    hudlessSrv.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(hudless, &hudlessSrv, heap.GetSrvCPU(0));

    D3D12_SHADER_RESOURCE_VIEW_DESC presentSrv {};
    presentSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    presentSrv.Format = DLSSGTranslateTypelessFormat(presentDesc.Format);
    presentSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    presentSrv.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(backBuffer, &presentSrv, heap.GetSrvCPU(1));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uiUav {};
    uiUav.Format = DLSSGTranslateTypelessFormat(presentDesc.Format);
    uiUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uiUav.Texture2D.MipSlice = 0;
    _device->CreateUnorderedAccessView(_synthUiResource[fIndex], nullptr, &uiUav, heap.GetUavCPU(0));

    DLSSGResourceBarrier(cmdList, hudless, hudlessState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DLSSGResourceBarrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DLSSGResourceBarrier(cmdList, _synthUiResource[fIndex], _synthUiResourceState[fIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_uiExtractRootSignature);
    cmdList->SetPipelineState(_uiExtractPipelineState);
    cmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    cmdList->Dispatch(
        (static_cast<UINT>(presentDesc.Width) + 15) / 16,
        (presentDesc.Height + 15) / 16,
        1);

    D3D12_RESOURCE_BARRIER uavBarrier {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = _synthUiResource[fIndex];
    cmdList->ResourceBarrier(1, &uavBarrier);

    DLSSGResourceBarrier(cmdList, _synthUiResource[fIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    _synthUiResourceState[fIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    DLSSGResourceBarrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    DLSSGResourceBarrier(cmdList, hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, hudlessState);

    backBuffer->Release();

    if (outState != nullptr)
        *outState = _synthUiResourceState[fIndex];

    static uint64_t synthUiLogCount = 0;
    synthUiLogCount++;
    if (synthUiLogCount <= 40 || synthUiLogCount % 120 == 0)
    {
        LOG_INFO("DLSSG synthesized UIColorAndAlpha #{} frame={} index={} resource={:X} hudless={:X} size={}x{} fmt={}",
                 synthUiLogCount,
                 State::Instance().frameCount,
                 fIndex,
                 reinterpret_cast<uintptr_t>(_synthUiResource[fIndex]),
                 reinterpret_cast<uintptr_t>(hudless),
                 width,
                 height,
                 static_cast<uint32_t>(format));
    }

    return _synthUiResource[fIndex];
}

// --- IFGFeature Implementation ---

const char* DLSSG_Dx12::Name()
{
    // Not static — each DLSSG_Dx12 instance owns its own name buffer
    thread_local std::string nameBuffer;

    if (nameBuffer.empty() || _nameStale)
    {
        _nameStale = false;
        if (_numFramesToGenerateMax <= 1)
        {
            nameBuffer = "DLSS-G";
        }
        else
        {
            auto count = _numFramesToGenerate + 1;
            nameBuffer = "DLSS-G " + std::to_string(count) + "x";
        }
    }

    return nameBuffer.c_str();
}

feature_version DLSSG_Dx12::Version()
{
    if (SLProxy::IsReady())
        return SLProxy::Version();

    return { 0, 0, 0 };
}

HWND DLSSG_Dx12::Hwnd() { return _hwnd; }

// --- Swapchain Creation ---
// SL interposer hooks DXGI internally. We init SL, then let the game create the
// swapchain normally. SL will intercept and wrap it transparently.

bool DLSSG_Dx12::CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                  IDXGISwapChain** swapChain, bool readyToRelease)
{
    (void) readyToRelease;

    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == desc->OutputWindow)
    {
        LOG_WARN("FG swapchain already created for the same output window!");
        auto result = State::Instance().currentFGSwapchain->ResizeBuffers(desc->BufferCount, desc->BufferDesc.Width,
                                                                          desc->BufferDesc.Height,
                                                                          desc->BufferDesc.Format, desc->Flags) == S_OK;
        *swapChain = State::Instance().currentFGSwapchain;
        return result;
    }

    if (State::Instance().currentD3D12Device == nullptr)
        return false;

    // Initialize Streamline if not done yet
    if (!_slInitialized && !InitStreamline(State::Instance().currentD3D12Device))
    {
        LOG_ERROR("Failed to initialize Streamline for swapchain creation");
        return false;
    }

    IDXGIFactory* realFactory = nullptr;
    ID3D12CommandQueue* realQueue = nullptr;

    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    if (!CheckForRealObject(__FUNCTION__, cmdQueue, (IUnknown**) &realQueue))
        realQueue = cmdQueue;

    // Upgrade factory through SL interposer so it hooks the swapchain
    if (SLProxy::UpgradeInterface() != nullptr)
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipSpoofing skipSpoofing {};
        ScopedSkipParentWrapping skipWrapping {};
        State::DisableChecks(0x534C4F50, "");

        auto result = SLProxy::UpgradeInterface()((void**)&realFactory);

        State::EnableChecks(0x534C4F50);

        if (result != sl::Result::eOk)
        {
            LOG_WARN("slUpgradeInterface on factory failed: {}, continuing anyway", (int) result);
        }
    }

    // Create the swapchain - SL will intercept this via its DXGI hooks
    IDXGIFactory2* factory2 = nullptr;
    if (realFactory->QueryInterface(IID_PPV_ARGS(&factory2)) != S_OK)
    {
        LOG_ERROR("Failed to get IDXGIFactory2");
        return false;
    }

    HWND hwnd = desc->OutputWindow;
    DXGI_SWAP_CHAIN_DESC1 scDesc {};
    scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scDesc.BufferCount = desc->BufferCount;
    scDesc.BufferUsage = desc->BufferUsage;
    scDesc.Flags = desc->Flags;
    scDesc.Format = desc->BufferDesc.Format;
    scDesc.Height = desc->BufferDesc.Height;
    scDesc.SampleDesc = desc->SampleDesc;

    switch (desc->BufferDesc.Scaling)
    {
    case DXGI_MODE_SCALING_CENTERED:
        scDesc.Scaling = DXGI_SCALING_ASPECT_RATIO_STRETCH;
        break;
    case DXGI_MODE_SCALING_STRETCHED:
        scDesc.Scaling = DXGI_SCALING_STRETCH;
        break;
    case DXGI_MODE_SCALING_UNSPECIFIED:
        scDesc.Scaling = DXGI_SCALING_NONE;
        break;
    }

    scDesc.Stereo = false;
    scDesc.SwapEffect = desc->SwapEffect;
    scDesc.Width = desc->BufferDesc.Width;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc {};
    fsDesc.RefreshRate = desc->BufferDesc.RefreshRate;
    fsDesc.Scaling = desc->BufferDesc.Scaling;
    fsDesc.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
    fsDesc.Windowed = desc->Windowed;

    IDXGISwapChain1* swapChain1 = nullptr;
    HRESULT hr;
    {
        ScopedSkipSpoofing skipSpoofing {};
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipWrapping {};

        hr = factory2->CreateSwapChainForHwnd(realQueue, hwnd, &scDesc, &fsDesc, nullptr, &swapChain1);
    }

    factory2->Release();

    if (FAILED(hr) || swapChain1 == nullptr)
    {
        LOG_ERROR("CreateSwapChainForHwnd failed: {:X}", (UINT) hr);
        return false;
    }

    *swapChain = swapChain1;
    _gameCommandQueue = realQueue;
    _swapChain = *swapChain;
    _hwnd = hwnd;

    LOG_INFO("DLSS-G swapchain created via SL interposer");
    return true;
}

bool DLSSG_Dx12::CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd,
                                   DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                   IDXGISwapChain1** swapChain, bool readyToRelease)
{
    (void) readyToRelease;

    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == hwnd)
    {
        LOG_WARN("FG swapchain already created for the same output window!");
        auto result = State::Instance().currentFGSwapchain->ResizeBuffers(desc->BufferCount, desc->Width, desc->Height,
                                                                          desc->Format, desc->Flags) == S_OK;
        *swapChain = (IDXGISwapChain1*) State::Instance().currentFGSwapchain;
        return result;
    }

    if (State::Instance().currentD3D12Device == nullptr)
        return false;

    // Initialize Streamline if not done yet
    if (!_slInitialized && !InitStreamline(State::Instance().currentD3D12Device))
    {
        LOG_ERROR("Failed to initialize Streamline for swapchain creation");
        return false;
    }

    IDXGIFactory* realFactory = nullptr;
    ID3D12CommandQueue* realQueue = nullptr;

    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    if (!CheckForRealObject(__FUNCTION__, cmdQueue, (IUnknown**) &realQueue))
        realQueue = cmdQueue;

    // Upgrade factory through SL interposer
    if (SLProxy::UpgradeInterface() != nullptr)
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipSpoofing skipSpoofing {};
        ScopedSkipParentWrapping skipWrapping {};
        State::DisableChecks(0x534C4F50, "");

        auto result = SLProxy::UpgradeInterface()((void**)&realFactory);

        State::EnableChecks(0x534C4F50);

        if (result != sl::Result::eOk)
        {
            LOG_WARN("slUpgradeInterface on factory failed: {}, continuing anyway", (int) result);
        }
    }

    IDXGIFactory2* factory2 = nullptr;
    if (realFactory->QueryInterface(IID_PPV_ARGS(&factory2)) != S_OK)
    {
        LOG_ERROR("Failed to get IDXGIFactory2");
        return false;
    }

    HRESULT hr;
    {
        ScopedSkipSpoofing skipSpoofing {};
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipWrapping {};

        hr = factory2->CreateSwapChainForHwnd(realQueue, hwnd, desc, pFullscreenDesc, nullptr, swapChain);
    }

    factory2->Release();

    if (FAILED(hr) || *swapChain == nullptr)
    {
        LOG_ERROR("CreateSwapChainForHwnd failed: {:X}", (UINT) hr);
        return false;
    }

    _gameCommandQueue = realQueue;
    _swapChain = *swapChain;
    _hwnd = hwnd;

    LOG_INFO("DLSS-G swapchain1 created via SL interposer");
    return true;
}

// --- Context Management ---

void DLSSG_Dx12::CreateContext(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_DEBUG("");

    _device = device;

    if (!_objectsCreated)
    {
        CreateObjects(device);
        _objectsCreated = true;
    }

    if (!_slInitialized)
    {
        InitStreamline(device);
    }

    if (_slInitialized && _dlssgFeatureReady)
    {
        // Do NOT set DLSS-G mode=On here. The FG input (upscaler) may not be providing
        // data yet (e.g. during loading screens). If we set mode=On now, SL's present
        // hooks will expect slEvaluateFeature calls that never come, causing a deadlock.
        // Instead, keep mode=eOff and let Activate() set mode=On when EvaluateState()
        // confirms the upscaler is actually running.
        LOG_INFO("DLSS-G context created, feature ready (mode=Off until Activate)");

        _lastDispatchedFrame = 0;
    }

    if (_isActive)
    {
        LOG_INFO("FG context recreated while active, pausing");
        State::Instance().FGchanged = true;
        UpdateTarget();
        Deactivate();
    }
}

void DLSSG_Dx12::Activate()
{
    LOG_DEBUG("");

    if (!_slInitialized || !_dlssgFeatureReady || !_objectsCreated)
        return;

    if (!_isActive)
    {
        sl::DLSSGOptions options {};
        options.mode = sl::DLSSGMode::eOn;
        options.numFramesToGenerate = _numFramesToGenerate;
        options.flags = sl::DLSSGFlags::eEnableFullscreenMenuDetection;
        const bool hudlessSubmitMode = Config::Instance()->FrameWarpDLSSGMode.value_or_default() == 4;
        if (hudlessSubmitMode)
            options.enableUserInterfaceRecomposition = sl::Boolean::eFalse;

        sl::ViewportHandle viewport(0);
        auto result = SLProxy::DLSSGSetOptions()(viewport, options);

        if (result == sl::Result::eOk)
        {
            _isActive = true;
            _lastDispatchedFrame = 0;
            for (size_t i = 0; i < BUFFER_COUNT; i++)
                _hasPrevProjection[i] = false;
            LOG_INFO("DLSS-G activated, numFramesToGenerate={}, uiRecomposition={}, hudlessSubmitMode={}",
                     _numFramesToGenerate,
                     options.enableUserInterfaceRecomposition == sl::Boolean::eTrue,
                     hudlessSubmitMode);
        }
        else
        {
            LOG_ERROR("Failed to activate DLSS-G: {}", (int) result);
        }
    }
}

void DLSSG_Dx12::Deactivate()
{
    LOG_DEBUG("");

    if (_isActive)
    {
        auto fIndex = GetIndex();
        if (_uiCommandListResetted[fIndex])
        {
            auto closeResult = _uiCommandList[fIndex]->Close();
            if (closeResult == S_OK)
            {
                _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[fIndex]);
                SignalUIFence(fIndex);
            }
            else
                LOG_ERROR("_uiCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            _uiCommandListResetted[fIndex] = false;
        }

        if (_slInitialized && _dlssgFeatureReady)
        {
            sl::DLSSGOptions options {};
            options.mode = sl::DLSSGMode::eOff;

            sl::ViewportHandle viewport(0);
            auto result = SLProxy::DLSSGSetOptions()(viewport, options);
            if (result == sl::Result::eOk)
                _isActive = false;
            else
                LOG_ERROR("Failed to deactivate DLSS-G: {}", (int) result);
        }
        else
        {
            _isActive = false;
        }

        _waitingNewFrameData = false;
        LOG_INFO("DLSS-G deactivated");
    }
}

void DLSSG_Dx12::DestroyFGContext()
{
    Deactivate();
    ReleaseObjects();
}

bool DLSSG_Dx12::Shutdown()
{
    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    DestroyFGContext();
    ShutdownStreamline();

    return true;
}

// --- State Evaluation ---

void DLSSG_Dx12::EvaluateState(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_FUNC();

    // Store the incoming FG constants so flags (depth inversion, HDR, jittered MVs, etc.)
    // are available later in SetSLConstants(). Without this, IsInvertedDepth() etc. always
    // return defaults because _constants was never updated from the Upscaler input path.
    _constants = fgConstants;

    auto& state = State::Instance();

    if (!SLProxy::IsReady() || state.currentFGSwapchain == nullptr)
        return;

    if (state.isShuttingDown)
    {
        DestroyFGContext();
        return;
    }

    // If FG Enabled from menu
    if (Config::Instance()->FGEnabled.value_or_default())
    {
        if (!_slInitialized)
        {
            CreateContext(device, fgConstants);
            UpdateTarget();
        }
        else if (state.FGchanged)
        {
            LOG_DEBUG("FGChanged");
            Deactivate();
            UpdateTarget();

            if (state.SCchanged)
                DestroyFGContext();
        }

        // Create command lists/allocators if not yet done (InitStreamline may have
        // been called during CreateSwapchain, but CreateObjects deferred until now)
        if (_slInitialized && _dlssgFeatureReady && !_objectsCreated)
        {
            _device = device;
            CreateObjects(device);
            _objectsCreated = true;
            _lastDispatchedFrame = 0;
            LOG_INFO("DLSS-G objects created (deferred from CreateSwapchain)");
        }

        if (_slInitialized && _dlssgFeatureReady && _objectsCreated &&
            State::Instance().activeFgInput == FGInput::Upscaler && !IsPaused() && !IsActive())
            Activate();
    }
    else
    {
        LOG_DEBUG("!FGEnabled");
        Deactivate();

        state.ClearCapturedHudlesses = true;
        Hudfix_Dx12::ResetCounters();
    }

    if (state.FGchanged)
    {
        LOG_DEBUG("FGchanged");
        state.FGchanged = false;

        Hudfix_Dx12::ResetCounters();
        UpdateTarget();

        if (Mutex.getOwner() == 2)
            Mutex.unlockThis(2);
    }

    state.SCchanged = false;
}

// --- Resource Tagging for SL ---

void DLSSG_Dx12::SetSLConstants(int fIndex)
{
    // Acquire shared lock — SetResource() writes to _frameResources under unique_lock,
    // so we need at least a shared_lock for safe concurrent reads (MV dimensions, etc.)
    std::shared_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);

    sl::Constants slConstants {};

    // Depth inversion flag
    slConstants.depthInverted =
        IsInvertedDepth() ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    // Motion vector flags
    slConstants.motionVectorsJittered =
        IsJitteredMVs() ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    slConstants.motionVectorsDilated =
        IsLowResMV() ? sl::Boolean::eFalse : sl::Boolean::eTrue;
    slConstants.cameraMotionIncluded = sl::Boolean::eTrue;
    slConstants.motionVectors3D = sl::Boolean::eFalse;

    // Camera values
    slConstants.cameraNear = _cameraNear[fIndex];
    slConstants.cameraFar = _cameraFar[fIndex];
    slConstants.cameraFOV = _cameraVFov[fIndex];
    slConstants.cameraAspectRatio = _cameraAspectRatio[fIndex];

    if (Config::Instance()->FrameWarpEnabled.value_or_default() &&
        Config::Instance()->FrameWarpDLSSGMode.value_or_default() == 4)
    {
        FrameWarpRuntime::BeginDLSSGResourceFrameAnchor(
            slConstants.cameraFOV,
            slConstants.cameraAspectRatio,
            "optiscaler-dlssg-constants");
    }

    // Jitter
    slConstants.jitterOffset = sl::float2(_jitterX[fIndex], _jitterY[fIndex]);

    // MV scale handling depends on the input path:
    // - Streamline path: pre-multiplied mvecScale by MV resource dimensions -> must divide to recover
    // - Upscaler path: raw NVNGX values (game provides MV_Scale_X/Y directly) -> different conversion
    // SL expects mvecScale to normalize MVs to [-1,1] range (ProgrammingGuideDLSS_G.md).
    if (IsMVScalePreMultiplied(fIndex))
    {
        // Streamline path: reverse the pre-multiplication to recover normalized scale
        float mvWidth = 0.0f, mvHeight = 0.0f;
        if (_frameResources[fIndex].contains(FG_ResourceType::Velocity))
        {
            auto& mvRes = _frameResources[fIndex][FG_ResourceType::Velocity];
            mvWidth = (float) mvRes.width;
            mvHeight = (float) mvRes.height;
        }

        if (mvWidth > 0.0f && mvHeight > 0.0f)
            slConstants.mvecScale = sl::float2(_mvScaleX[fIndex] / mvWidth, _mvScaleY[fIndex] / mvHeight);
        else
            slConstants.mvecScale = sl::float2(_mvScaleX[fIndex], _mvScaleY[fIndex]);
    }
    else
    {
        // Upscaler path: NVNGX MV_Scale_X/Y converts MV texels to pixel displacement.
        // SL mvecScale converts MV texels to [-1,1] normalized range.
        // NVNGX convention: pixel_displacement = MV_texel * MV_Scale
        // SL convention: normalized_mv = MV_texel * mvecScale, where normalized is [-1,1] over MV buffer.
        // Per NVIDIA docs: "consts.mvecScale = {1.0f / renderWidth, 1.0f / renderHeight};"
        // So: mvecScale = MV_Scale / mvWidth (for X), MV_Scale / mvHeight (for Y).
        // CRITICAL: Must use MV resource dimensions, NOT display dimensions.
        // MVs are at render resolution (e.g. 2953x1661), not display (3840x2160).
        float mvScaleX = _mvScaleX[fIndex];
        float mvScaleY = _mvScaleY[fIndex];

        // Get MV resource dimensions (render resolution)
        float mvWidth = 0.0f, mvHeight = 0.0f;
        if (_frameResources[fIndex].contains(FG_ResourceType::Velocity))
        {
            auto& mvRes = _frameResources[fIndex][FG_ResourceType::Velocity];
            mvWidth = (float) mvRes.width;
            mvHeight = (float) mvRes.height;
        }

        if (mvWidth > 0.0f && mvHeight > 0.0f && mvScaleX != 0.0f && mvScaleY != 0.0f)
            slConstants.mvecScale = sl::float2(mvScaleX / mvWidth, mvScaleY / mvHeight);
        else
            slConstants.mvecScale = sl::float2(mvScaleX, mvScaleY);
    }

    // Camera position and orientation
    // When camera vectors are unavailable (Upscaler path never calls SetCameraData()),
    // all values are zero-initialized. In that case, leave slConstants at its default
    // INVALID_FLOAT values (set by sl::Constants constructor) so DLSS-G knows the data
    // is unavailable and can use its own internal motion estimation heuristics.
    // Setting (0,0,0) was wrong — it told DLSS-G the camera was at the world origin.
    bool haveCameraVectors = (_cameraPosition[fIndex][0] != 0.0f || _cameraPosition[fIndex][1] != 0.0f ||
                              _cameraPosition[fIndex][2] != 0.0f);
    if (haveCameraVectors)
    {
        slConstants.cameraPos = sl::float3(
            _cameraPosition[fIndex][0], _cameraPosition[fIndex][1], _cameraPosition[fIndex][2]);
        slConstants.cameraUp = sl::float3(
            _cameraUp[fIndex][0], _cameraUp[fIndex][1], _cameraUp[fIndex][2]);
        slConstants.cameraRight = sl::float3(
            _cameraRight[fIndex][0], _cameraRight[fIndex][1], _cameraRight[fIndex][2]);
        slConstants.cameraFwd = sl::float3(
            _cameraForward[fIndex][0], _cameraForward[fIndex][1], _cameraForward[fIndex][2]);
    }
    else
    {
        // Leave at INVALID_FLOAT default — camera data unavailable
        static bool loggedOnce = false;
        if (!loggedOnce)
        {
            LOG_INFO("Camera vectors unavailable (Upscaler path) — using INVALID_FLOAT sentinels for DLSS-G");
            loggedOnce = true;
        }
    }

    // Reset flag — respect FGSkipReset config like FSRFG/XeFG do
    if (!Config::Instance()->FGSkipReset.value_or_default())
        slConstants.reset = _reset[fIndex] ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    else
        slConstants.reset = sl::Boolean::eFalse;

    // Camera cut / scene transition: invalidate previous projection matrices.
    // Stale prev matrices after a reset cause DLSS-G to compute wrong clipToPrevClip,
    // producing severe ghosting/warping artifacts on scene transitions and camera cuts.
    if (_reset[fIndex])
    {
        _hasPrevProjection[fIndex] = false;
        _hasPrevReflexCamera[fIndex] = false;
    }

    // Build projection and view matrices from camera data.
    // clipToPrevClip requires the FULL view*projection transform to capture camera
    // translation and rotation between frames, not just projection changes.
    // When camera vectors are unavailable (Upscaler path), falls back to projection-only.
    {
        float nearVal = _cameraNear[fIndex];
        float farVal = _cameraFar[fIndex];
        float vfov = _cameraVFov[fIndex];
        float aspect = _cameraAspectRatio[fIndex];
        bool isLH = _isLeftHanded[fIndex];

        if (nearVal > 0.f && !XMScalarNearEqual(vfov, 0.0f, 0.00001f) &&
            !XMScalarNearEqual(aspect, 0.0f, 0.00001f))
        {
            bool isInfinite = (farVal <= 0.0f || std::isinf(farVal) ||
                               XMScalarNearEqual(nearVal, farVal, 0.00001f));

            // Build perspective projection with proper infinite far plane and handedness support
            XMMATRIX proj;
            if (isInfinite)
            {
                // Construct proper infinite projection matrix instead of clamping far plane
                float f = 1.0f / tanf(vfov * 0.5f);
                proj = {};
                proj.r[0] = XMVectorSet(f / aspect, 0.0f, 0.0f, 0.0f);
                proj.r[1] = XMVectorSet(0.0f, f, 0.0f, 0.0f);
                float perspSign = isLH ? 1.0f : -1.0f;
                if (IsInvertedDepth())
                {
                    // Reversed-Z infinite: Z=1 at near, Z=0 at infinity
                    proj.r[2] = XMVectorSet(0.0f, 0.0f, 0.0f, perspSign);
                    proj.r[3] = XMVectorSet(0.0f, 0.0f, nearVal, 0.0f);
                }
                else
                {
                    // Standard infinite: Z=0 at near, Z=1 at infinity
                    proj.r[2] = XMVectorSet(0.0f, 0.0f, perspSign, perspSign);
                    proj.r[3] = XMVectorSet(0.0f, 0.0f, -nearVal, 0.0f);
                }
            }
            else if (IsInvertedDepth())
            {
                proj = isLH ? XMMatrixPerspectiveFovLH(vfov, aspect, farVal, nearVal)
                            : XMMatrixPerspectiveFovRH(vfov, aspect, farVal, nearVal);
            }
            else
            {
                proj = isLH ? XMMatrixPerspectiveFovLH(vfov, aspect, nearVal, farVal)
                            : XMMatrixPerspectiveFovRH(vfov, aspect, nearVal, farVal);
            }

            // Try to build view matrix from camera vectors when available
            bool haveView = false;
            XMMATRIX view = XMMatrixIdentity();

            if (_cameraPosition[fIndex][0] != 0.0f || _cameraPosition[fIndex][1] != 0.0f ||
                _cameraPosition[fIndex][2] != 0.0f)
            {
                XMVECTOR right = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(_cameraRight[fIndex]));
                XMVECTOR up = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(_cameraUp[fIndex]));
                XMVECTOR forward = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(_cameraForward[fIndex]));
                XMVECTOR pos = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(_cameraPosition[fIndex]));

                float tx = -XMVectorGetX(XMVector3Dot(pos, right));
                float ty = -XMVectorGetX(XMVector3Dot(pos, up));
                float tz = -XMVectorGetX(XMVector3Dot(pos, forward));

                // Build world-to-view matrix (transposed basis with translation)
                // Same construction as XeFG_Dx12.cpp
                view = { XMVectorSet(XMVectorGetX(right), XMVectorGetX(up), XMVectorGetX(forward), 0.0f),
                         XMVectorSet(XMVectorGetY(right), XMVectorGetY(up), XMVectorGetY(forward), 0.0f),
                         XMVectorSet(XMVectorGetZ(right), XMVectorGetZ(up), XMVectorGetZ(forward), 0.0f),
                         XMVectorSet(tx, ty, tz, 1.0f) };

                haveView = true;
            }

            // viewProj captures the complete world-to-clip transform
            XMMATRIX viewProj = haveView ? XMMatrixMultiply(view, proj) : proj;
            XMMATRIX viewProjInv = XMMatrixInverse(nullptr, viewProj);

            // cameraViewToClip = projection only (camera view space → clip space)
            // clipToCameraView = inverse projection only
            XMMATRIX clipToView = XMMatrixInverse(nullptr, proj);
            memcpy(&slConstants.cameraViewToClip, proj.r, sizeof(sl::float4x4));
            memcpy(&slConstants.clipToCameraView, clipToView.r, sizeof(sl::float4x4));

            // clipToPrevClip = currentViewProjInverse * prevViewProj
            // This captures both camera movement AND projection changes between frames.
            // Per NVIDIA guide: clipToPrevClip = clipToView * viewToViewPrev * viewToClipPrev
            int prevIndex = (fIndex + BUFFER_COUNT - 1) % BUFFER_COUNT;
            if (_hasPrevProjection[prevIndex])
            {
                XMMATRIX clipToPrevClip = XMMatrixMultiply(viewProjInv, _prevViewProj[prevIndex]);
                XMMATRIX prevClipToClip = XMMatrixMultiply(_prevViewProjInv[prevIndex], viewProj);
                memcpy(&slConstants.clipToPrevClip, clipToPrevClip.r, sizeof(sl::float4x4));
                memcpy(&slConstants.prevClipToClip, prevClipToClip.r, sizeof(sl::float4x4));
            }
            // else: First frame or after reset — leave clipToPrevClip/prevClipToClip at
            // INVALID_FLOAT defaults (from sl::Constants constructor). Identity was WRONG
            // because it tells DLSS-G "camera didn't move" which contradicts MVs that show motion.
            // INVALID_FLOAT signals "matrix unavailable" so DLSS-G falls back to MV-based estimation.

            // Override clipToPrevClip with game-provided NVNGX matrix when available.
            // This captures full camera-aware transforms, superior to the projection-only fallback.
            if (HasClipToPrevClipMatrix(fIndex))
            {
                const float* mat = GetClipToPrevClipMatrix(fIndex);
                XMMATRIX gameClipToPrevClip = XMMATRIX(mat);
                XMMATRIX gamePrevClipToClip = XMMatrixInverse(nullptr, gameClipToPrevClip);
                memcpy(&slConstants.clipToPrevClip, gameClipToPrevClip.r, sizeof(sl::float4x4));
                memcpy(&slConstants.prevClipToClip, gamePrevClipToClip.r, sizeof(sl::float4x4));

                static bool loggedOnce = false;
                if (!loggedOnce)
                {
                    LOG_INFO("Using game-provided ClipToPrevClipMatrix for DLSS-G");
                    loggedOnce = true;
                }
            }

            SubmitReflexCameraData(fIndex, view, proj, haveView);

            // Store current view*proj for next frame's clipToPrevClip computation
            _prevViewProj[fIndex] = viewProj;
            _prevViewProjInv[fIndex] = viewProjInv;
            _hasPrevProjection[fIndex] = true;
        }

        // No stereo offset — set pinhole to 0 to silence the SL warning
        slConstants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    }

    // Set constants via SL
    sl::ViewportHandle viewport(0);
    auto result = SLProxy::SetConstants()(slConstants, *_currentFrameToken, viewport);
    if (result != sl::Result::eOk)
    {
        LOG_ERROR("slSetConstants failed: {}", (int) result);
    }
}

bool DLSSG_Dx12::TagResources(int fIndex, uint64_t willDispatchFrame)
{
    if (_currentFrameToken == nullptr)
        return false;

    // Shared lock for reading _frameResources — SetResource() writes under unique_lock
    std::shared_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);

    sl::ViewportHandle viewport(0);
    std::vector<sl::ResourceTag> tags;

    // Persistent storage for sl::Resource objects — must outlive the slSetTagForFrame call.
    // ResourceTag stores a raw Resource* pointer, so locals going out of scope would dangle.
    std::vector<sl::Resource> slResources;
    std::vector<sl::Extent> extents;
    slResources.reserve(6);
    extents.reserve(6);

    ID3D12Resource* hudlessForSynthUi = nullptr;
    D3D12_RESOURCE_STATES hudlessForSynthUiState = D3D12_RESOURCE_STATE_COMMON;
    UINT synthUiWidth = 0;
    UINT synthUiHeight = 0;
    DXGI_FORMAT synthUiFormat = DXGI_FORMAT_UNKNOWN;
    bool hasExplicitUiColor = false;
    bool synthesizedUiTagged = false;
    const bool hudlessSubmitMode = Config::Instance()->FrameWarpDLSSGMode.value_or_default() == 4;

    // Depth
    if (_frameResources[fIndex].contains(FG_ResourceType::Depth))
    {
        auto& fRes = _frameResources[fIndex][FG_ResourceType::Depth];
        slResources.push_back(MakeSLResource(fRes.GetResource(), fRes.state));
        extents.push_back({ fRes.top, fRes.left, (uint32_t) fRes.width, fRes.height });
        tags.emplace_back(&slResources.back(), sl::kBufferTypeDepth,
                          (fRes.validity == FG_ResourceValidity::ValidNow) ? sl::eOnlyValidNow : sl::eValidUntilPresent,
                          &extents.back());

        if (State::Instance().activeFgOutput == FGOutput::DLSSG &&
            Config::Instance()->FrameWarpDLSSGMode.value_or_default() == 3)
        {
            auto depthResource = fRes.GetResource();
            auto depthDesc = depthResource != nullptr ? depthResource->GetDesc() : D3D12_RESOURCE_DESC {};
            FrameWarpRuntime::TrackDepthSource(
                depthResource,
                fRes.state,
                fRes.width != 0 ? static_cast<UINT>(fRes.width) : static_cast<UINT>(depthDesc.Width),
                fRes.height != 0 ? fRes.height : depthDesc.Height,
                depthDesc.Format,
                "dlssg-depth");
        }
    }

    // Motion Vectors
    if (_frameResources[fIndex].contains(FG_ResourceType::Velocity))
    {
        auto& fRes = _frameResources[fIndex][FG_ResourceType::Velocity];
        slResources.push_back(MakeSLResource(fRes.GetResource(), fRes.state));
        extents.push_back({ fRes.top, fRes.left, (uint32_t) fRes.width, fRes.height });
        tags.emplace_back(&slResources.back(), sl::kBufferTypeMotionVectors,
                          (fRes.validity == FG_ResourceValidity::ValidNow) ? sl::eOnlyValidNow : sl::eValidUntilPresent,
                          &extents.back());

        if (Config::Instance()->FrameWarpSensitivityAuditLog.value_or_default())
        {
            auto mvResource = fRes.GetResource();
            auto mvDesc = mvResource != nullptr ? mvResource->GetDesc() : D3D12_RESOURCE_DESC {};
            FrameWarpRuntime::TrackMotionVectorSource(
                mvResource,
                fRes.state,
                fRes.width != 0 ? static_cast<UINT>(fRes.width) : static_cast<UINT>(mvDesc.Width),
                fRes.height != 0 ? fRes.height : mvDesc.Height,
                mvDesc.Format,
                _mvScaleX[fIndex],
                _mvScaleY[fIndex],
                IsMVScalePreMultiplied(fIndex),
                "dlssg-motion");
        }
    }

    // HUDLess Color
    if (_frameResources[fIndex].contains(FG_ResourceType::HudlessColor))
    {
        auto& fRes = _frameResources[fIndex][FG_ResourceType::HudlessColor];
        hudlessForSynthUi = fRes.GetResource();
        hudlessForSynthUiState = fRes.state;
        synthUiWidth = static_cast<UINT>(fRes.width);
        synthUiHeight = fRes.height;
        synthUiFormat = hudlessForSynthUi != nullptr ? hudlessForSynthUi->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
        slResources.push_back(MakeSLResource(fRes.GetResource(), fRes.state));
        extents.push_back({ fRes.top, fRes.left, (uint32_t) fRes.width, fRes.height });
        tags.emplace_back(&slResources.back(), sl::kBufferTypeHUDLessColor,
                          (fRes.validity == FG_ResourceValidity::ValidNow) ? sl::eOnlyValidNow : sl::eValidUntilPresent,
                          &extents.back());
    }

    // UI Color
    if (!hudlessSubmitMode && _frameResources[fIndex].contains(FG_ResourceType::UIColor))
    {
        hasExplicitUiColor = true;
        auto& fRes = _frameResources[fIndex][FG_ResourceType::UIColor];
        slResources.push_back(MakeSLResource(fRes.GetResource(), fRes.state));
        extents.push_back({ fRes.top, fRes.left, (uint32_t) fRes.width, fRes.height });
        tags.emplace_back(&slResources.back(), sl::kBufferTypeUIColorAndAlpha,
                          (fRes.validity == FG_ResourceValidity::ValidNow) ? sl::eOnlyValidNow : sl::eValidUntilPresent,
                          &extents.back());
    }

    // Bidirectional Distortion Field
    if (_frameResources[fIndex].contains(FG_ResourceType::Distortion))
    {
        auto& fRes = _frameResources[fIndex][FG_ResourceType::Distortion];
        slResources.push_back(MakeSLResource(fRes.GetResource(), fRes.state));
        extents.push_back({ fRes.top, fRes.left, (uint32_t) fRes.width, fRes.height });
        tags.emplace_back(&slResources.back(), sl::kBufferTypeBidirectionalDistortionField,
                          (fRes.validity == FG_ResourceValidity::ValidNow) ? sl::eOnlyValidNow : sl::eValidUntilPresent,
                          &extents.back());
    }

    if (tags.empty())
    {
        LOG_WARN("No resources to tag");
        return false;
    }

    // Use the tag command list to submit tags
    auto& tagAlloc = _tagCommandAllocator[fIndex];
    auto& tagCmdList = _tagCommandList[fIndex];

    if (tagAlloc != nullptr && tagCmdList != nullptr)
    {
        // Wait for previous GPU work on this command allocator to complete
        auto& fence = _tagFence[fIndex];
        if (fence != nullptr && _tagFenceValue[fIndex] > 0)
        {
            if (fence->GetCompletedValue() < _tagFenceValue[fIndex])
            {
                fence->SetEventOnCompletion(_tagFenceValue[fIndex], _tagFenceEvent);
                WaitForSingleObject(_tagFenceEvent, 5000);
            }
        }

        tagAlloc->Reset();
        tagCmdList->Reset(tagAlloc, nullptr);

        if (!hudlessSubmitMode && !hasExplicitUiColor && hudlessForSynthUi != nullptr)
        {
            D3D12_RESOURCE_STATES synthUiState = D3D12_RESOURCE_STATE_COMMON;
            ID3D12Resource* synthUi = SynthesizeUiColorAlpha(
                fIndex,
                tagCmdList,
                hudlessForSynthUi,
                hudlessForSynthUiState,
                synthUiWidth,
                synthUiHeight,
                synthUiFormat,
                &synthUiState);
            if (synthUi != nullptr)
            {
                slResources.push_back(MakeSLResource(synthUi, synthUiState));
                extents.push_back({ 0, 0, synthUiWidth, synthUiHeight });
                tags.emplace_back(
                    &slResources.back(),
                    sl::kBufferTypeUIColorAndAlpha,
                    sl::eValidUntilPresent,
                    &extents.back());
                synthesizedUiTagged = true;
            }
        }

        auto result = SLProxy::SetTagForFrame()(*_currentFrameToken, viewport, tags.data(), (uint32_t) tags.size(),
                                                 (sl::CommandBuffer*) tagCmdList);

        tagCmdList->Close();
        _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &tagCmdList);

        // Signal fence after execution
        if (fence != nullptr)
        {
            _tagFenceValue[fIndex]++;
            _gameCommandQueue->Signal(fence, _tagFenceValue[fIndex]);
        }

        if (result != sl::Result::eOk)
        {
            LOG_ERROR("slSetTagForFrame failed: {}", (int) result);
            return false;
        }
    }
    else
    {
        // Fallback: tag without command list (resources must be eValidUntilPresent)
        auto result = SLProxy::SetTagForFrame()(*_currentFrameToken, viewport, tags.data(), (uint32_t) tags.size(),
                                                 nullptr);
        if (result != sl::Result::eOk)
        {
            LOG_ERROR("slSetTagForFrame (no cmdList) failed: {}", (int) result);
            return false;
        }
    }

    static uint64_t dlssgTagLogCount = 0;
    dlssgTagLogCount++;
    if (dlssgTagLogCount <= 40 || dlssgTagLogCount % 120 == 0 || synthesizedUiTagged)
    {
        LOG_INFO("DLSSG tagged {} resources for frame {} (Depth:{}, MV:{}, Hudless:{}, UI:{}, SynthUI:{}, Dist:{})",
                 tags.size(), (uint32_t) willDispatchFrame,
                 _frameResources[fIndex].contains(FG_ResourceType::Depth) ? 1 : 0,
                 _frameResources[fIndex].contains(FG_ResourceType::Velocity) ? 1 : 0,
                 _frameResources[fIndex].contains(FG_ResourceType::HudlessColor) ? 1 : 0,
                 hasExplicitUiColor ? 1 : 0,
                 synthesizedUiTagged ? 1 : 0,
                 _frameResources[fIndex].contains(FG_ResourceType::Distortion) ? 1 : 0);
    }
    return true;
}

// --- Frame Generation Dispatch ---

bool DLSSG_Dx12::Dispatch()
{
    LOG_FUNC();

    // DFG: measure pure render time (postSleep -> Dispatch) excluding VSync/Reflex sleep.
    // Check preconditions BEFORE GetDispatchIndex, which has the side effect
    // of advancing _lastDispatchedFrame. Without these guards, the frame counter
    // gets out of sync when DLSS-G isn't ready yet.
    if (!IsActive() || IsPaused())
        return false;

    if (!_slInitialized || !_dlssgFeatureReady || !_objectsCreated)
        return false;

    UINT64 willDispatchFrame = 0;
    auto fIndex = GetDispatchIndex(willDispatchFrame);
    if (fIndex < 0)
        return false;

    LOG_DEBUG("_frameCount: {}, willDispatchFrame: {}, fIndex: {}", _frameCount, willDispatchFrame, fIndex);

    // Check required resources
    if (!_resourceReady[fIndex].contains(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].contains(FG_ResourceType::Velocity) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Velocity))
    {
        LOG_WARN("Depth or Velocity is not ready, skipping");
        return false;
    }

    // Copy late-sent resources
    if (!_noHudless[fIndex])
    {
        auto res = &_frameResources[fIndex][FG_ResourceType::HudlessColor];
        if (res->validity != FG_ResourceValidity::ValidNow)
        {
            res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
            res->frameIndex = fIndex;
            SetResource(res);
        }
    }

    if (!_noUi[fIndex])
    {
        auto res = &_frameResources[fIndex][FG_ResourceType::UIColor];
        if (res->validity != FG_ResourceValidity::ValidNow)
        {
            res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
            res->frameIndex = fIndex;
            SetResource(res);
        }
    }

    if (!_noDistortionField[fIndex])
    {
        auto res = &_frameResources[fIndex][FG_ResourceType::Distortion];
        if (res->validity != FG_ResourceValidity::ValidNow)
        {
            res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
            res->frameIndex = fIndex;
            SetResource(res);
        }
    }

    // DLSS-G REQUIRES kBufferTypeHUDLessColor. If Hudfix didn't capture one,
    // use the current backbuffer as a fallback. At this point in the Present
    // chain the backbuffer has just been rendered to (RENDER_TARGET state) and
    // may contain UI elements. DLSS-G's eEnableFullscreenMenuDetection flag
    // provides its own HUD detection to mitigate UI artifacts.
    if (!_frameResources[fIndex].contains(FG_ResourceType::HudlessColor) ||
        _frameResources[fIndex][FG_ResourceType::HudlessColor].resource == nullptr)
    {
        if (_swapChain != nullptr)
        {
            IDXGISwapChain3* sc3 = nullptr;
            if (((IDXGISwapChain*) _swapChain)->QueryInterface(IID_PPV_ARGS(&sc3)) == S_OK)
            {
                UINT bbIndex = sc3->GetCurrentBackBufferIndex();
                ID3D12Resource* backBuffer = nullptr;
                if (sc3->GetBuffer(bbIndex, IID_PPV_ARGS(&backBuffer)) == S_OK && backBuffer != nullptr)
                {
                    auto desc = backBuffer->GetDesc();
                    auto& fRes = _frameResources[fIndex][FG_ResourceType::HudlessColor];
                    fRes.type = FG_ResourceType::HudlessColor;
                    fRes.resource = backBuffer;
                    // Backbuffer is in RENDER_TARGET state at this point — the game just
                    // finished rendering into it. Using PRESENT here was incorrect and
                    // could cause SL to read from a resource in the wrong state.
                    fRes.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    fRes.validity = FG_ResourceValidity::UntilPresent;
                    fRes.left = 0;
                    fRes.top = 0;
                    fRes.width = (LONG) desc.Width;
                    fRes.height = desc.Height;
                    fRes.cmdList = nullptr;
                    fRes.frameIndex = fIndex;
                    _noHudless[fIndex] = false;
                    backBuffer->Release();  // GetBuffer AddRef'd; resource stays alive via swapchain
                    static bool loggedBackbufferFallback = false;
                    if (!loggedBackbufferFallback)
                    {
                        LOG_WARN("Using backbuffer as HudlessColor fallback ({}x{}) — may contain UI",
                                 desc.Width, desc.Height);
                        loggedBackbufferFallback = true;
                    }
                }
                sc3->Release();
            }
        }
    }

    // Get new frame token — per the DLSS-G programming guide, this should be called
    // "at the start of each frame" (simulation start). In OptiScaler's architecture,
    // Dispatch() is called during FGPresent, which is the per-frame work before Present.
    sl::FrameToken* newToken = nullptr;
    auto tokenResult = SLProxy::GetNewFrameToken()(newToken, nullptr);
    _currentFrameToken = newToken;
    if (tokenResult != sl::Result::eOk || _currentFrameToken == nullptr)
    {
        LOG_ERROR("slGetNewFrameToken failed: {}", (int) tokenResult);
        return false;
    }

    // PCL marker: Simulation start
    // The DLSS-G programming guide requires these markers to be in sync with frame tokens.
    // "If you see 'common constants cannot be found for frame N', PCL markers are out of sync."
    if (SLProxy::PCLSetMarker() != nullptr)
    {
        SLProxy::PCLSetMarker()(sl::PCLMarker::eSimulationStart, *_currentFrameToken);
    }

    // Set SL constants for this frame — must be as early as possible after frame token
    SetSLConstants(fIndex);

    // PCL marker: Simulation end
    if (SLProxy::PCLSetMarker() != nullptr)
    {
        SLProxy::PCLSetMarker()(sl::PCLMarker::eSimulationEnd, *_currentFrameToken);
    }

    // PCL marker: Render submit start
    if (SLProxy::PCLSetMarker() != nullptr)
    {
        SLProxy::PCLSetMarker()(sl::PCLMarker::eRenderSubmitStart, *_currentFrameToken);
    }

    // Tag resources
    if (!TagResources(fIndex, willDispatchFrame))
    {
        LOG_ERROR("Failed to tag resources for DLSS-G");
        return false;
    }

    // PCL marker: Render submit end
    if (SLProxy::PCLSetMarker() != nullptr)
    {
        SLProxy::PCLSetMarker()(sl::PCLMarker::eRenderSubmitEnd, *_currentFrameToken);
    }

    // DLSS-G frame generation is triggered automatically by the SL interposer's
    // Present hook — there is no slEvaluateFeature callback for DLSS-G.
    // Our job in Dispatch is just to tag resources, set constants, and provide
    // PCL markers each frame. The actual interpolation happens during Present.
    //
    // CRITICAL: ePresentStart and ePresentEnd markers must be called around
    // the actual swapchain Present call in FG_Hooks.cpp, not here.

    _slFrameIndex++;
    LOG_DEBUG("DLSS-G dispatch OK, frame index: {}", _slFrameIndex);

    return true;
}

// --- Present ---

bool DLSSG_Dx12::PrepareHudlessBackbufferForPresent()
{
    auto invalidate = [](const char* reason) {
        FrameWarpRuntime::InvalidateDLSSGHudlessSubmit(reason);
        static uint64_t skipLogCount = 0;
        skipLogCount++;
        if (skipLogCount <= 40 || skipLogCount % 120 == 0)
        {
            LOG_INFO("VF2DLSSGHudlessSubmit frame={} prepared=false reason={}",
                State::Instance().frameCount,
                reason != nullptr ? reason : "skipped");
        }
        return false;
    };

    if (!_objectsCreated || !_slInitialized || !_dlssgFeatureReady || _swapChain == nullptr ||
        _gameCommandQueue == nullptr || _device == nullptr)
    {
        return invalidate("DLSSG not ready");
    }

    const auto policy = FrameWarpDLSSG::GetResourceWarpPolicy();
    if (!policy.strictHudlessUi)
    {
        return invalidate("HUD-less submit disabled");
    }

    const int fIndex = static_cast<int>(_lastDispatchedFrame % BUFFER_COUNT);
    Dx12Resource hudless {};
    {
        std::shared_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);
        auto it = _frameResources[fIndex].find(FG_ResourceType::HudlessColor);
        if (it == _frameResources[fIndex].end() || it->second.GetResource() == nullptr)
            return invalidate("no HUD-less resource");

        hudless = it->second;
    }

    ID3D12Resource* hudlessResource = hudless.GetResource();
    if (hudlessResource == nullptr)
        return invalidate("invalid HUD-less resource");

    IDXGISwapChain3* sc3 = nullptr;
    if (((IDXGISwapChain*) _swapChain)->QueryInterface(IID_PPV_ARGS(&sc3)) != S_OK || sc3 == nullptr)
        return invalidate("swapchain3 unavailable");

    ID3D12Resource* backBuffer = nullptr;
    const UINT backBufferIndex = sc3->GetCurrentBackBufferIndex();
    HRESULT hr = sc3->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer));
    sc3->Release();
    if (FAILED(hr) || backBuffer == nullptr)
        return invalidate("backbuffer unavailable");

    if (backBuffer == hudlessResource)
    {
        backBuffer->Release();
        return invalidate("HUD-less is backbuffer fallback");
    }

    auto backBufferDesc = backBuffer->GetDesc();
    auto hudlessDesc = hudlessResource->GetDesc();
    if (backBufferDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        hudlessDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        backBufferDesc.Width != hudlessDesc.Width ||
        backBufferDesc.Height != hudlessDesc.Height ||
        backBufferDesc.Format != hudlessDesc.Format ||
        backBufferDesc.SampleDesc.Count != 1 ||
        hudlessDesc.SampleDesc.Count != 1 ||
        backBufferDesc.MipLevels != 1 ||
        hudlessDesc.MipLevels != 1 ||
        backBufferDesc.DepthOrArraySize != 1 ||
        hudlessDesc.DepthOrArraySize != 1)
    {
        static uint64_t mismatchLogCount = 0;
        mismatchLogCount++;
        if (mismatchLogCount <= 20 || mismatchLogCount % 120 == 0)
        {
            LOG_INFO("VF2DLSSGHudlessSubmit frame={} prepared=false reason=mismatch backbuffer={}x{} fmt={} hudless={}x{} fmt={}",
                State::Instance().frameCount,
                backBufferDesc.Width,
                backBufferDesc.Height,
                static_cast<uint32_t>(backBufferDesc.Format),
                hudlessDesc.Width,
                hudlessDesc.Height,
                static_cast<uint32_t>(hudlessDesc.Format));
        }
        backBuffer->Release();
        FrameWarpRuntime::InvalidateDLSSGHudlessSubmit("HUD-less mismatch");
        return false;
    }

    if (!FrameWarpRuntime::EnsureInitialized(
            _device,
            static_cast<UINT>(backBufferDesc.Width),
            backBufferDesc.Height,
            backBufferDesc.Format))
    {
        backBuffer->Release();
        return invalidate("FrameWarp init failed");
    }

    ID3D12GraphicsCommandList* cmdList = GetUICommandList(fIndex);
    if (cmdList == nullptr)
    {
        backBuffer->Release();
        return invalidate("command list unavailable");
    }

    const D3D12_RESOURCE_STATES backBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (!FrameWarpRuntime::PrepareDLSSGHudlessSubmit(
            cmdList,
            hudlessResource,
            hudless.state,
            backBuffer,
            backBufferState))
    {
        _uiCommandList[fIndex]->Close();
        _uiCommandListResetted[fIndex] = false;
        backBuffer->Release();
        return invalidate("UI extraction failed");
    }

    // Keep the real swapchain backbuffer intact. Replacing it with the HUD-less
    // texture makes normal displayed frames lose UI, while DLSSG generated frames
    // get UI restored later, producing an alternating UI flicker.
    const bool replacedBackbuffer = false;

    hr = cmdList->Close();
    if (FAILED(hr))
    {
        _uiCommandListResetted[fIndex] = false;
        backBuffer->Release();
        return invalidate("command list close failed");
    }

    ID3D12CommandList* lists[] = { cmdList };
    _gameCommandQueue->ExecuteCommandLists(1, lists);
    SignalUIFence(fIndex);
    _uiCommandListResetted[fIndex] = false;

    static uint64_t submitLogCount = 0;
    submitLogCount++;
    if (submitLogCount <= 40 || submitLogCount % 120 == 0)
    {
        LOG_INFO("VF2DLSSGHudlessSubmit frame={} prepared=true replaced={} nonDestructive=true fIndex={} backbuffer={} hudless={:X} size={}x{} fmt={}",
            State::Instance().frameCount,
            replacedBackbuffer,
            fIndex,
            backBufferIndex,
            reinterpret_cast<uintptr_t>(hudlessResource),
            static_cast<UINT>(backBufferDesc.Width),
            backBufferDesc.Height,
            static_cast<uint32_t>(backBufferDesc.Format));
    }

    backBuffer->Release();
    return true;
}

bool DLSSG_Dx12::Present()
{
    auto fIndex = GetIndexWillBeDispatched();
    LOG_DEBUG("fIndex: {}", fIndex);

    // Early out if DLSS-G is not fully initialized yet
    // (objects may not be created until the upscaler starts providing data)
    if (!_objectsCreated || !_slInitialized || !_dlssgFeatureReady)
    {
        _fgFramePresentId++;
        return false;
    }

    if (IsActive() && !IsPaused() && State::Instance().FGHudlessCompare)
    {
        auto hudless = GetResource(FG_ResourceType::HudlessColor, fIndex);
        if (hudless != nullptr && (hudless->validity == FG_ResourceValidity::UntilPresent ||
                                   hudless->validity == FG_ResourceValidity::JustTrackCmdlist ||
                                   hudless->validity == FG_ResourceValidity::UntilPresentFromDispatch))
        {
            if (_hudlessCompare.get() == nullptr)
            {
                _hudlessCompare = std::make_unique<HC_Dx12>("HudlessCompare", _device);
            }
            else if (_hudlessCompare->IsInit())
            {
                auto commandList = GetUICommandList(fIndex);
                _hudlessCompare->Dispatch((IDXGISwapChain3*) _swapChain, commandList, hudless->GetResource(),
                                          hudless->state);
            }
        }
    }

    // Execute UI command list if pending (serves HudlessCompare above)
    {
        if (_uiCommandListResetted[fIndex])
        {
            auto closeResult = _uiCommandList[fIndex]->Close();
            if (closeResult == S_OK)
            {
                _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[fIndex]);
                SignalUIFence(fIndex);
            }
            else
                LOG_ERROR("_uiCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            _uiCommandListResetted[fIndex] = false;
        }
    }

    if ((_fgFramePresentId - _lastFGFramePresentId) > 3 && IsActive() && !_waitingNewFrameData)
    {
        LOG_DEBUG("Pausing FG");
        Deactivate();
        _waitingNewFrameData = true;
        return false;
    }

    _fgFramePresentId++;

    return Dispatch();
}

// --- Resource Management ---

bool DLSSG_Dx12::SetResource(Dx12Resource* inputResource)
{
    if (inputResource == nullptr || inputResource->resource == nullptr || !IsActive() || IsPaused())
        return false;

    auto fIndex = inputResource->frameIndex;
    if (fIndex < 0)
        fIndex = GetIndex();

    auto& type = inputResource->type;

    std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);

    if (type == FG_ResourceType::HudlessColor)
    {
        if (Config::Instance()->FGDisableHudless.value_or_default())
            return false;

        if (!_noHudless[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
            return false;

        if (!_noHudless[fIndex] && Config::Instance()->FGOnlyAcceptFirstHudless.value_or_default() &&
            inputResource->validity != FG_ResourceValidity::UntilPresentFromDispatch)
            return false;
    }

    if (type == FG_ResourceType::UIColor)
    {
        if (Config::Instance()->FGDisableUI.value_or_default())
            return false;

        if (!_noUi[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
            return false;
    }

    if (type == FG_ResourceType::Distortion)
    {
        if (!_noDistortionField[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
            return false;
    }

    // For Depth/Velocity, only skip if the existing resource is ValidNow (already fresh this frame).
    // Previously this returned false whenever the resource existed at all, which prevented fresh
    // data from being copied — causing stale depth/MV artifacts.
    if ((type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity) &&
        _frameResources[fIndex].contains(type) &&
        _frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow)
        return false;

    if (inputResource->cmdList == nullptr && inputResource->validity == FG_ResourceValidity::ValidNow)
    {
        LOG_ERROR("{}, validity == ValidNow but cmdList is nullptr!", magic_enum::enum_name(type));
        return false;
    }

    auto fResource = &_frameResources[fIndex][type];
    fResource->type = type;
    fResource->state = inputResource->state;
    fResource->validity = inputResource->validity;
    fResource->resource = inputResource->resource;
    fResource->top = inputResource->top;
    fResource->left = inputResource->left;
    fResource->width = inputResource->width;
    fResource->height = inputResource->height;
    fResource->cmdList = inputResource->cmdList;

    auto willFlip = State::Instance().activeFgInput == FGInput::Upscaler &&
                    Config::Instance()->FGResourceFlip.value_or_default() &&
                    (type == FG_ResourceType::Velocity || type == FG_ResourceType::Depth);

    if (willFlip && _device != nullptr)
        FlipResource(fResource);

    // Copy resources that need it
    if (inputResource->cmdList != nullptr && fResource->validity == FG_ResourceValidity::ValidButMakeCopy)
    {
        LOG_DEBUG("Making a resource copy of: {}", magic_enum::enum_name(type));

        ID3D12Resource* copyOutput = nullptr;
        if (_resourceCopy[fIndex].contains(type))
            copyOutput = _resourceCopy[fIndex][type];

        if (!CopyResource(inputResource->cmdList, inputResource->resource, &copyOutput, inputResource->state))
        {
            LOG_ERROR("{}, CopyResource error!", magic_enum::enum_name(type));
            return false;
        }

        _resourceCopy[fIndex][type] = copyOutput;
        _resourceCopy[fIndex][type]->SetName(std::format(L"_resourceCopy[{}][{}]", fIndex, (UINT) type).c_str());
        fResource->copy = copyOutput;
        fResource->state = D3D12_RESOURCE_STATE_COPY_DEST;
        fResource->validity = FG_ResourceValidity::UntilPresent;
    }

    if (type == FG_ResourceType::UIColor)
        _noUi[fIndex] = false;
    else if (type == FG_ResourceType::Distortion)
        _noDistortionField[fIndex] = false;
    else if (type == FG_ResourceType::HudlessColor)
        _noHudless[fIndex] = false;

    if ((type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity) ||
        (fResource->validity != FG_ResourceValidity::UntilPresent &&
         fResource->validity != FG_ResourceValidity::JustTrackCmdlist))
    {
        fResource->validity = (fResource->validity != FG_ResourceValidity::ValidNow || willFlip)
                                  ? FG_ResourceValidity::UntilPresent
                                  : FG_ResourceValidity::ValidNow;

        SetResourceReady(type, fIndex);
    }

    LOG_TRACE("_frameResources[{}][{}]: {:X}", fIndex, magic_enum::enum_name(type), (size_t) fResource->GetResource());

    return true;
}

void DLSSG_Dx12::SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) { _gameCommandQueue = queue; }

// --- Object Lifecycle ---

void DLSSG_Dx12::CreateObjects(ID3D12Device* InDevice)
{
    if (_uiCommandAllocator[0] != nullptr)
        return;

    LOG_DEBUG("");

    do
    {
        HRESULT result;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* cmdList = nullptr;

        for (size_t i = 0; i < BUFFER_COUNT; i++)
        {
            // UI command resources (same as XeFG/FSRFG pattern)
            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_uiCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocator _uiCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _uiCommandAllocator[i]->SetName(std::format(L"DLSSG_uiCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandAllocator[i], (IUnknown**) &allocator))
                _uiCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _uiCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_uiCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _uiCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _uiCommandList[i]->SetName(std::format(L"DLSSG_uiCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandList[i], (IUnknown**) &cmdList))
                _uiCommandList[i] = cmdList;

            result = _uiCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_uiCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            // Tag command resources for SL resource tagging
            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_tagCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocator _tagCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _tagCommandAllocator[i]->SetName(std::format(L"DLSSG_tagCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _tagCommandAllocator[i], (IUnknown**) &allocator))
                _tagCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _tagCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_tagCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _tagCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _tagCommandList[i]->SetName(std::format(L"DLSSG_tagCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _tagCommandList[i], (IUnknown**) &cmdList))
                _tagCommandList[i] = cmdList;

            result = _tagCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_tagCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            // Create fence for tag command list GPU synchronization
            result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_tagFence[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateFence _tagFence[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _tagFence[i]->SetName(std::format(L"DLSSG_tagFence[{}]", i).c_str());
            _tagFenceValue[i] = 0;

            if (!_uiExtractHeaps[i].Initialize(InDevice, 2, 1, 0))
            {
                LOG_ERROR("DLSSG UI extract descriptor heap init failed [{}]", i);
                break;
            }
        }

        EnsureUiExtractionPipeline(InDevice);

        _tagFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (_tagFenceEvent == nullptr)
        {
            LOG_ERROR("CreateEvent for tag fence failed");
        }

        // Create fences for UI command allocator synchronization
        if (!CreateUIFences())
        {
            LOG_ERROR("CreateUIFences failed");
        }
    } while (false);
}

void DLSSG_Dx12::ReleaseObjects()
{
    _objectsCreated = false;
    _mvFlip.reset();
    _depthFlip.reset();
    _hudlessCompare.reset();

    // Reset per-index projection tracking
    for (size_t i = 0; i < BUFFER_COUNT; i++)
        _hasPrevProjection[i] = false;

    // Release cached resource copies to avoid GPU memory leak
    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        for (auto& [type, resource] : _resourceCopy[i])
        {
            if (resource != nullptr)
                resource->Release();
        }
        _resourceCopy[i].clear();
    }

    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        if (_uiCommandList[i] != nullptr)
        {
            _uiCommandList[i]->Release();
            _uiCommandList[i] = nullptr;
        }
        if (_uiCommandAllocator[i] != nullptr)
        {
            _uiCommandAllocator[i]->Release();
            _uiCommandAllocator[i] = nullptr;
        }
        if (_tagCommandList[i] != nullptr)
        {
            _tagCommandList[i]->Release();
            _tagCommandList[i] = nullptr;
        }
        if (_tagCommandAllocator[i] != nullptr)
        {
            _tagCommandAllocator[i]->Release();
            _tagCommandAllocator[i] = nullptr;
        }
        if (_tagFence[i] != nullptr)
        {
            _tagFence[i]->Release();
            _tagFence[i] = nullptr;
        }
        if (_synthUiResource[i] != nullptr)
        {
            _synthUiResource[i]->Release();
            _synthUiResource[i] = nullptr;
        }
        _synthUiResourceState[i] = D3D12_RESOURCE_STATE_COMMON;
        _tagFenceValue[i] = 0;
    }

    if (_uiExtractPipelineState != nullptr)
    {
        _uiExtractPipelineState->Release();
        _uiExtractPipelineState = nullptr;
    }
    if (_uiExtractRootSignature != nullptr)
    {
        _uiExtractRootSignature->Release();
        _uiExtractRootSignature = nullptr;
    }

    if (_tagFenceEvent != nullptr)
    {
        CloseHandle(_tagFenceEvent);
        _tagFenceEvent = nullptr;
    }

    ReleaseUIFences();
}

void* DLSSG_Dx12::FrameGenerationContext() { return _slInitialized ? (void*) 1 : nullptr; }
void* DLSSG_Dx12::SwapchainContext() { return _slInitialized ? (void*) 1 : nullptr; }

bool DLSSG_Dx12::ReleaseSwapchain(HWND hwnd)
{
    if (hwnd != _hwnd || _hwnd == NULL)
        return false;

    LOG_DEBUG("");

    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        LOG_TRACE("Waiting Mutex 1, current: {}", Mutex.getOwner());
        Mutex.lock(1);
        LOG_TRACE("Acquired Mutex: {}", Mutex.getOwner());
    }

    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    DestroyFGContext();

    if (State::Instance().isShuttingDown)
        ShutdownStreamline();

    ReleaseObjects();

    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        LOG_TRACE("Releasing Mutex: {}", Mutex.getOwner());
        Mutex.unlockThis(1);
    }

    return true;
}

DLSSG_Dx12::~DLSSG_Dx12() { Shutdown(); }

void DLSSG_Dx12::SetPCLPresentStart()
{
    if (_currentFrameToken != nullptr && SLProxy::PCLSetMarker() != nullptr)
    {
        auto result = SLProxy::PCLSetMarker()(sl::PCLMarker::ePresentStart, *_currentFrameToken);
        LOG_DEBUG("PCL ePresentStart result: {}", (int) result);
    }
}

void DLSSG_Dx12::SetPCLPresentEnd()
{
    if (_currentFrameToken != nullptr && SLProxy::PCLSetMarker() != nullptr)
    {
        auto result = SLProxy::PCLSetMarker()(sl::PCLMarker::ePresentEnd, *_currentFrameToken);
        LOG_DEBUG("PCL ePresentEnd result: {}", (int) result);
    }
}

void DLSSG_Dx12::CallReflexSleep()
{
    if (_currentFrameToken != nullptr && SLProxy::ReflexSleep() != nullptr)
    {
        auto result = SLProxy::ReflexSleep()(*_currentFrameToken);
        LOG_DEBUG("slReflexSleep result: {}", (int) result);
        LogReflexState("sleep");
    }
}

void DLSSG_Dx12::UpdateReflexFrameLimit(float fpsCap)
{
    if (!_slInitialized || SLProxy::ReflexSetOptions() == nullptr)
        return;

    sl::ReflexOptions reflexOptions {};
    reflexOptions.mode = sl::ReflexMode::eLowLatency;
    reflexOptions.useMarkersToOptimize = false;
    reflexOptions.virtualKey = 0;
    reflexOptions.idThread = 0;
    reflexOptions.frameLimitUs =
        (fpsCap > 0.0f) ? static_cast<uint32_t>(1'000'000.0f / fpsCap) : 0;

    auto result = SLProxy::ReflexSetOptions()(reflexOptions);
    LOG_INFO("Updated SL Reflex frameLimitUs to {} ({}fps), result: {}", reflexOptions.frameLimitUs, fpsCap, (int) result);
}

bool DLSSG_Dx12::SetInterpolatedFrameCount(UINT interpolatedFrameCount)
{
    if (interpolatedFrameCount < 1)
        interpolatedFrameCount = 1;
    if (interpolatedFrameCount > _numFramesToGenerateMax)
        interpolatedFrameCount = _numFramesToGenerateMax;

    _numFramesToGenerate = interpolatedFrameCount;
    _framesToInterpolate = interpolatedFrameCount;
    _nameStale = true;

    if (_slInitialized && _dlssgFeatureReady && _isActive)
    {
        sl::DLSSGOptions options {};
        options.mode = sl::DLSSGMode::eOn;
        options.numFramesToGenerate = _numFramesToGenerate;
        options.flags = sl::DLSSGFlags::eEnableFullscreenMenuDetection;

        sl::ViewportHandle viewport(0);
        auto result = SLProxy::DLSSGSetOptions()(viewport, options);
        if (result != sl::Result::eOk)
        {
            LOG_ERROR("SetInterpolatedFrameCount: slDLSSGSetOptions failed: {}", (int) result);
            return false;
        }

        LOG_INFO("DLSS-G frame count updated to {}", _numFramesToGenerate);
    }

    return true;
}



// Late Warp PCL markers (Reflex 2 / Frame Warp integration)
void DLSSG_Dx12::SetPCLLateWarpPresentStart()
{
    if (_currentFrameToken != nullptr && SLProxy::PCLSetMarker() != nullptr)
    {
        auto result = SLProxy::PCLSetMarker()(sl::PCLMarker::eLateWarpPresentStart, *_currentFrameToken);
        LOG_DEBUG("PCL eLateWarpPresentStart result: {}", (int) result);
    }
}

void DLSSG_Dx12::SetPCLLateWarpPresentEnd()
{
    if (_currentFrameToken != nullptr && SLProxy::PCLSetMarker() != nullptr)
    {
        auto result = SLProxy::PCLSetMarker()(sl::PCLMarker::eLateWarpPresentEnd, *_currentFrameToken);
        LOG_DEBUG("PCL eLateWarpPresentEnd result: {}", (int) result);
    }
}

void DLSSG_Dx12::SetPCLCameraConstructed()
{
    if (_currentFrameToken != nullptr && SLProxy::PCLSetMarker() != nullptr)
    {
        auto result = SLProxy::PCLSetMarker()(sl::PCLMarker::eCameraConstructed, *_currentFrameToken);
        LOG_DEBUG("PCL eCameraConstructed result: {}", (int) result);
    }
}
