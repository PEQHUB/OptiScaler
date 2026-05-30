#include "pch.h"
#include "NativeLowLatency.h"

#include <State.h>
#include <proxies/XeLL_Proxy.h>
#include <proxies/FfxApi_Proxy.h>
#include <nvapi/fakenvapi.h>

// Anti-Lag 2 SDK (header-only, MIT licensed)
#include <ffx_antilag2_dx12.h>

#include <magic_enum.hpp>

// Helper to map Reflex marker types to XeLL marker types
static xell_latency_marker_type_t ReflexToXeLL(NV_LATENCY_MARKER_TYPE reflexMarker)
{
    switch (reflexMarker)
    {
    case SIMULATION_START:
        return XELL_SIMULATION_START;
    case SIMULATION_END:
        return XELL_SIMULATION_END;
    case RENDERSUBMIT_START:
        return XELL_RENDERSUBMIT_START;
    case RENDERSUBMIT_END:
        return XELL_RENDERSUBMIT_END;
    case PRESENT_START:
        return XELL_PRESENT_START;
    case PRESENT_END:
        return XELL_PRESENT_END;
    case INPUT_SAMPLE:
        return XELL_INPUT_SAMPLE;
    default:
        return (xell_latency_marker_type_t) -1;
    }
}

uint32_t NativeLowLatency::DetectGPUVendor(ID3D12Device* device)
{
    // Fast path: use VendorId already detected by D3D12_Hooks during device creation
    if (State::Instance().gpuVendorId != 0)
    {
        LOG_INFO("NativeLowLatency: Using cached GPU vendor: {:#x}", State::Instance().gpuVendorId);
        return State::Instance().gpuVendorId;
    }

    // Fallback: use LUID from D3D12 device to find matching DXGI adapter
    // NOTE: ID3D12Device does NOT implement IDXGIDevice, so we can't QI for it.
    //       Instead we match by adapter LUID.
    if (!device)
    {
        LOG_WARN("NativeLowLatency: No device provided for vendor detection");
        return 0;
    }

    LUID luid = device->GetAdapterLuid();
    LOG_DEBUG("NativeLowLatency: Device LUID: {:08X}-{:08X}", luid.HighPart, luid.LowPart);

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
    {
        LOG_WARN("NativeLowLatency: Failed to create DXGI factory for vendor detection");
        return 0;
    }

    uint32_t vendorId = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
    {
        DXGI_ADAPTER_DESC1 desc {};
        ScopedSkipSpoofing skipSpoofing {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            desc.AdapterLuid.LowPart == luid.LowPart &&
            desc.AdapterLuid.HighPart == luid.HighPart)
        {
            vendorId = desc.VendorId;
            adapter->Release();
            break;
        }
        adapter->Release();
    }
    factory->Release();

    if (vendorId != 0)
    {
        LOG_INFO("NativeLowLatency: Detected GPU vendor via LUID: {:#x}", vendorId);
        State::Instance().gpuVendorId = vendorId;
    }
    else
    {
        LOG_WARN("NativeLowLatency: Could not find matching adapter by LUID");
    }

    return vendorId;
}

bool NativeLowLatency::Initialize(ID3D12Device* device, uint32_t userChoice)
{
    LOG_INFO("NativeLowLatency: Initialize called, userChoice={}, device={:X}",
             userChoice, (size_t)device);

    if (_initialized)
    {
        LOG_DEBUG("NativeLowLatency: Already initialized with backend {}",
                  magic_enum::enum_name(_backend));
        return _backend != NativeLatencyBackend::None;
    }

    auto method = static_cast<LatencyMethod>(userChoice);
    LOG_INFO("NativeLowLatency: LatencyMethod = {} ({})", userChoice, magic_enum::enum_name(method));

    // Off = disable everything
    if (method == LatencyMethod::Off)
    {
        _backend = NativeLatencyBackend::None;
        _initialized = true;
        LOG_INFO("NativeLowLatency: Disabled by user");
        return false;
    }

    // Reflex = pass-through to original NvAPI
    if (method == LatencyMethod::Reflex)
    {
        _backend = NativeLatencyBackend::Reflex;
        _initialized = true;
        LOG_INFO("NativeLowLatency: Using Reflex pass-through");
        return false; // Not "available" in the native sense - hooks use original path
    }

    // LatencyFlex = route to fakenvapi
    if (method == LatencyMethod::LatencyFlex)
    {
        _backend = NativeLatencyBackend::LatencyFlex;
        _initialized = true;
        LOG_INFO("NativeLowLatency: Using LatencyFlex via fakenvapi");
        return false; // Not "available" natively - hooks use fakenvapi path
    }

    _gpuVendorId = DetectGPUVendor(device);
    LOG_INFO("NativeLowLatency: GPU vendor detected: {:#x}", _gpuVendorId);

    // Auto-detect or user-forced backend
    bool tryAntiLag2 = (method == LatencyMethod::Auto && _gpuVendorId == VendorId::AMD) ||
                       method == LatencyMethod::AntiLag2;
    bool tryXeLL = (method == LatencyMethod::Auto && _gpuVendorId == VendorId::Intel) ||
                   method == LatencyMethod::XeLL;

    LOG_INFO("NativeLowLatency: tryAntiLag2={}, tryXeLL={}", tryAntiLag2, tryXeLL);

    // Try Anti-Lag 2
    if (tryAntiLag2)
    {
        auto* ctx = new AMD::AntiLag2DX12::Context {};
        HRESULT hr = AMD::AntiLag2DX12::Initialize(ctx, device);

        if (hr == S_OK)
        {
            _antiLag2Context = ctx;
            _backend = NativeLatencyBackend::AntiLag2;
            _initialized = true;
            State::Instance().nativeLowLatencyAvailable = true;
            LOG_INFO("NativeLowLatency: Anti-Lag 2 initialized successfully");
            return true;
        }
        else
        {
            delete ctx;
            LOG_WARN("NativeLowLatency: Anti-Lag 2 initialization failed: {:#x}", (uint32_t) hr);

            if (method == LatencyMethod::AntiLag2)
            {
                // User explicitly wanted AL2 but it failed
                _backend = NativeLatencyBackend::None;
                _initialized = true;
                return false;
            }
            // Auto mode: fall through
        }
    }

    // Try XeLL
    if (tryXeLL)
    {
        // XeLL uses XeLLProxy's shared context
        if (XeLLProxy::Context() != nullptr || XeLLProxy::CreateContext(device))
        {
            _backend = NativeLatencyBackend::XeLL;
            _initialized = true;
            State::Instance().nativeLowLatencyAvailable = true;
            LOG_INFO("NativeLowLatency: XeLL initialized successfully");
            return true;
        }
        else
        {
            LOG_WARN("NativeLowLatency: XeLL initialization failed");

            if (method == LatencyMethod::XeLL)
            {
                _backend = NativeLatencyBackend::None;
                _initialized = true;
                return false;
            }
        }
    }

    // Nothing worked
    _backend = NativeLatencyBackend::None;
    _initialized = true;
    LOG_INFO("NativeLowLatency: No native backend available, will use fallback");
    return false;
}

void NativeLowLatency::DeInitialize()
{
    if (_backend == NativeLatencyBackend::AntiLag2 && _antiLag2Context)
    {
        auto* ctx = static_cast<AMD::AntiLag2DX12::Context*>(_antiLag2Context);
        auto refCount = AMD::AntiLag2DX12::DeInitialize(ctx);
        LOG_INFO("NativeLowLatency: Anti-Lag 2 deinitialized, refcount: {}", refCount);
        delete ctx;
        _antiLag2Context = nullptr;
    }

    // XeLL context is managed by XeLLProxy - don't destroy it here
    // as XeFG_Dx12 may still need it

    _backend = NativeLatencyBackend::None;
    _initialized = false;
    _enabled = false;
    _maxFPS = 0;
    _currentFrameId = 0;
    State::Instance().nativeLowLatencyAvailable = false;
}

NativeLatencyBackend NativeLowLatency::GetBackend() { return _backend; }

bool NativeLowLatency::IsAvailable()
{
    return _initialized && (_backend == NativeLatencyBackend::AntiLag2 || _backend == NativeLatencyBackend::XeLL);
}

bool NativeLowLatency::IsInitialized() { return _initialized; }

bool NativeLowLatency::CanInitBackend(NativeLatencyBackend backend)
{
    switch (backend)
    {
    case NativeLatencyBackend::AntiLag2:
        return _gpuVendorId == VendorId::AMD || GetModuleHandleA("amdxc64.dll") != nullptr;
    case NativeLatencyBackend::XeLL:
        return _gpuVendorId == VendorId::Intel || XeLLProxy::Module() != nullptr;
    case NativeLatencyBackend::Reflex:
        return true; // Always available (pass-through)
    case NativeLatencyBackend::LatencyFlex:
        return true; // Always available via fakenvapi
    default:
        return false;
    }
}

NvAPI_Status NativeLowLatency::SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* params)
{
    if (!IsAvailable() || !params)
        return NVAPI_ERROR;

    _enabled = params->bLowLatencyMode;

    if (_backend == NativeLatencyBackend::AntiLag2)
    {
        // Only update cached state here — the actual Update() call happens in Sleep()
        // to avoid calling Update() twice per frame (which would double the frame-pacing delay)
        unsigned int maxFPS = 0;
        if (params->minimumIntervalUs > 0)
            maxFPS = static_cast<unsigned int>(std::round(1000000.0 / params->minimumIntervalUs));

        _maxFPS = maxFPS;
        return NVAPI_OK;
    }
    else if (_backend == NativeLatencyBackend::XeLL)
    {
        auto xellCtx = XeLLProxy::Context();
        if (!xellCtx || !XeLLProxy::SetSleepMode())
            return NVAPI_ERROR;

        xell_sleep_params_t sleepParams {};
        sleepParams.bLowLatencyMode = _enabled ? 1 : 0;
        sleepParams.minimumIntervalUs = params->minimumIntervalUs;
        _maxFPS = params->minimumIntervalUs > 0
                      ? static_cast<unsigned int>(std::round(1000000.0 / params->minimumIntervalUs))
                      : 0;

        auto result = XeLLProxy::SetSleepMode()(xellCtx, &sleepParams);
        return (result == XELL_RESULT_SUCCESS) ? NVAPI_OK : NVAPI_ERROR;
    }

    return NVAPI_ERROR;
}

NvAPI_Status NativeLowLatency::Sleep(IUnknown* pDev)
{
    if (!IsAvailable())
        return NVAPI_ERROR;

    if (_backend == NativeLatencyBackend::AntiLag2)
    {
        auto* ctx = static_cast<AMD::AntiLag2DX12::Context*>(_antiLag2Context);
        // Update with current state inserts the latency-reducing delay
        HRESULT hr = AMD::AntiLag2DX12::Update(ctx, _enabled, _maxFPS);
        return (hr == S_OK) ? NVAPI_OK : NVAPI_ERROR;
    }
    else if (_backend == NativeLatencyBackend::XeLL)
    {
        auto xellCtx = XeLLProxy::Context();
        if (!xellCtx || !XeLLProxy::Sleep())
            return NVAPI_ERROR;

        auto result = XeLLProxy::Sleep()(xellCtx, static_cast<uint32_t>(_currentFrameId));
        return (result == XELL_RESULT_SUCCESS) ? NVAPI_OK : NVAPI_ERROR;
    }

    return NVAPI_ERROR;
}

NvAPI_Status NativeLowLatency::SetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* params)
{
    if (!IsAvailable() || !params)
        return NVAPI_ERROR;

    _currentFrameId = params->frameID;

    if (_backend == NativeLatencyBackend::AntiLag2)
    {
        auto* ctx = static_cast<AMD::AntiLag2DX12::Context*>(_antiLag2Context);

        // Anti-Lag 2 only needs MarkEndOfFrameRendering on RENDERSUBMIT_END
        if (params->markerType == RENDERSUBMIT_END)
        {
            AMD::AntiLag2DX12::MarkEndOfFrameRendering(ctx);
        }
        // SetFrameGenFrameType is handled in ReportFGPresent instead

        return NVAPI_OK;
    }
    else if (_backend == NativeLatencyBackend::XeLL)
    {
        auto xellCtx = XeLLProxy::Context();
        if (!xellCtx || !XeLLProxy::AddMarkerData())
            return NVAPI_ERROR;

        auto xellMarker = ReflexToXeLL(params->markerType);
        if (static_cast<int>(xellMarker) < 0)
            return NVAPI_OK; // Unknown marker, skip

        auto result = XeLLProxy::AddMarkerData()(xellCtx, static_cast<uint32_t>(params->frameID), xellMarker);
        return (result == XELL_RESULT_SUCCESS) ? NVAPI_OK : NVAPI_ERROR;
    }

    return NVAPI_ERROR;
}

NvAPI_Status NativeLowLatency::GetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* params)
{
    if (!IsAvailable() || !params)
        return NVAPI_ERROR;

    if (_backend == NativeLatencyBackend::AntiLag2)
    {
        // Anti-Lag 2 has no native latency reporting API
        // Return success with zeroed data - the timing graph will be blank
        memset(params, 0, sizeof(NV_LATENCY_RESULT_PARAMS));
        params->version = NV_LATENCY_RESULT_PARAMS_VER;
        return NVAPI_OK;
    }
    else if (_backend == NativeLatencyBackend::XeLL)
    {
        auto xellCtx = XeLLProxy::Context();
        if (!xellCtx)
            return NVAPI_ERROR;

        // Try to get XeLL frame reports and map to Reflex format
        // xellGetFramesReports returns last 64 frame reports
        // For now, return zeroed data (XeLL report format doesn't map 1:1 to Reflex)
        memset(params, 0, sizeof(NV_LATENCY_RESULT_PARAMS));
        params->version = NV_LATENCY_RESULT_PARAMS_VER;
        return NVAPI_OK;
    }

    return NVAPI_ERROR;
}

void NativeLowLatency::SetFPSCap(float fps)
{
    if (!IsAvailable())
        return;

    if (_backend == NativeLatencyBackend::AntiLag2)
    {
        auto* ctx = static_cast<AMD::AntiLag2DX12::Context*>(_antiLag2Context);
        unsigned int maxFPS = (fps > 0.0f) ? static_cast<unsigned int>(std::round(fps)) : 0;
        _maxFPS = maxFPS;
        AMD::AntiLag2DX12::Update(ctx, _enabled, maxFPS);
        LOG_INFO("NativeLowLatency: Anti-Lag 2 FPS cap set to {}", maxFPS);
    }
    else if (_backend == NativeLatencyBackend::XeLL)
    {
        auto xellCtx = XeLLProxy::Context();
        if (!xellCtx || !XeLLProxy::SetSleepMode())
            return;

        uint32_t intervalUs = (fps > 0.0f) ? static_cast<uint32_t>(std::round(1000000.0 / fps)) : 0;
        _maxFPS = (fps > 0.0f) ? static_cast<unsigned int>(std::round(fps)) : 0;

        xell_sleep_params_t sleepParams {};
        sleepParams.bLowLatencyMode = _enabled ? 1 : 0;
        sleepParams.minimumIntervalUs = intervalUs;

        XeLLProxy::SetSleepMode()(xellCtx, &sleepParams);
        LOG_INFO("NativeLowLatency: XeLL FPS cap set to {} ({}us)", _maxFPS, intervalUs);
    }
}

void NativeLowLatency::ReportFGPresent(IDXGISwapChain* pSwapChain, bool fg_state, bool frame_interpolated)
{
    if (!IsAvailable())
        return;

    if (_backend == NativeLatencyBackend::AntiLag2)
    {
        auto* ctx = static_cast<AMD::AntiLag2DX12::Context*>(_antiLag2Context);

        if (fg_state)
        {
            if (State::Instance().activeFgOutput == FGOutput::FSRFG)
            {
                // For FSR >= 3.1.1 we pass Anti-Lag 2 context via swapchain private data
                // so FSR FG calls SetFrameGenFrameType itself
                auto ffxApiVersion = FfxApiProxy::VersionDx12();
                constexpr feature_version requiredVersion = {3, 1, 1};
                if (ffxApiVersion >= requiredVersion)
                {
                    struct AntiLag2Data
                    {
                        void* context;
                        bool enabled;
                    };

                    AntiLag2Data data {};
                    data.enabled = _enabled;
                    data.context = _enabled ? _antiLag2Context : nullptr;

                    pSwapChain->SetPrivateData(fakenvapi::IID_IFfxAntiLag2Data, sizeof(data), &data);
                }
                else
                {
                    // Older FSR: call SetFrameGenFrameType directly
                    AMD::AntiLag2DX12::SetFrameGenFrameType(ctx, frame_interpolated);
                }
            }
            else
            {
                // XeFG or DLSSG output: call SetFrameGenFrameType directly
                AMD::AntiLag2DX12::SetFrameGenFrameType(ctx, frame_interpolated);
            }
        }
        else
        {
            // FG disabled: clear private data
            pSwapChain->SetPrivateData(fakenvapi::IID_IFfxAntiLag2Data, 0, nullptr);
        }
    }
    // XeLL: no action needed - XeFG_Dx12 manages XeLL context for frame generation
}

void* NativeLowLatency::GetAntiLag2Context()
{
    return (_backend == NativeLatencyBackend::AntiLag2) ? _antiLag2Context : nullptr;
}
