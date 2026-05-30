#pragma once

#include <Config.h>
#include <State.h>

#include <cstdint>
#include <cstring>
#include <d3d12.h>
#include <dxgi.h>

namespace FrameWarpDLSSG
{
enum class Mode : uint32_t
{
    Off = 0,
    TelemetryOnly = 1,
    DistortionField = 2,
    SafeLatePresent = 3,
    ResourceCopyWarp = 4,
};

struct ResourceWarpPolicy
{
    uint32_t mode = 0;
    const char* modeName = "off";
    bool frameWarpEnabled = false;
    bool dlssgActive = false;
    bool resourceCopyEnabled = false;
    bool detailedTrace = false;
    bool strictHudlessUi = false;
};

inline const char* ModeName(uint32_t mode)
{
    switch (static_cast<Mode>(mode))
    {
    case Mode::Off: return "off";
    case Mode::TelemetryOnly: return "telemetry";
    case Mode::DistortionField: return "distortion-field";
    case Mode::SafeLatePresent: return "safe-late-present";
    case Mode::ResourceCopyWarp: return "resource-copy-warp";
    default: return "unknown";
    }
}

inline bool IsDLSSGActive(const State& state)
{
    return state.activeFgOutput == FGOutput::DLSSG ||
           state.dlssgNativeStreamlineDetected ||
           state.dlssgNativeAttachActive ||
           state.dlssgNativePassthroughActive;
}

inline ResourceWarpPolicy GetResourceWarpPolicy()
{
    auto config = Config::Instance();
    auto& state = State::Instance();

    ResourceWarpPolicy policy {};
    policy.mode = config->FrameWarpDLSSGMode.value_or_default();
    policy.modeName = ModeName(policy.mode);
    policy.frameWarpEnabled = config->FrameWarpEnabled.value_or_default();
    policy.dlssgActive = IsDLSSGActive(state);
    policy.resourceCopyEnabled = policy.frameWarpEnabled && policy.mode == static_cast<uint32_t>(Mode::ResourceCopyWarp);
    policy.detailedTrace = config->FrameWarpTimingAuditLog.value_or_default();
    policy.strictHudlessUi = policy.resourceCopyEnabled && config->FGHUDFix.value_or_default();
    return policy;
}

inline bool ShouldTraceQueue()
{
    const auto policy = GetResourceWarpPolicy();
    return policy.dlssgActive && (policy.detailedTrace || policy.resourceCopyEnabled);
}

inline bool ShouldEvaluateResourceCopy()
{
    const auto policy = GetResourceWarpPolicy();
    return policy.dlssgActive && policy.resourceCopyEnabled;
}

inline bool IsResourceCopyModule(const char* moduleName)
{
    return moduleName != nullptr && _stricmp(moduleName, "sl.common.dll") == 0;
}

inline bool IsStreamlineModule(const char* moduleName)
{
    if (moduleName == nullptr)
        return false;

    return _stricmp(moduleName, "sl.common.dll") == 0 ||
           _stricmp(moduleName, "sl.dlss_g.dll") == 0 ||
           _stricmp(moduleName, "sl.interposer.dll") == 0;
}

inline bool IsResourceCopyQueueShape(const D3D12_COMMAND_QUEUE_DESC& desc)
{
    return desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
           desc.Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
}

inline bool IsSimpleCopyTraceShape(size_t resourceCount, uint64_t eventCount)
{
    return resourceCount == 2 && eventCount >= 4 && eventCount <= 12;
}

inline bool IsDirectCopySafeFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

inline void CopyReason(char* dest, size_t destSize, const char* reason)
{
    if (dest == nullptr || destSize == 0)
        return;

    strncpy_s(dest, destSize, reason != nullptr ? reason : "unknown", _TRUNCATE);
}

inline bool IsCompatibleOutputTexture(D3D12_RESOURCE_DIMENSION dimension,
                                      UINT64 width,
                                      UINT height,
                                      DXGI_FORMAT format,
                                      const DXGI_SWAP_CHAIN_DESC& swapchainDesc,
                                      char* reason,
                                      size_t reasonSize)
{
    if (dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        CopyReason(reason, reasonSize, "resource is not texture2d");
        return false;
    }

    if (width == 0 || height == 0 ||
        swapchainDesc.BufferDesc.Width == 0 || swapchainDesc.BufferDesc.Height == 0)
    {
        CopyReason(reason, reasonSize, "resource size unavailable");
        return false;
    }

    if (width != swapchainDesc.BufferDesc.Width || height != swapchainDesc.BufferDesc.Height)
    {
        CopyReason(reason, reasonSize, "resource size mismatch");
        return false;
    }

    if (!IsDirectCopySafeFormat(format))
    {
        CopyReason(reason, reasonSize, "resource format unsupported");
        return false;
    }

    CopyReason(reason, reasonSize, "compatible");
    return true;
}
}
