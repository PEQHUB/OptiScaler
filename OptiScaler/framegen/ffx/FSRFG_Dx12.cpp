#include "pch.h"

#include "FSRFG_Dx12.h"
#include <State.h>

#include <framewarp/FrameWarp.h>
#include <hudfix/Hudfix_Dx12.h>
#include <menu/menu_overlay_dx.h>

#include <magic_enum.hpp>

#define FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION_MAJOR 3
#define FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION_MINOR 1
#define FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION_PATCH 6

#define FFX_FRAMEGENERATION_SWAPCHAIN_DX12_MAKE_VERSION(major, minor, patch)                                           \
    (((major) << 22) | ((minor) << 12) | (patch))

#define FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION                                                                     \
    FFX_FRAMEGENERATION_SWAPCHAIN_DX12_MAKE_VERSION(FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION_MAJOR,                  \
                                                    FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION_MINOR,                  \
                                                    FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION_PATCH)

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12 0x3000bu
struct ffxCreateContextDescFrameGenerationSwapChainVersionDX12
{
    ffxCreateContextDescHeader header; ///< Description header for frame generation swapchain version context creation.
    uint32_t version;                  ///< The API version the application was built against. This must be set to
                                       ///< FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION.
};

static D3D12_RESOURCE_STATES GetD3D12State(FfxApiResourceState state)
{
    switch (state)
    {
    case FFX_API_RESOURCE_STATE_COMMON:
        return D3D12_RESOURCE_STATE_COMMON;
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_READ:
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        return (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    case FFX_API_RESOURCE_STATE_COPY_SRC:
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_API_RESOURCE_STATE_COPY_DEST:
        return D3D12_RESOURCE_STATE_COPY_DEST;
    case FFX_API_RESOURCE_STATE_GENERIC_READ:
        return D3D12_RESOURCE_STATE_GENERIC_READ;
    case FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT:
        return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET:
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case FFX_API_RESOURCE_STATE_PRESENT:
        return D3D12_RESOURCE_STATE_PRESENT;
    default:
        return D3D12_RESOURCE_STATE_COMMON;
    }
}

static void FSRFGFrameWarpTransitionResource(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    if (cmdList == nullptr || resource == nullptr || before == after)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    cmdList->ResourceBarrier(1, &barrier);
}

static bool FSRFGFrameWarpCopyResource(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* destination,
    D3D12_RESOURCE_STATES destinationState,
    ID3D12Resource* source,
    D3D12_RESOURCE_STATES sourceState)
{
    if (cmdList == nullptr || destination == nullptr || source == nullptr)
        return false;

    if (destination == source)
        return true;

    FSRFGFrameWarpTransitionResource(cmdList, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    FSRFGFrameWarpTransitionResource(cmdList, destination, destinationState, D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyResource(destination, source);

    FSRFGFrameWarpTransitionResource(cmdList, destination, D3D12_RESOURCE_STATE_COPY_DEST, destinationState);
    FSRFGFrameWarpTransitionResource(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
    return true;
}

enum class FrameWarpFGDiagnosticMode : uint32_t
{
    Normal = 0,
    CopyBoth = 1,
    WarpGeneratedOnly = 2,
    WarpRealOnly = 3,
    WarpBothSameDelta = 4,
    AutoCycle = 5,
};

static const char* FrameWarpFGDiagnosticModeName(uint32_t mode)
{
    switch (static_cast<FrameWarpFGDiagnosticMode>(mode))
    {
    case FrameWarpFGDiagnosticMode::Normal: return "normal";
    case FrameWarpFGDiagnosticMode::CopyBoth: return "copy-both";
    case FrameWarpFGDiagnosticMode::WarpGeneratedOnly: return "warp-all-latched";
    case FrameWarpFGDiagnosticMode::WarpRealOnly: return "warp-all-latched";
    case FrameWarpFGDiagnosticMode::WarpBothSameDelta: return "warp-all-latched";
    case FrameWarpFGDiagnosticMode::AutoCycle: return "warp-all-latched";
    default: return "unknown";
    }
}

static uint32_t FrameWarpResolveFGDiagnosticMode(UINT64 callbackCounter)
{
    uint32_t configuredMode = Config::Instance()->FrameWarpFGDiagnosticMode.value_or_default();
    if (configuredMode == static_cast<uint32_t>(FrameWarpFGDiagnosticMode::CopyBoth))
        return configuredMode;

    (void)callbackCounter;
    return static_cast<uint32_t>(FrameWarpFGDiagnosticMode::WarpBothSameDelta);
}

inline static int GetFormatGroup(DXGI_FORMAT format)
{
    switch (format)
    {

    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 1;

    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 2;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
        return 3;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return 4;

    case DXGI_FORMAT_R11G11B10_FLOAT:
        return 5;

    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
        return 6;

    case DXGI_FORMAT_B5G6R5_UNORM:
        return 7;

    case DXGI_FORMAT_B5G5R5A1_UNORM:
        return 8;

    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return 9;

    case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
        return 10;

    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return 11;

    default:
        return -1;
    }
}

inline static bool CompareResourceFormats(DXGI_FORMAT sc, DXGI_FORMAT hudless)
{
    if (sc == hudless)
        return true;

    auto scGroup = GetFormatGroup(sc);
    auto hudlessGroup = GetFormatGroup(hudless);
    return scGroup == hudlessGroup;
}

static inline void ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                                   D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState)
{
    if (InBeforeState == InAfterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = InResource;
    barrier.Transition.StateBefore = InBeforeState;
    barrier.Transition.StateAfter = InAfterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    InCommandList->ResourceBarrier(1, &barrier);
}

bool FSRFG_Dx12::HudlessFormatTransfer(int index, ID3D12Device* device, DXGI_FORMAT targetFormat,
                                       Dx12Resource* resource)
{
    if (_hudlessTransfer[index].get() == nullptr || !_hudlessTransfer[index].get()->IsFormatCompatible(targetFormat))
    {
        LOG_DEBUG("Format change, recreate the FormatTransfer");

        if (_hudlessTransfer[index].get() != nullptr)
            _hudlessTransfer[index].reset();

        _hudlessTransfer[index] = std::make_unique<FT_Dx12>("FormatTransfer", device, targetFormat);

        return false;
    }

    if (_hudlessTransfer[index].get() != nullptr &&
        _hudlessTransfer[index].get()->CreateBufferResource(device, resource->GetResource(),
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
        (resource->cmdList == nullptr ||
         CreateBufferResource(device, resource->GetResource(), D3D12_RESOURCE_STATE_COPY_DEST,
                              &_hudlessCopyResource[index])))
    {
        auto cmdList = GetUICommandList(index);

        if (resource->cmdList != nullptr && _hudlessCopyResource[index] != nullptr)
        {
            ResourceBarrier(resource->cmdList, resource->GetResource(), resource->state,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);

            resource->cmdList->CopyResource(_hudlessCopyResource[index], resource->GetResource());

            ResourceBarrier(resource->cmdList, resource->GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                            resource->state);

            ResourceBarrier(resource->cmdList, _hudlessCopyResource[index], D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            _hudlessTransfer[index].get()->Dispatch(device, cmdList, _hudlessCopyResource[index],
                                                    _hudlessTransfer[index].get()->Buffer());

            ResourceBarrier(cmdList, _hudlessCopyResource[index], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_DEST);
        }
        else
        {
            ResourceBarrier(cmdList, resource->GetResource(), resource->state,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            _hudlessTransfer[index].get()->Dispatch(device, cmdList, resource->GetResource(),
                                                    _hudlessTransfer[index].get()->Buffer());

            ResourceBarrier(cmdList, resource->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            resource->state);
        }

        resource->copy = _hudlessTransfer[index].get()->Buffer();
        resource->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        return true;
    }

    return false;
}

bool FSRFG_Dx12::UIFormatTransfer(int index, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                                  DXGI_FORMAT targetFormat, Dx12Resource* resource)
{
    if (_uiTransfer[index].get() == nullptr || !_uiTransfer[index].get()->IsFormatCompatible(targetFormat))
    {
        LOG_DEBUG("Format change, recreate the FormatTransfer");

        if (_uiTransfer[index].get() != nullptr)
            _uiTransfer[index].reset();

        _uiTransfer[index] = std::make_unique<FT_Dx12>("FormatTransfer", device, targetFormat);

        return false;
    }

    if (_uiTransfer[index].get() != nullptr &&
        _uiTransfer[index].get()->CreateBufferResource(device, resource->GetResource(),
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        ResourceBarrier(cmdList, resource->GetResource(), resource->state,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        _uiTransfer[index].get()->Dispatch(device, cmdList, resource->GetResource(),
                                           _uiTransfer[index].get()->Buffer());

        ResourceBarrier(cmdList, resource->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        resource->state);

        resource->copy = _uiTransfer[index].get()->Buffer();
        return true;
    }

    return false;
}

typedef struct FfxSwapchainFramePacingTuning
{
    float safetyMarginInMs;  // in Millisecond. Default is 0.1ms
    float varianceFactor;    // valid range [0.0,1.0]. Default is 0.1
    bool allowHybridSpin;    // Allows pacing spinlock to sleep. Default is false.
    uint32_t hybridSpinTime; // How long to spin if allowHybridSpin is true. Measured in timer resolution units. Not
                             // recommended to go below 2. Will result in frequent overshoots. Default is 2.
    bool allowWaitForSingleObjectOnFence; // Allows WaitForSingleObject instead of spinning for fence value. Default is
                                          // false.
} FfxSwapchainFramePacingTuning;

void FSRFG_Dx12::ConfigureFramePaceTuning()
{
    State::Instance().FSRFGFTPchanged = false;

    if (_swapChainContext == nullptr || Version() < feature_version { 3, 1, 3 })
        return;

    FfxSwapchainFramePacingTuning fpt {};
    if (Config::Instance()->FGFramePacingTuning.value_or_default())
    {
        fpt.allowHybridSpin = Config::Instance()->FGFPTAllowHybridSpin.value_or_default();
        fpt.allowWaitForSingleObjectOnFence =
            Config::Instance()->FGFPTAllowWaitForSingleObjectOnFence.value_or_default();
        fpt.hybridSpinTime = Config::Instance()->FGFPTHybridSpinTime.value_or_default();
        fpt.safetyMarginInMs = Config::Instance()->FGFPTSafetyMarginInMs.value_or_default();
        fpt.varianceFactor = Config::Instance()->FGFPTVarianceFactor.value_or_default();

        ffxConfigureDescFrameGenerationSwapChainKeyValueDX12 cfgDesc {};
        cfgDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12;
        cfgDesc.key = 2; // FfxSwapchainFramePacingTuning
        cfgDesc.ptr = &fpt;

        auto result = FfxApiProxy::D3D12_Configure(&_swapChainContext, &cfgDesc.header);
        LOG_DEBUG("HybridSpin D3D12_Configure result: {}", FfxApiProxy::ReturnCodeToString(result));
    }
}

feature_version FSRFG_Dx12::Version()
{

    if (_fgContext == nullptr && _version.major == 0)
    {
        if (!FfxApiProxy::IsFGReady())
            FfxApiProxy::InitFfxDx12();

        if (FfxApiProxy::IsFGReady())
            _version = FfxApiProxy::VersionDx12_FG();
    }

    return _version;
}

HWND FSRFG_Dx12::Hwnd() { return _hwnd; }

const char* FSRFG_Dx12::Name() { return "FSR-FG"; }

static void fgLogCallback(uint32_t type, const wchar_t* message)
{
    auto message_str = wstring_to_string(std::wstring(message));

    if (type == FFX_API_MESSAGE_TYPE_ERROR)
        spdlog::error("FFX FG Callback: {}", message_str);
    else if (type == FFX_API_MESSAGE_TYPE_WARNING)
        spdlog::warn("FFX FG Callback: {}", message_str);
}

bool FSRFG_Dx12::Dispatch()
{
    LOG_FUNC();

    auto& state = State::Instance();
    auto config = Config::Instance();
    const bool frameWarpFsrfgRequested = FrameWarpRuntime::IsFsrfgPresentCallbackRequested();

    auto reportFrameWarpSkip = [&](const char* reason)
    {
        if (frameWarpFsrfgRequested)
            FrameWarpRuntime::ReportFsrfgDispatchSkipped(reason);
        LOG_DEBUG("FSRFG dispatch skipped: {}", reason != nullptr ? reason : "unknown");
    };

    if (_fgContext == nullptr)
    {
        reportFrameWarpSkip("no fg context");
        return false;
    }

    if (!IsActive())
    {
        reportFrameWarpSkip("inactive");
        return false;
    }

    if (IsPaused())
    {
        reportFrameWarpSkip("paused");
        return false;
    }

    UINT64 willDispatchFrame = 0;
    auto fIndex = GetDispatchIndex(willDispatchFrame, false);
    if (fIndex < 0)
    {
        reportFrameWarpSkip("already dispatched");
        return false;
    }

    if (state.FSRFGFTPchanged)
        ConfigureFramePaceTuning();

    LOG_DEBUG("_frameCount: {}, willDispatchFrame: {}, fIndex: {}", _frameCount, willDispatchFrame, fIndex);

    if (!_resourceReady[fIndex].contains(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].contains(FG_ResourceType::Velocity) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Velocity))
    {
        LOG_WARN("Depth or Velocity is not ready, skipping");
        if (!_resourceReady[fIndex].contains(FG_ResourceType::Depth) ||
            !_resourceReady[fIndex].at(FG_ResourceType::Depth))
            reportFrameWarpSkip("missing depth");
        else
            reportFrameWarpSkip("missing velocity");
        return false;
    }

    UINT64 committedDispatchFrame = 0;
    auto committedIndex = GetDispatchIndex(committedDispatchFrame, true);
    if (committedIndex < 0)
    {
        reportFrameWarpSkip("commit failed");
        return false;
    }

    willDispatchFrame = committedDispatchFrame;
    fIndex = committedIndex;

    const bool useFrameWarpPresentCallback =
        frameWarpFsrfgRequested;

    if (useFrameWarpPresentCallback)
    {
        static uint64_t fsrfgPresentPathLogCount = 0;
        fsrfgPresentPathLogCount++;
        if (fsrfgPresentPathLogCount <= 20 || fsrfgPresentPathLogCount % 300 == 0)
        {
            LOG_DEBUG("FrameWarp: FSRFG present callback path active; dispatch inputs left unwarped index={}", fIndex);
        }
    }

    ffxConfigureDescFrameGeneration fgConfig = {};
    fgConfig.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;

    ffxConfigureDescFrameGenerationRegisterDistortionFieldResource distortionFieldDesc {};
    distortionFieldDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE;

    auto distortion = GetResource(FG_ResourceType::Distortion, fIndex);
    if (distortion != nullptr && IsResourceReady(FG_ResourceType::Distortion, fIndex))
    {
        LOG_TRACE("Using Distortion Field: {:X}", (size_t) distortion->GetResource());

        distortionFieldDesc.distortionField =
            ffxApiGetResourceDX12(distortion->GetResource(), GetFfxApiState(distortion->state));

        distortionFieldDesc.header.pNext = fgConfig.header.pNext;
        fgConfig.header.pNext = &distortionFieldDesc.header;
    }

    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiDesc {};
    uiDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    uiDesc.uiResource = FfxApiResource({});

    auto hudless = GetResource(FG_ResourceType::HudlessColor, fIndex);

    if (hudless != nullptr && IsResourceReady(FG_ResourceType::HudlessColor, fIndex))
    {
        LOG_TRACE("Using hudless: {:X}", (size_t) hudless->GetResource());

        fgConfig.HUDLessColor = ffxApiGetResourceDX12(hudless->GetResource(), GetFfxApiState(hudless->state));

        // Reset of _paramHudless[fIndex] happens in DispatchCallback
        // as we might use it in Preset to remove hud from swapchain
    }
    else
    {
        fgConfig.HUDLessColor = FfxApiResource({});
    }

    _frameWarpContexts[fIndex] = {};
    if (useFrameWarpPresentCallback)
    {
        auto depthContextResource = GetResource(FG_ResourceType::Depth, fIndex);
        if (depthContextResource != nullptr && IsResourceReady(FG_ResourceType::Depth, fIndex))
        {
            ID3D12Resource* depthResource = depthContextResource->GetResource();
            ID3D12Resource* hudlessResource =
                (hudless != nullptr && IsResourceReady(FG_ResourceType::HudlessColor, fIndex))
                    ? hudless->GetResource()
                    : nullptr;

            if (depthResource != nullptr)
            {
                auto depthDesc = depthResource->GetDesc();
                _frameWarpContexts[fIndex].valid = true;
                _frameWarpContexts[fIndex].frameID = willDispatchFrame;
                _frameWarpContexts[fIndex].depth = depthResource;
                _frameWarpContexts[fIndex].depthState = depthContextResource->state;
                _frameWarpContexts[fIndex].hudless = hudlessResource;
                _frameWarpContexts[fIndex].hudlessState =
                    (hudlessResource != nullptr) ? hudless->state : D3D12_RESOURCE_STATE_COMMON;
                _frameWarpContexts[fIndex].width =
                    static_cast<UINT>(_interpolationWidth[fIndex] != 0 ? _interpolationWidth[fIndex] : depthDesc.Width);
                _frameWarpContexts[fIndex].height =
                    _interpolationHeight[fIndex] != 0 ? _interpolationHeight[fIndex] : depthDesc.Height;
                _frameWarpContexts[fIndex].format =
                    (hudlessResource != nullptr) ? hudlessResource->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
                _frameWarpContexts[fIndex].vFovRadians = _cameraVFov[fIndex];
                _frameWarpContexts[fIndex].aspectRatio = _cameraAspectRatio[fIndex];
                _frameWarpContexts[fIndex].cameraNear = _cameraNear[fIndex];
                _frameWarpContexts[fIndex].cameraFar = _cameraFar[fIndex];
                _frameWarpContexts[fIndex].invertedDepth = IsInvertedDepth();
                _frameWarpContexts[fIndex].infiniteDepth = IsInfiniteDepth();

                if (auto frameWarp = FrameWarpRuntime::Get();
                    frameWarp != nullptr && _cameraVFov[fIndex] > 0.0f && _cameraAspectRatio[fIndex] > 0.0f)
                {
                    frameWarp->SetCameraContext(_cameraVFov[fIndex], _cameraAspectRatio[fIndex], "fsrfg");
                }
            }
        }
    }

    FfxApiProxy::D3D12_Configure(&_swapChainContext, &uiDesc.header);

    if (fgConfig.HUDLessColor.resource != nullptr)
    {
        static auto localLastHudlessFormat = (FfxApiSurfaceFormat) fgConfig.HUDLessColor.description.format;
        _lastHudlessFormat = (FfxApiSurfaceFormat) fgConfig.HUDLessColor.description.format;

        if (localLastHudlessFormat != _lastHudlessFormat)
        {
            state.FGchanged = true;
            state.SCchanged = true;
            LOG_DEBUG("HUDLESS format changed, triggering FG reinit");
        }

        localLastHudlessFormat = _lastHudlessFormat;
    }

    fgConfig.frameGenerationEnabled = _isActive;
    fgConfig.flags = 0;

    if (config->FGDebugView.value_or_default())
        fgConfig.flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW;

    if (config->FGDebugTearLines.value_or_default())
        fgConfig.flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES;

    if (config->FGDebugResetLines.value_or_default())
        fgConfig.flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS;

    if (config->FGDebugPacingLines.value_or_default())
        fgConfig.flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES;

    fgConfig.allowAsyncWorkloads = config->FGAsync.value_or_default();

    // if (state.activeFgInput != FGInput::Upscaler)
    {
        // use swapchain buffer info
        DXGI_SWAP_CHAIN_DESC scDesc1 {};
        bool hasSwapChainDesc = _swapChain->GetDesc(&scDesc1) == S_OK;

        int bufferWidth = hasSwapChainDesc ? scDesc1.BufferDesc.Width : 0;
        int bufferHeight = hasSwapChainDesc ? scDesc1.BufferDesc.Height : 0;

        int defaultLeft = 0;
        int defaultTop = 0;
        int defaultWidth = 0;
        int defaultHeight = 0;

        defaultLeft = static_cast<int>(hasSwapChainDesc ? (bufferWidth - _interpolationWidth[fIndex]) / 2 : 0);
        defaultTop = hasSwapChainDesc ? (bufferHeight - _interpolationHeight[fIndex]) / 2 : 0;
        defaultWidth = static_cast<int>(_interpolationWidth[fIndex]);
        defaultHeight = _interpolationHeight[fIndex];

        fgConfig.generationRect.left = config->FGRectLeft.value_or(_interpolationLeft[fIndex].value_or(defaultLeft));
        fgConfig.generationRect.top = config->FGRectTop.value_or(_interpolationTop[fIndex].value_or(defaultTop));
        fgConfig.generationRect.width = config->FGRectWidth.value_or(defaultWidth);
        fgConfig.generationRect.height = config->FGRectHeight.value_or(defaultHeight);
    }

    fgConfig.frameGenerationCallbackUserContext = this;
    fgConfig.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params, void* pUserCtx) -> ffxReturnCode_t
    {
        FSRFG_Dx12* fsrFG = nullptr;

        if (pUserCtx != nullptr)
            fsrFG = reinterpret_cast<FSRFG_Dx12*>(pUserCtx);

        if (fsrFG != nullptr)
            return fsrFG->DispatchCallback(params);

        return FFX_API_RETURN_ERROR;
    };

    if (useFrameWarpPresentCallback)
    {
        fgConfig.presentCallbackUserContext = this;
        fgConfig.presentCallback = [](ffxCallbackDescFrameGenerationPresent* params, void* pUserCtx) -> ffxReturnCode_t
        {
            FSRFG_Dx12* fsrFG = nullptr;

            if (pUserCtx != nullptr)
                fsrFG = reinterpret_cast<FSRFG_Dx12*>(pUserCtx);

            if (fsrFG != nullptr)
                return fsrFG->PresentCallback(params);

            return FFX_API_RETURN_ERROR;
        };
    }
    else
    {
        fgConfig.presentCallbackUserContext = nullptr;
        fgConfig.presentCallback = nullptr;
    }

    fgConfig.onlyPresentGenerated = state.FGonlyGenerated;
    fgConfig.frameID = willDispatchFrame;
    fgConfig.swapChain = _swapChain;

    ffxReturnCode_t retCode = FfxApiProxy::D3D12_Configure(&_fgContext, &fgConfig.header);
    LOG_DEBUG("D3D12_Configure result: {0:X}, frame: {1}, fIndex: {2}", retCode, willDispatchFrame, fIndex);
    if (useFrameWarpPresentCallback)
    {
        if (retCode == FFX_API_RETURN_OK)
            FrameWarpRuntime::ReportFsrfgPresentCallbackConfigured(true, "present callback configured");
        else
            FrameWarpRuntime::ReportFsrfgDispatchSkipped("configure failed");
    }

    ffxConfigureDescGlobalDebug1 fgLogging = {};
    fgLogging.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
    fgLogging.fpMessage = &fgLogCallback;
    fgLogging.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
    ffxReturnCode_t loggingRetCode = FfxApiProxy::D3D12_Configure(&_fgContext, &fgLogging.header);

    bool dispatchResult = false;
    if (retCode == FFX_API_RETURN_OK && _isActive)
    {
        ffxCreateBackendDX12Desc backendDesc {};
        backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backendDesc.device = _device;

        ffxDispatchDescFrameGenerationPrepareCameraInfo dfgCameraData {};
        dfgCameraData.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
        dfgCameraData.header.pNext = &backendDesc.header;

        std::memcpy(dfgCameraData.cameraPosition, _cameraPosition[fIndex], 3 * sizeof(float));
        std::memcpy(dfgCameraData.cameraUp, _cameraUp[fIndex], 3 * sizeof(float));
        std::memcpy(dfgCameraData.cameraRight, _cameraRight[fIndex], 3 * sizeof(float));
        std::memcpy(dfgCameraData.cameraForward, _cameraForward[fIndex], 3 * sizeof(float));

        ffxDispatchDescFrameGenerationPrepare dfgPrepare {};
        dfgPrepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        dfgPrepare.header.pNext = &dfgCameraData.header;

        // Prepare command list
        auto allocator = _fgCommandAllocator[fIndex];
        auto result = allocator->Reset();
        if (result != S_OK)
        {
            LOG_ERROR("allocator->Reset() error: {:X}", (UINT) result);
            return false;
        }

        result = _fgCommandList[fIndex]->Reset(allocator, nullptr);
        if (result != S_OK)
        {
            LOG_ERROR("_hudlessCommandList[fIndex]->Reset error: {:X}", (UINT) result);
            return false;
        }

        dfgPrepare.commandList = _fgCommandList[fIndex];
        dfgPrepare.frameID = willDispatchFrame;
        dfgPrepare.flags = fgConfig.flags;

        auto velocity = GetResource(FG_ResourceType::Velocity, fIndex);
        auto depth = GetResource(FG_ResourceType::Depth, fIndex);

        if (velocity != nullptr && IsResourceReady(FG_ResourceType::Velocity, fIndex))
        {
            LOG_DEBUG("Velocity resource: {:X}", (size_t) velocity->GetResource());
            dfgPrepare.motionVectors = ffxApiGetResourceDX12(velocity->GetResource(), GetFfxApiState(velocity->state));
        }
        else
        {
            LOG_ERROR("Velocity is missing");
            _fgCommandList[fIndex]->Close();
            return false;
        }

        if (depth != nullptr && IsResourceReady(FG_ResourceType::Depth, fIndex))
        {
            LOG_DEBUG("Depth resource: {:X}", (size_t) depth->GetResource());
            dfgPrepare.depth = ffxApiGetResourceDX12(depth->GetResource(), GetFfxApiState(depth->state));
        }
        else
        {
            LOG_ERROR("Depth is missing");
            _fgCommandList[fIndex]->Close();
            return false;
        }

        if (state.currentFeature && state.activeFgInput == FGInput::Upscaler)
            dfgPrepare.renderSize = { state.currentFeature->RenderWidth(), state.currentFeature->RenderHeight() };
        else if (depth != nullptr)
            dfgPrepare.renderSize = { static_cast<uint32_t>(depth->width), depth->height };
        else
            dfgPrepare.renderSize = { dfgPrepare.depth.description.width, dfgPrepare.depth.description.height };

        dfgPrepare.jitterOffset.x = _jitterX[fIndex];
        dfgPrepare.jitterOffset.y = _jitterY[fIndex];
        dfgPrepare.motionVectorScale.x = _mvScaleX[fIndex];
        dfgPrepare.motionVectorScale.y = _mvScaleY[fIndex];
        dfgPrepare.cameraFar = _cameraFar[fIndex];
        dfgPrepare.cameraNear = _cameraNear[fIndex];
        dfgPrepare.cameraFovAngleVertical = _cameraVFov[fIndex];

        dfgPrepare.frameTimeDelta = config->FTInput.value_or_default() == FrameTimeSource::Input
                                        ? (float) _ftDelta[fIndex]
                                        : static_cast<float>(state.lastFGFrameTime);

        dfgPrepare.viewSpaceToMetersFactor = _meterFactor[fIndex];

        retCode = FfxApiProxy::D3D12_Dispatch(&_fgContext, &dfgPrepare.header);
        LOG_DEBUG("D3D12_Dispatch result: {0}, frame: {1}, fIndex: {2}, commandList: {3:X}", retCode, willDispatchFrame,
                  fIndex, (size_t) dfgPrepare.commandList);

        if (retCode == FFX_API_RETURN_OK)
        {
            _fgCommandList[fIndex]->Close();
            _waitingExecute[fIndex] = true;
            dispatchResult = ExecuteCommandList(fIndex);
        }
    }

    if (config->FGUseMutexForSwapchain.value_or_default() && Mutex.getOwner() == 1)
    {
        LOG_TRACE("Releasing FG->Mutex: {}", Mutex.getOwner());
        Mutex.unlockThis(1);
    };

    return dispatchResult;
}

ffxReturnCode_t FSRFG_Dx12::DispatchCallback(ffxDispatchDescFrameGeneration* params)
{
    const int fIndex = params->frameID % BUFFER_COUNT;

    auto& state = State::Instance();

    if (!Config::Instance()->FGSkipReset.value_or_default())
        params->reset = (_reset[fIndex] != 0);
    else
        params->reset = 0;

    LOG_DEBUG("frameID: {}, commandList: {:X}, numGeneratedFrames: {}", params->frameID, (size_t) params->commandList,
              params->numGeneratedFrames);

    // check for status
    if (!Config::Instance()->FGEnabled.value_or_default() || _fgContext == nullptr || state.SCchanged)
    {
        LOG_WARN("Cancel async dispatch");
        params->numGeneratedFrames = 0;
    }

    // If fg is active but upscaling paused
    if ((state.currentFeature == nullptr && state.activeFgInput == FGInput::Upscaler) || state.FGchanged ||
        fIndex < 0 || !IsActive() || (state.currentFeature && state.currentFeature->FrameCount() == 0))
    {
        LOG_WARN("Upscaling paused! frameID: {}", params->frameID);
        params->numGeneratedFrames = 0;
    }

    static UINT64 _lastFrameId = 0;
    if (params->frameID == _lastFrameId)
    {
        LOG_WARN("Dispatched with the same frame id! frameID: {}", params->frameID);
        params->numGeneratedFrames = 0;
    }

    auto scFormat = (FfxApiSurfaceFormat) params->presentColor.description.format;
    auto lhFormat = _lastHudlessFormat;
    auto uhFormat = _usingHudlessFormat;

    if (_lastHudlessFormat != FFX_API_SURFACE_FORMAT_UNKNOWN && lhFormat != scFormat &&
        (_usingHudlessFormat == FFX_API_SURFACE_FORMAT_UNKNOWN || uhFormat != lhFormat))
    {
        LOG_DEBUG("Hudless format doesn't match, hudless: {}, present: {}", (uint32_t) _lastHudlessFormat,
                  params->presentColor.description.format);

        params->numGeneratedFrames = 0;
        _lastFrameId = params->frameID;

        state.FGchanged = true;
        state.SCchanged = true;

        return FFX_API_RETURN_OK;
    }

    static bool lastFGDisableHudless = Config::Instance()->FGDisableHudless.value_or_default();
    if (lastFGDisableHudless != Config::Instance()->FGDisableHudless.value_or_default())
    {
        lastFGDisableHudless = Config::Instance()->FGDisableHudless.value_or_default();

        // We can't just stop sending hudless if we have provided a hudless desc
        // Recreate FG if it was linked at creation
        if (_linkedHudlesDesc)
        {
            if (lastFGDisableHudless)
                _lastHudlessFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;

            params->numGeneratedFrames = 0;
            _lastFrameId = params->frameID;

            state.FGchanged = true;
            state.SCchanged = true;

            return FFX_API_RETURN_OK;
        }
    }

    if (State::Instance().gameQuirks & GameQuirk::FSRFGHudlessMismatchFixup && !lastFGDisableHudless)
    {
        auto presentWithHud = (ID3D12Resource*) params->presentColor.resource;
        auto hudlessResource = _resourceCopy[fIndex][FG_ResourceType::HudlessColor];
        auto hudlessState = D3D12_RESOURCE_STATE_COPY_DEST;

        if (hudlessResource == nullptr)
        {
            auto hudless = _frameResources[fIndex][FG_ResourceType::HudlessColor];
            if (hudless.validity == FG_ResourceValidity::UntilPresent)
                hudlessResource = hudless.GetResource();

            // hudless.state only holds the state for the original resource, not the copy that we could get here
            if (hudlessResource && hudlessResource == hudless.resource)
                hudlessState = hudless.state;
        }

        if (presentWithHud && hudlessResource)
        {
            auto cmdList = (ID3D12GraphicsCommandList*) params->commandList;

            if (_hudCopy[fIndex].get() == nullptr)
            {
                _hudCopy[fIndex] = std::make_unique<HudCopy_Dx12>("HudCopy", _device);
            }

            if (auto hudCopy = _hudCopy[fIndex].get(); hudCopy && hudCopy->IsInit())
            {
                // In Cyberprank - DLSSG has noise issues, FSR FG has noise + vignetting
                // In Death Stranding 2 - DLSSG has wrong colormapping it seems, FSR FG is fine
                const bool isCyberpunk = State::Instance().gameQuirks[GameQuirk::CyberpunkHudlessState];
                float hudDetectionThreshold = 0.03f;

                if (isCyberpunk && State::Instance().activeFgInput != FGInput::FSRFG)
                    hudDetectionThreshold = 0.01f;

                hudCopy->Dispatch(_device, cmdList, hudlessResource, presentWithHud, hudlessState,
                                  GetD3D12State((FfxApiResourceState) params->presentColor.state),
                                  hudDetectionThreshold);
            }
        }
    }

    auto dispatchResult = FfxApiProxy::D3D12_Dispatch(&_fgContext, &params->header);
    LOG_DEBUG("D3D12_Dispatch result: {}, fIndex: {}", (UINT) dispatchResult, fIndex);

    _lastFrameId = params->frameID;

    return dispatchResult;
}

ffxReturnCode_t FSRFG_Dx12::PresentCallback(ffxCallbackDescFrameGenerationPresent* params)
{
    if (params == nullptr)
        return FFX_API_RETURN_ERROR;

    auto cmdList = reinterpret_cast<ID3D12GraphicsCommandList*>(params->commandList);
    auto currentBackBuffer = reinterpret_cast<ID3D12Resource*>(params->currentBackBuffer.resource);
    auto outputSwapChainBuffer = reinterpret_cast<ID3D12Resource*>(params->outputSwapChainBuffer.resource);
    auto currentUI = reinterpret_cast<ID3D12Resource*>(params->currentUI.resource);

    if (cmdList == nullptr || currentBackBuffer == nullptr || outputSwapChainBuffer == nullptr)
        return FFX_API_RETURN_ERROR;

    D3D12_RESOURCE_STATES currentState = GetD3D12State((FfxApiResourceState) params->currentBackBuffer.state);
    D3D12_RESOURCE_STATES outputState = GetD3D12State((FfxApiResourceState) params->outputSwapChainBuffer.state);
    auto outputDesc = outputSwapChainBuffer->GetDesc();
    auto sourceDesc = currentBackBuffer->GetDesc();
    const bool isGeneratedFrame = params->isGeneratedFrame != 0;
    FrameWarpRuntime::ReportFsrfgPresentCallbackSeen(params->frameID, isGeneratedFrame);

    const bool enabled = FrameWarpRuntime::IsFsrfgPresentCallbackRequested();

    if (!enabled)
    {
        return FSRFGFrameWarpCopyResource(cmdList, outputSwapChainBuffer, outputState, currentBackBuffer, currentState)
                   ? FFX_API_RETURN_OK
                   : FFX_API_RETURN_ERROR;
    }

    _frameWarpPresentCallbackCounter++;
    const uint32_t requestedDiagnosticMode = Config::Instance()->FrameWarpFGDiagnosticMode.value_or_default();
    const uint32_t effectiveDiagnosticMode = FrameWarpResolveFGDiagnosticMode(_frameWarpPresentCallbackCounter);
    const bool diagnosticModeChanged = effectiveDiagnosticMode != _frameWarpDiagLastMode;
    if (diagnosticModeChanged)
    {
        LOG_DEBUG("FrameWarp FSRFG diagnostic mode={} requested={} callback={} frameID={}",
            FrameWarpFGDiagnosticModeName(effectiveDiagnosticMode),
            FrameWarpFGDiagnosticModeName(requestedDiagnosticMode),
            _frameWarpPresentCallbackCounter,
            params->frameID);
        _frameWarpDiagLastMode = effectiveDiagnosticMode;
    }

    if (isGeneratedFrame)
    {
        _frameWarpGeneratedSeen = true;
        _frameWarpLastGeneratedFrameID = params->frameID;
        _frameWarpLastGeneratedCallback = _frameWarpPresentCallbackCounter;
        _frameWarpRealCallbacksSinceGenerated = 0;
    }
    else
    {
        _frameWarpRealCallbacksSinceGenerated++;
    }

    const bool realMatchesCurrentGenerated =
        !isGeneratedFrame &&
        _frameWarpGeneratedSeen &&
        _frameWarpLastGeneratedFrameID == params->frameID &&
        (_frameWarpPresentCallbackCounter - _frameWarpLastGeneratedCallback) <= 2;
    const bool sameFrameLatched =
        _frameWarpLatchedDecisionValid &&
        _frameWarpLatchedFrameID == params->frameID;
    bool shouldWarpThisCallback =
        effectiveDiagnosticMode != static_cast<uint32_t>(FrameWarpFGDiagnosticMode::CopyBoth);
    bool reuseLatchedWarpResult = sameFrameLatched && _frameWarpLatchedWarped;
    const bool latchedCopyOnly = sameFrameLatched && _frameWarpLatchedCopyOnly;
    const bool latchedZeroPose =
        sameFrameLatched && !_frameWarpLatchedWarped && !_frameWarpLatchedCopyOnly;

    auto markLatchedWarped = [&]()
    {
        _frameWarpLatchedFrameID = params->frameID;
        _frameWarpLatchedDecisionValid = true;
        _frameWarpLatchedWarped = true;
        _frameWarpLatchedCopyOnly = false;
        strncpy_s(State::Instance().frameWarpStatus.fsrfgLastPolicy, "warp-all-latched", _TRUNCATE);
    };

    auto markLatchedCopyOnly = [&]()
    {
        _frameWarpLatchedFrameID = params->frameID;
        _frameWarpLatchedDecisionValid = true;
        _frameWarpLatchedWarped = false;
        _frameWarpLatchedCopyOnly = true;
        strncpy_s(State::Instance().frameWarpStatus.fsrfgLastPolicy, "copy-frame", _TRUNCATE);
    };

    auto markLatchedZeroPose = [&]()
    {
        _frameWarpLatchedFrameID = params->frameID;
        _frameWarpLatchedDecisionValid = true;
        _frameWarpLatchedWarped = false;
        _frameWarpLatchedCopyOnly = false;
        strncpy_s(State::Instance().frameWarpStatus.fsrfgLastPolicy, "zero-pose", _TRUNCATE);
    };

    if (latchedCopyOnly)
        shouldWarpThisCallback = false;

    if (isGeneratedFrame)
        _frameWarpDiagGeneratedCallbacks++;
    else
        _frameWarpDiagRealCallbacks++;

    auto logDiagnosticStats = [&](const char* outcome)
    {
        const uint32_t summaryEvery = std::max<uint32_t>(
            60,
            Config::Instance()->FrameWarpFGDiagnosticCycleFrames.value_or_default());
        if (diagnosticModeChanged ||
            (_frameWarpPresentCallbackCounter - _frameWarpDiagLastSummaryCallback) >= summaryEvery)
        {
            LOG_DEBUG("FrameWarp FSRFG stats mode={} outcome={} callbacks={} gen={} real={} warpedGen={} warpedReal={} zeroPose={} copies={} invalidCopies={} noopCopies={} stableUI={} lastFrameID={} phase={} src={:X} dst={:X}",
                FrameWarpFGDiagnosticModeName(effectiveDiagnosticMode),
                outcome != nullptr ? outcome : "unknown",
                _frameWarpPresentCallbackCounter,
                _frameWarpDiagGeneratedCallbacks,
                _frameWarpDiagRealCallbacks,
                _frameWarpDiagGeneratedWarped,
                _frameWarpDiagRealWarped,
                _frameWarpDiagZeroPose,
                _frameWarpDiagCopies,
                _frameWarpDiagInvalidCopies,
                _frameWarpDiagNoopCopies,
                _frameWarpDiagStableUiApplied,
                params->frameID,
                isGeneratedFrame ? "generated" : "real",
                (size_t) currentBackBuffer,
                (size_t) outputSwapChainBuffer);
            _frameWarpDiagLastSummaryCallback = _frameWarpPresentCallbackCounter;
        }
    };

    _frameWarpCadenceLogCount++;
    {
        auto& fwStatus = State::Instance().frameWarpStatus;
        const char* policyName =
            shouldWarpThisCallback
                ? (latchedZeroPose ? "zero-pose" : "warp-all-latched")
                : "copy-frame";
        fwStatus.fsrfgPresentCallbackOrdinal = _frameWarpPresentCallbackCounter;
        fwStatus.fsrfgLastSourceResource = (uint64_t) currentBackBuffer;
        fwStatus.fsrfgLastDestinationResource = (uint64_t) outputSwapChainBuffer;
        fwStatus.fsrfgLastReuseFramePose = reuseLatchedWarpResult;
        strncpy_s(fwStatus.fsrfgLastPhase, isGeneratedFrame ? "generated" : "real", _TRUNCATE);
        strncpy_s(fwStatus.fsrfgLastPolicy, policyName, _TRUNCATE);
    }

    if (_frameWarpCadenceLogCount <= 80 || _frameWarpCadenceLogCount % 120 == 0 || diagnosticModeChanged)
    {
        const char* policyName =
            shouldWarpThisCallback
                ? (latchedZeroPose ? "zero-pose" : "warp")
                : "copy";
        LOG_DEBUG("FrameWarp FSRFG cadence #{} frameID={} phase={} mode={} policy={} pairedGenerated={} realSinceGenerated={} reuseFramePose={} src={:X} dst={:X} srcFmt={} dstFmt={} srcState={} dstState={}",
            _frameWarpPresentCallbackCounter,
            params->frameID,
            isGeneratedFrame ? "generated" : "real",
            FrameWarpFGDiagnosticModeName(effectiveDiagnosticMode),
            policyName,
            realMatchesCurrentGenerated,
            _frameWarpRealCallbacksSinceGenerated,
            reuseLatchedWarpResult,
            (size_t) currentBackBuffer,
            (size_t) outputSwapChainBuffer,
            (UINT) sourceDesc.Format,
            (UINT) outputDesc.Format,
            (UINT) currentState,
            (UINT) outputState);
    }

    if (!shouldWarpThisCallback)
    {
        markLatchedCopyOnly();
        _frameWarpDiagCopies++;
        if (_frameWarpCadenceLogCount <= 80 || _frameWarpCadenceLogCount % 120 == 0 || diagnosticModeChanged)
        {
            LOG_DEBUG("FrameWarp FSRFG phase={} copy frameID={} mode={} pairedGenerated={} realSinceGenerated={} reuseFramePose={} src={:X} dst={:X}",
                isGeneratedFrame ? "generated" : "real",
                params->frameID,
                FrameWarpFGDiagnosticModeName(effectiveDiagnosticMode),
                realMatchesCurrentGenerated,
                _frameWarpRealCallbacksSinceGenerated,
                reuseLatchedWarpResult,
                (size_t) currentBackBuffer,
                (size_t) outputSwapChainBuffer);
        }

        logDiagnosticStats("copy-frame");
        return FSRFGFrameWarpCopyResource(cmdList, outputSwapChainBuffer, outputState, currentBackBuffer, currentState)
                   ? FFX_API_RETURN_OK
                   : FFX_API_RETURN_ERROR;
    }

    ID3D12Device* device = _device != nullptr ? _device : reinterpret_cast<ID3D12Device*>(params->device);

    if (device == nullptr || outputDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        markLatchedCopyOnly();
        return FSRFGFrameWarpCopyResource(cmdList, outputSwapChainBuffer, outputState, currentBackBuffer, currentState)
                   ? FFX_API_RETURN_OK
                   : FFX_API_RETURN_ERROR;
    }

    FrameWarpRuntime::EnsureInitialized(
        device,
        static_cast<UINT>(outputDesc.Width),
        outputDesc.Height,
        outputDesc.Format);
    FrameWarpRuntime::SyncConfig();

    auto frameWarp = FrameWarpRuntime::Get();
    if (frameWarp == nullptr || frameWarp->GetWarpedPresentOutput() == nullptr ||
        frameWarp->GetOutputWidth() != static_cast<UINT>(outputDesc.Width) ||
        frameWarp->GetOutputHeight() != outputDesc.Height ||
        frameWarp->GetSourceFormat() != outputDesc.Format)
    {
        static uint64_t fallbackLogCount = 0;
        fallbackLogCount++;
        if (fallbackLogCount <= 20 || fallbackLogCount % 300 == 0)
        {
            LOG_DEBUG("FrameWarp: FSRFG present callback fallback copy; runtime mismatch output={}x{} fmt={} generated={} frameID={}",
                outputDesc.Width,
                outputDesc.Height,
                (UINT) outputDesc.Format,
                params->isGeneratedFrame,
                params->frameID);
        }

        markLatchedCopyOnly();
        return FSRFGFrameWarpCopyResource(cmdList, outputSwapChainBuffer, outputState, currentBackBuffer, currentState)
                   ? FFX_API_RETURN_OK
                   : FFX_API_RETURN_ERROR;
    }

    FrameWarpFrameContext* frameContext = nullptr;
    const int directIndex = static_cast<int>(params->frameID % BUFFER_COUNT);
    if (_frameWarpContexts[directIndex].valid && _frameWarpContexts[directIndex].frameID == params->frameID)
    {
        frameContext = &_frameWarpContexts[directIndex];
    }
    else
    {
        UINT64 bestDistance = UINT64_MAX;
        for (int i = 0; i < BUFFER_COUNT; ++i)
        {
            if (!_frameWarpContexts[i].valid || _frameWarpContexts[i].frameID > params->frameID)
                continue;

            UINT64 distance = params->frameID - _frameWarpContexts[i].frameID;
            if (distance < bestDistance)
            {
                bestDistance = distance;
                frameContext = &_frameWarpContexts[i];
            }
        }

        if (bestDistance > BUFFER_COUNT)
            frameContext = nullptr;
    }

    ID3D12Resource* depthInput = frameContext != nullptr ? frameContext->depth : nullptr;
    D3D12_RESOURCE_STATES depthState =
        frameContext != nullptr ? frameContext->depthState : D3D12_RESOURCE_STATE_COMMON;
    const bool depthSuppressedForFsrfgDisplay =
        depthInput != nullptr && Config::Instance()->FrameWarpDepthAware.value_or_default();
    if (depthSuppressedForFsrfgDisplay)
    {
        static uint64_t depthSuppressLogCount = 0;
        depthSuppressLogCount++;
        if (depthSuppressLogCount <= 20 || depthSuppressLogCount % 300 == 0 || diagnosticModeChanged)
        {
            LOG_DEBUG("FrameWarp: FSRFG depth input suppressed in present callback frameID={} phase={} because generated/currentBackBuffer depth cadence is not validated",
                params->frameID,
                isGeneratedFrame ? "generated" : "real");
        }
        strncpy_s(State::Instance().frameWarpStatus.lastDepthReason, "FSRFG depth suppressed", _TRUNCATE);
        depthInput = nullptr;
        depthState = D3D12_RESOURCE_STATE_COMMON;
    }

    if (frameContext != nullptr && frameContext->vFovRadians > 0.0f && frameContext->aspectRatio > 0.0f)
        frameWarp->SetCameraContext(frameContext->vFovRadians, frameContext->aspectRatio, "fsrfg-present");

    const bool hudfixEnabled = Config::Instance()->FGHUDFix.value_or_default();
    bool stableUiAttempted = false;
    bool stableUiApplied = false;
    bool stableUiExtracted = false;
    bool useHudlessSceneSource = false;
    bool validHudlessForUi = false;

    if (hudfixEnabled && frameContext != nullptr && frameContext->hudless != nullptr)
    {
        auto hudlessDesc = frameContext->hudless->GetDesc();
        validHudlessForUi =
            hudlessDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            hudlessDesc.Width == outputDesc.Width &&
            hudlessDesc.Height == outputDesc.Height &&
            hudlessDesc.Format == outputDesc.Format &&
            sourceDesc.Width == outputDesc.Width &&
            sourceDesc.Height == outputDesc.Height &&
            sourceDesc.Format == outputDesc.Format;

        if (validHudlessForUi && !isGeneratedFrame)
        {
            stableUiAttempted = true;
            stableUiExtracted = frameWarp->ExtractUiLayerFromHudless(
                cmdList,
                params->frameID,
                frameContext->hudless,
                frameContext->hudlessState,
                currentBackBuffer,
                currentState,
                "hudfix-real");

            useHudlessSceneSource = stableUiExtracted;

            static uint64_t uiExtractAttemptLogCount = 0;
            uiExtractAttemptLogCount++;
            if (uiExtractAttemptLogCount <= 40 || uiExtractAttemptLogCount % 120 == 0 || diagnosticModeChanged)
            {
                LOG_DEBUG("FrameWarp UI extract phase=real source=hudfix-real frameID={} valid={} useHudlessScene={} explicitUi={}",
                    params->frameID,
                    stableUiExtracted,
                    useHudlessSceneSource,
                    currentUI != nullptr);
            }
        }
        else if (!validHudlessForUi)
        {
            static uint64_t invalidHudlessLogCount = 0;
            invalidHudlessLogCount++;
            if (invalidHudlessLogCount <= 20 || invalidHudlessLogCount % 300 == 0)
            {
                LOG_DEBUG("FrameWarp UI extract skipped; hudless {}x{} fmt={} source {}x{} fmt={} output {}x{} fmt={}",
                    hudlessDesc.Width,
                    hudlessDesc.Height,
                    (UINT) hudlessDesc.Format,
                    sourceDesc.Width,
                    sourceDesc.Height,
                    (UINT) sourceDesc.Format,
                    outputDesc.Width,
                    outputDesc.Height,
                    (UINT) outputDesc.Format);
            }
        }
    }

    ID3D12Resource* warpInput = useHudlessSceneSource ? frameContext->hudless : currentBackBuffer;
    D3D12_RESOURCE_STATES warpInputState = useHudlessSceneSource ? frameContext->hudlessState : currentState;
    const char* warpLabel =
        useHudlessSceneSource
            ? "fsrfg-hudless-real"
            : (isGeneratedFrame ? "fsrfg-present-generated" : "fsrfg-present-real");

    ID3D12Resource* warpedFrame = nullptr;
    FrameWarp_Dx12::PresentResult presentResult = FrameWarp_Dx12::PresentResult::Invalid;
    if (latchedZeroPose)
    {
        warpedFrame = warpInput;
        presentResult = FrameWarp_Dx12::PresentResult::ZeroPose;
    }
    else
    {
        presentResult = frameWarp->OnPrePresentEx(
            cmdList,
            warpInput,
            depthInput,
            nullptr,
            &warpedFrame,
            warpInputState,
            frameWarp->GetWarpedPresentOutput(),
            depthState,
            warpLabel,
            reuseLatchedWarpResult);
    }

    const bool warped = presentResult == FrameWarp_Dx12::PresentResult::Warped && warpedFrame != nullptr;
    const bool zeroPose = presentResult == FrameWarp_Dx12::PresentResult::ZeroPose && warpedFrame != nullptr;

    if (warped || zeroPose)
    {
        if (warped)
        {
            markLatchedWarped();
            if (isGeneratedFrame)
                _frameWarpDiagGeneratedWarped++;
            else
                _frameWarpDiagRealWarped++;
        }
        else
        {
            markLatchedZeroPose();
            _frameWarpDiagZeroPose++;
        }

        if (warped &&
            warpInputState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
            warpInputState != D3D12_RESOURCE_STATE_COMMON)
        {
            FSRFGFrameWarpTransitionResource(
                cmdList,
                warpInput,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                warpInputState);
        }

        if (warped &&
            depthInput != nullptr &&
            depthState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
            depthState != D3D12_RESOURCE_STATE_COMMON)
        {
            FSRFGFrameWarpTransitionResource(
                cmdList,
                depthInput,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                depthState);
        }

        ID3D12Resource* sceneForUi = warped ? warpedFrame : warpInput;
        D3D12_RESOURCE_STATES sceneForUiState =
            warped ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : warpInputState;

        const bool shouldTryCachedUi =
            hudfixEnabled &&
            ((isGeneratedFrame && !stableUiExtracted) || (!isGeneratedFrame && stableUiExtracted));
        const bool suppressWarpedUiUnderlay =
            isGeneratedFrame && !useHudlessSceneSource;
        const bool useWarpTransformForUnderlay =
            suppressWarpedUiUnderlay && warped;
        const UINT64 uiMaxAge = isGeneratedFrame ? BUFFER_COUNT : 0;
        if (shouldTryCachedUi)
        {
            stableUiApplied = frameWarp->CompositeCachedUiLayer(
                cmdList,
                params->frameID,
                uiMaxAge,
                sceneForUi,
                sceneForUiState,
                outputSwapChainBuffer,
                outputState,
                isGeneratedFrame ? "generated" : "real",
                suppressWarpedUiUnderlay,
                useWarpTransformForUnderlay);

            if (stableUiApplied)
            {
                _frameWarpDiagStableUiApplied++;
                static uint64_t stableUiCacheLogCount = 0;
                stableUiCacheLogCount++;
                if (stableUiCacheLogCount <= 40 || stableUiCacheLogCount % 120 == 0 || diagnosticModeChanged)
                {
                    LOG_DEBUG("FrameWarp: FSRFG UI layer composite phase={} frameID={} source={} poseResult={} suppressUnderlay={} repairWarp={} extractedThisFrame={} reuseFramePose={} reuseZeroPose={}",
                        isGeneratedFrame ? "generated" : "real",
                        params->frameID,
                        isGeneratedFrame ? "real-cache" : "hudfix-real",
                        warped ? "warped" : "zero-pose",
                        suppressWarpedUiUnderlay,
                        useWarpTransformForUnderlay,
                        stableUiExtracted,
                        reuseLatchedWarpResult,
                        latchedZeroPose);
                }

                logDiagnosticStats(warped ? "ui-layer-warp" : "ui-layer-zero-pose");
                return FFX_API_RETURN_OK;
            }
        }

        if (zeroPose)
        {
            static uint64_t zeroPoseCopyLogCount = 0;
            zeroPoseCopyLogCount++;
            if (zeroPoseCopyLogCount <= 40 || zeroPoseCopyLogCount % 120 == 0 || diagnosticModeChanged)
            {
                LOG_DEBUG("FrameWarp FSRFG phase={} zero-pose copy frameID={} mode={} stableUiAttempted={} stableUiApplied={} reuseZeroPose={} src={:X} dst={:X}",
                    isGeneratedFrame ? "generated" : "real",
                    params->frameID,
                    FrameWarpFGDiagnosticModeName(effectiveDiagnosticMode),
                    stableUiAttempted,
                    stableUiApplied,
                    latchedZeroPose,
                    (size_t) currentBackBuffer,
                    (size_t) outputSwapChainBuffer);
            }

            logDiagnosticStats("zero-pose-copy");
            return FSRFGFrameWarpCopyResource(cmdList, outputSwapChainBuffer, outputState, currentBackBuffer, currentState)
                       ? FFX_API_RETURN_OK
                       : FFX_API_RETURN_ERROR;
        }

        if (!FSRFGFrameWarpCopyResource(
                cmdList,
                outputSwapChainBuffer,
                outputState,
                warpedFrame,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            return FFX_API_RETURN_ERROR;
        }

        static uint64_t presentWarpLogCount = 0;
        presentWarpLogCount++;
        if (presentWarpLogCount <= 40 || presentWarpLogCount % 120 == 0 || diagnosticModeChanged)
        {
            LOG_DEBUG("FrameWarp FSRFG phase={} warped frameID={} mode={} depth={} stableUiAttempted={} explicitUi={} reuseFramePose={} src={:X} dst={:X}",
                isGeneratedFrame ? "generated" : "real",
                params->frameID,
                FrameWarpFGDiagnosticModeName(effectiveDiagnosticMode),
                depthInput != nullptr,
                stableUiAttempted,
                currentUI != nullptr,
                reuseLatchedWarpResult,
                (size_t) warpInput,
                (size_t) outputSwapChainBuffer);
        }

        if (currentUI != nullptr)
        {
            static uint64_t explicitUiLogCount = 0;
            explicitUiLogCount++;
            if (explicitUiLogCount <= 20 || explicitUiLogCount % 300 == 0)
            {
                LOG_DEBUG("FrameWarp: FSRFG explicit UI resource present but alpha composite is not yet enabled in callback path");
            }
        }

        logDiagnosticStats("present-warp");
        return FFX_API_RETURN_OK;
    }

    static uint64_t noWarpLogCount = 0;
    noWarpLogCount++;
    markLatchedCopyOnly();
    _frameWarpDiagInvalidCopies++;
    if (noWarpLogCount <= 40 || noWarpLogCount % 120 == 0 || diagnosticModeChanged)
    {
        LOG_DEBUG("FrameWarp: FSRFG present callback invalid copy frameID={} phase={} mode={} depth={} stableUiAttempted={} stableUiApplied={} reuseFramePose={}",
            params->frameID,
            isGeneratedFrame ? "generated" : "real",
            FrameWarpFGDiagnosticModeName(effectiveDiagnosticMode),
            depthInput != nullptr,
            stableUiAttempted,
            stableUiApplied,
            reuseLatchedWarpResult);
    }

    logDiagnosticStats("invalid-copy");
    return FSRFGFrameWarpCopyResource(cmdList, outputSwapChainBuffer, outputState, currentBackBuffer, currentState)
               ? FFX_API_RETURN_OK
               : FFX_API_RETURN_ERROR;
}

FSRFG_Dx12::~FSRFG_Dx12() { Shutdown(); }

bool FSRFG_Dx12::SetInterpolatedFrameCount(UINT interpolatedFrameCount) { return true; }

void* FSRFG_Dx12::FrameGenerationContext()
{
    LOG_DEBUG("");
    return (void*) _fgContext;
}

void* FSRFG_Dx12::SwapchainContext()
{
    LOG_DEBUG("");
    return _swapChainContext;
}

void FSRFG_Dx12::DestroyFGContext()
{
    _frameCount = 1;
    // _lastDispatchedFrame = 0;
    _version = {};

    LOG_DEBUG("");

    Deactivate();

    if (_fgContext != nullptr)
    {
        auto result = FfxApiProxy::D3D12_DestroyContext(&_fgContext, nullptr);

        if (!(State::Instance().isShuttingDown))
            LOG_INFO("D3D12_DestroyContext result: {0:X}", result);

        _fgContext = nullptr;
    }

    ReleaseObjects();
}

bool FSRFG_Dx12::Shutdown()
{
    Deactivate();

    if (_swapChainContext != nullptr)
        ReleaseSwapchain(_hwnd);

    ReleaseObjects();

    return true;
}

bool FSRFG_Dx12::CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                 IDXGISwapChain** swapChain, bool readyToRelease)
{
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == desc->OutputWindow)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("FG swapchain already created for the same output window!");
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              desc->BufferCount, desc->BufferDesc.Width, desc->BufferDesc.Height,
                              desc->BufferDesc.Format, desc->Flags) == S_OK;

            *swapChain = State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating new swapchain without releasing old one,
        // we need to release it to avoid errors
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            ReleaseSwapchain(_hwnd);

            // Not sure why but XeFG sometimes doesn't release the swapchain properly
            // maybe needed for FSR-FG too, so added it here
            // so we force release it here to be able to recreate swapchain for same hwnd
            if (State::Instance().currentRealSwapchain != nullptr)
            {
                UINT release = 0;
                do
                {
                    release = State::Instance().currentRealSwapchain->Release();
                    LOG_DEBUG("Releasing swapchain, ref count: {}", release);
                } while (release > 0);
            }
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    IDXGIFactory* realFactory = nullptr;
    ID3D12CommandQueue* realQueue = nullptr;

    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    if (!CheckForRealObject(__FUNCTION__, cmdQueue, (IUnknown**) &realQueue))
        realQueue = cmdQueue;

    ffxCreateContextDescFrameGenerationSwapChainNewDX12 createSwapChainDesc {};
    createSwapChainDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12;
    createSwapChainDesc.dxgiFactory = realFactory;
    createSwapChainDesc.gameQueue = realQueue;
    createSwapChainDesc.desc = desc;
    createSwapChainDesc.swapchain = (IDXGISwapChain4**) swapChain;

    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 versionDesc {};
    versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    versionDesc.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;

    createSwapChainDesc.header.pNext = &versionDesc.header;

    auto result = FfxApiProxy::D3D12_CreateContext(&_swapChainContext, &createSwapChainDesc.header, nullptr);

    if (result == FFX_API_RETURN_OK)
    {
        ConfigureFramePaceTuning();

        _gameCommandQueue = realQueue;
        _swapChain = *swapChain;
        _hwnd = desc->OutputWindow;

        return true;
    }

    return false;
}

bool FSRFG_Dx12::CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd,
                                  DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                  IDXGISwapChain1** swapChain, bool readyToRelease)
{
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == hwnd)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("XeFG swapchain already created for the same output window!");
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              desc->BufferCount, desc->Width, desc->Height, desc->Format, desc->Flags) == S_OK;

            *swapChain = (IDXGISwapChain1*) State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating new swapchain without releasing old one,
        // we need to release it to avoid errors
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            ReleaseSwapchain(_hwnd);

            // Not sure why but XeFG sometimes doesn't release the swapchain properly
            // maybe needed for FSR-FG too, so added it here
            // so we force release it here to be able to recreate swapchain for same hwnd
            if (State::Instance().currentRealSwapchain != nullptr)
            {
                UINT release = 0;
                do
                {
                    release = State::Instance().currentRealSwapchain->Release();
                    LOG_DEBUG("Releasing swapchain, ref count: {}", release);
                } while (release > 0);
            }
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    IDXGIFactory* realFactory = nullptr;
    ID3D12CommandQueue* realQueue = nullptr;

    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    if (!CheckForRealObject(__FUNCTION__, cmdQueue, (IUnknown**) &realQueue))
        realQueue = cmdQueue;

    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 createSwapChainDesc {};
    createSwapChainDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    createSwapChainDesc.fullscreenDesc = pFullscreenDesc;
    createSwapChainDesc.hwnd = hwnd;
    createSwapChainDesc.dxgiFactory = realFactory;
    createSwapChainDesc.gameQueue = realQueue;
    createSwapChainDesc.desc = desc;
    createSwapChainDesc.swapchain = (IDXGISwapChain4**) swapChain;

    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 versionDesc {};
    versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    versionDesc.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;

    createSwapChainDesc.header.pNext = &versionDesc.header;

    auto result = FfxApiProxy::D3D12_CreateContext(&_swapChainContext, &createSwapChainDesc.header, nullptr);

    if (result == FFX_API_RETURN_OK)
    {
        ConfigureFramePaceTuning();

        _gameCommandQueue = realQueue;
        _swapChain = *swapChain;
        _hwnd = hwnd;

        return true;
    }

    return false;
}

bool FSRFG_Dx12::ReleaseSwapchain(HWND hwnd)
{
    if (hwnd != _hwnd || _hwnd == NULL)
        return false;

    LOG_DEBUG("");

    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        if (Mutex.getOwner() == 1)
        {
            LOG_WARN("Skipping Mutex we are already in ReleaseSwapchain");
            return true;
        }

        LOG_TRACE("Waiting Mutex 1, current: {}", Mutex.getOwner());
        Mutex.lock(1);
        LOG_TRACE("Accuired Mutex: {}", Mutex.getOwner());
    }

    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    if (_fgContext != nullptr)
        DestroyFGContext();

    if (!Config::Instance()->FGPreserveSwapChain.value_or_default())
    {
        if (!State::Instance().isShuttingDown)
        {
            if (_swapChainContext != nullptr)
            {
                auto result = FfxApiProxy::D3D12_DestroyContext(&_swapChainContext, nullptr);
                LOG_INFO("Destroy Ffx Swapchain Result: {}({})", result, FfxApiProxy::ReturnCodeToString(result));
            }

            _swapChainContext = nullptr;
            State::Instance().currentFGSwapchain = nullptr;
        }
    }

    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        LOG_TRACE("Releasing Mutex: {}", Mutex.getOwner());
        Mutex.unlockThis(1);
    }

    return true;
}

void FSRFG_Dx12::CreateContext(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_DEBUG("");

    CreateObjects(device);

    _constants = fgConstants;

    // Changing the format of the hudless resource requires a new context
    if (_fgContext != nullptr && (_lastHudlessFormat != _usingHudlessFormat))
    {
        auto result = FfxApiProxy::D3D12_DestroyContext(&_fgContext, nullptr);
        _fgContext = nullptr;
    }

    if (_fgContext != nullptr)
    {
        ffxConfigureDescFrameGeneration m_FrameGenerationConfig = {};
        m_FrameGenerationConfig.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        m_FrameGenerationConfig.frameGenerationEnabled = true;
        m_FrameGenerationConfig.swapChain = _swapChain;
        m_FrameGenerationConfig.presentCallback = nullptr;
        m_FrameGenerationConfig.HUDLessColor = FfxApiResource({});

        auto result = FfxApiProxy::D3D12_Configure(&_fgContext, &m_FrameGenerationConfig.header);

        _isActive = (result == FFX_API_RETURN_OK);

        LOG_DEBUG("Reactivate");

        return;
    }

    ffxQueryDescGetVersions versionQuery {};
    versionQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versionQuery.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    versionQuery.device = device; // only for DirectX 12 applications
    uint64_t versionCount = 0;
    versionQuery.outputCount = &versionCount;
    // get number of versions for allocation
    FfxApiProxy::D3D12_Query(nullptr, &versionQuery.header);

    State::Instance().ffxFGVersionIds.resize(versionCount);
    State::Instance().ffxFGVersionNames.resize(versionCount);
    versionQuery.versionIds = State::Instance().ffxFGVersionIds.data();
    versionQuery.versionNames = State::Instance().ffxFGVersionNames.data();
    // fill version ids and names arrays.
    FfxApiProxy::D3D12_Query(nullptr, &versionQuery.header);

    ffxCreateBackendDX12Desc backendDesc {};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.device = device;

    // Only gets linked if _lastHudlessFormat != FFX_API_SURFACE_FORMAT_UNKNOWN
    ffxCreateContextDescFrameGenerationHudless hudlessDesc {};
    hudlessDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hudlessDesc.hudlessBackBufferFormat = _lastHudlessFormat;
    hudlessDesc.header.pNext = &backendDesc.header;

    ffxCreateContextDescFrameGeneration createFg {};
    createFg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;

    // use swapchain buffer info
    DXGI_SWAP_CHAIN_DESC desc {};
    if (State::Instance().currentSwapchain->GetDesc(&desc) == S_OK)
    {
        createFg.displaySize = { desc.BufferDesc.Width, desc.BufferDesc.Height };

        if (fgConstants.displayWidth != 0 && fgConstants.displayHeight != 0)
            createFg.maxRenderSize = { fgConstants.displayWidth, fgConstants.displayHeight };
        else
            createFg.maxRenderSize = { desc.BufferDesc.Width, desc.BufferDesc.Height };
    }
    else
    {
        // this might cause issues
        createFg.displaySize = { fgConstants.displayWidth, fgConstants.displayHeight };
        createFg.maxRenderSize = { fgConstants.displayWidth, fgConstants.displayHeight };
    }

    _maxRenderWidth = createFg.maxRenderSize.width;
    _maxRenderHeight = createFg.maxRenderSize.height;

    createFg.flags = 0;

    if (fgConstants.flags & FG_Flags::Hdr)
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;

    if (fgConstants.flags & FG_Flags::InvertedDepth)
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;

    if (fgConstants.flags & FG_Flags::JitteredMVs)
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;

    if (fgConstants.flags & FG_Flags::DisplayResolutionMVs)
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;

    if (fgConstants.flags & FG_Flags::Async)
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;

    if (fgConstants.flags & FG_Flags::InfiniteDepth)
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;

    if (spdlog::default_logger()->level() == SPDLOG_LEVEL_TRACE)
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;

    createFg.backBufferFormat = ffxApiGetSurfaceFormatDX12(desc.BufferDesc.Format);

    if (_lastHudlessFormat != FFX_API_SURFACE_FORMAT_UNKNOWN)
    {
        _usingHudlessFormat = _lastHudlessFormat;
        _lastHudlessFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;
        createFg.header.pNext = &hudlessDesc.header;
        _linkedHudlesDesc = true;
    }
    else
    {
        _usingHudlessFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;
        createFg.header.pNext = &backendDesc.header;
        _linkedHudlesDesc = false;
    }

    {
        ScopedSkipSpoofing skipSpoofing {};
        ScopedSkipHeapCapture skipHeapCapture {};

        // Currently 0 is non-ML FG and 1 is ML FG
        if (Config::Instance()->FfxFGIndex.value_or_default() < 0 ||
            Config::Instance()->FfxFGIndex.value_or_default() >= State::Instance().ffxFGVersionIds.size())
            Config::Instance()->FfxFGIndex.set_volatile_value(0);

        ffxOverrideVersion override = { 0 };
        override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        override.versionId = State::Instance().ffxFGVersionIds[Config::Instance()->FfxFGIndex.value_or_default()];
        backendDesc.header.pNext = &override.header;

        ParseVersion(State::Instance().ffxFGVersionNames[Config::Instance()->FfxFGIndex.value_or_default()], &_version);

        ffxReturnCode_t retCode = FfxApiProxy::D3D12_CreateContext(&_fgContext, &createFg.header, nullptr);

        LOG_INFO("D3D12_CreateContext result: {:X}", retCode);
        _isActive = (retCode == FFX_API_RETURN_OK);
        _lastDispatchedFrame = 0;
    }

    LOG_DEBUG("Create");
}

void FSRFG_Dx12::Activate()
{
    if (_fgContext != nullptr && _swapChain != nullptr && !_isActive)
    {
        FrameWarpRuntime::ReportFsrfgPresentCallbackConfigured(false, "activating");

        ffxConfigureDescFrameGeneration fgConfig = {};
        fgConfig.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        fgConfig.frameGenerationEnabled = true;
        fgConfig.swapChain = _swapChain;
        fgConfig.presentCallback = nullptr;
        fgConfig.HUDLessColor = FfxApiResource({});

        auto result = FfxApiProxy::D3D12_Configure(&_fgContext, &fgConfig.header);

        if (result == FFX_API_RETURN_OK)
        {
            _isActive = true;
            _lastDispatchedFrame = 0;
        }

        LOG_INFO("D3D12_Configure Enabled: true, result: {} ({})", magic_enum::enum_name((FfxApiReturnCodes) result),
                 (UINT) result);
    }
}

void FSRFG_Dx12::Deactivate()
{
    FrameWarpRuntime::ReportFsrfgPresentCallbackConfigured(false, "deactivated");

    _frameWarpPresentCallbackCounter = 0;
    _frameWarpLastGeneratedFrameID = 0;
    _frameWarpLastGeneratedCallback = 0;
    _frameWarpRealCallbacksSinceGenerated = 0;
    _frameWarpCadenceLogCount = 0;
    _frameWarpDiagGeneratedCallbacks = 0;
    _frameWarpDiagRealCallbacks = 0;
    _frameWarpDiagGeneratedWarped = 0;
    _frameWarpDiagRealWarped = 0;
    _frameWarpDiagCopies = 0;
    _frameWarpDiagStableUiApplied = 0;
    _frameWarpDiagNoopCopies = 0;
    _frameWarpDiagZeroPose = 0;
    _frameWarpDiagInvalidCopies = 0;
    _frameWarpDiagLastSummaryCallback = 0;
    _frameWarpDiagLastMode = UINT32_MAX;
    _frameWarpGeneratedSeen = false;
    _frameWarpLatchedFrameID = 0;
    _frameWarpLatchedDecisionValid = false;
    _frameWarpLatchedWarped = false;
    _frameWarpLatchedCopyOnly = false;

    if (_isActive)
    {
        auto fIndex = GetIndex();
        if (_uiCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _uiCommandList[fIndex][{}]: {:X}", fIndex, (size_t) _uiCommandList[fIndex]);
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

        ffxReturnCode_t result = FFX_API_RETURN_ERROR;

        if (_fgContext != nullptr)
        {
            ffxConfigureDescFrameGeneration fgConfig = {};
            fgConfig.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            fgConfig.frameGenerationEnabled = false;
            fgConfig.swapChain = _swapChain;
            fgConfig.presentCallback = nullptr;
            fgConfig.HUDLessColor = FfxApiResource({});

            result = FfxApiProxy::D3D12_Configure(&_fgContext, &fgConfig.header);

            if (result == FFX_API_RETURN_OK)
                _isActive = false;
        }
        else
        {
            LOG_DEBUG("No context to deactivate, just set  active to false");
            _isActive = false;
        }

        // _lastDispatchedFrame = 0;

        LOG_INFO("D3D12_Configure Enabled: false, result: {} ({})", magic_enum::enum_name((FfxApiReturnCodes) result),
                 (UINT) result);
    }
}

void FSRFG_Dx12::EvaluateState(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_FUNC();

    _constants = fgConstants;

    if (!FfxApiProxy::IsFGReady())
        FfxApiProxy::InitFfxDx12();

    // If needed hooks are missing or XeFG proxy is not inited or FG swapchain is not created
    if (!FfxApiProxy::IsFGReady() || State::Instance().currentFGSwapchain == nullptr)
        return;

    if (State::Instance().isShuttingDown)
    {
        DestroyFGContext();
        return;
    }

    static bool lastInfiniteDepth = false;
    bool currentInfiniteDepth = static_cast<bool>(fgConstants.flags & FG_Flags::InfiniteDepth);
    if (lastInfiniteDepth != currentInfiniteDepth)
    {
        lastInfiniteDepth = currentInfiniteDepth;
        LOG_DEBUG("Infinite Depth changed: {}", currentInfiniteDepth);

        State::Instance().FGchanged = true;
        State::Instance().SCchanged = true;
    }

    if (_maxRenderWidth != 0 && _maxRenderHeight != 0 && IsActive() && !IsPaused() &&
        (fgConstants.displayWidth > _maxRenderWidth || fgConstants.displayHeight > _maxRenderHeight))

    {
        State::Instance().FGchanged = true;
        State::Instance().SCchanged = true;
    }

    // If FG Enabled from menu
    if (Config::Instance()->FGEnabled.value_or_default())
    {
        // If FG context is nullptr
        if (_fgContext == nullptr)
        {
            // Create it again
            CreateContext(device, fgConstants);

            // Pause for 10 frames
            UpdateTarget();
        }
        // If there is a change deactivate it
        else if (State::Instance().FGchanged)
        {
            Deactivate();

            // Pause for 10 frames
            UpdateTarget();

            // Destroy if Swapchain has a change destroy FG Context too
            if (State::Instance().SCchanged)
                DestroyFGContext();
        }

        if (_fgContext != nullptr && State::Instance().activeFgInput == FGInput::Upscaler && !IsPaused() && !IsActive())
            Activate();
    }
    else if (IsActive())
    {
        Deactivate();

        State::Instance().ClearCapturedHudlesses = true;
        Hudfix_Dx12::ResetCounters();
    }

    if (State::Instance().FGchanged)
    {
        LOG_DEBUG("FGchanged");

        State::Instance().FGchanged = false;

        Hudfix_Dx12::ResetCounters();

        // Pause for 10 frames
        UpdateTarget();

        // Release FG mutex
        if (Mutex.getOwner() == 2)
            Mutex.unlockThis(2);
    }

    State::Instance().SCchanged = false;
}

void FSRFG_Dx12::ReleaseObjects()
{
    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        if (_fgCommandAllocator[i] != nullptr)
        {
            _fgCommandAllocator[i]->Release();
            _fgCommandAllocator[i] = nullptr;
        }

        if (_fgCommandList[i] != nullptr)
        {
            _fgCommandList[i]->Release();
            _fgCommandList[i] = nullptr;
        }

        if (_uiCommandAllocator[i] != nullptr)
        {
            _uiCommandAllocator[i]->Release();
            _uiCommandAllocator[i] = nullptr;
        }

        if (_uiCommandList[i] != nullptr)
        {
            _uiCommandList[i]->Release();
            _uiCommandList[i] = nullptr;
        }

        if (_scCommandAllocator[i] != nullptr)
        {
            _scCommandAllocator[i]->Release();
            _scCommandAllocator[i] = nullptr;
        }

        if (_scCommandList[i] != nullptr)
        {
            _scCommandList[i]->Release();
            _scCommandList[i] = nullptr;
        }
    }

    _renderUI.reset();
    _hudlessCompare.reset();
    _mvFlip.reset();
    _depthFlip.reset();
    ReleaseUIFences();
}

bool FSRFG_Dx12::ExecuteCommandList(int index)
{
    if (_waitingExecute[index])
    {
        LOG_DEBUG("Executing FG cmdList: {:X}", (size_t) _fgCommandList[index]);
        _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_fgCommandList[index]);
        SetExecuted(index);
    }

    return true;
}

bool FSRFG_Dx12::SetResource(Dx12Resource* inputResource)
{
    if (inputResource == nullptr || inputResource->resource == nullptr ||
        (inputResource->type != FG_ResourceType::UIColor && inputResource->type != FG_ResourceType::Distortion && (!IsActive() || IsPaused())))
    {
        return false;
    }

    // For late sent SL resources
    // we use provided frame index
    auto fIndex = inputResource->frameIndex;
    if (fIndex < 0)
        fIndex = GetIndex();

    auto& type = inputResource->type;

    std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);

    if (_frameResources[fIndex].contains(type) &&
        _frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow)
    {
        return false;
    }

    if (type == FG_ResourceType::HudlessColor)
    {
        if (Config::Instance()->FGDisableHudless.value_or_default())
            return false;

        if (!_noHudless[fIndex] && Config::Instance()->FGOnlyAcceptFirstHudless.value_or_default() &&
            inputResource->validity != FG_ResourceValidity::UntilPresentFromDispatch)
        {
            return false;
        }
    }

    if (type == FG_ResourceType::UIColor && Config::Instance()->FGDisableUI.value_or_default())
        return false;

    if (inputResource->cmdList == nullptr && inputResource->validity == FG_ResourceValidity::ValidNow)
    {
        LOG_ERROR("{}, validity == ValidNow but cmdList is nullptr!", magic_enum::enum_name(type));
        return false;
    }

    _frameResources[fIndex][type] = {};
    auto fResource = &_frameResources[fIndex][type];
    fResource->type = type;
    fResource->state = inputResource->state;
    fResource->validity = inputResource->validity;
    fResource->resource = inputResource->resource;
    fResource->width = inputResource->width;
    fResource->height = inputResource->height;
    fResource->cmdList = inputResource->cmdList;

    auto willFlip = State::Instance().activeFgInput == FGInput::Upscaler &&
                    Config::Instance()->FGResourceFlip.value_or_default() &&
                    (fResource->type == FG_ResourceType::Velocity || fResource->type == FG_ResourceType::Depth);

    // Resource flipping
    if (willFlip && _device != nullptr)
    {
        FlipResource(fResource);
    }

    if (type == FG_ResourceType::UIColor)
    {
        // auto format = State::Instance().currentSwapchainDesc.BufferDesc.Format;

        // auto uiFormat = (FfxApiSurfaceFormat) ffxApiGetSurfaceFormatDX12(fResource->GetResource()->GetDesc().Format);
        // auto scFormat = (FfxApiSurfaceFormat) ffxApiGetSurfaceFormatDX12(format);

        // if (uiFormat == -1 || scFormat == -1 || uiFormat != scFormat)
        //{
        //     if (!UIFormatTransfer(fIndex, _device, GetUICommandList(fIndex), format, fResource))
        //     {
        //         LOG_WARN("Skipping UI resource due to format mismatch! UI: {}, swapchain: {}",
        //                  magic_enum::enum_name(uiFormat), magic_enum::enum_name(scFormat));

        //        _frameResources[fIndex][type] = {};
        //        return false;
        //    }
        //    else
        //    {
        //          fResource->validity = FG_ResourceValidity::UntilPresent;
        //    }
        //}

        // fResource->validity = FG_ResourceValidity::UntilPresent;
        _noUi[fIndex] = false;
    }
    else if (type == FG_ResourceType::Distortion)
    {
        _noDistortionField[fIndex] = false;
    }
    else if (type == FG_ResourceType::HudlessColor)
    {
        auto scFormat = State::Instance().currentSwapchainDesc.BufferDesc.Format;
        auto scFfxFormat =
            (FfxApiSurfaceFormat) ffxApiGetSurfaceFormatDX12(State::Instance().currentSwapchainDesc.BufferDesc.Format);

        auto resFormat = fResource->GetResource()->GetDesc().Format;
        _lastHudlessFormat = (FfxApiSurfaceFormat) ffxApiGetSurfaceFormatDX12(resFormat);

        if (_lastHudlessFormat != FFX_API_SURFACE_FORMAT_UNKNOWN && !CompareResourceFormats(resFormat, scFormat))
        {
            if (!HudlessFormatTransfer(fIndex, _device, scFormat, fResource))
            {
                LOG_WARN("Skipping hudless resource due to format mismatch! hudless: {}, swapchain: {}",
                         magic_enum::enum_name(_lastHudlessFormat), magic_enum::enum_name(scFfxFormat));

                _lastHudlessFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;
                _frameResources[fIndex][type] = {};
                return false;
            }
            else
            {
                fResource->validity = FG_ResourceValidity::UntilPresent;
            }
        }

        _noHudless[fIndex] = false;
    }

    // For FSR FG we always copy ValidNow
    if (fResource->validity == FG_ResourceValidity::ValidButMakeCopy)
        fResource->validity = FG_ResourceValidity::ValidNow;

    fResource->validity = (fResource->validity != FG_ResourceValidity::ValidNow || willFlip)
                              ? FG_ResourceValidity::UntilPresent
                              : FG_ResourceValidity::ValidNow;

    // Copy ValidNow
    if (fResource->validity == FG_ResourceValidity::ValidNow)
    {
        ID3D12Resource* copyOutput = nullptr;

        if (_resourceCopy[fIndex].contains(type))
            copyOutput = _resourceCopy[fIndex].at(type);

        if (!CopyResource(inputResource->cmdList, inputResource->resource, &copyOutput, inputResource->state))
        {
            LOG_ERROR("{}, CopyResource error!", magic_enum::enum_name(type));
            return false;
        }

        copyOutput->SetName(std::format(L"_resourceCopy[{}][{}]", fIndex, (UINT) type).c_str());

        _resourceCopy[fIndex][type] = copyOutput;
        fResource->copy = copyOutput;
        fResource->state = D3D12_RESOURCE_STATE_COPY_DEST;
        LOG_TRACE("Made a copy: {:X} of input: {:X}", (size_t) fResource->copy, (size_t) fResource->resource);
    }

    SetResourceReady(type, fIndex);

    // if (inputResource->validity == FG_ResourceValidity::UntilPresent)
    //     SetResourceReady(type, fIndex);
    // else
    //     ResTrack_Dx12::SetResourceCmdList(type, inputResource->cmdList);

    LOG_TRACE("_frameResources[{}][{}]: {:X}", fIndex, magic_enum::enum_name(type), (size_t) fResource->GetResource());
    return true;
}

void FSRFG_Dx12::SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) { _gameCommandQueue = queue; }

void FSRFG_Dx12::CreateObjects(ID3D12Device* InDevice)
{
    _device = InDevice;

    if (_fgCommandAllocator[0] != nullptr)
        return;

    LOG_DEBUG("");

    do
    {
        HRESULT result;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* cmdList = nullptr;
        ID3D12CommandQueue* cmdQueue = nullptr;

        // FG
        for (size_t i = 0; i < BUFFER_COUNT; i++)
        {
            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_fgCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _fgCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _fgCommandAllocator[i]->SetName(std::format(L"_fgCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _fgCommandAllocator[i], (IUnknown**) &allocator))
                _fgCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _fgCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_fgCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _fgCommandList[i]->SetName(std::format(L"_fgCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _fgCommandList[i], (IUnknown**) &cmdList))
                _fgCommandList[i] = cmdList;

            result = _fgCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_fgCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_uiCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _uiCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _uiCommandAllocator[i]->SetName(std::format(L"_uiCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandAllocator[i], (IUnknown**) &allocator))
                _uiCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _uiCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_uiCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _uiCommandList[i]->SetName(std::format(L"_uiCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandList[i], (IUnknown**) &cmdList))
                _uiCommandList[i] = cmdList;

            result = _uiCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_uiCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_scCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _scCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _scCommandAllocator[i]->SetName(std::format(L"_scCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandAllocator[i], (IUnknown**) &allocator))
                _scCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _scCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_scCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _scCommandList[i]->SetName(std::format(L"_scCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandList[i], (IUnknown**) &cmdList))
                _scCommandList[i] = cmdList;

            result = _scCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_scCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }
        }

        // Create fences for UI command allocator synchronization
        if (!CreateUIFences())
        {
            LOG_ERROR("CreateUIFences failed");
        }
    } while (false);
}

bool FSRFG_Dx12::Present()
{
    auto fIndex = GetIndexWillBeDispatched();

    bool drawUiOverFg = Config::Instance()->FGDrawUIOverFG.value_or_default();

    if (drawUiOverFg)
    {
        auto ui = GetResource(FG_ResourceType::UIColor, fIndex);
        if (ui == nullptr)
        {
            static uint64_t missingUiLogCount = 0;
            missingUiLogCount++;
            if (missingUiLogCount <= 5 || missingUiLogCount % 600 == 0)
            {
                LOG_DEBUG("{} ignored: UI resource is nullptr",
                    FrameWarpRuntime::IsFsrfgPresentCallbackRequested()
                        ? "DrawUIOverFG under FrameWarp"
                        : "DrawUIOverFG");
            }
        }
        else
        {
            LOG_DEBUG("UI[{}] resource: {:X}, copy: {}", fIndex, (size_t) ui->resource, (size_t) ui->copy);
            if (_renderUI.get() == nullptr)
            {
                _renderUI = std::make_unique<RUI_Dx12>("RenderUI", _device,
                                                       Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
            }
            else
            {
                if (Config::Instance()->FGUIPremultipliedAlpha.value_or_default() != _renderUI->IsPreMultipliedAlpha())
                {
                    LOG_INFO("UI premultiplied alpha changed, recreating RenderUI");
                    _renderUI = std::make_unique<RUI_Dx12>(
                        "RenderUI", _device, Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
                }
                else if (_renderUI->IsInit())
                {
                    auto commandList = GetSCCommandList(fIndex);
                    _renderUI->Dispatch((IDXGISwapChain3*) _swapChain, commandList, ui->GetResource(), ui->state);
                }
            }
        }
    }

    if (IsActive() && !IsPaused() && State::Instance().FGHudlessCompare)
    {
        auto hudless = GetResource(FG_ResourceType::HudlessColor, fIndex);
        if (hudless != nullptr)
        {
            if (_hudlessCompare.get() == nullptr)
            {
                _hudlessCompare = std::make_unique<HC_Dx12>("HudlessCompare", _device);
            }
            else
            {
                if (_hudlessCompare->IsInit())
                {
                    auto commandList = GetSCCommandList(fIndex);
                    _hudlessCompare->Dispatch((IDXGISwapChain3*) _swapChain, commandList, hudless->GetResource(),
                                              hudless->state);
                }
            }
        }
    }

    bool result = false;

    // if (IsActive() && !IsPaused())
    {
        if (_uiCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _uiCommandList[{}]: {:X}", fIndex, (size_t) _uiCommandList[fIndex]);
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

        if (_scCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _scCommandList[{}]: {:X}", fIndex, (size_t) _scCommandList[fIndex]);
            auto closeResult = _scCommandList[fIndex]->Close();

            if (closeResult == S_OK)
                _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_scCommandList[fIndex]);
            else
                LOG_ERROR("_scCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            _scCommandListResetted[fIndex] = false;
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
