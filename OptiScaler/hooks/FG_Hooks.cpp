#include "pch.h"
#include "FG_Hooks.h"
#include <Config.h>

#include <framegen/ffx/FSRFG_Dx12.h>
#include <framegen/xefg/XeFG_Dx12.h>
#include <framegen/dlssg/DLSSG_Dx12.h>
#include <framegen/dlssg/DLSSG_Native.h>

#include <inputs/FG/FSR3_Dx12_FG.h>
#include <inputs/FG/FfxApi_Dx12_FG.h>

#include <hudfix/Hudfix_Dx12.h>
#include <framewarp/FrameWarp.h>
#include <resource_tracking/ResTrack_Dx12.h>

#include <misc/FrameLimit.h>
#include <menu/menu_overlay_dx.h>
#include <upscaler_time/UpscalerTime_Dx12.h>

#include <detours/detours.h>

#include <d3d12.h>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <limits>

#define XEFG_RESOURCE_REF_LIMIT 1

inline static ID3D12Fence* resizeFence = nullptr;
inline static UINT64 resizeFenceValue = 0;
inline static HANDLE resizeFenceEvent = nullptr;
inline static IUnknown* oldSwapChain = nullptr;
inline static ID3D12CommandQueue* currentCommandQueue = nullptr;
inline static bool _forcedHdrForXeFG = false;
inline static thread_local uint32_t _xefgInternalPresentDepth = 0;

static const char* VF2PresentTraceModuleFromAddress(void* address, char (&moduleName)[MAX_PATH])
{
    moduleName[0] = '\0';
    if (address == nullptr)
        return "unknown";

    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address),
                            &module) ||
        module == nullptr)
    {
        return "unknown";
    }

    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(module, path, MAX_PATH) == 0)
        return "unknown";

    const char* slash = strrchr(path, '\\');
    const char* fwdSlash = strrchr(path, '/');
    if (fwdSlash != nullptr && (slash == nullptr || fwdSlash > slash))
        slash = fwdSlash;

    strncpy_s(moduleName, slash != nullptr ? slash + 1 : path, _TRUNCATE);
    return moduleName[0] != '\0' ? moduleName : "unknown";
}

static void TraceVF2PresentCaller(const char* method,
                                  IDXGISwapChain* swapChain,
                                  UINT syncInterval,
                                  UINT flags,
                                  const DXGI_PRESENT_PARAMETERS* presentParameters,
                                  bool skipPresent,
                                  bool skipPresent1,
                                  void* caller)
{
    auto& state = State::Instance();
    if (!Config::Instance()->FrameWarpTimingAuditLog.value_or_default() ||
        !(state.activeFgOutput == FGOutput::DLSSG ||
          state.dlssgNativeStreamlineDetected ||
          state.dlssgNativeAttachActive ||
          state.dlssgNativePassthroughActive))
    {
        return;
    }

    static uint64_t presentCallerCount = 0;
    presentCallerCount++;
    if (presentCallerCount > 900 && presentCallerCount % 30 != 0)
        return;

    char callerModule[MAX_PATH] = {};
    const char* moduleName = VF2PresentTraceModuleFromAddress(caller, callerModule);
    auto& status = state.frameWarpStatus;
    UINT backBufferIndex = UINT_MAX;
    IDXGISwapChain3* swapChain3 = nullptr;
    if (swapChain != nullptr && SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) && swapChain3 != nullptr)
    {
        backBufferIndex = swapChain3->GetCurrentBackBufferIndex();
        swapChain3->Release();
    }

    LOG_INFO("VF2PresentCaller #{} frame={} method={} caller={} callerAddr={:X} swapchain={:X} backbuffer={} sync={} flags={:X} params={} dirty={} scrollRect={} scrollOffset={} skipPresent={} skipPresent1={} commandQueue={:X} activeFg={} renderSnapshot={} rawSeq={} snapSeq={} sinceSnap={} slPresented={} nativeDetected={} nativeAttach={} nativePassthrough={}",
             presentCallerCount,
             state.frameCount,
             method != nullptr ? method : "Present",
             moduleName,
             reinterpret_cast<uintptr_t>(caller),
             reinterpret_cast<uintptr_t>(swapChain),
             backBufferIndex,
             syncInterval,
             flags,
             presentParameters != nullptr,
             presentParameters != nullptr ? presentParameters->DirtyRectsCount : 0,
             presentParameters != nullptr && presentParameters->pScrollRect != nullptr,
             presentParameters != nullptr && presentParameters->pScrollOffset != nullptr,
             skipPresent,
             skipPresent1,
             reinterpret_cast<uintptr_t>(currentCommandQueue),
             static_cast<uint32_t>(state.activeFgOutput),
             status.renderSnapshotCount,
             status.rawInputSequence,
             status.rawInputSnapshotSequence,
             status.rawInputSamplesSinceSnapshot,
             status.dlssgLastFramesPresented,
             state.dlssgNativeStreamlineDetected,
             state.dlssgNativeAttachActive,
             state.dlssgNativePassthroughActive);
}

#if (XEFG_RESOURCE_REF_LIMIT == 0)
inline static std::vector<void*> oldBackBuffers;
#endif

static void PauseFG(IFGFeature_Dx12* fg)
{
    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().FGchanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }
}

struct ScopedXeFGInternalPresent
{
    bool active = false;

    ScopedXeFGInternalPresent()
    {
        active = State::Instance().activeFgOutput == FGOutput::XeFG;
        if (active)
            _xefgInternalPresentDepth++;
    }

    ~ScopedXeFGInternalPresent()
    {
        if (active && _xefgInternalPresentDepth > 0)
            _xefgInternalPresentDepth--;
    }
};

bool FGHooks::IsXeFGInternalPresentActive()
{
    return _xefgInternalPresentDepth > 0;
}

static void WaitForGPUIdle()
{
    if (currentCommandQueue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
    {
        LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

        resizeFenceValue++;
        currentCommandQueue->Signal(resizeFence, resizeFenceValue);

        if (resizeFence->GetCompletedValue() < resizeFenceValue)
        {
            resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
            // Max 5 sec
            auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
            LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
        }
    }
}

static bool CheckForFGStatus()
{
    if (State::Instance().activeFgInput == FGInput::NoFG || State::Instance().activeFgInput == FGInput::Nukems)
        return false;

    // Disable FG if amd dll is not found
    if (State::Instance().activeFgOutput == FGOutput::FSRFG)
    {
        FfxApiProxy::InitFfxDx12();
        if (!FfxApiProxy::IsFGReady())
        {
            LOG_DEBUG("Can't init FfxApiProxy, disabling FGOutput");
            Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
            State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();
        }
    }
    else if (State::Instance().activeFgOutput == FGOutput::XeFG && !XeFGProxy::InitXeFG())
    {
        LOG_DEBUG("Can't init XeFGProxy, disabling FGOutput");
        Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
        State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();
    }
    else if (State::Instance().activeFgOutput == FGOutput::DLSSG && DLSSGNative::ShouldSkipOptiDLSSGContext())
    {
        static uint64_t nativeDlssgSkipLogCount = 0;
        nativeDlssgSkipLogCount++;
        if (nativeDlssgSkipLogCount <= 10 || nativeDlssgSkipLogCount % 300 == 0)
        {
            LOG_INFO("Native DLSSG {} mode active; skipping OptiScaler-owned DLSSG swapchain",
                     DLSSGNative::ModeName(Config::Instance()->FGDLSSGNativeMode.value_or_default()));
        }
        return false;
    }
    else if (State::Instance().activeFgOutput == FGOutput::DLSSG && !SLProxy::InitSL())
    {
        LOG_DEBUG("Can't init SLProxy, disabling FGOutput");
        Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
        State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();
    }

    if (State::Instance().activeFgOutput != FGOutput::FSRFG && State::Instance().activeFgOutput != FGOutput::XeFG &&
        State::Instance().activeFgOutput != FGOutput::DLSSG)
    {
        LOG_WARN("FGOutput is not set to FSR-FG, XeFG, or DLSS-G");
        return false;
    }

    return true;
}

static void LogFGSwapchainInitSkipped(const char* functionName)
{
    if (State::Instance().activeFgOutput == FGOutput::DLSSG && DLSSGNative::ShouldSkipOptiDLSSGContext())
    {
        static uint64_t nativeDlssgSwapchainSkipLogCount = 0;
        nativeDlssgSwapchainSkipLogCount++;
        if (nativeDlssgSwapchainSkipLogCount <= 10 || nativeDlssgSwapchainSkipLogCount % 300 == 0)
        {
            LOG_DEBUG("{} Native DLSSG owns the swapchain; passing through OptiScaler FG swapchain creation",
                      functionName != nullptr ? functionName : "FGHooks");
        }
        return;
    }

    LOG_WARN("{} Can't init FG Feature or invalid FGOutput setting!",
             functionName != nullptr ? functionName : "FGHooks");
}

HRESULT FGHooks::CreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                 IDXGISwapChain** ppSwapChain)
{
    if (!CheckForFGStatus())
    {
        LogFGSwapchainInitSkipped("FGHooks::CreateSwapChain");
        return E_NOINTERFACE;
    }

    // Check if it's Dx12
    ID3D12CommandQueue* cq = nullptr;
    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) != S_OK)
    {
        LOG_ERROR("FG Feature requires D3D12 Command Queue!");
        return E_INVALIDARG;
    }

    currentCommandQueue = cq;
    cq->Release();

    if (State::Instance().currentFG == nullptr)
    {
        // FG Init
        if (State::Instance().activeFgOutput == FGOutput::FSRFG)
        {
            State::Instance().currentFG = new FSRFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            State::Instance().currentFG = new XeFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::DLSSG)
        {
            State::Instance().currentFG =
                new DLSSG_Dx12(Config::Instance()->FGDLSSGInterpolationCount.value_or_default());
        }
    }

    // Create FG swapchain
    auto fg = State::Instance().currentFG;
    bool scResult = false;

    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = true;

        if (State::Instance().activeFgOutput == FGOutput::XeFG && !pDesc->Windowed)
            LOG_WARN("Using exclusive fullscreen with XeFG!!!");

        // These effects are not supported in DX12
        if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_SEQUENTIAL is not supported in DX12, changing to FLIP_SEQUENTIAL");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        }
        else if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_DISCARD is not supported in DX12, changing to FLIP_DISCARD");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        }

        // Looks like game is creating new swapchain,
        // without releasing old one, be sure gpu is in idle state
        if (!Config::Instance()->FGPreserveSwapChain.value_or_default() &&
            State::Instance().currentFGSwapchain != nullptr)
        {
            LOG_WARN("Looks like game is creating new swapchain, without releasing old one!");

            WaitForGPUIdle();
            oldSwapChain = State::Instance().currentFGSwapchain;
        }

        scResult = fg->CreateSwapchain(pFactory, cq, pDesc, ppSwapChain, true);

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = false;
    }

    if (scResult)
    {
        if (State::Instance().currentD3D12Device != nullptr)
        {
            if (resizeFence != nullptr)
            {
                resizeFence->Release();
                resizeFence = nullptr;
            }

            if (resizeFenceEvent != nullptr)
            {
                CloseHandle(resizeFenceEvent);
                resizeFenceEvent = nullptr;
            }

            State::Instance().currentD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence));
            resizeFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }

        _hwnd = pDesc->OutputWindow;
        State::Instance().currentFGSwapchain = *ppSwapChain;

        HookFGSwapchain(*ppSwapChain);

        State::Instance().currentSwapchain = *ppSwapChain;

        return S_OK;
    }

    return E_INVALIDARG;
}

HRESULT FGHooks::CreateSwapChainForHwnd(IDXGIFactory* pFactory, IUnknown* pDevice, HWND hWnd,
                                        DXGI_SWAP_CHAIN_DESC1* pDesc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                        IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    if (!CheckForFGStatus())
    {
        LogFGSwapchainInitSkipped("FGHooks::CreateSwapChainForHwnd");
        return E_NOINTERFACE;
    }

    // Check if it's Dx12
    ID3D12CommandQueue* cq = nullptr;
    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) != S_OK)
    {
        LOG_ERROR("FG Feature requires D3D12 Command Queue!");
        return E_INVALIDARG;
    }

    currentCommandQueue = cq;
    cq->Release();

    if (State::Instance().currentFG == nullptr)
    {
        // FG Init
        if (State::Instance().activeFgOutput == FGOutput::FSRFG)
        {
            State::Instance().currentFG = new FSRFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            State::Instance().currentFG = new XeFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::DLSSG)
        {
            State::Instance().currentFG =
                new DLSSG_Dx12(Config::Instance()->FGDLSSGInterpolationCount.value_or_default());
        }
    }

    // Create FG swapchain
    auto fg = State::Instance().currentFG;
    bool scResult = false;
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = true;

        if (State::Instance().activeFgOutput == FGOutput::XeFG && pFullscreenDesc != nullptr &&
            !pFullscreenDesc->Windowed)
            LOG_WARN("Using exclusive fullscreen with XeFG!!!");

        // These effects are not supported in DX12
        if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_SEQUENTIAL is not supported in DX12, changing to FLIP_SEQUENTIAL");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        }
        else if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_DISCARD is not supported in DX12, changing to FLIP_DISCARD");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        }

        // Looks like game is creating new swapchain,
        // without releasing old one, be sure gpu is in idle state
        if (!Config::Instance()->FGPreserveSwapChain.value_or_default() &&
            State::Instance().currentFGSwapchain != nullptr)
        {
            LOG_WARN("Looks like game is creating new swapchain, without releasing old one!");

            WaitForGPUIdle();
            oldSwapChain = State::Instance().currentFGSwapchain;
        }

        scResult = fg->CreateSwapchain1(pFactory, cq, hWnd, pDesc, pFullscreenDesc, ppSwapChain, true);

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = false;
    }

    if (scResult)
    {
        if (State::Instance().currentD3D12Device != nullptr)
        {
            if (resizeFence != nullptr)
            {
                resizeFence->Release();
                resizeFence = nullptr;
            }

            if (resizeFenceEvent != nullptr)
            {
                CloseHandle(resizeFenceEvent);
                resizeFenceEvent = nullptr;
            }

            State::Instance().currentD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence));
            resizeFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }

        _hwnd = hWnd;
        State::Instance().currentFGSwapchain = *ppSwapChain;

        HookFGSwapchain(*ppSwapChain);
        State::Instance().currentSwapchain = *ppSwapChain;

        return S_OK;
    }

    return E_INVALIDARG;
}

void FGHooks::HookFGSwapchain(IDXGISwapChain* pSwapChain)
{
    if (o_FGSCPresent != nullptr || pSwapChain == nullptr)
        return;

    void** pFactoryVTable = *reinterpret_cast<void***>(pSwapChain);

    o_FGRelease = (PFN_Release) pFactoryVTable[2];
    o_FGSCPresent = (PFN_Present) pFactoryVTable[8];
    o_FGSCSetFullscreenState = (PFN_SetFullscreenState) pFactoryVTable[10];
    o_FGSCGetFullscreenState = (PFN_GetFullscreenState) pFactoryVTable[11];
    o_FGSCResizeBuffers = (PFN_ResizeBuffers) pFactoryVTable[13];
    o_FGSCResizeTarget = (PFN_ResizeTarget) pFactoryVTable[14];
    o_FGSCGetFullscreenDesc = (PFN_GetFullscreenDesc) pFactoryVTable[19];
    o_FGSCPresent1 = (PFN_Present1) pFactoryVTable[22];
    o_FGSCResizeBuffers1 = (PFN_ResizeBuffers1) pFactoryVTable[39];

    if (o_FGSCPresent != nullptr)
    {
        LOG_INFO("Hooking FG SwapChain present");
        LOG_TRACE("FGRelease: {:X}", (size_t) o_FGRelease);
        LOG_TRACE("FGSCPresent: {:X}", (size_t) o_FGSCPresent);
        LOG_TRACE("FGSCSetFullscreenState: {:X}", (size_t) o_FGSCSetFullscreenState);
        LOG_TRACE("FGSCGetFullscreenState: {:X}", (size_t) o_FGSCGetFullscreenState);
        LOG_TRACE("FGSCResizeBuffers: {:X}", (size_t) o_FGSCResizeBuffers);
        LOG_TRACE("FGSCResizeTarget: {:X}", (size_t) o_FGSCResizeTarget);
        LOG_TRACE("FGSCGetFullscreenDesc: {:X}", (size_t) o_FGSCGetFullscreenDesc);
        LOG_TRACE("FGSCPresent1: {:X}", (size_t) o_FGSCPresent1);
        LOG_TRACE("FGSCResizeBuffers1: {:X}", (size_t) o_FGSCResizeBuffers1);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_FGRelease, hkFGRelease);
        DetourAttach(&(PVOID&) o_FGSCPresent, hkFGPresent);
        DetourAttach(&(PVOID&) o_FGSCResizeTarget, hkResizeTarget);
        DetourAttach(&(PVOID&) o_FGSCResizeBuffers, hkResizeBuffers);
        DetourAttach(&(PVOID&) o_FGSCSetFullscreenState, hkSetFullscreenState);

        if (o_FGSCPresent1 != nullptr)
            DetourAttach(&(PVOID&) o_FGSCPresent1, hkFGPresent1);

        if (o_FGSCResizeBuffers1 != nullptr)
            DetourAttach(&(PVOID&) o_FGSCResizeBuffers1, hkResizeBuffers1);

        if (State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            DetourAttach(&(PVOID&) o_FGSCGetFullscreenState, hkGetFullscreenState);

            if (o_FGSCGetFullscreenDesc != nullptr)
                DetourAttach(&(PVOID&) o_FGSCGetFullscreenDesc, hkGetFullscreenDesc);
        }

        DetourTransactionCommit();
    }
}

HRESULT FGHooks::hkSetFullscreenState(IDXGISwapChain* This, BOOL Fullscreen, IDXGIOutput* pTarget)
{
    auto fg = State::Instance().currentFG;
    PauseFG(fg);

    bool modeChanged = false;
    bool orgFS = Fullscreen;
    if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        if (Fullscreen)
        {
            Fullscreen = false;

            if (!State::Instance().SCExclusiveFullscreen)
            {
                State::Instance().SCExclusiveFullscreen = true;
                modeChanged = true;
            }

            LOG_DEBUG("Prevented exclusive fullscreen");
        }
        else
        {
            if (State::Instance().SCExclusiveFullscreen)
            {
                modeChanged = true;
                State::Instance().SCExclusiveFullscreen = false;
            }
        }
    }

    State::Instance().realExclusiveFullscreen = Fullscreen;

    if (State::Instance().activeFgOutput == FGOutput::XeFG && Fullscreen)
        LOG_WARN("Using exclusive fullscreen with XeFG!!!");

    auto result = S_OK;

    if (!Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        result = o_FGSCSetFullscreenState(This, Fullscreen, pTarget);
        LOG_DEBUG("Fullscreen: {}, pTarget: {:X}, Result: {:X}", Fullscreen, (size_t) pTarget, (UINT) result);
    }

    if (result == S_OK && modeChanged)
    {
        LOG_DEBUG("Mode changed");

        DXGI_SWAP_CHAIN_DESC scDesc {};
        This->GetDesc(&scDesc);

        if (State::Instance().SCExclusiveFullscreen)
        {
            SetWindowLongPtr(_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

            Util::MonitorInfo info;

            if (pTarget != nullptr)
                info = Util::GetMonitorInfoForOutput(pTarget);
            else
                info = Util::GetMonitorInfoForWindow(_hwnd);

            LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                      info.y, wstring_to_string(info.name));

            SetWindowPos(_hwnd, HWND_TOP, info.x, info.y, info.width, info.height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        else
        {
            SetWindowLongPtr(_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_OVERLAPPEDWINDOW);
            SetWindowPos(_hwnd, nullptr, 0, 0, scDesc.BufferDesc.Width, scDesc.BufferDesc.Height,
                         SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    return result;
}

HRESULT FGHooks::hkGetFullscreenDesc(IDXGISwapChain1* This, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    auto result = o_FGSCGetFullscreenDesc(This, pDesc);

    if (result == S_OK && State::Instance().SCExclusiveFullscreen &&
        Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        pDesc->Windowed = false;
    }

    return result;
}

HRESULT FGHooks::hkGetFullscreenState(IDXGISwapChain* This, BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    auto result = o_FGSCGetFullscreenState(This, pFullscreen, ppTarget);

    if (result == S_OK && State::Instance().SCExclusiveFullscreen &&
        Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        *pFullscreen = true;
    }

    return result;
}

HRESULT FGHooks::hkResizeBuffers(IDXGISwapChain* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                 UINT SwapChainFlags)
{
    // Skip XeFG's internal call
    if (_skipResize)
    {
        LOG_DEBUG("XeFG call skipping");
        _skipResize = false;

        IDXGISwapChain* sc = nullptr;

        if (sc != nullptr)
        {
            auto result = sc->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
            LOG_DEBUG("XeFG internal ResizeBuffers result: {:X}", (UINT) result);
            return result;
        }

        return o_FGSCResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    auto fg = State::Instance().currentFG;

    OwnedLockGuard lg(fg->Mutex, 6677);

    // Disable frame generation when resizing swapchain
    PauseFG(fg);

    // Wait for GPU to finish rendering before resizing buffers to prevent issues with unreleased backbuffers
    WaitForGPUIdle();

    // Prevent mode switch when using borderless workaround for XeFG
    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
        {
            SwapChainFlags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        }

        if (State::Instance().SCLastFlags != SwapChainFlags)
        {
            LOG_WARN("SwapChainFlags changed from {:X} to {:X}", State::Instance().SCLastFlags, SwapChainFlags);

            if (State::Instance().activeFgOutput == FGOutput::XeFG)
            {
                LOG_WARN("Preventing flag change for XeFG!");
                SwapChainFlags = State::Instance().SCLastFlags;
            }
        }
    }

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", BufferCount, Width, Height,
              (UINT) NewFormat, SwapChainFlags);

    // Skip resize checks for XeFG
    if (State::Instance().activeFgOutput == FGOutput::XeFG && !State::Instance().SCExclusiveFullscreen &&
        Config::Instance()->FGSkipResizeBuffers.value_or_default())
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        if (This->GetDesc(&desc) == S_OK)
        {
            LOG_DEBUG("SC BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", desc.BufferCount,
                      desc.BufferDesc.Width, desc.BufferDesc.Height, (UINT) desc.BufferDesc.Format,
                      State::Instance().SCLastFlags);

            if (BufferCount == 0)
                BufferCount = desc.BufferCount;

            if ((desc.BufferDesc.Width == Width || Width == 0) && (desc.BufferDesc.Height == Height || Height == 0) &&
                (NewFormat == desc.BufferDesc.Format || NewFormat == 0) &&
                State::Instance().SCLastFlags == SwapChainFlags &&
                (BufferCount == desc.BufferCount || BufferCount == 0))
            {
                LOG_DEBUG("Skipping resize");

                if (Config::Instance()->FGModifyBufferState.value_or_default() ||
                    Config::Instance()->FGModifySCIndex.value_or_default())
                {
                    auto swapchain = ((IDXGISwapChain3*) This);
                    auto swapchainIndex = swapchain->GetCurrentBackBufferIndex();

                    if (fg != nullptr && Config::Instance()->FGModifyBufferState.value_or_default())
                    {
                        LOG_INFO("Trying to change backbuffer state to COMMON");

                        auto cmdList = fg->GetUICommandList();

                        if (cmdList != nullptr)
                        {
                            for (size_t i = 0; i < desc.BufferCount; i++)
                            {
                                ID3D12Resource* backBuffer = nullptr;
                                if (swapchain->GetBuffer(swapchainIndex, IID_PPV_ARGS(&backBuffer)) == S_OK)
                                {
                                    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                                        backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);

                                    cmdList->ResourceBarrier(1, &barrier);

                                    backBuffer->Release();
                                }
                            }
                        }
                    }

                    if (swapchainIndex != 0 && Config ::Instance()->FGModifySCIndex.value_or_default())
                    {
                        auto presents = desc.BufferCount - swapchainIndex;

                        LOG_DEBUG("Trying to reset backbuffer index: {} with {} present calls", swapchainIndex,
                                  presents);

                        for (size_t i = 0; i < presents; i++)
                        {
                            swapchain->Present(0, 0);
                        }
                    }
                }

                return S_OK;
            }
        }

        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    _skipResize1 = true;
    State::Instance().FGResizing = true;

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;
    State::Instance().SCLastFlags = SwapChainFlags;

    HRESULT result = E_FAIL;

    // Release menu render targets
    if (Config::Instance()->OverlayMenu.value_or_default())
        MenuOverlayDx::CleanupRenderTarget(false, NULL);

    // Release swapchain backbuffers to prevent errors when resizing
    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        for (UINT i = 0; i < 8; i++)
        {
            ID3D12Resource* backBuffer = nullptr;
            auto bbResult = This->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

            if (bbResult == S_OK)
            {
                LOG_DEBUG("Backbuffer {}: {:X}", i, (size_t) backBuffer);
                auto refCount = backBuffer->Release();
                while (refCount > XEFG_RESOURCE_REF_LIMIT)
                {
                    LOG_DEBUG("Releasing backbuffer {}: RefCount {}", i, refCount);
                    refCount = backBuffer->Release();
                }

#if (XEFG_RESOURCE_REF_LIMIT == 0)
                oldBackBuffers.push_back(backBuffer);
#endif
            }
            else
            {
                LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Force HDR10 for XeFG if HDR16 is used
    if (State::Instance().activeFgOutput == FGOutput::XeFG && NewFormat >= DXGI_FORMAT_R16G16B16A16_TYPELESS &&
        NewFormat <= DXGI_FORMAT_R16G16B16A16_SINT && !Config::Instance()->ForceHDR.has_value())
    {
        if (!Config::Instance()->ForceHDR.has_value())
        {
            LOG_INFO("XeFG is active, forcing HDR10");
            Config::Instance()->ForceHDR.set_volatile_value(true);
            Config::Instance()->UseHDR10.set_volatile_value(true);
            Config::Instance()->SkipColorSpace.set_volatile_value(true);
            _forcedHdrForXeFG = true;
        }
    }
    else if (_forcedHdrForXeFG)
    {
        LOG_INFO("Disabling forced HDR10");
        Config::Instance()->ForceHDR.reset();
        Config::Instance()->UseHDR10.reset();
        Config::Instance()->SkipColorSpace.reset();
        _forcedHdrForXeFG = false;
    }

    // Crude implementation of EndlesslyFlowering's AutoHDR-ReShade
    // https://github.com/EndlesslyFlowering/AutoHDR-ReShade
    if (Config::Instance()->ForceHDR.value_or_default())
    {
        LOG_INFO("Force HDR on");

        IDXGISwapChain3* _real3 = nullptr;
        if (This->QueryInterface(IID_PPV_ARGS(&_real3)) == S_OK)
        {
            do
            {
                NewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
                DXGI_COLOR_SPACE_TYPE hdrCS = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

                if (Config::Instance()->UseHDR10.value_or_default())
                {
                    NewFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
                    hdrCS = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                }

                if (!Config::Instance()->SkipColorSpace.value_or_default())
                {
                    UINT css = 0;

                    auto result = _real3->CheckColorSpaceSupport(hdrCS, &css);

                    if (result != S_OK)
                    {
                        LOG_ERROR("CheckColorSpaceSupport error: {:X}", (UINT) result);
                        break;
                    }

                    if (DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT & css)
                    {
                        result = _real3->SetColorSpace1(hdrCS);

                        if (result != S_OK)
                        {
                            LOG_ERROR("SetColorSpace1 error: {:X}", (UINT) result);
                            break;
                        }
                    }

                    LOG_INFO("HDR format and color space are set");
                }

            } while (false);

            _real3->Release();
        }
    }

    {
        ScopedSkipSpoofing skipSpoofing {};
        result = o_FGSCResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    LOG_DEBUG("Result: {:X}, Caller: {}", (UINT) result, Util::WhoIsTheCaller(_ReturnAddress()));

    // Resize window to cover the screen
    if (result == S_OK && Config::Instance()->FGXeFGForceBorderless.value_or_default() &&
        State::Instance().SCExclusiveFullscreen)
    {
        SetWindowLongPtr(_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

        Util::MonitorInfo info;
        info = Util::GetMonitorInfoForWindow(_hwnd);

        LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                  info.y, wstring_to_string(info.name));

        SetWindowPos(_hwnd, HWND_TOP, info.x, info.y, info.width, info.height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    State::Instance().FGResizing = false;
    _skipResize1 = false;

    return result;
}

HRESULT FGHooks::hkResizeTarget(IDXGISwapChain* This, const DXGI_MODE_DESC* pNewTargetParameters)
{
    if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        LOG_DEBUG("Skipping resize target.");
        return S_OK;
    }

    return o_FGSCResizeTarget(This, pNewTargetParameters);
}

HRESULT FGHooks::hkResizeBuffers1(IDXGISwapChain3* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                  UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue)
{
    // Skip XeFG's internal call
    if (_skipResize1)
    {
        LOG_DEBUG("XeFG call skipping");
        _skipResize1 = false;

        IDXGISwapChain3* sc = nullptr;

        if (sc != nullptr)
        {
            auto result = sc->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                             ppPresentQueue);

            LOG_DEBUG("XeFG internal ResizeBuffers1 result: {:X}", (UINT) result);
            return result;
        }

        return o_FGSCResizeBuffers1(This, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                    ppPresentQueue);
    }

    auto fg = State::Instance().currentFG;

    OwnedLockGuard lg(fg->Mutex, 6678);

    // Disable frame generation when resizing swapchain
    PauseFG(fg);

    // Wait for GPU to finish rendering before resizing buffers to prevent issues with unreleased backbuffers
    WaitForGPUIdle();

    // Prevent mode switch when using borderless workaround for XeFG
    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
        {
            SwapChainFlags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        }

        if (State::Instance().SCLastFlags != SwapChainFlags)
        {
            LOG_WARN("SwapChainFlags changed from {:X} to {:X}", State::Instance().SCLastFlags, SwapChainFlags);

            if (State::Instance().activeFgOutput == FGOutput::XeFG)
            {
                LOG_WARN("Preventing flag change for XeFG!");
                SwapChainFlags = State::Instance().SCLastFlags;
            }
        }
    }

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}, Caller: {}", BufferCount,
              Width, Height, (UINT) Format, SwapChainFlags, Util::WhoIsTheCaller(_ReturnAddress()));

    // Skip resize checks for XeFG
    if (State::Instance().activeFgOutput == FGOutput::XeFG && !State::Instance().SCExclusiveFullscreen &&
        Config::Instance()->FGSkipResizeBuffers.value_or_default())
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        if (This->GetDesc(&desc) == S_OK)
        {
            LOG_DEBUG("SC BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", desc.BufferCount,
                      desc.BufferDesc.Width, desc.BufferDesc.Height, (UINT) desc.BufferDesc.Format,
                      State::Instance().SCLastFlags);

            if (BufferCount == 0)
                BufferCount = desc.BufferCount;

            if ((desc.BufferDesc.Width == Width || Width == 0) && (desc.BufferDesc.Height == Height || Height == 0) &&
                (Format == desc.BufferDesc.Format || Format == 0) && State::Instance().SCLastFlags == SwapChainFlags &&
                (BufferCount == desc.BufferCount || BufferCount == 0))
            {
                LOG_DEBUG("Skipping resize");

                if (Config::Instance()->FGModifyBufferState.value_or_default() ||
                    Config::Instance()->FGModifySCIndex.value_or_default())
                {
                    auto swapchain = ((IDXGISwapChain3*) This);
                    auto swapchainIndex = swapchain->GetCurrentBackBufferIndex();

                    if (fg != nullptr && Config::Instance()->FGModifyBufferState.value_or_default())
                    {
                        LOG_INFO("Trying to change backbuffer state to COMMON");

                        auto cmdList = fg->GetUICommandList();

                        if (cmdList != nullptr)
                        {
                            for (size_t i = 0; i < desc.BufferCount; i++)
                            {
                                ID3D12Resource* backBuffer = nullptr;
                                if (swapchain->GetBuffer(swapchainIndex, IID_PPV_ARGS(&backBuffer)) == S_OK)
                                {
                                    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                                        backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);

                                    cmdList->ResourceBarrier(1, &barrier);

                                    backBuffer->Release();
                                }
                            }
                        }
                    }

                    if (swapchainIndex != 0 && Config ::Instance()->FGModifySCIndex.value_or_default())
                    {
                        auto presents = desc.BufferCount - swapchainIndex;

                        LOG_DEBUG("Trying to reset backbuffer index: {} with {} present calls", swapchainIndex,
                                  presents);

                        for (size_t i = 0; i < presents; i++)
                        {
                            swapchain->Present(0, 0);
                        }
                    }
                }

                return S_OK;
            }
        }

        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    _skipResize = true;
    State::Instance().FGResizing = true;

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;
    State::Instance().SCLastFlags = SwapChainFlags;

    HRESULT result;

    // Release menu render targets
    if (Config::Instance()->OverlayMenu.value_or_default())
        MenuOverlayDx::CleanupRenderTarget(false, NULL);

    // Release swapchain backbuffers to prevent errors when resizing
    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        for (UINT i = 0; i < 8; i++)
        {
            ID3D12Resource* backBuffer = nullptr;
            auto bbResult = This->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

            if (bbResult == S_OK)
            {
                LOG_DEBUG("Backbuffer {}: {:X}", i, (size_t) backBuffer);
                auto refCount = backBuffer->Release();
                while (refCount > XEFG_RESOURCE_REF_LIMIT)
                {
                    LOG_DEBUG("Releasing backbuffer {}: RefCount {}", i, refCount);
                    refCount = backBuffer->Release();
                }

#if (XEFG_RESOURCE_REF_LIMIT == 0)
                oldBackBuffers.push_back(backBuffer);
#endif
            }
            else
            {
                LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Force HDR10 for XeFG if HDR16 is used
    if (State::Instance().activeFgOutput == FGOutput::XeFG && Format >= DXGI_FORMAT_R16G16B16A16_TYPELESS &&
        Format <= DXGI_FORMAT_R16G16B16A16_SINT && !Config::Instance()->ForceHDR.has_value())
    {
        if (!Config::Instance()->ForceHDR.has_value())
        {
            LOG_INFO("XeFG is active, forcing HDR10");
            Config::Instance()->ForceHDR.set_volatile_value(true);
            Config::Instance()->UseHDR10.set_volatile_value(true);
            Config::Instance()->SkipColorSpace.set_volatile_value(true);
            _forcedHdrForXeFG = true;
        }
    }
    else if (_forcedHdrForXeFG)
    {
        LOG_INFO("Disabling forced HDR10");
        Config::Instance()->ForceHDR.reset();
        Config::Instance()->UseHDR10.reset();
        Config::Instance()->SkipColorSpace.reset();
        _forcedHdrForXeFG = false;
    }

    // Crude implementation of EndlesslyFlowering's AutoHDR-ReShade
    // https://github.com/EndlesslyFlowering/AutoHDR-ReShade
    if (Config::Instance()->ForceHDR.value_or_default())
    {
        LOG_INFO("Force HDR on");

        IDXGISwapChain3* _real3 = nullptr;
        if (This->QueryInterface(IID_PPV_ARGS(&_real3)) == S_OK)
        {
            do
            {
                Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                DXGI_COLOR_SPACE_TYPE hdrCS = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

                if (Config::Instance()->UseHDR10.value_or_default())
                {
                    Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                    hdrCS = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                }

                if (!Config::Instance()->SkipColorSpace.value_or_default())
                {
                    UINT css = 0;

                    auto result = _real3->CheckColorSpaceSupport(hdrCS, &css);

                    if (result != S_OK)
                    {
                        LOG_ERROR("CheckColorSpaceSupport error: {:X}", (UINT) result);
                        break;
                    }

                    if (DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT & css)
                    {
                        result = _real3->SetColorSpace1(hdrCS);

                        if (result != S_OK)
                        {
                            LOG_ERROR("SetColorSpace1 error: {:X}", (UINT) result);
                            break;
                        }
                    }

                    LOG_INFO("HDR format and color space are set");
                }

            } while (false);

            _real3->Release();
        }
    }

    {
        ScopedSkipSpoofing skipSpoofing {};
        result = o_FGSCResizeBuffers1(This, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                      ppPresentQueue);
    }

    LOG_DEBUG("Result: {:X}, Caller: {}", (UINT) result, Util::WhoIsTheCaller(_ReturnAddress()));

    // Resize window to cover the screen
    if (result == S_OK && Config::Instance()->FGXeFGForceBorderless.value_or_default() &&
        State::Instance().SCExclusiveFullscreen)
    {
        SetWindowLongPtr(_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

        Util::MonitorInfo info;
        info = Util::GetMonitorInfoForWindow(_hwnd);

        LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                  info.y, wstring_to_string(info.name));

        SetWindowPos(_hwnd, HWND_TOP, info.x, info.y, info.width, info.height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    State::Instance().FGResizing = false;
    _skipResize = false;

    return result;
}

HRESULT FGHooks::hkFGPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
{
    TraceVF2PresentCaller("Present", This, SyncInterval, Flags, nullptr, _skipPresent, _skipPresent1, _ReturnAddress());

    // Skip XeFG's internal call
    if (_skipPresent)
    {
        LOG_DEBUG("XeFG call skipping");
        ScopedXeFGInternalPresent xefgInternalPresentScope;

        IDXGISwapChain* sc = nullptr;

        HRESULT result;

        if (sc != nullptr)
        {
            result = sc->Present(SyncInterval, Flags);
            LOG_DEBUG("sc->Present result: {:X}", (UINT) result);
        }
        else
        {
            result = o_FGSCPresent(This, SyncInterval, Flags);
            LOG_DEBUG("o_FGSCPresent result: {:X}", (UINT) result);
        }

        return result;
    }

    LOG_DEBUG("SyncInterval: {}, Flags: {:X}", SyncInterval, Flags);

    _skipPresent1 = true;
    auto result = FGPresent(This, SyncInterval, Flags, nullptr);
    _skipPresent1 = false;

    return result;
}

HRESULT FGHooks::hkFGPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT Flags,
                               const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    TraceVF2PresentCaller("Present1", This, SyncInterval, Flags, pPresentParameters, _skipPresent, _skipPresent1,
                          _ReturnAddress());

    // Skip XeFG's internal call
    if (_skipPresent1)
    {
        LOG_DEBUG("XeFG call skipping");
        ScopedXeFGInternalPresent xefgInternalPresentScope;

        IDXGISwapChain3* sc = nullptr;

        HRESULT result;

        if (sc != nullptr)
        {
            result = sc->Present1(SyncInterval, Flags, pPresentParameters);
            LOG_DEBUG("sc->Present result: {:X}", (UINT) result);
        }
        else
        {
            result = o_FGSCPresent1(This, SyncInterval, Flags, pPresentParameters);
            LOG_DEBUG("o_FGSCPresent result: {:X}", (UINT) result);
        }

        return result;
    }

    LOG_DEBUG("SyncInterval: {}, Flags: {:X}", SyncInterval, Flags);
    _skipPresent = true;
    auto result = FGPresent(This, SyncInterval, Flags, pPresentParameters);
    _skipPresent = false;

    return result;
}

static void FrameWarpRestoreColorInputState(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource,
                                            D3D12_RESOURCE_STATES originalState)
{
    if (cmdList == nullptr || resource == nullptr ||
        originalState == D3D12_RESOURCE_STATE_COMMON ||
        originalState == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = originalState;
    cmdList->ResourceBarrier(1, &barrier);
}

static void TryInjectFrameWarpDistortion(IFGFeature_Dx12* fg, IDXGISwapChain* swapChain)
{
    (void)swapChain;
    auto& state = State::Instance();
    auto& status = state.frameWarpStatus;

    if (state.activeFgOutput == FGOutput::FSRFG)
    {
        strncpy_s(status.lastSkipReason, "FSRFG distortion disabled; present callback owns FrameWarp", _TRUNCATE);
        return;
    }

    if (state.activeFgOutput != FGOutput::DLSSG)
        return;

    const auto dlssgMode = Config::Instance()->FrameWarpDLSSGMode.value_or_default();
    status.dlssgFrameWarpMode = dlssgMode;

    auto markSkipped = [&](const char* reason) {
        status.dlssgDistortionSkippedCount++;
        status.dlssgDistortionLastInjected = false;
        strncpy_s(status.dlssgDistortionLastReason, reason != nullptr ? reason : "unknown", _TRUNCATE);
        strncpy_s(status.lastSkipReason, status.dlssgDistortionLastReason, _TRUNCATE);
    };

    if (dlssgMode == 0)
    {
        status.dlssgDistortionLastInjected = false;
        strncpy_s(status.dlssgDistortionLastReason, "DLSSG mode off", _TRUNCATE);
        return;
    }

    if (dlssgMode == 1)
    {
        status.dlssgDistortionLastInjected = false;
        strncpy_s(status.dlssgDistortionLastReason, "DLSSG telemetry only", _TRUNCATE);
        return;
    }

    if (dlssgMode == 3)
    {
        status.dlssgDistortionLastInjected = false;
        strncpy_s(status.dlssgDistortionLastReason, "DLSSG late-present warp owns FrameWarp", _TRUNCATE);
        return;
    }
    if (dlssgMode == 4)
    {
        status.dlssgDistortionLastInjected = false;
        strncpy_s(status.dlssgDistortionLastReason, "DLSSG resource-copy warp owns FrameWarp", _TRUNCATE);
        return;
    }

    status.dlssgDistortionAttemptCount++;
    const auto owner = FrameWarpRuntime::ResolvePresentationOwner("DLSSGDistortionInject", true);
    if (owner != FrameWarpPresentationOwner::DLSSGDistortionField)
    {
        char reason[64] = {};
        strncpy_s(reason,
                  status.lastPresentationOwnerReason[0] != '\0' ? status.lastPresentationOwnerReason
                                                                 : FrameWarpRuntime::PresentationOwnerName(owner),
                  _TRUNCATE);
        markSkipped(reason);
        return;
    }

    if (fg == nullptr || !fg->IsActive() || fg->IsPaused())
    {
        markSkipped("DLSSG inactive");
        return;
    }

    auto fIndex = fg->GetIndexWillBeDispatched();
    if (fIndex < 0)
    {
        markSkipped("invalid DLSSG frame index");
        return;
    }

    auto cmdList = fg->GetUICommandList(fIndex);
    if (cmdList == nullptr)
    {
        markSkipped("missing DLSSG UI command list");
        return;
    }

    auto frameWarp = FrameWarpRuntime::Get();
    if (frameWarp == nullptr)
    {
        markSkipped("FrameWarp runtime missing");
        return;
    }

    ID3D12Resource* distortion = nullptr;
    if (!frameWarp->GenerateStreamlineDistortionField(cmdList, &distortion) || distortion == nullptr)
    {
        char reason[64] = {};
        strncpy_s(reason, status.lastSkipReason[0] != '\0' ? status.lastSkipReason : "distortion generation failed",
                  _TRUNCATE);
        markSkipped(reason);
        return;
    }

    auto desc = distortion->GetDesc();
    Dx12Resource distortionResource {};
    distortionResource.type = FG_ResourceType::Distortion;
    distortionResource.resource = distortion;
    distortionResource.width = desc.Width;
    distortionResource.height = desc.Height;
    distortionResource.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    distortionResource.validity = FG_ResourceValidity::ValidNow;
    distortionResource.cmdList = cmdList;
    distortionResource.frameIndex = fIndex;

    if (!fg->SetResource(&distortionResource))
    {
        markSkipped("DLSSG distortion SetResource failed");
        return;
    }

    status.dlssgDistortionInjectedCount++;
    status.dlssgDistortionLastInjected = true;
    status.dlssgDistortionLastFrameIndex = static_cast<uint32_t>(fIndex);
    strncpy_s(status.dlssgDistortionLastReason, "DLSSG distortion tagged", _TRUNCATE);

    if (status.dlssgDistortionInjectedCount <= 10 || status.dlssgDistortionInjectedCount % 60 == 0)
    {
        LOG_INFO("DLSSG FrameWarp SL bidirectional distortion injected count={} attempt={} frameIndex={} resource={:X} size={}x{} format={}",
                 status.dlssgDistortionInjectedCount,
                 status.dlssgDistortionAttemptCount,
                 fIndex,
                 reinterpret_cast<size_t>(distortion),
                 static_cast<uint32_t>(desc.Width),
                 desc.Height,
                 static_cast<uint32_t>(desc.Format));
    }
}

static const char* VF2DLSSGLiveWarpPolicyName(uint32_t phaseMode, bool unsafeLiveWarp);

static bool TryApplyDLSSGLatePresentWarp(IDXGISwapChain* swapChain)
{
    auto& state = State::Instance();
    auto& status = state.frameWarpStatus;

    status.dlssgLatePresentLastApplied = false;
    status.lastPreparedPixelShift = 0.0f;
    status.lastPreparedDeltaYaw = 0.0f;
    status.lastPreparedDeltaPitch = 0.0f;
    status.lastPreparedStatus[0] = '\0';
    status.lastPreparedReason[0] = '\0';
    status.dlssgLatePresentSafetySuppressed = false;

    UINT auditBackBufferIndex = UINT_MAX;
    UINT auditWidth = 0;
    UINT auditHeight = 0;
    uint32_t auditFormat = 0;
    bool auditSubmitted = false;
    bool auditApplied = false;
    double auditApplyCpuMs = 0.0;
    auto logAuditPresent = [&](const char* outcome) {
        static uint64_t auditCount = 0;
        auditCount++;
        if (!Config::Instance()->FrameWarpTimingAuditLog.value_or_default() ||
            (auditCount > 600 && auditCount % 60 != 0))
            return;

        const uint32_t phaseMode = Config::Instance()->FrameWarpDLSSGPhaseMode.value_or_default();
        const bool unsafeLiveWarp = Config::Instance()->FrameWarpDLSSGUnsafeLiveWarp.value_or_default();
        LOG_INFO("VF2AuditPresent #{} frame={} outcome={} attempt={} testMode={} phaseMode={} livePolicy={} safetySuppressed={} submitted={} applied={} cpuMs={:.3f} backbuffer={} size={}x{} fmt={} owner={} ownerReason={} rawSeq={} snapSeq={} sinceSnap={} snapshots={} snapSrc={} rawSrc={} activeSrc={} snapAgeMs={:.3f} sampleAgeMs={:.3f} finalMouse=({:.1f},{:.1f}) step=({:.1f},{:.1f}) yaw={:.6f} pitch={:.6f} px={:.3f} path={} depthAge={:.0f} depthHist={} depthMask={:.3f} ui={} uiAge={} skip={} reason={}",
            auditCount,
            state.frameCount,
            outcome != nullptr ? outcome : "unknown",
            status.dlssgLatePresentAttemptCount,
            Config::Instance()->FrameWarpDLSSGLatePresentTestMode.value_or_default(),
            phaseMode,
            VF2DLSSGLiveWarpPolicyName(phaseMode, unsafeLiveWarp),
            status.dlssgLatePresentSafetySuppressed,
            auditSubmitted,
            auditApplied,
            auditApplyCpuMs,
            auditBackBufferIndex,
            auditWidth,
            auditHeight,
            auditFormat,
            status.lastPresentationOwnerName[0] ? status.lastPresentationOwnerName : "none",
            status.lastPresentationOwnerReason[0] ? status.lastPresentationOwnerReason : "none",
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
            status.lastPreparedDeltaYaw,
            status.lastPreparedDeltaPitch,
            status.lastPreparedPixelShift,
            status.lastStandaloneWarpPath[0] ? status.lastStandaloneWarpPath : "none",
            status.depthInfillAgeFrames,
            status.depthInfillHistoryUsed,
            status.depthInfillMaskCoveragePct,
            status.lastStableUiSource[0] ? status.lastStableUiSource : "none",
            status.lastStableUiAge,
            status.lastSkipReason[0] ? status.lastSkipReason : "none",
            status.dlssgLatePresentLastReason[0] ? status.dlssgLatePresentLastReason : "none");
    };

    if (state.activeFgOutput != FGOutput::DLSSG)
        return false;

    const auto dlssgMode = Config::Instance()->FrameWarpDLSSGMode.value_or_default();
    status.dlssgFrameWarpMode = dlssgMode;
    if (dlssgMode != 3)
    {
        if (dlssgMode == 0)
            strncpy_s(status.dlssgLatePresentLastReason, "DLSSG mode off", _TRUNCATE);
        else if (dlssgMode == 1)
            strncpy_s(status.dlssgLatePresentLastReason, "DLSSG telemetry only", _TRUNCATE);
        else if (dlssgMode == 4)
        {
            FrameWarpRuntime::LatchDLSSGResourceCopyWarpPose(swapChain);
            strncpy_s(status.dlssgLatePresentLastReason, "DLSSG resource-copy warp mode", _TRUNCATE);
        }
        else
            strncpy_s(status.dlssgLatePresentLastReason, "DLSSG distortion field mode", _TRUNCATE);
        return false;
    }

    auto markSkipped = [&](const char* reason) {
        status.dlssgLatePresentSkippedCount++;
        status.dlssgLatePresentLastApplied = false;
        strncpy_s(status.dlssgLatePresentLastReason, reason != nullptr ? reason : "unknown", _TRUNCATE);
        strncpy_s(status.lastSkipReason, status.dlssgLatePresentLastReason, _TRUNCATE);

        if (status.dlssgLatePresentAttemptCount <= 10 || status.dlssgLatePresentAttemptCount % 60 == 0)
        {
            LOG_DEBUG("DLSSG late-present FrameWarp skipped attempt={} reason={}",
                status.dlssgLatePresentAttemptCount,
                status.dlssgLatePresentLastReason);
        }
        logAuditPresent("skip");
    };

    status.dlssgLatePresentAttemptCount++;

    const uint32_t latePresentTestMode = Config::Instance()->FrameWarpDLSSGLatePresentTestMode.value_or_default();
    if (latePresentTestMode == 1)
    {
        strncpy_s(status.lastStandaloneWarpPath, "test-no-submit", _TRUNCATE);
        strncpy_s(status.lastSkipReason, "diagnostic no submit", _TRUNCATE);
        markSkipped("diagnostic no submit");
        return false;
    }

    const auto owner = FrameWarpRuntime::ResolvePresentationOwner("DLSSGLatePresent", true);
    if (owner != FrameWarpPresentationOwner::DLSSGLatePresent)
    {
        markSkipped(status.lastPresentationOwnerReason[0] != '\0'
            ? status.lastPresentationOwnerReason
            : FrameWarpRuntime::PresentationOwnerName(owner));
        return false;
    }

    auto fg = state.currentFG;
    if (fg == nullptr || !fg->IsActive() || fg->IsPaused())
    {
        markSkipped("DLSSG inactive");
        return false;
    }

    ID3D12CommandQueue* queue = currentCommandQueue != nullptr ? currentCommandQueue : state.currentCommandQueue;
    if (queue == nullptr)
    {
        markSkipped("missing command queue");
        return false;
    }

    static uint64_t lastPhaseGateRenderSnapshot = UINT64_MAX;
    const uint32_t phaseMode = Config::Instance()->FrameWarpDLSSGPhaseMode.value_or_default();
    const bool unsafeLiveWarp = Config::Instance()->FrameWarpDLSSGUnsafeLiveWarp.value_or_default();
    const bool liveWarpTestMode = latePresentTestMode == 0 || latePresentTestMode == 3;
    const bool renderSnapshotChanged =
        lastPhaseGateRenderSnapshot == UINT64_MAX ||
        lastPhaseGateRenderSnapshot != status.renderSnapshotCount;
    const bool slGeneratedHeuristic = status.dlssgLastFramesPresented > 1;
    const char* phaseSkipReason = nullptr;
    if (liveWarpTestMode && phaseMode != 0)
    {
        if (phaseMode == 1 && !renderSnapshotChanged)
            phaseSkipReason = "phase gate snapshot unchanged";
        else if (phaseMode == 2 && renderSnapshotChanged)
            phaseSkipReason = "phase gate snapshot changed";
        else if (phaseMode == 3 && (status.dlssgLatePresentAttemptCount % 2) == 0)
            phaseSkipReason = "phase gate alternate present";
        else if (phaseMode == 4 && !slGeneratedHeuristic)
            phaseSkipReason = "phase gate SL generated heuristic";
    }
    lastPhaseGateRenderSnapshot = status.renderSnapshotCount;
    if (liveWarpTestMode && phaseMode == 0 && !unsafeLiveWarp)
    {
        status.dlssgLatePresentSafetySuppressed = true;
        strncpy_s(status.lastStandaloneWarpPath, "safety-policy-skip", _TRUNCATE);
        markSkipped(renderSnapshotChanged
            ? "safety policy snapshot-change live warp suppressed"
            : "safety policy unknown DLSSG phase live warp suppressed");
        return false;
    }
    if (phaseSkipReason != nullptr)
    {
        strncpy_s(status.lastStandaloneWarpPath, "phase-gate-skip", _TRUNCATE);
        markSkipped(phaseSkipReason);
        return false;
    }

    IDXGISwapChain3* swapChain3 = nullptr;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) || swapChain3 == nullptr)
    {
        markSkipped("swapchain3 unavailable");
        return false;
    }

    ID3D12Resource* backBuffer = nullptr;
    const UINT backBufferIndex = swapChain3->GetCurrentBackBufferIndex();
    auditBackBufferIndex = backBufferIndex;
    HRESULT hr = swapChain3->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer));
    swapChain3->Release();
    if (FAILED(hr) || backBuffer == nullptr)
    {
        markSkipped("backbuffer unavailable");
        return false;
    }

    auto desc = backBuffer->GetDesc();
    auditWidth = static_cast<UINT>(desc.Width);
    auditHeight = desc.Height;
    auditFormat = static_cast<uint32_t>(desc.Format);
    backBuffer->Release();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0)
    {
        markSkipped("unsupported backbuffer");
        return false;
    }

    ID3D12Device* device = nullptr;
    hr = queue->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr) || device == nullptr)
    {
        markSkipped("device unavailable");
        return false;
    }

    const bool initialized = FrameWarpRuntime::EnsureInitialized(
        device,
        static_cast<UINT>(desc.Width),
        desc.Height,
        desc.Format);
    device->Release();
    if (!initialized)
    {
        markSkipped("FrameWarp init failed");
        return false;
    }

    const double applyStartMs = Util::MillisecondsNow();
    const bool submitted = FrameWarpRuntime::ApplyStandalone(swapChain, queue);
    const double applyCpuMs = Util::MillisecondsNow() - applyStartMs;
    const bool applied = submitted && status.lastWarpApplied;
    auditSubmitted = submitted;
    auditApplied = applied;
    auditApplyCpuMs = applyCpuMs;
    auto logPacing = [&](const char* outcome) {
        if (!Config::Instance()->FrameWarpPacingLog.value_or_default())
            return;

        static uint64_t pacingLogCount = 0;
        pacingLogCount++;
        if (pacingLogCount <= 10 || pacingLogCount % 120 == 0)
        {
            LOG_INFO("VF2Pacing #{} outcome={} submitted={} applied={} cpuMs={:.3f} attempts={} submittedCount={} appliedCount={} skipped={} zeroPose={} inFlight={} reason={}",
                pacingLogCount,
                outcome != nullptr ? outcome : "unknown",
                submitted,
                applied,
                applyCpuMs,
                status.dlssgLatePresentAttemptCount,
                status.dlssgLatePresentSubmittedCount,
                status.dlssgLatePresentAppliedCount,
                status.dlssgLatePresentSkippedCount,
                status.dlssgLatePresentZeroPoseCount,
                status.dlssgLatePresentInFlightSkipCount,
                status.lastSkipReason[0] != '\0' ? status.lastSkipReason : status.dlssgLatePresentLastReason);
        }
    };

    if (!applied)
    {
        markSkipped(status.lastSkipReason[0] != '\0' ? status.lastSkipReason : "warp not submitted");
        logPacing(submitted ? "submitted-no-apply" : "not-submitted");
        return submitted;
    }

    status.dlssgLatePresentAppliedCount++;
    status.dlssgLatePresentLastApplied = true;
    strncpy_s(status.dlssgLatePresentLastReason, "DLSSG late-present warped", _TRUNCATE);

    if (status.dlssgLatePresentAppliedCount <= 10 || status.dlssgLatePresentAppliedCount % 60 == 0)
    {
        LOG_INFO("DLSSG late-present FrameWarp applied count={} attempt={} backbuffer={} size={}x{} format={}",
            status.dlssgLatePresentAppliedCount,
            status.dlssgLatePresentAttemptCount,
            backBufferIndex,
            static_cast<UINT>(desc.Width),
            desc.Height,
            static_cast<UINT>(desc.Format));
    }

    logPacing("applied");
    logAuditPresent("applied");
    return submitted;
}

static const char* VF2FGInputName(FGInput input)
{
    switch (input)
    {
    case FGInput::NoFG: return "NoFG";
    case FGInput::Nukems: return "Nukems";
    case FGInput::FSRFG: return "FSRFG";
    case FGInput::DLSSG: return "DLSSG";
    case FGInput::XeFG: return "XeFG";
    case FGInput::Upscaler: return "Upscaler";
    case FGInput::FSRFG30: return "FSRFG30";
    default: return "Unknown";
    }
}

static const char* VF2FGOutputName(FGOutput output)
{
    switch (output)
    {
    case FGOutput::NoFG: return "NoFG";
    case FGOutput::Nukems: return "Nukems";
    case FGOutput::FSRFG: return "FSRFG";
    case FGOutput::DLSSG: return "DLSSG";
    case FGOutput::XeFG: return "XeFG";
    default: return "Unknown";
    }
}

static double VF2QpcDeltaUs(uint64_t newer, uint64_t older)
{
    if (newer == 0 || older == 0 || newer < older)
        return 0.0;

    static double qpcToUs = []() {
        LARGE_INTEGER freq {};
        QueryPerformanceFrequency(&freq);
        return freq.QuadPart > 0 ? (1'000'000.0 / static_cast<double>(freq.QuadPart)) : 0.0;
    }();

    return static_cast<double>(newer - older) * qpcToUs;
}

static int64_t VF2QpcNow()
{
    LARGE_INTEGER counter {};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

static double VF2QpcDeltaMs(int64_t newer, int64_t older)
{
    if (newer == 0 || older == 0 || newer < older)
        return 0.0;

    static double qpcToMs = []() {
        LARGE_INTEGER freq {};
        QueryPerformanceFrequency(&freq);
        return freq.QuadPart > 0 ? (1000.0 / static_cast<double>(freq.QuadPart)) : 0.0;
    }();

    return static_cast<double>(newer - older) * qpcToMs;
}

struct VF2DlssgStateSample
{
    sl::Result result = sl::Result::eErrorFeatureMissing;
    uint32_t presented = 0;
    uint32_t status = 0;
};

static VF2DlssgStateSample QueryVF2DLSSGState()
{
    VF2DlssgStateSample sample {};
    if (State::Instance().activeFgOutput != FGOutput::DLSSG || SLProxy::DLSSGGetState() == nullptr)
        return sample;

    sl::DLSSGState dlssgState {};
    sl::ViewportHandle viewport(0);
    sample.result = SLProxy::DLSSGGetState()(viewport, dlssgState, nullptr);
    sample.presented = dlssgState.numFramesActuallyPresented;
    sample.status = static_cast<uint32_t>(dlssgState.status);
    return sample;
}

static void QueryVF2Backbuffer(IDXGISwapChain* swapChain, UINT& index, UINT& width, UINT& height, uint32_t& format)
{
    index = UINT_MAX;
    width = 0;
    height = 0;
    format = 0;

    IDXGISwapChain3* swapChain3 = nullptr;
    if (swapChain == nullptr || FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) || swapChain3 == nullptr)
        return;

    index = swapChain3->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = nullptr;
    if (SUCCEEDED(swapChain3->GetBuffer(index, IID_PPV_ARGS(&backBuffer))) && backBuffer != nullptr)
    {
        auto desc = backBuffer->GetDesc();
        width = static_cast<UINT>(desc.Width);
        height = desc.Height;
        format = static_cast<uint32_t>(desc.Format);
        backBuffer->Release();
    }
    swapChain3->Release();
}

struct VF2DLSSGPhaseClassification
{
    const char* label = "unknown";
    const char* reason = "unclassified";
    float confidence = 0.0f;
    bool snapshotChanged = false;
    bool futureEligible = false;
    bool slHeuristic = false;
};

static VF2DLSSGPhaseClassification ClassifyVF2DLSSGPhase(
    const VF2DlssgStateSample& slBefore,
    const VF2DlssgStateSample& slAfter,
    uint64_t presentSeq,
    uint64_t renderSnapshot,
    uint64_t previousRenderSnapshot)
{
    VF2DLSSGPhaseClassification phase {};
    phase.snapshotChanged = previousRenderSnapshot != renderSnapshot;

    if (presentSeq == 0)
    {
        phase.label = "no-present";
        phase.reason = "no DLSSG present sequence";
        return phase;
    }

    if (phase.snapshotChanged)
    {
        phase.label = "snapshot-boundary";
        phase.reason = "render snapshot changed; whole-backbuffer residual warp unsafe";
        phase.confidence = 1.0f;
        phase.futureEligible = false;
        return phase;
    }

    const bool slBeforeOk = slBefore.result == sl::Result::eOk;
    const bool slAfterOk = slAfter.result == sl::Result::eOk;
    const bool slMultiPresented =
        (slBeforeOk && slBefore.presented > 1) ||
        (slAfterOk && slAfter.presented > 1);

    if (slMultiPresented)
    {
        phase.label = "sl-presented-heuristic";
        phase.reason = "same render snapshot with SL multi-presented heuristic; not a final frame-type label";
        phase.confidence = 0.35f;
        phase.futureEligible = true;
        phase.slHeuristic = true;
        return phase;
    }

    if (slBeforeOk || slAfterOk)
    {
        phase.label = "same-snapshot-candidate";
        phase.reason = "same render snapshot; generated-output phase still unknown";
        phase.confidence = 0.25f;
        phase.futureEligible = true;
        return phase;
    }

    phase.label = "same-snapshot-no-sl";
    phase.reason = "same render snapshot but SL state unavailable";
    phase.confidence = 0.15f;
    phase.futureEligible = false;
    return phase;
}

static void StoreVF2DLSSGPhaseClassification(
    State::FrameWarpStatus& status,
    const VF2DLSSGPhaseClassification& phase,
    uint64_t presentSeq,
    uint64_t renderSnapshot,
    uint64_t previousRenderSnapshot,
    const VF2DlssgStateSample& slBefore,
    const VF2DlssgStateSample& slAfter)
{
    status.dlssgPhaseClassifyCount++;
    if (phase.snapshotChanged)
        status.dlssgPhaseClassifyBoundaryCount++;
    else if (phase.slHeuristic)
        status.dlssgPhaseClassifyHeuristicCount++;
    else if (phase.futureEligible)
        status.dlssgPhaseClassifySameSnapshotCount++;
    else
        status.dlssgPhaseClassifyUnknownCount++;

    status.dlssgPhaseClassifyPresentSeq = presentSeq;
    status.dlssgPhaseClassifyRenderSnapshot = renderSnapshot;
    status.dlssgPhaseClassifyPreviousRenderSnapshot = previousRenderSnapshot;
    status.dlssgPhaseClassifySnapshotChanged = phase.snapshotChanged;
    status.dlssgPhaseClassifyFutureEligible = phase.futureEligible;
    status.dlssgPhaseClassifySlHeuristic = phase.slHeuristic;
    status.dlssgPhaseClassifyConfidence = phase.confidence;
    status.dlssgPhaseClassifySlBefore = slBefore.presented;
    status.dlssgPhaseClassifySlAfter = slAfter.presented;
    status.dlssgPhaseClassifySlResultBefore = static_cast<int32_t>(slBefore.result);
    status.dlssgPhaseClassifySlResultAfter = static_cast<int32_t>(slAfter.result);
    strncpy_s(status.dlssgPhaseClassifyLabel, phase.label != nullptr ? phase.label : "unknown", _TRUNCATE);
    strncpy_s(status.dlssgPhaseClassifyReason, phase.reason != nullptr ? phase.reason : "unknown", _TRUNCATE);
}

static const char* VF2DLSSGLiveWarpPolicyName(uint32_t phaseMode, bool unsafeLiveWarp)
{
    if (phaseMode != 0)
        return "phase-diagnostic";

    return unsafeLiveWarp ? "raw-experimental" : "safe-no-live-warp";
}

static void LogVF2Comparison(IFGFeature_Dx12* fg, DLSSG_Dx12* dlssg, HRESULT presentResult, double presentMs)
{
    if (!Config::Instance()->FrameWarpComparisonLog.value_or_default())
        return;

    static uint64_t compareCount = 0;
    compareCount++;
    if (compareCount > 10 && compareCount % 60 != 0)
        return;

    auto& state = State::Instance();
    auto& status = state.frameWarpStatus;

    const bool fgActive = fg != nullptr && fg->IsActive();
    const bool fgPaused = fg != nullptr && fg->IsPaused();
    const uint32_t dlssgRequested = dlssg != nullptr ? dlssg->GetCurrentFramesToGenerate() : 0;
    const uint32_t dlssgMax = dlssg != nullptr ? dlssg->GetMaxFramesToGenerate() : 0;

    const VF2DlssgStateSample dlssgState = Config::Instance()->FrameWarpTimingAuditLog.value_or_default()
        ? VF2DlssgStateSample {
            static_cast<sl::Result>(status.dlssgLastStateResult),
            status.dlssgLastFramesPresented,
            status.dlssgLastStatus
        }
        : QueryVF2DLSSGState();

    status.dlssgLastRequestedFrames = dlssgRequested;
    status.dlssgLastMaxFrames = dlssgMax;
    status.dlssgLastFramesPresented = dlssgState.presented;
    status.dlssgLastStatus = dlssgState.status;
    status.dlssgLastStateResult = static_cast<int32_t>(dlssgState.result);

    sl::Result reflexResult = sl::Result::eErrorFeatureMissing;
    sl::ReflexState reflexState {};
    uint64_t reflexFrameId = 0;
    uint32_t reflexGpuActiveUs = 0;
    uint32_t reflexGpuFrameUs = 0;
    double reflexInputToPresentUs = 0.0;
    double reflexQueueUs = 0.0;
    uint64_t reflexCameraConstructed = 0;

    if (SLProxy::ReflexGetState() != nullptr)
    {
        reflexResult = SLProxy::ReflexGetState()(reflexState);
        if (reflexResult == sl::Result::eOk && reflexState.latencyReportAvailable)
        {
            int latestIndex = -1;
            for (int i = 0; i < sl::kReflexFrameReportCount; ++i)
            {
                if (reflexState.frameReport[i].frameID >= reflexFrameId)
                {
                    reflexFrameId = reflexState.frameReport[i].frameID;
                    latestIndex = i;
                }
            }

            if (latestIndex >= 0 && reflexFrameId != 0)
            {
                const auto& report = reflexState.frameReport[latestIndex];
                reflexGpuActiveUs = report.gpuActiveRenderTimeUs;
                reflexGpuFrameUs = report.gpuFrameTimeUs;
                reflexInputToPresentUs = VF2QpcDeltaUs(
                    report.presentEndTime != 0 ? report.presentEndTime : report.presentStartTime,
                    report.inputSampleTime);
                reflexQueueUs = VF2QpcDeltaUs(report.osRenderQueueEndTime, report.osRenderQueueStartTime);
                reflexCameraConstructed = reflexState.frameReport2[latestIndex].cameraConstructedTime;
            }
        }
    }

    LOG_INFO(
        "VF2Compare #{} present={:X} fgInput={} fgOutput={} active={} paused={} dlssgMode={} dlssgReq={} dlssgMax={} slDLSSGResult={} slDLSSGStatus={:#x} slDLSSGPresented={} reflexResult={} reflexLowLatency={} reflexReport={} reflexFrame={} reflexInputToPresentUs={:.1f} reflexQueueUs={:.1f} reflexGpuActiveUs={} reflexGpuFrameUs={} cameraConstructed={} owner={} ownerReason={} skip={} rawAgeMs={:.3f} snapshotAgeMs={:.3f} mouse=({:.1f},{:.1f}) finalMouse=({:.1f},{:.1f}) warpApplied={} depthAware={} depthUsed={} depthReason={} depth={}x{} fmt={} dlssgDistortionInjected={} dlssgDistortionReason={} dlssgLatePresentApplied={} dlssgLatePresentReason={} dlssgLatePresentCounts={}/{} skipped={} presentMs={:.3f}",
        compareCount,
        static_cast<uint32_t>(presentResult),
        VF2FGInputName(state.activeFgInput),
        VF2FGOutputName(state.activeFgOutput),
        fgActive,
        fgPaused,
        Config::Instance()->FrameWarpDLSSGMode.value_or_default(),
        dlssgRequested,
        dlssgMax,
        static_cast<int32_t>(dlssgState.result),
        dlssgState.status,
        dlssgState.presented,
        static_cast<int32_t>(reflexResult),
        reflexState.lowLatencyAvailable,
        reflexState.latencyReportAvailable,
        reflexFrameId,
        reflexInputToPresentUs,
        reflexQueueUs,
        reflexGpuActiveUs,
        reflexGpuFrameUs,
        reflexCameraConstructed,
        status.lastPresentationOwnerName,
        status.lastPresentationOwnerReason,
        status.lastSkipReason,
        status.lastInputSampleAgeMs,
        status.lastSnapshotAgeMs,
        status.lastMouseDeltaDx,
        status.lastMouseDeltaDy,
        status.lastFinalMouseDeltaDx,
        status.lastFinalMouseDeltaDy,
        status.lastWarpApplied,
        status.lastDepthAwareEnabled,
        status.lastDepthUsed,
        status.lastDepthReason,
        status.lastDepthWidth,
        status.lastDepthHeight,
        status.lastDepthFormat,
        status.dlssgDistortionLastInjected,
        status.dlssgDistortionLastReason,
        status.dlssgLatePresentLastApplied,
        status.dlssgLatePresentLastReason,
        status.dlssgLatePresentAppliedCount,
        status.dlssgLatePresentAttemptCount,
        status.dlssgLatePresentSkippedCount,
        presentMs);
}

static void ReportXeFGLastPresentStatus()
{
    auto& state = State::Instance();
    if (state.activeFgOutput != FGOutput::XeFG || state.currentFG == nullptr)
        return;

    auto getLastPresentStatus = XeFGProxy::GetLastPresentStatus();
    auto swapchainContext = reinterpret_cast<xefg_swapchain_handle_t>(state.currentFG->SwapchainContext());
    if (getLastPresentStatus == nullptr || swapchainContext == nullptr)
        return;

    xefg_swapchain_present_status_t presentStatus {};
    auto result = getLastPresentStatus(swapchainContext, &presentStatus);
    FrameWarpRuntime::ReportXeFGPresentStatus(
        presentStatus.framesPresented,
        static_cast<int32_t>(presentStatus.frameGenResult),
        presentStatus.isFrameGenEnabled != 0,
        static_cast<int32_t>(result));

    static uint64_t xefgStatusLogCount = 0;
    static uint32_t lastFramesPresented = std::numeric_limits<uint32_t>::max();
    static int32_t lastFrameGenResult = std::numeric_limits<int32_t>::min();
    static int32_t lastQueryResult = std::numeric_limits<int32_t>::min();
    static bool lastFrameGenEnabled = false;
    const bool frameGenEnabled = presentStatus.isFrameGenEnabled != 0;
    xefgStatusLogCount++;
    if (xefgStatusLogCount <= 10 ||
        xefgStatusLogCount % 300 == 0 ||
        lastFramesPresented != presentStatus.framesPresented ||
        lastFrameGenResult != static_cast<int32_t>(presentStatus.frameGenResult) ||
        lastQueryResult != static_cast<int32_t>(result) ||
        lastFrameGenEnabled != frameGenEnabled)
    {
        LOG_DEBUG("XeFG present status #{} framesPresented={} fgEnabled={} frameGenResult={} query={}",
            xefgStatusLogCount,
            presentStatus.framesPresented,
            frameGenEnabled,
            static_cast<int32_t>(presentStatus.frameGenResult),
            static_cast<int32_t>(result));
    }

    lastFramesPresented = presentStatus.framesPresented;
    lastFrameGenResult = static_cast<int32_t>(presentStatus.frameGenResult);
    lastQueryResult = static_cast<int32_t>(result);
    lastFrameGenEnabled = frameGenEnabled;
}

HRESULT FGHooks::FGPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags,
                           const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    _lastPresentFlags = Flags;

    if (State::Instance().isShuttingDown)
    {
        if (pPresentParameters == nullptr)
            return o_FGSCPresent(This, SyncInterval, Flags);
        else
            return o_FGSCPresent1((IDXGISwapChain1*) This, SyncInterval, Flags, pPresentParameters);
    }

    auto willPresent = (Flags & DXGI_PRESENT_TEST) == 0;
    double fgPresentMs = 0.0;

    if (willPresent)
    {
        State::Instance().FGLastFrame++;

        double ftDelta = 0.0f;
        auto now = Util::MillisecondsNow();

        if (_lastFGFrameTime > 0.0)
            ftDelta = now - _lastFGFrameTime;

        _lastFGFrameTime = now;
        fgPresentMs = ftDelta;
        State::Instance().lastFGFrameTime = ftDelta;

        LOG_DEBUG("flags: {:X}, Frametime: {}", Flags, ftDelta);
    }

    if (willPresent && currentCommandQueue != nullptr)
    {
        UpscalerTimeDx12::ReadUpscalingTime(State::Instance().currentCommandQueue);
    }

    auto fg = State::Instance().currentFG;
    bool mutexUsed = false;
    if (willPresent && fg != nullptr && fg->IsActive() && !fg->IsPaused() &&
        Config::Instance()->FGUseMutexForSwapchain.value_or_default() && fg->Mutex.getOwner() != 2)
    {
        LOG_TRACE("Waiting FG->Mutex 2, current: {}", fg->Mutex.getOwner());
        fg->Mutex.lock(2);
        mutexUsed = true;
        LOG_TRACE("Accuired FG->Mutex: {}", fg->Mutex.getOwner());
    }

    if (willPresent && fg != nullptr)
    {
        // Some games use this callback to render UI even when
        // FG is disabled. So call it when there is FGFeature
        if (State::Instance().activeFgInput == FGInput::FSRFG)
            ffxPresentCallback();
        else if (State::Instance().activeFgInput == FGInput::FSRFG30)
            FSR3FG::ffxPresentCallback();

        // FSRFG now consumes FrameWarp resources inside FSRFG_Dx12::Dispatch(),
        // after the authoritative ring-buffer index is known. Injecting here
        // guesses with GetIndexWillBeDispatched() and can populate the wrong slot.
        if (State::Instance().activeFgOutput != FGOutput::FSRFG)
            TryInjectFrameWarpDistortion(fg, This);

        // And if Optiscalers FG is active call
        // FG Features present
        fg->Present();
    }

    if (willPresent)
    {
        ResTrack_Dx12::ClearPossibleHudless();
        Hudfix_Dx12::PresentStart();
    }

    if (willPresent && Config::Instance()->ForceVsync.has_value())
    {
        LOG_DEBUG("ForceVsync: {}, VsyncInterval: {}, SCAllowTearing: {}, realExclusiveFullscreen: {}",
                  Config::Instance()->ForceVsync.value(), Config::Instance()->VsyncInterval.value_or_default(),
                  State::Instance().SCAllowTearing, State::Instance().realExclusiveFullscreen);

        if (!Config::Instance()->ForceVsync.value())
        {
            SyncInterval = 0;

            if (State::Instance().SCAllowTearing && !State::Instance().realExclusiveFullscreen)
            {
                LOG_DEBUG("Adding DXGI_PRESENT_ALLOW_TEARING");
                Flags |= DXGI_PRESENT_ALLOW_TEARING;
            }
        }
        else
        {
            SyncInterval = Config::Instance()->VsyncInterval.value_or_default();

            if (SyncInterval < 1)
                SyncInterval = 1;

            LOG_DEBUG("Removing DXGI_PRESENT_ALLOW_TEARING");
            Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        }

        LOG_DEBUG("Final SyncInterval: {}", SyncInterval);
    }

    // Used at wrapped_swapchain LocalPresent to determine is frame is interpolated or not
    if (willPresent)
        State::Instance().FGPresentIsCalled = true;

    // DLSS-G requires PCL ePresentStart/ePresentEnd markers around the actual Present call.
    // These markers MUST match the frame token and be in sync with the per-frame data.
    // After Present, slReflexSleep provides native driver-level frame pacing.
    bool isDLSSG = (State::Instance().activeFgOutput == FGOutput::DLSSG);
    DLSSG_Dx12* dlssg = nullptr;
    if (isDLSSG && willPresent && fg != nullptr)
        dlssg = dynamic_cast<DLSSG_Dx12*>(fg);

    static uint64_t vf2DlssgPresentSeqCounter = 0;
    static uint64_t vf2LastRenderSnapshotAtPresent = 0;
    const uint64_t vf2PresentSeq = (willPresent && dlssg != nullptr) ? ++vf2DlssgPresentSeqCounter : 0;
    auto& vf2Status = State::Instance().frameWarpStatus;
    const float vf2PreviousAppliedPx = vf2Status.lastAppliedPixelShift;
    const float vf2PreviousAppliedYaw = vf2Status.lastAppliedDeltaYaw;
    const float vf2PreviousAppliedPitch = vf2Status.lastAppliedDeltaPitch;
    const uint64_t vf2PreviousRenderSnapshot = vf2LastRenderSnapshotAtPresent;
    UINT vf2BackBufferIndex = UINT_MAX;
    UINT vf2BackBufferWidth = 0;
    UINT vf2BackBufferHeight = 0;
    uint32_t vf2BackBufferFormat = 0;
    if (willPresent && dlssg != nullptr && Config::Instance()->FrameWarpTimingAuditLog.value_or_default())
        QueryVF2Backbuffer(This, vf2BackBufferIndex, vf2BackBufferWidth, vf2BackBufferHeight, vf2BackBufferFormat);
    const VF2DlssgStateSample vf2SlBefore =
        (willPresent && dlssg != nullptr && Config::Instance()->FrameWarpTimingAuditLog.value_or_default())
            ? QueryVF2DLSSGState()
            : VF2DlssgStateSample {};
    if (willPresent && dlssg != nullptr && Config::Instance()->FrameWarpTimingAuditLog.value_or_default())
    {
        vf2Status.dlssgLastFramesPresented = vf2SlBefore.presented;
        vf2Status.dlssgLastStatus = vf2SlBefore.status;
        vf2Status.dlssgLastStateResult = static_cast<int32_t>(vf2SlBefore.result);
    }

    // Experimental VibeFlex2 path: warp the DLSSG FG swapchain backbuffer as late as
    // we can reach it, after DLSSG dispatch and immediately before the actual Present.
    const int64_t vf2QpcTryBegin = (willPresent && dlssg != nullptr) ? VF2QpcNow() : 0;
    const bool frameWarpDlssgLatePresentSubmitted =
        willPresent && dlssg != nullptr ? TryApplyDLSSGLatePresentWarp(This) : false;
    const bool frameWarpDlssgHudlessSubmitPrepared =
        willPresent && dlssg != nullptr &&
        Config::Instance()->FrameWarpEnabled.value_or_default() &&
        Config::Instance()->FrameWarpDLSSGMode.value_or_default() == 4
            ? dlssg->PrepareHudlessBackbufferForPresent()
            : false;
    (void) frameWarpDlssgHudlessSubmitPrepared;
    const int64_t vf2QpcTryEnd = (willPresent && dlssg != nullptr) ? VF2QpcNow() : 0;

    if (dlssg != nullptr)
        dlssg->SetPCLPresentStart();

    const uint32_t presentParamMode = Config::Instance()->FrameWarpPresentParamMode.value_or_default();
    const bool controlPresentParams =
        isDLSSG &&
        willPresent &&
        pPresentParameters != nullptr &&
        Config::Instance()->FrameWarpEnabled.value_or_default() &&
        Config::Instance()->FrameWarpDLSSGMode.value_or_default() == 3;
    const UINT originalDirtyRects =
        pPresentParameters != nullptr ? pPresentParameters->DirtyRectsCount : 0;
    const bool originalHasScrollRect =
        pPresentParameters != nullptr && pPresentParameters->pScrollRect != nullptr;
    const bool originalHasScrollOffset =
        pPresentParameters != nullptr && pPresentParameters->pScrollOffset != nullptr;
    DXGI_PRESENT_PARAMETERS emptyPresentParams {};
    DXGI_PRESENT_PARAMETERS fullFramePresentParams {};
    RECT fullFrameDirtyRect {};
    const DXGI_PRESENT_PARAMETERS* paramsForPresent = pPresentParameters;
    const char* presentParamPath = pPresentParameters == nullptr ? "present" : "original";
    bool presentParamsReplaced = false;

    if (controlPresentParams)
    {
        if (presentParamMode == 0)
        {
            if (frameWarpDlssgLatePresentSubmitted)
            {
                paramsForPresent = &emptyPresentParams;
                presentParamPath = "current-empty-on-submit";
                presentParamsReplaced = paramsForPresent != pPresentParameters;
            }
        }
        else if (presentParamMode == 1)
        {
            paramsForPresent = pPresentParameters;
            presentParamPath = "original";
        }
        else if (presentParamMode == 2)
        {
            paramsForPresent = &emptyPresentParams;
            presentParamPath = "empty";
            presentParamsReplaced = paramsForPresent != pPresentParameters;
        }
        else if (presentParamMode == 3)
        {
            DXGI_SWAP_CHAIN_DESC swapDesc {};
            if (SUCCEEDED(This->GetDesc(&swapDesc)) &&
                swapDesc.BufferDesc.Width > 0 &&
                swapDesc.BufferDesc.Height > 0)
            {
                fullFrameDirtyRect.left = 0;
                fullFrameDirtyRect.top = 0;
                fullFrameDirtyRect.right = static_cast<LONG>(swapDesc.BufferDesc.Width);
                fullFrameDirtyRect.bottom = static_cast<LONG>(swapDesc.BufferDesc.Height);
                fullFramePresentParams.DirtyRectsCount = 1;
                fullFramePresentParams.pDirtyRects = &fullFrameDirtyRect;
                paramsForPresent = &fullFramePresentParams;
                presentParamPath = "full-frame";
            }
            else
            {
                paramsForPresent = &emptyPresentParams;
                presentParamPath = "full-frame-fallback-empty";
            }
            presentParamsReplaced = paramsForPresent != pPresentParameters;
        }
    }

    HRESULT result;
    const int64_t vf2QpcPresentBegin = (willPresent && dlssg != nullptr) ? VF2QpcNow() : 0;
    if (pPresentParameters == nullptr)
        result = o_FGSCPresent(This, SyncInterval, Flags);
    else
        result = o_FGSCPresent1((IDXGISwapChain1*) This, SyncInterval, Flags, paramsForPresent);
    const int64_t vf2QpcPresentEnd = (willPresent && dlssg != nullptr) ? VF2QpcNow() : 0;

    if (controlPresentParams && Config::Instance()->FrameWarpTimingAuditLog.value_or_default())
    {
        static uint64_t presentAuditCount = 0;
        presentAuditCount++;
        if (presentAuditCount <= 600 || presentAuditCount % 60 == 0)
        {
            const UINT usedDirtyRects =
                paramsForPresent != nullptr ? paramsForPresent->DirtyRectsCount : 0;
            const bool usedHasScrollRect =
                paramsForPresent != nullptr && paramsForPresent->pScrollRect != nullptr;
            const bool usedHasScrollOffset =
                paramsForPresent != nullptr && paramsForPresent->pScrollOffset != nullptr;
            LOG_INFO("VF2PresentAudit #{} frame={} submitted={} testMode={} paramMode={} path={} replaced={} originalPtr={:X} originalDirty={} originalScrollRect={} originalScrollOffset={} usedDirty={} usedScrollRect={} usedScrollOffset={} sync={} flags={:X} result={:X} fgPresentMs={:.3f}",
                presentAuditCount,
                State::Instance().frameCount,
                frameWarpDlssgLatePresentSubmitted,
                Config::Instance()->FrameWarpDLSSGLatePresentTestMode.value_or_default(),
                presentParamMode,
                presentParamPath,
                presentParamsReplaced,
                reinterpret_cast<size_t>(pPresentParameters),
                originalDirtyRects,
                originalHasScrollRect,
                originalHasScrollOffset,
                usedDirtyRects,
                usedHasScrollRect,
                usedHasScrollOffset,
                SyncInterval,
                Flags,
                static_cast<uint32_t>(result),
                fgPresentMs);
        }
    }

    if (willPresent && dlssg != nullptr && Config::Instance()->FrameWarpTimingAuditLog.value_or_default())
    {
        static uint64_t phaseAuditCount = 0;
        phaseAuditCount++;
        const bool shouldLogPhase = phaseAuditCount <= 600 || phaseAuditCount % 60 == 0;
        const VF2DlssgStateSample vf2SlAfter = QueryVF2DLSSGState();
        vf2Status.dlssgLastFramesPresented = vf2SlAfter.presented;
        vf2Status.dlssgLastStatus = vf2SlAfter.status;
        vf2Status.dlssgLastStateResult = static_cast<int32_t>(vf2SlAfter.result);

        const VF2DLSSGPhaseClassification vf2Phase = ClassifyVF2DLSSGPhase(
            vf2SlBefore,
            vf2SlAfter,
            vf2PresentSeq,
            vf2Status.renderSnapshotCount,
            vf2PreviousRenderSnapshot);
        StoreVF2DLSSGPhaseClassification(
            vf2Status,
            vf2Phase,
            vf2PresentSeq,
            vf2Status.renderSnapshotCount,
            vf2PreviousRenderSnapshot,
            vf2SlBefore,
            vf2SlAfter);

        const bool vf2FrameWarpApplied = frameWarpDlssgLatePresentSubmitted && vf2Status.lastWarpApplied;
        if (vf2FrameWarpApplied)
        {
            vf2Status.lastAppliedPresentSeq = vf2PresentSeq;
            vf2Status.lastRenderSnapshotAtPresent = vf2Status.renderSnapshotCount;
        }
        vf2LastRenderSnapshotAtPresent = vf2Status.renderSnapshotCount;

        if (shouldLogPhase)
        {
            const float appliedDeltaYaw = vf2Status.lastAppliedDeltaYaw - vf2PreviousAppliedYaw;
            const float appliedDeltaPitch = vf2Status.lastAppliedDeltaPitch - vf2PreviousAppliedPitch;
            const float appliedDeltaPx = vf2Status.lastAppliedPixelShift - vf2PreviousAppliedPx;
            const uint32_t phaseMode = Config::Instance()->FrameWarpDLSSGPhaseMode.value_or_default();
            const bool unsafeLiveWarp = Config::Instance()->FrameWarpDLSSGUnsafeLiveWarp.value_or_default();
            LOG_INFO("VF2PhaseAudit #{} frame={} presentSeq={} phaseClass={} phaseConfidence={:.2f} phaseEligible={} phaseSlHeuristic={} phaseReason={} phaseMode={} livePolicy={} safetySuppressed={} qpcTryBegin={} qpcTryEnd={} qpcPresentBegin={} qpcPresentEnd={} fgPresentMs={:.3f} backbuffer={} size={}x{} fmt={} sync={} flags={:X} presentResult={:X} frameWarpSubmitted={} frameWarpApplied={} skip={} preparedStatus={} preparedReason={} preparedPx={:.3f} preparedYaw={:.6f} preparedPitch={:.6f} previousAppliedPx={:.3f} previousAppliedYaw={:.6f} previousAppliedPitch={:.6f} appliedDeltaPx={:.3f} appliedDeltaYaw={:.6f} appliedDeltaPitch={:.6f} renderSnapshot={} previousRenderSnapshot={} renderSnapshotChanged={} rawSeq={} snapSeq={} sinceSnap={} snapAgeMs={:.3f} sampleAgeMs={:.3f} slStateResultBefore={} slPresentedBefore={} slStatusBefore={:#x} slStateResultAfter={} slPresentedAfter={} slStatusAfter={:#x} presentParamsPath={} previousFenceComplete={} previousSubmitAgeMs={:.3f} tryCpuMs={:.3f} presentCpuMs={:.3f}",
                phaseAuditCount,
                State::Instance().frameCount,
                vf2PresentSeq,
                vf2Phase.label,
                vf2Phase.confidence,
                vf2Phase.futureEligible,
                vf2Phase.slHeuristic,
                vf2Phase.reason,
                phaseMode,
                VF2DLSSGLiveWarpPolicyName(phaseMode, unsafeLiveWarp),
                vf2Status.dlssgLatePresentSafetySuppressed,
                vf2QpcTryBegin,
                vf2QpcTryEnd,
                vf2QpcPresentBegin,
                vf2QpcPresentEnd,
                fgPresentMs,
                vf2BackBufferIndex,
                vf2BackBufferWidth,
                vf2BackBufferHeight,
                vf2BackBufferFormat,
                SyncInterval,
                Flags,
                static_cast<uint32_t>(result),
                frameWarpDlssgLatePresentSubmitted,
                vf2FrameWarpApplied,
                vf2Status.lastSkipReason[0] ? vf2Status.lastSkipReason : "none",
                vf2Status.lastPreparedStatus[0] ? vf2Status.lastPreparedStatus : "none",
                vf2Status.lastPreparedReason[0] ? vf2Status.lastPreparedReason : "none",
                vf2Status.lastPreparedPixelShift,
                vf2Status.lastPreparedDeltaYaw,
                vf2Status.lastPreparedDeltaPitch,
                vf2PreviousAppliedPx,
                vf2PreviousAppliedYaw,
                vf2PreviousAppliedPitch,
                appliedDeltaPx,
                appliedDeltaYaw,
                appliedDeltaPitch,
                vf2Status.renderSnapshotCount,
                vf2PreviousRenderSnapshot,
                vf2Phase.snapshotChanged,
                vf2Status.rawInputSequence,
                vf2Status.rawInputSnapshotSequence,
                vf2Status.rawInputSamplesSinceSnapshot,
                vf2Status.lastSnapshotAgeMs,
                vf2Status.lastInputSampleAgeMs,
                static_cast<int32_t>(vf2SlBefore.result),
                vf2SlBefore.presented,
                vf2SlBefore.status,
                static_cast<int32_t>(vf2SlAfter.result),
                vf2SlAfter.presented,
                vf2SlAfter.status,
                presentParamPath,
                vf2Status.lastVf2PreviousFenceComplete,
                vf2Status.lastVf2PreviousSubmitAgeMs,
                VF2QpcDeltaMs(vf2QpcTryEnd, vf2QpcTryBegin),
                VF2QpcDeltaMs(vf2QpcPresentEnd, vf2QpcPresentBegin));
        }
    }

    if (result == S_OK)
    {
        LOG_DEBUG("Result: {:X}", result);
        if (willPresent)
            ReportXeFGLastPresentStatus();
    }
    else
    {
        if (result == DXGI_ERROR_DEVICE_REMOVED && State::Instance().currentD3D12Device != nullptr)
            Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);
    }

    if (dlssg != nullptr)
    {
        dlssg->SetPCLPresentEnd();
        dlssg->CallReflexSleep();
    }

    if (willPresent)
        LogVF2Comparison(fg, dlssg, result, fgPresentMs);

    Hudfix_Dx12::PresentEnd();

    // DLSSG uses native SL Reflex sleep (CallReflexSleep above); skip software frame limiter
    // to prevent double frame pacing that halves the intended frame rate
    if (willPresent && !State::Instance().reflexLimitsFps && State::Instance().activeFgOutput != FGOutput::NoFG &&
        State::Instance().activeFgOutput != FGOutput::DLSSG && !State::Instance().isRunningOnDXVK)
    {
        FrameLimit::sleep(fg != nullptr ? fg->IsActive() && !fg->IsPaused() : false);
    }

    if (mutexUsed && fg != nullptr)
    {
        LOG_TRACE("Releasing FG->Mutex: {}", fg->Mutex.getOwner());
        fg->Mutex.unlockThis(2);
    }

    LOG_DEBUG("Present finished");

    return result;
}

ULONG FGHooks::hkFGRelease(IUnknown* This)
{
    // We already released this one, prevent crashes
    if (This == oldSwapChain)
    {
        LOG_DEBUG("Release called on old swapchain, skipping release and returning 0");
        return 0;
    }

#if (XEFG_RESOURCE_REF_LIMIT == 0)
    // find if this resource in oldBackBuffers, if it is, skip release and return 0
    if (oldBackBuffers.size() > 0)
    {
        for (auto it = oldBackBuffers.begin(); it != oldBackBuffers.end(); ++it)
        {
            if (*it == This)
            {
                LOG_DEBUG("Release called on old backbuffer, skipping release and returning 0");
                return 0;
            }
        }
    }
#endif // (XEFG_RESOURCE_REF_LIMIT == 0)

    static bool skipReleaseChecks = false;

    if (skipReleaseChecks || State::Instance().currentFGSwapchain != This || State::Instance().isShuttingDown)
        return o_FGRelease(This);

    This->AddRef();

    if (!Config::Instance()->FGPreserveSwapChain.value_or_default())
    {
        if (o_FGRelease(This) == 1)
        {
            LOG_DEBUG("");

            WaitForGPUIdle();

            DXGI_SWAP_CHAIN_DESC scDesc {};
            ((IDXGISwapChain*) This)->GetDesc(&scDesc);

            // Release swapchain backbuffers to prevent errors when releasing FG swapchain
            {
                for (UINT i = 0; i < scDesc.BufferCount; i++)
                {
                    ID3D12Resource* backBuffer = nullptr;
                    auto bbResult = ((IDXGISwapChain*) This)->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

                    if (bbResult == S_OK)
                    {
                        LOG_DEBUG("Backbuffer {}: {:X}", i, (size_t) backBuffer);
                        auto refCount = backBuffer->Release();
                        while (refCount > XEFG_RESOURCE_REF_LIMIT)
                        {
                            LOG_DEBUG("Releasing backbuffer {}: RefCount {}", i, refCount);
                            refCount = backBuffer->Release();
                        }

#if (XEFG_RESOURCE_REF_LIMIT == 0)
                        oldBackBuffers.push_back(backBuffer);
#endif
                    }
                    else
                    {
                        LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                        break;
                    }
                }
            }

            // To prevent deadlock when FG release the swapchain
            skipReleaseChecks = true;

            if (State::Instance().currentFG != nullptr)
            {
                LOG_DEBUG("FG Swapchain released, release FG & swapchain context");
                State::Instance().currentFG->ReleaseSwapchain(_hwnd);
            }

            LOG_DEBUG("FG Swapchain released, clearing currentFGSwapchain");
            State::Instance().currentFGSwapchain = nullptr;

            if (State::Instance().currentWrappedSwapchain != nullptr &&
                State::Instance().currentSwapchainDesc.OutputWindow == _hwnd)
            {
                auto refCount = State::Instance().currentWrappedSwapchain->Release();

                while (refCount > 0 && refCount < 0xffffff00)
                {
                    refCount = State::Instance().currentWrappedSwapchain->Release();
                }

                State::Instance().currentWrappedSwapchain = nullptr;
            }

            skipReleaseChecks = false;

            return 0;
        }
    }
    else
    {
        if (o_FGRelease(This) == 1)
        {
            LOG_INFO("Preserving FG Swapchain from release");
            return 0;
        }
    }

    return o_FGRelease(This);
}

HRESULT FGHooks::CallOriginalPresent(void* This, UINT SyncInterval, UINT Flags)
{
    if (o_FGSCPresent != nullptr)
        return o_FGSCPresent((IDXGISwapChain*) This, SyncInterval, Flags);
    return E_FAIL;
}
