#include "pch.h"
#include "wrapped_swapchain.h"

#include <Util.h>
#include <Config.h>

#include <nvapi/fakenvapi.h>
#include <hooks/Reflex_Hooks.h>
#include <latency/NativeLowLatency.h>
#include <hooks/D3D12_Hooks.h>
#include <hooks/FG_Hooks.h>
#include <framewarp/FrameWarp.h>
#include <framegen/dlssg/DLSSG_Native.h>

#include <menu/menu_overlay_dx.h>

#include <misc/FrameLimit.h>
#include <upscaler_time/UpscalerTime_Dx11.h>
#include <upscaler_time/UpscalerTime_Dx12.h>

#include <d3d11.h>
#include <d3d12.h>

#ifdef DXGI_DEBUG_ENABLED
#include <magic_enum.hpp>
#include <dxgidebug.h>

#pragma comment(lib, "dxguid.lib")

#ifdef ENABLE_DEBUG_LAYER_DX12
#include <d3d12sdklayers.h>
#endif
#endif

#pragma intrinsic(_ReturnAddress)

// Used RenderDoc's wrapped object as referance
// https://github.com/baldurk/renderdoc/blob/v1.x/renderdoc/driver/dxgi/dxgi_wrapped.cpp

static int scCount = 0;
static UINT64 _frameCounter = 0;
static double _lastFrameTime = 0;
static bool _dx11Device = false;
static bool _dx12Device = false;

const GUID IID_IUnwrappedDXGISwapChain = {
    0xe8a33b4a, 0x1405, 0x424c, { 0xae, 0x88, 0xd, 0x3e, 0x9d, 0x46, 0xc9, 0x14 }
};

static ID3D12Fence* resizeFence = nullptr;
static UINT64 resizeFenceValue = 0;
static HANDLE resizeFenceEvent = nullptr;

static void WaitForGPUIdle(IUnknown* object)
{
    if (State::Instance().currentD3D12Device == nullptr || object == nullptr)
        return;

    ID3D12CommandQueue* queue = nullptr;
    if (object->QueryInterface(IID_PPV_ARGS(&queue)) != S_OK || queue == nullptr)
        return;

    LOG_DEBUG("Command queue obtained for GPU idle wait");

    if (resizeFence == nullptr)
    {
        HRESULT hr = State::Instance().currentD3D12Device->CreateFence(
            resizeFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence));
        if (FAILED(hr) || resizeFence == nullptr)
        {
            LOG_WARN("WaitForGPUIdle: CreateFence failed {:X}", hr);
            queue->Release();
            return;
        }
    }

    if (resizeFenceEvent == nullptr)
    {
        resizeFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (resizeFenceEvent == nullptr)
        {
            LOG_WARN("WaitForGPUIdle: CreateEvent failed");
            queue->Release();
            return;
        }
    }

    LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

    resizeFenceValue++;
    HRESULT signalHr = queue->Signal(resizeFence, resizeFenceValue);
    if (FAILED(signalHr))
    {
        LOG_WARN("WaitForGPUIdle: Signal failed {:X}", signalHr);
        queue->Release();
        return;
    }

    if (resizeFence->GetCompletedValue() < resizeFenceValue)
    {
        HRESULT eventHr = resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
        if (FAILED(eventHr))
        {
            LOG_WARN("WaitForGPUIdle: SetEventOnCompletion failed {:X}", eventHr);
            queue->Release();
            return;
        }

        // Max 5 sec
        auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
        if (waitResult != WAIT_OBJECT_0)
            LOG_WARN("WaitForGPUIdle timeout or failed: {}", waitResult);
    }

    queue->Release();
}

#ifdef DXGI_DEBUG_ENABLED
void ReportDXGILiveObjects()
{
    IDXGIDebug1* dxgiDebug = nullptr;

    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
    {
        dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
        dxgiDebug->Release();
    }
}

void ReadDxgiInfoQueue()
{
    IDXGIInfoQueue* dxgiInfoQueue = nullptr;
    if (DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiInfoQueue)) == S_OK)
    {
        UINT64 msgCount = dxgiInfoQueue->GetNumStoredMessages(DXGI_DEBUG_ALL);
        for (UINT64 i = 0; i < msgCount; ++i)
        {
            SIZE_T msgLen = 0;
            dxgiInfoQueue->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &msgLen);
            std::vector<char> buf(msgLen);
            auto* msg = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(buf.data());
            dxgiInfoQueue->GetMessage(DXGI_DEBUG_ALL, i, msg, &msgLen);

            auto description = std::string(msg->pDescription, msg->DescriptionByteLength);
            LOG_DEBUG("DXGI Debug Message: Category: {}, Severity: {}, ID: {}, Description: {}",
                      magic_enum::enum_name(msg->Category), magic_enum::enum_name(msg->Severity), msg->ID, description);
        }
    }
}

#ifdef ENABLE_DEBUG_LAYER_DX12
void ReportD3D12LiveObjects(ID3D12Device* device)
{
    ID3D12DebugDevice* debugDevice = nullptr;

    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&debugDevice))))
    {
        debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
        debugDevice->Release();
    }
}
#endif
#endif

static HRESULT LocalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                            const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP)
{
    if (State::Instance().isShuttingDown)
    {
        if (pPresentParameters == nullptr)
            return pSwapChain->Present(SyncInterval, Flags);
        else
            return ((IDXGISwapChain1*) pSwapChain)->Present1(SyncInterval, Flags, pPresentParameters);
    }

    LOG_DEBUG("{}", _frameCounter);

    HRESULT presentResult;

    auto willPresent = (Flags & DXGI_PRESENT_TEST) == 0;
    bool frameWarpInputFrameApplied = false;

    if (willPresent)
    {
        double ftDelta = 0.0;

        auto now = Util::MillisecondsNow();

        if (_lastFrameTime != 0)
            ftDelta = now - _lastFrameTime;

        _lastFrameTime = now;
        State::Instance().presentFrameTime = ftDelta;

        if (State::Instance().currentFG == nullptr)
            State::Instance().lastFGFrameTime = ftDelta;

        LOG_DEBUG("SyncInterval: {}, Flags: {:X}, Frametime: {:0.3f} ms", SyncInterval, Flags, ftDelta);

        // Update swapchain info evey frame
        if (pSwapChain->GetDesc(&State::Instance().currentSwapchainDesc) != S_OK)
            LOG_WARN("Can't get swapchain desc!");
    }

    ID3D11Device* device = nullptr;
    ID3D12Device* device12 = nullptr;
    ID3D12CommandQueue* cq = nullptr;

    bool isD3D11 = false;

    // try to obtain directx objects and find the path
    if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
    {
        isD3D11 = true;
        device->Release();

        if (!_dx11Device)
            LOG_DEBUG("D3D11Device captured");

        _dx11Device = true;
        State::Instance().swapchainApi = DX11;
        State::Instance().currentD3D11Device = device;

        if (!State::Instance().DeviceAdapterNames.contains(device))
        {
            IDXGIDevice* dxgiDevice = nullptr;
            auto qResult = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));

            if (qResult == S_OK)
            {
                IDXGIAdapter* dxgiAdapter = nullptr;
                qResult = dxgiDevice->GetAdapter(&dxgiAdapter);

                if (qResult == S_OK)
                {
                    ScopedSkipSpoofing skipSpoofing {};

                    std::wstring szName;
                    DXGI_ADAPTER_DESC desc {};

                    if (dxgiAdapter->GetDesc(&desc) == S_OK)
                    {
                        szName = desc.Description;
                        auto adapterDesc = wstring_to_string(szName);
                        LOG_INFO("Adapter Desc: {}", adapterDesc);
                        State::Instance().DeviceAdapterNames[device] = adapterDesc;
                    }
                    else
                    {
                        LOG_ERROR("GetDesc: {:X}", (UINT) qResult);
                    }
                }
                else
                {
                    LOG_ERROR("GetAdapter: {:X}", (UINT) qResult);
                }

                if (dxgiAdapter != nullptr)
                    dxgiAdapter->Release();
            }
            else
            {
                LOG_ERROR("QueryInterface: {:X}", (UINT) qResult);
            }

            if (dxgiDevice != nullptr)
                dxgiDevice->Release();
        }
    }
    else if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        cq->Release();

        if (!_dx12Device)
            LOG_DEBUG("D3D12CommandQueue captured");

        ID3D12CommandQueue* realQueue = nullptr;
        if (Util::CheckForRealObject(__FUNCTION__, cq, (IUnknown**) &realQueue))
            cq = realQueue;

        State::Instance().swapchainApi = DX12;

        if (State::Instance().currentCommandQueue == nullptr)
            State::Instance().currentCommandQueue = cq;

        if (cq->GetDevice(IID_PPV_ARGS(&device12)) == S_OK)
        {
            device12->Release();

            if (!_dx12Device)
                LOG_DEBUG("D3D12Device captured");

            _dx12Device = true;

            State::Instance().currentD3D12Device = device12;
            D3D12Hooks::HookDevice(device12);
        }
    }

    auto fg = State::Instance().currentFG;
    const bool optiScalerDlssgLatePresentOwnedByFgHook =
        willPresent &&
        State::Instance().activeFgOutput == FGOutput::DLSSG &&
        Config::Instance()->FrameWarpDLSSGMode.value_or_default() == 3 &&
        !DLSSGNative::IsAttachActive();
    auto frameWarpOwner = optiScalerDlssgLatePresentOwnedByFgHook
        ? FrameWarpPresentationOwner::Disabled
        : FrameWarpRuntime::ResolvePresentationOwner("LocalPresent", willPresent);
    const bool frameWarpStandaloneOwner = frameWarpOwner == FrameWarpPresentationOwner::StandaloneNoFG;
    const bool frameWarpXeFGFinalOwner = frameWarpOwner == FrameWarpPresentationOwner::XeFGInputFrame;
    const bool frameWarpNativeDLSSGOwner = frameWarpOwner == FrameWarpPresentationOwner::NativeDLSSGLatePresent;
    const bool frameWarpXeFGInternalPresent = frameWarpXeFGFinalOwner && FGHooks::IsXeFGInternalPresentActive();
    if (willPresent && (frameWarpXeFGFinalOwner || frameWarpNativeDLSSGOwner))
    {
        auto& fwStatus = State::Instance().frameWarpStatus;
        if (frameWarpXeFGFinalOwner)
        {
            fwStatus.xefgFinalPresentCount++;
            fwStatus.xefgFinalPresentLastInternal = frameWarpXeFGInternalPresent;
            if (frameWarpXeFGInternalPresent)
                fwStatus.xefgFinalPresentInternalCount++;
            else
                fwStatus.xefgFinalPresentAsyncCount++;

            static uint64_t xefgLocalPresentLogCount = 0;
            xefgLocalPresentLogCount++;
            if (xefgLocalPresentLogCount <= 10 || xefgLocalPresentLogCount % 300 == 0)
                LOG_DEBUG("FrameWarp: XeFG final LocalPresent #{} internal={} async={}",
                    fwStatus.xefgFinalPresentCount,
                    frameWarpXeFGInternalPresent,
                    !frameWarpXeFGInternalPresent);
        }
        else
        {
            fwStatus.dlssgLatePresentAttemptCount++;
            fwStatus.dlssgLatePresentLastApplied = false;
            strncpy_s(fwStatus.dlssgLatePresentLastReason, "native DLSSG pending warp", _TRUNCATE);
        }
    }
    if (willPresent && fg != nullptr)
        ReflexHooks::update(fg->IsActive(), false);
    else
        ReflexHooks::update(false, false);

    // Upscaler GPU time computation
    if (willPresent && (fg == nullptr || !fg->IsActive() || fg->IsPaused()))
    {
        if (cq != nullptr)
        {
            UpscalerTimeDx12::ReadUpscalingTime(cq);
        }
        else if (device != nullptr)
        {
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            UpscalerTimeDx11::ReadUpscalingTime(context);
            context->Release();
        }
    }

    // Fallback when FGPresent is not hooked for V-sync
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
            // Remove allow tearing
            SyncInterval = Config::Instance()->VsyncInterval.value_or_default();

            if (SyncInterval < 1)
                SyncInterval = 1;

            LOG_DEBUG("Removing DXGI_PRESENT_ALLOW_TEARING");
            Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        }

        LOG_DEBUG("Final SyncInterval: {}", SyncInterval);
    }

    // DXVK check, it's here because of upscaler time calculations
    if (State::Instance().isRunningOnDXVK)
    {
        if (pPresentParameters == nullptr)
            presentResult = pSwapChain->Present(SyncInterval, Flags);
        else
            presentResult = ((IDXGISwapChain1*) pSwapChain)->Present1(SyncInterval, Flags, pPresentParameters);

        if (presentResult == S_OK)
        {
            LOG_TRACE("3 {}", (UINT) presentResult);
        }
        else if (presentResult == DXGI_ERROR_DEVICE_REMOVED)
        {
            if (isD3D11)
            {
                if (State::Instance().currentD3D11Device != nullptr)
                    Util::GetDeviceRemovedReason(State::Instance().currentD3D11Device);
            }
            else
            {
                if (State::Instance().currentD3D12Device != nullptr)
                    Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);
            }
        }
        else
        {
            LOG_ERROR("3 {:X}", (UINT) presentResult);
        }

        return presentResult;
    }

    if (willPresent)
    {
        // Tick feature to let it know if it's frozen
        if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
            currentFeature->TickFrozenCheck();

        // FrameWarp input-frame/final-present path: run before the overlay so UI is not warped.
        // FSRFG owns its generated-frame path through the present callback.
        // XeFG's public API does not expose generated-frame callbacks; warp every
        // real LocalPresent it emits so both synchronous and async outputs are covered.
        if (!State::Instance().dx11FGMode && Config::Instance()->FrameWarpEnabled.value_or_default() &&
            (frameWarpStandaloneOwner || frameWarpXeFGFinalOwner || frameWarpNativeDLSSGOwner))
        {
            ID3D12CommandQueue* frameWarpQueue = nullptr;
            if (pDevice != nullptr && pDevice->QueryInterface(IID_PPV_ARGS(&frameWarpQueue)) == S_OK)
            {
                ID3D12Device* frameWarpDevice = nullptr;
                if (frameWarpQueue->GetDevice(IID_PPV_ARGS(&frameWarpDevice)) == S_OK && frameWarpDevice != nullptr)
                {
                    IDXGISwapChain3* swapChain3 = nullptr;
                    if (pSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain3)) == S_OK && swapChain3 != nullptr)
                    {
                        ID3D12Resource* backBuffer = nullptr;
                        UINT backBufferIndex = swapChain3->GetCurrentBackBufferIndex();
                        if (swapChain3->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer)) == S_OK &&
                            backBuffer != nullptr)
                        {
                            auto bbDesc = backBuffer->GetDesc();
                            UINT width = static_cast<UINT>(bbDesc.Width);
                            UINT height = bbDesc.Height;
                            DXGI_FORMAT format = bbDesc.Format;

                            if (FrameWarpRuntime::EnsureInitialized(frameWarpDevice, width, height, format))
                            {
                                auto& fwStatus = State::Instance().frameWarpStatus;
                                if (frameWarpXeFGFinalOwner)
                                    fwStatus.xefgFinalWarpAttemptCount++;

                                frameWarpInputFrameApplied =
                                    FrameWarpRuntime::ApplyStandalone(pSwapChain, frameWarpQueue);

                                if (frameWarpXeFGFinalOwner)
                                {
                                    if (fwStatus.lastWarpApplied)
                                        fwStatus.xefgFinalWarpAppliedCount++;
                                    else
                                        fwStatus.xefgFinalWarpSkippedCount++;
                                }
                                else if (frameWarpNativeDLSSGOwner)
                                {
                                    fwStatus.dlssgLatePresentLastApplied = fwStatus.lastWarpApplied;
                                    if (fwStatus.lastWarpApplied)
                                    {
                                        fwStatus.dlssgLatePresentAppliedCount++;
                                        strncpy_s(fwStatus.dlssgLatePresentLastReason,
                                                  "native DLSSG late-present warped", _TRUNCATE);
                                    }
                                    else
                                    {
                                        fwStatus.dlssgLatePresentSkippedCount++;
                                        strncpy_s(fwStatus.dlssgLatePresentLastReason,
                                                  fwStatus.lastSkipReason[0] ? fwStatus.lastSkipReason
                                                                            : "native warp not applied",
                                                  _TRUNCATE);
                                    }
                                }
                            }
                            else if (frameWarpNativeDLSSGOwner)
                            {
                                auto& fwStatus = State::Instance().frameWarpStatus;
                                fwStatus.dlssgLatePresentSkippedCount++;
                                strncpy_s(fwStatus.dlssgLatePresentLastReason, "FrameWarp init failed", _TRUNCATE);
                            }

                            backBuffer->Release();
                        }
                        else
                        {
                            LOG_DEBUG("FrameWarp input-frame skipped: current backbuffer unavailable");
                            if (frameWarpNativeDLSSGOwner)
                            {
                                auto& fwStatus = State::Instance().frameWarpStatus;
                                fwStatus.dlssgLatePresentSkippedCount++;
                                strncpy_s(fwStatus.dlssgLatePresentLastReason, "backbuffer unavailable", _TRUNCATE);
                            }
                        }

                        swapChain3->Release();
                    }
                    else
                    {
                        LOG_DEBUG("FrameWarp input-frame skipped: swapchain3 unavailable during init");
                        if (frameWarpNativeDLSSGOwner)
                        {
                            auto& fwStatus = State::Instance().frameWarpStatus;
                            fwStatus.dlssgLatePresentSkippedCount++;
                            strncpy_s(fwStatus.dlssgLatePresentLastReason, "swapchain3 unavailable", _TRUNCATE);
                        }
                    }

                    frameWarpDevice->Release();
                }

                frameWarpQueue->Release();
            }
            else if (frameWarpNativeDLSSGOwner)
            {
                auto& fwStatus = State::Instance().frameWarpStatus;
                fwStatus.dlssgLatePresentSkippedCount++;
                strncpy_s(fwStatus.dlssgLatePresentLastReason, "command queue unavailable", _TRUNCATE);
            }
        }

        // Draw overlay — skip in DX11 FG mode: LocalPresent runs on Streamline's background
        // thread, but RenderImGui_DX11 uses the DX11 device context (single-threaded) and
        // ImGui (not thread-safe). The menu is already rendered on the game thread in
        // WrappedIDXGISwapChain4::Present before Dx11FGProxyPresent copies to the FG backbuffer.
        if (!State::Instance().dx11FGMode)
            MenuOverlayDx::Present(pSwapChain, SyncInterval, Flags, pPresentParameters, pDevice, hWnd, isUWP);

        LOG_DEBUG("Calling fakenvapi");
        if (State::Instance().activeFgOutput == FGOutput::FSRFG || State::Instance().activeFgOutput == FGOutput::XeFG ||
            State::Instance().activeFgOutput == FGOutput::DLSSG)
        {
            static UINT64 fgPresentFrame = 0;
            auto fgIsActive = fg != nullptr && fg->IsActive() && !fg->IsPaused();

            if (State::Instance().FGPresentIsCalled)
            {
                State::Instance().FGPresentIsCalled = false;
                fgPresentFrame = _frameCounter;
            }

            auto isInterpolated = fgIsActive && (_frameCounter - fgPresentFrame) > 0;

            if (NativeLowLatency::IsAvailable())
                NativeLowLatency::ReportFGPresent(pSwapChain, fgIsActive, isInterpolated);
            else
                fakenvapi::reportFGPresent(pSwapChain, fgIsActive, isInterpolated);
        }

        _frameCounter++;
        State::Instance().frameCount = _frameCounter;
    }

    LOG_DEBUG("Calling original present");

    // swapchain present
    if (pPresentParameters == nullptr)
        presentResult = pSwapChain->Present(SyncInterval, Flags);
    else if (frameWarpInputFrameApplied)
    {
        DXGI_PRESENT_PARAMETERS frameWarpPresentParams {};
        presentResult = ((IDXGISwapChain1*) pSwapChain)->Present1(SyncInterval, Flags, &frameWarpPresentParams);
    }
    else
        presentResult = ((IDXGISwapChain1*) pSwapChain)->Present1(SyncInterval, Flags, pPresentParameters);

    LOG_DEBUG("Original present result: {:X}", (UINT) presentResult);

    if (willPresent && State::Instance().activeFgOutput == FGOutput::DLSSG &&
        Config::Instance()->FrameWarpComparisonLog.value_or_default() &&
        (State::Instance().dlssgNativeStreamlineDetected || DLSSGNative::IsNativeRuntimeActive()))
    {
        static uint64_t nativeCompareCount = 0;
        nativeCompareCount++;
        if (nativeCompareCount <= 10 || nativeCompareCount % 60 == 0)
        {
            auto& state = State::Instance();
            auto& status = state.frameWarpStatus;
            LOG_INFO("VF2NativeDLSSG #{} present={:X} nativeDetected={} attach={} passthrough={} evalCount={} lastEvalFrame={} owner={} reason={} warpApplied={} rawAgeMs={:.3f} mouse=({:.1f},{:.1f}) depthUsed={} depthReason={} latePresent={}/{} skip={} lateReason={} module={} path={}",
                     nativeCompareCount,
                     static_cast<uint32_t>(presentResult),
                     state.dlssgNativeStreamlineDetected,
                     state.dlssgNativeAttachActive,
                     state.dlssgNativePassthroughActive,
                     state.dlssgNativeEvaluateCount,
                     state.dlssgNativeLastEvaluateFrame,
                     status.lastPresentationOwnerName,
                     status.lastPresentationOwnerReason,
                     status.lastWarpApplied,
                     status.lastInputSampleAgeMs,
                     status.lastFinalMouseDeltaDx,
                     status.lastFinalMouseDeltaDy,
                     status.lastDepthUsed,
                     status.lastDepthReason,
                     status.dlssgLatePresentAppliedCount,
                     status.dlssgLatePresentAttemptCount,
                     status.dlssgLatePresentSkippedCount,
                     status.dlssgLatePresentLastReason,
                     state.dlssgNativeLastModule[0] ? state.dlssgNativeLastModule : "none",
                     state.dlssgNativeLastPath[0] ? state.dlssgNativeLastPath : "unknown");
        }
    }

    if (presentResult == S_OK)
    {
        LOG_TRACE("4 {}, Present result: {:X}", _frameCounter, (UINT) presentResult);
    }
    else
        LOG_ERROR("4 {:X}", (UINT) presentResult);

    LOG_DEBUG("Done");

    return presentResult;
}

WrappedIDXGISwapChain4::WrappedIDXGISwapChain4(IDXGISwapChain* real, IUnknown* pDevice, HWND hWnd, UINT flags,
                                               bool isUWP)
    : _real(real), _device(pDevice), _handle(hWnd), _refcount(1), _uwp(isUWP)
{
    _id = ++scCount;
    _lastFlags = flags;

    _real->QueryInterface(IID_PPV_ARGS(&_real1));
    if (_real1 != nullptr)
        _real1->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real2));
    if (_real2 != nullptr)
        _real2->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real3));
    if (_real3 != nullptr)
        _real3->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real4));
    if (_real4 != nullptr)
        _real4->Release();

    _real->AddRef();
    auto refCount = _real->Release();

    _device2 = _device;

    LOG_INFO("{} created, real: {:X}, refCount: {}", _id, (UINT64) real, refCount);
}

WrappedIDXGISwapChain4::~WrappedIDXGISwapChain4() {}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::QueryInterface(REFIID riid, void** ppvObject)
{
    LOG_TRACE("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));

    if (riid == __uuidof(IDXGISwapChain))
    {
        AddRef();
        *ppvObject = (IDXGISwapChain*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGISwapChain1))
    {
        if (_real1)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain1*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain2))
    {
        if (_real2)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain2*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain3))
    {
        if (_real3)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain3*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain4))
    {
        if (_real4)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain4*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(WrappedIDXGISwapChain4))
    {
        AddRef();
        *ppvObject = this;
        return S_OK;
    }
    else if (riid == __uuidof(IUnknown))
    {
        AddRef();
        *ppvObject = (IUnknown*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGIObject))
    {
        AddRef();
        *ppvObject = (IDXGIObject*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGIDeviceSubObject))
    {
        AddRef();
        *ppvObject = (IDXGIDeviceSubObject*) this;
        return S_OK;
    }

    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain4::AddRef()
{
    InterlockedIncrement(&_refcount);
    LOG_TRACE("Count: {}, caller: {}", _refcount, Util::WhoIsTheCaller(_ReturnAddress()));
    return _refcount;
}

ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain4::Release()
{
    ULONG ret = InterlockedDecrement(&_refcount);

    LOG_TRACE("Count: {}, caller: {}", _refcount, Util::WhoIsTheCaller(_ReturnAddress()));

    // Preserve swapchain when SL releasing it
    if (ret == 0 && State::Instance().activeFgOutput != FGOutput::NoFG &&
        State::Instance().activeFgOutput != FGOutput::Nukems &&
        Config::Instance()->FGPreserveSwapChain.value_or_default() && !State::Instance().isShuttingDown)
    {
        LOG_DEBUG("Real swapchain is released, probaby SL. Preserving FG swapchain");
        AddRef();
        return ret;
    }

    if (ret == 0)
    {
#ifdef USE_LOCAL_MUTEX
        OwnedLockGuard lock(_localMutex, 999);
#endif

        MenuOverlayDx::CleanupRenderTarget(true, _handle);

        if (State::Instance().currentSwapchain == this)
            State::Instance().currentSwapchain = nullptr;

        if (State::Instance().currentRealSwapchain == this)
            State::Instance().currentRealSwapchain = nullptr;

        auto fg = State::Instance().currentFG;
        if (fg != nullptr && fg->Mutex.getOwner() != 1 && fg->SwapchainContext() != nullptr)
        {
            fg->Deactivate();
            fg->ReleaseSwapchain(_handle);

            if (State::Instance().currentFGSwapchain != nullptr)
                State::Instance().currentFGSwapchain = nullptr;
        }

        // Clean up DX11 FG proxy resources and reset state for re-initialization.
        // Some games (e.g., Crysis 2 Remastered) create a throwaway swapchain during
        // init, destroy it, then create the real one. Without cleanup, proxy resources
        // leak and dx11FGMode stays true, preventing the second swapchain from getting
        // a proper FG proxy.
        if (_dx11FGProxy)
        {
            CleanupDx11FGProxy();
            State::Instance().dx11FGMode = false;
            State::Instance().fgSwapchainWidth = 0;
            State::Instance().fgSwapchainHeight = 0;
            if (State::Instance().currentWrappedSwapchain == (IDXGISwapChain*)this)
                State::Instance().currentWrappedSwapchain = nullptr;
            LOG_INFO("DX11 FG: Proxy destroyed, state reset for re-initialization");
        }

        auto refCount = _real->Release();

        // Disabled for now, cause issues with some games
        /*
        IDXGISwapChain* skSC = nullptr;
        if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
        {
            skSC->Release();
            LOG_DEBUG("Found SK swapchain, skip releasing of main swapchain");
        }
        else
        {
            // Release real swapchain, otherwise it can cause issues when re-creating swapchain with same handle
            while (refCount > 0)
            {
                LOG_DEBUG("Waiting for real swapchain to be released, refCount: {}", refCount);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                refCount = _real->Release();
            }
        }
        */

        LOG_DEBUG("Real swapchain released, refCount: {}", refCount);

        delete this;
    }

    return ret;
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return _real->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return _real->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return _real->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetParent(REFIID riid, void** ppParent)
{
    return _real->GetParent(riid, ppParent);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDevice(REFIID riid, void** ppDevice)
{
    // DX11 FG proxy: _real is DX12 FG swapchain — QI for ID3D11Device would fail
    // Return the real DX11 device stored during proxy init
    if (_dx11FGProxy && _proxyDx11Device != nullptr)
    {
        return _proxyDx11Device->QueryInterface(riid, ppDevice);
    }
    return _real->GetDevice(riid, ppDevice);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present(UINT SyncInterval, UINT Flags)
{
    if (_real == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

    // Deferred FG swapchain resize — NO mutex held, safe from SL re-entry deadlock
    if (_dx11FGProxy && _pendingFGResize.load())
    {
        _pendingFGResize.store(false);
        LOG_INFO("DX11 FG Proxy: Applying deferred FG resize {}x{}", _pendingResizeWidth, _pendingResizeHeight);
        spdlog::default_logger()->flush();
        HRESULT hr = _fgSwapChain->ResizeBuffers(
            _pendingResizeBufferCount, _pendingResizeWidth, _pendingResizeHeight,
            _pendingResizeFormat, _pendingResizeFlags);
        if (FAILED(hr))
            LOG_ERROR("DX11 FG Proxy: Deferred FG resize failed {:X}", (UINT)hr);
        else
            LOG_INFO("DX11 FG Proxy: FG swapchain resized to {}x{}", _pendingResizeWidth, _pendingResizeHeight);
        spdlog::default_logger()->flush();
    }

#ifdef USE_LOCAL_MUTEX
    OwnedLockGuard lock(_localMutex, 4);
#endif

    HRESULT result;

    if ((Flags & DXGI_PRESENT_TEST) == 0)
    {
        // DX11 FG proxy: fence sync + copy to FG backbuffer + trigger FG present
        if (_dx11FGProxy && _fgSwapChain != nullptr)
        {
            // Render menu overlay HERE (outside Dx11FGProxyPresent) so ImGui's
            // stack frames are fully unwound before the deep FG present chain.
            // The combined depth of ImGui + Streamline + DLSSG + NVIDIA driver
            // exceeds the 1MB thread stack limit, causing stack overflow.
            MenuOverlayDx::Present(this, SyncInterval, Flags, nullptr, _device, _handle, _uwp);
            result = Dx11FGProxyPresent(SyncInterval, Flags);
        }
        else
        {
            result = LocalPresent(_real, SyncInterval, Flags, nullptr, _device, _handle, _uwp);

            // When Reflex can't be used to limit, sleep in present
            if (!State::Instance().reflexLimitsFps && State::Instance().activeFgOutput == FGOutput::NoFG &&
                !State::Instance().isRunningOnDXVK)
                FrameLimit::sleep(false);
        }
    }
    else
    {
        result = _real->Present(SyncInterval, Flags);
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    // DX11 FG proxy: return shared DX11 texture instead of real backbuffer
    if (_dx11FGProxy && Buffer < _proxyBufferCount && _proxyBuffers[Buffer] != nullptr)
    {
        return _proxyBuffers[Buffer]->QueryInterface(riid, ppSurface);
    }
    auto result = _real->GetBuffer(Buffer, riid, ppSurface);
    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    LOG_DEBUG("Fullscreen: {}, pTarget: {:X}, Caller: {}", Fullscreen, (size_t) pTarget,
              Util::WhoIsTheCaller(_ReturnAddress()));

    HRESULT result = S_OK;

    bool ffxLock = false;

    {
#ifdef USE_LOCAL_MUTEX
        // dlssg calls this from present it seems
        // don't try to get a mutex when present owns it while dlssg mod is enabled
        if (!(_localMutex.getOwner() == 4 && Config::Instance()->FGInput.value_or_default() == FGInput::Nukems))
            OwnedLockGuard lock(_localMutex, 3);
#endif
        if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
        {

            if (State::Instance().currentFG != nullptr && State::Instance().currentFG->IsActive() &&
                State::Instance().currentFG->Mutex.getOwner() != 3)
            {
                LOG_TRACE("Waiting ffxMutex 3, current: {}", State::Instance().currentFG->Mutex.getOwner());
                State::Instance().currentFG->Mutex.lock(3);
                ffxLock = true;
                LOG_TRACE("Accuired ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
            }
            else
            {
                LOG_TRACE("Skipping ffxMutex, owner is already 3");
            }
        }

        State::Instance().realExclusiveFullscreen = Fullscreen;

        result = _real->SetFullscreenState(Fullscreen, pTarget);

        if (result != S_OK)
            LOG_ERROR("result: {:X}", (UINT) result);
        else
            LOG_DEBUG("result: {:X}", result);
    }

    if (ffxLock)
    {
        LOG_TRACE("Releasing ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
        State::Instance().currentFG->Mutex.unlockThis(3);
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    return _real->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
    // DX11 FG proxy: return the original DX11-compatible desc, not the FG swapchain's
    if (_dx11FGProxy && pDesc)
    {
        *pDesc = _proxyDesc;
        return S_OK;
    }
    return _real->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                                DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    LOG_DEBUG("");

#ifdef USE_LOCAL_MUTEX
    // dlssg calls this from present it seems
    // don't try to get a mutex when present owns it while dlssg mod is enabled
    if (!(_localMutex.getOwner() == 4 && Config::Instance()->FGInput.value_or_default() == FGInput::Nukems))
        OwnedLockGuard lock(_localMutex, 1);
#endif

    if (State::Instance().currentFG != nullptr && Config::Instance()->FGUseMutexForSwapchain.value_or_default() &&
        State::Instance().currentFG->Mutex.getOwner() != 6677 && State::Instance().currentFG->Mutex.getOwner() != 6678)
    {
        LOG_TRACE("Waiting ffxMutex 3, current: {}", State::Instance().currentFG->Mutex.getOwner());
        State::Instance().currentFG->Mutex.lock(3);
        LOG_TRACE("Accuired ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
    }

    HRESULT result;
    DXGI_SWAP_CHAIN_DESC desc {};
    _real->GetDesc(&desc);

    if (Config::Instance()->FGEnabled.value_or_default())
    {
        State::Instance().FGresetCapturedResources = true;
        State::Instance().FGonlyUseCapturedResources = false;
        State::Instance().FGchanged = true;
    }

    MenuOverlayDx::CleanupRenderTarget(true, _handle);

    State::Instance().SCchanged = true;

    if (Config::Instance()->OverrideVsync.value_or_default() && !State::Instance().SCExclusiveFullscreen &&
        State::Instance().currentFG == nullptr)
    {
        LOG_DEBUG("Overriding flags");
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (BufferCount < 2)
            BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    LOG_DEBUG("BufferCount: {0}, Width: {1}, Height: {2}, NewFormat: {3}, SwapChainFlags: {4:X}", BufferCount, Width,
              Height, (UINT) NewFormat, SwapChainFlags);

    // DX11 FG proxy mode: recreate proxy shared textures, do NOT forward to FG swapchain
    // (forwarding causes Streamline to re-enter ResizeBuffers → deadlock on non-recursive mutex)
    if (_dx11FGProxy)
    {
        LOG_INFO("DX11 FG Proxy: ResizeBuffers {}x{}, fmt {}", Width, Height, (UINT)NewFormat);

        // Release old proxy DX12 resources and shared handles (keep fence + cmd list)
        for (UINT i = 0; i < 4; i++)
        {
            if (_proxyDx12Resources[i]) { _proxyDx12Resources[i]->Release(); _proxyDx12Resources[i] = nullptr; }
            if (_proxySharedHandles[i]) { CloseHandle(_proxySharedHandles[i]); _proxySharedHandles[i] = nullptr; }
            if (_proxyBuffers[i]) { _proxyBuffers[i]->Release(); _proxyBuffers[i] = nullptr; }
        }

        // Update proxy desc with new dimensions
        if (Width > 0)  _proxyDesc.BufferDesc.Width = Width;
        if (Height > 0) _proxyDesc.BufferDesc.Height = Height;
        if (NewFormat != DXGI_FORMAT_UNKNOWN) _proxyDesc.BufferDesc.Format = NewFormat;
        if (BufferCount > 0) _proxyBufferCount = std::min(BufferCount, 4u);

        // Recreate shared DX11 textures with new dimensions
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = _proxyDesc.BufferDesc.Width;
        texDesc.Height = _proxyDesc.BufferDesc.Height;
        texDesc.Format = _proxyDesc.BufferDesc.Format;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.SampleDesc = {1, 0};
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        result = S_OK;
        for (UINT i = 0; i < _proxyBufferCount; i++)
        {
            HRESULT hr = _proxyDx11Device->CreateTexture2D(&texDesc, nullptr, &_proxyBuffers[i]);
            if (FAILED(hr) || !_proxyBuffers[i])
            {
                LOG_ERROR("DX11 FG Proxy: ResizeBuffers failed to create texture {} ({:X})", i, (UINT)hr);
                result = hr;
                break;
            }

            IDXGIResource1* dxgiRes = nullptr;
            hr = _proxyBuffers[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes));
            if (FAILED(hr) || !dxgiRes) { result = hr; break; }

            hr = dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &_proxySharedHandles[i]);
            dxgiRes->Release();
            if (FAILED(hr)) { result = hr; break; }

            hr = _proxyDx12Device->OpenSharedHandle(_proxySharedHandles[i], IID_PPV_ARGS(&_proxyDx12Resources[i]));
            if (FAILED(hr)) { result = hr; break; }

            LOG_INFO("DX11 FG Proxy: Resized texture {} ({}x{}, fmt {})",
                     i, texDesc.Width, texDesc.Height, (UINT)texDesc.Format);
        }

        _proxyCurrentBuffer = 0;

        if (result == S_OK && State::Instance().currentFeature == nullptr)
        {
            State::Instance().screenWidth = static_cast<float>(_proxyDesc.BufferDesc.Width);
            State::Instance().screenHeight = static_cast<float>(_proxyDesc.BufferDesc.Height);
            State::Instance().lastMipBias = 100.0f;
            State::Instance().lastMipBiasMax = -100.0f;
        }

        // Update SCbuffers with proxy buffers
        State::Instance().SCbuffers.clear();
        for (UINT i = 0; i < _proxyBufferCount; i++)
        {
            if (_proxyBuffers[i])
            {
                State::Instance().SCbuffers.push_back(_proxyBuffers[i]);
            }
        }

        LOG_DEBUG("DX11 FG Proxy: ResizeBuffers result: {:X}", (UINT)result);

        // Schedule deferred FG swapchain resize (can't do it here — mutex deadlock)
        _pendingResizeBufferCount = _proxyBufferCount;
        _pendingResizeWidth = _proxyDesc.BufferDesc.Width;
        _pendingResizeHeight = _proxyDesc.BufferDesc.Height;
        _pendingResizeFormat = _proxyDesc.BufferDesc.Format;
        _pendingResizeFlags = SwapChainFlags;
        _pendingFGResize.store(true);
        State::Instance().fgSwapchainWidth = _proxyDesc.BufferDesc.Width;
        State::Instance().fgSwapchainHeight = _proxyDesc.BufferDesc.Height;
        LOG_INFO("DX11 FG Proxy: Scheduled deferred FG resize {}x{}", _pendingResizeWidth, _pendingResizeHeight);
        spdlog::default_logger()->flush();

        if (State::Instance().currentFG != nullptr && Config::Instance()->FGUseMutexForSwapchain.value_or_default())
        {
            LOG_TRACE("Releasing ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
            State::Instance().currentFG->Mutex.unlockThis(3);
        }

        return result;
    }

    WaitForGPUIdle(_device);

    // Release swapchain backbuffers to prevent errors when resizing
    if (State::Instance().activeFgOutput != FGOutput::NoFG && State::Instance().activeFgOutput != FGOutput::Nukems &&
        State::Instance().currentFG != nullptr)
    {
        IDXGISwapChain* skSC = nullptr;
        if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
        {
            skSC->Release();
            LOG_DEBUG("Found SK swapchain, skip releasing backbuffers of main swapchain");
        }
        else
        {
            LOG_DEBUG("Releasing backbuffers, count: {}", desc.BufferCount);

            for (UINT i = 0; i < desc.BufferCount; i++)
            {
                ID3D12Resource* backBuffer = nullptr;
                auto bbResult = _real->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

                if (bbResult == S_OK)
                {
                    backBuffer->AddRef();

                    auto refCount = backBuffer->Release();
                    refCount = backBuffer->Release();
                    LOG_DEBUG("Current backbuffer {}, RefCount {}", i, refCount);

                    while (refCount > 1 && refCount < 4294967200ul)
                    {
                        refCount = backBuffer->Release();
                        LOG_DEBUG("Releasing backbuffer {}, RefCount {}", i, refCount);
                    }

                    LOG_DEBUG("Backbuffer {}, RefCount {}", i, refCount);
                }
                else
                {
                    LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                    break;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

#ifdef DXGI_DEBUG_ENABLED
    ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
    ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

    if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        _lastFlags = SwapChainFlags;
        result = _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    else
    {
        _lastFlags = SwapChainFlags;
        result = _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

#ifdef DXGI_DEBUG_ENABLED
    if (result != S_OK)
        ReadDxgiInfoQueue();
#endif

    if (result == S_OK && State::Instance().currentFeature == nullptr)
    {
        State::Instance().screenWidth = static_cast<float>(Width);
        State::Instance().screenHeight = static_cast<float>(Height);
        State::Instance().lastMipBias = 100.0f;
        State::Instance().lastMipBiasMax = -100.0f;
    }

    // Crude implementation of EndlesslyFlowering's AutoHDR-ReShade
    // https://github.com/EndlesslyFlowering/AutoHDR-ReShade
    if (Config::Instance()->ForceHDR.value_or_default())
    {
        LOG_INFO("Force HDR on");

        do
        {
            if (_real3 == nullptr)
                break;

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

                result = _real3->CheckColorSpaceSupport(hdrCS, &css);

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
    }

    State::Instance().SCbuffers.clear();
    UINT bc = BufferCount;
    if (bc == 0 && _real1 != nullptr)
    {
        DXGI_SWAP_CHAIN_DESC1 desc {};

        if (_real1->GetDesc1(&desc) == S_OK)
            bc = desc.BufferCount;
    }

    for (UINT i = 0; i < bc; i++)
    {
        IUnknown* buffer;

        if (_real->GetBuffer(i, IID_PPV_ARGS(&buffer)) == S_OK)
        {
            State::Instance().SCbuffers.push_back(buffer);
            buffer->Release();
        }
    }

    LOG_DEBUG("result: {0:X}", (UINT) result);

    if (State::Instance().currentFG != nullptr && Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        LOG_TRACE("Releasing ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
        State::Instance().currentFG->Mutex.unlockThis(3);
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return _real->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetContainingOutput(IDXGIOutput** ppOutput)
{
    return _real->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return _real->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetLastPresentCount(UINT* pLastPresentCount)
{
    return _real->GetLastPresentCount(pLastPresentCount);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    // DX11 FG proxy: return desc matching proxy dimensions and original swap effect
    if (_dx11FGProxy && pDesc)
    {
        pDesc->Width = _proxyDesc.BufferDesc.Width;
        pDesc->Height = _proxyDesc.BufferDesc.Height;
        pDesc->Format = _proxyDesc.BufferDesc.Format;
        pDesc->Stereo = FALSE;
        pDesc->SampleDesc = _proxyDesc.SampleDesc;
        pDesc->BufferUsage = _proxyDesc.BufferUsage;
        pDesc->BufferCount = _proxyBufferCount;
        pDesc->Scaling = DXGI_SCALING_STRETCH;
        pDesc->SwapEffect = _proxyDesc.SwapEffect;
        pDesc->AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        pDesc->Flags = _proxyDesc.Flags;
        return S_OK;
    }
    return _real1->GetDesc1(pDesc);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    return _real1->GetFullscreenDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetHwnd(HWND* pHwnd) { return _real1->GetHwnd(pHwnd); }

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return _real1->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present1(UINT SyncInterval, UINT Flags,
                                                           const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    if (_real1 == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

    // Deferred FG swapchain resize — NO mutex held, safe from SL re-entry deadlock
    if (_dx11FGProxy && _pendingFGResize.load())
    {
        _pendingFGResize.store(false);
        LOG_INFO("DX11 FG Proxy: Applying deferred FG resize {}x{}", _pendingResizeWidth, _pendingResizeHeight);
        spdlog::default_logger()->flush();
        HRESULT hr = _fgSwapChain->ResizeBuffers(
            _pendingResizeBufferCount, _pendingResizeWidth, _pendingResizeHeight,
            _pendingResizeFormat, _pendingResizeFlags);
        if (FAILED(hr))
            LOG_ERROR("DX11 FG Proxy: Deferred FG resize failed {:X}", (UINT)hr);
        else
            LOG_INFO("DX11 FG Proxy: FG swapchain resized to {}x{}", _pendingResizeWidth, _pendingResizeHeight);
        spdlog::default_logger()->flush();
    }

#ifdef USE_LOCAL_MUTEX
    OwnedLockGuard lock(_localMutex, 5);
#endif

    HRESULT result;

    if ((Flags & DXGI_PRESENT_TEST) == 0)
    {
        // DX11 FG proxy: fence sync + copy to FG backbuffer + trigger FG present
        if (_dx11FGProxy && _fgSwapChain != nullptr)
        {
            MenuOverlayDx::Present(this, SyncInterval, Flags, nullptr, _device, _handle, _uwp);
            result = Dx11FGProxyPresent(SyncInterval, Flags);
        }
        else
        {
            result = LocalPresent(_real1, SyncInterval, Flags, pPresentParameters, _device, _handle, _uwp);

            // When Reflex can't be used to limit, sleep in present
            if (!State::Instance().reflexLimitsFps && State::Instance().activeFgOutput == FGOutput::NoFG &&
                !State::Instance().isRunningOnDXVK)
                FrameLimit::sleep(false);
        }
    }
    else
    {
        result = _real1->Present1(SyncInterval, Flags, pPresentParameters);
    }

    return result;
}

BOOL STDMETHODCALLTYPE WrappedIDXGISwapChain4::IsTemporaryMonoSupported(void)
{
    return _real1->IsTemporaryMonoSupported();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return _real1->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return _real1->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return _real1->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return _real1->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _real1->GetRotation(pRotation);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetSourceSize(UINT Width, UINT Height)
{
    return _real2->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return _real2->GetSourceSize(pWidth, pHeight);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetMaximumFrameLatency(UINT MaxLatency)
{
    return _real2->SetMaximumFrameLatency(MaxLatency);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    return _real2->GetMaximumFrameLatency(pMaxLatency);
}

HANDLE STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFrameLatencyWaitableObject(void)
{
    return _real2->GetFrameLatencyWaitableObject();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2->GetMatrixTransform(pMatrix);
}

UINT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetCurrentBackBufferIndex(void)
{
    if (_dx11FGProxy)
        return _proxyCurrentBuffer;
    auto index = _real3->GetCurrentBackBufferIndex();
    // LOG_TRACE("index: {}", index);
    return index;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                         UINT* pColorSpaceSupport)
{
    return _real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    State::Instance().isHdrActive = ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                                    ColorSpace == DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020 ||
                                    ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020 ||
                                    ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

    return _real3->SetColorSpace1(ColorSpace);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                                 DXGI_FORMAT Format, UINT SwapChainFlags,
                                                                 const UINT* pCreationNodeMask,
                                                                 IUnknown* const* ppPresentQueue)
{
    LOG_DEBUG("");

#ifdef USE_LOCAL_MUTEX
    // dlssg calls this from present it seems
    // don't try to get a mutex when present owns it while dlssg mod is enabled
    if (!(_localMutex.getOwner() == 4 && Config::Instance()->FGInput.value_or_default() == FGInput::Nukems))
        OwnedLockGuard lock(_localMutex, 2);
#endif

    if (State::Instance().currentFG != nullptr && Config::Instance()->FGUseMutexForSwapchain.value_or_default() &&
        State::Instance().currentFG->Mutex.getOwner() != 6677 && State::Instance().currentFG->Mutex.getOwner() != 6678)
    {
        LOG_TRACE("Waiting ffxMutex 3, current: {}", State::Instance().currentFG->Mutex.getOwner());
        State::Instance().currentFG->Mutex.lock(3);
        LOG_TRACE("Accuired ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
    }

    HRESULT result = E_FAIL;
    DXGI_SWAP_CHAIN_DESC desc {};
    _real->GetDesc(&desc);

    if (Config::Instance()->FGEnabled.value_or_default())
    {
        State::Instance().FGresetCapturedResources = true;
        State::Instance().FGonlyUseCapturedResources = false;
        State::Instance().FGchanged = true;
    }

    MenuOverlayDx::CleanupRenderTarget(true, _handle);

    State::Instance().SCchanged = true;

    if (Config::Instance()->OverrideVsync.value_or_default() && !State::Instance().SCExclusiveFullscreen &&
        State::Instance().currentFG == nullptr)
    {
        LOG_DEBUG("Overriding flags");
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (BufferCount < 2)
            BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat: {}, SwapChainFlags: {:X}", BufferCount, Width, Height,
              (UINT) Format, SwapChainFlags);

    // DX11 FG proxy mode: recreate proxy shared textures, do NOT forward to FG swapchain
    if (_dx11FGProxy)
    {
        LOG_INFO("DX11 FG Proxy: ResizeBuffers1 {}x{}, fmt {}", Width, Height, (UINT)Format);

        // Release old proxy DX12 resources and shared handles (keep fence + cmd list)
        for (UINT i = 0; i < 4; i++)
        {
            if (_proxyDx12Resources[i]) { _proxyDx12Resources[i]->Release(); _proxyDx12Resources[i] = nullptr; }
            if (_proxySharedHandles[i]) { CloseHandle(_proxySharedHandles[i]); _proxySharedHandles[i] = nullptr; }
            if (_proxyBuffers[i]) { _proxyBuffers[i]->Release(); _proxyBuffers[i] = nullptr; }
        }

        if (Width > 0)  _proxyDesc.BufferDesc.Width = Width;
        if (Height > 0) _proxyDesc.BufferDesc.Height = Height;
        if (Format != DXGI_FORMAT_UNKNOWN) _proxyDesc.BufferDesc.Format = Format;
        if (BufferCount > 0) _proxyBufferCount = std::min(BufferCount, 4u);

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = _proxyDesc.BufferDesc.Width;
        texDesc.Height = _proxyDesc.BufferDesc.Height;
        texDesc.Format = _proxyDesc.BufferDesc.Format;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.SampleDesc = {1, 0};
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        result = S_OK;
        for (UINT i = 0; i < _proxyBufferCount; i++)
        {
            HRESULT hr = _proxyDx11Device->CreateTexture2D(&texDesc, nullptr, &_proxyBuffers[i]);
            if (FAILED(hr) || !_proxyBuffers[i]) { result = hr; break; }

            IDXGIResource1* dxgiRes = nullptr;
            hr = _proxyBuffers[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes));
            if (FAILED(hr) || !dxgiRes) { result = hr; break; }

            hr = dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &_proxySharedHandles[i]);
            dxgiRes->Release();
            if (FAILED(hr)) { result = hr; break; }

            hr = _proxyDx12Device->OpenSharedHandle(_proxySharedHandles[i], IID_PPV_ARGS(&_proxyDx12Resources[i]));
            if (FAILED(hr)) { result = hr; break; }
        }

        _proxyCurrentBuffer = 0;

        if (result == S_OK && State::Instance().currentFeature == nullptr)
        {
            State::Instance().screenWidth = static_cast<float>(_proxyDesc.BufferDesc.Width);
            State::Instance().screenHeight = static_cast<float>(_proxyDesc.BufferDesc.Height);
            State::Instance().lastMipBias = 100.0f;
            State::Instance().lastMipBiasMax = -100.0f;
        }

        State::Instance().SCbuffers.clear();
        for (UINT i = 0; i < _proxyBufferCount; i++)
        {
            if (_proxyBuffers[i])
                State::Instance().SCbuffers.push_back(_proxyBuffers[i]);
        }

        LOG_DEBUG("DX11 FG Proxy: ResizeBuffers1 result: {:X}", (UINT)result);

        // Schedule deferred FG swapchain resize (can't do it here — mutex deadlock)
        _pendingResizeBufferCount = _proxyBufferCount;
        _pendingResizeWidth = _proxyDesc.BufferDesc.Width;
        _pendingResizeHeight = _proxyDesc.BufferDesc.Height;
        _pendingResizeFormat = _proxyDesc.BufferDesc.Format;
        _pendingResizeFlags = SwapChainFlags;
        _pendingFGResize.store(true);
        State::Instance().fgSwapchainWidth = _proxyDesc.BufferDesc.Width;
        State::Instance().fgSwapchainHeight = _proxyDesc.BufferDesc.Height;
        LOG_INFO("DX11 FG Proxy: Scheduled deferred FG resize {}x{}", _pendingResizeWidth, _pendingResizeHeight);
        spdlog::default_logger()->flush();

        if (State::Instance().activeFgOutput == FGOutput::FSRFG &&
            Config::Instance()->FGUseMutexForSwapchain.value_or_default())
        {
            LOG_TRACE("Releasing ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
            State::Instance().currentFG->Mutex.unlockThis(3);
        }

        return result;
    }

    WaitForGPUIdle(_device);

    // Release swapchain backbuffers to prevent errors when resizing
    if (State::Instance().activeFgOutput != FGOutput::NoFG && State::Instance().activeFgOutput != FGOutput::Nukems &&
        State::Instance().currentFG != nullptr)
    {
        IDXGISwapChain* skSC = nullptr;
        if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
        {
            skSC->Release();
            LOG_DEBUG(
                "Found SK swapchain, skip releasing backbuffersand using ResizeBuffers instead of ResizeBuffers1");

#ifdef DXGI_DEBUG_ENABLED
            ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
            ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            {
                ScopedSkipHeapCapture skipHeapCapture {};

                _lastFlags = SwapChainFlags;
                result = _real3->ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
            }
            else
            {
                _lastFlags = SwapChainFlags;
                result = _real3->ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
            }
        }
        else
        {
            LOG_DEBUG("Releasing backbuffers, count: {}", desc.BufferCount);

            for (UINT i = 0; i < desc.BufferCount; i++)
            {
                ID3D12Resource* backBuffer = nullptr;
                auto bbResult = _real->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

                if (bbResult == S_OK)
                {
                    backBuffer->AddRef();

                    auto refCount = backBuffer->Release();
                    refCount = backBuffer->Release();
                    LOG_DEBUG("Current backbuffer {}, RefCount {}", i, refCount);

                    while (refCount > 1 && refCount < 4294967200ul)
                    {
                        refCount = backBuffer->Release();
                        LOG_DEBUG("Releasing backbuffer {}, RefCount {}", i, refCount);
                    }

                    LOG_DEBUG("Backbuffer {}, RefCount {}", i, refCount);
                }
                else
                {
                    LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                    break;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

#ifdef DXGI_DEBUG_ENABLED
        ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
        ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
        {
            ScopedSkipHeapCapture skipHeapCapture {};

            _lastFlags = SwapChainFlags;
            result = _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                            ppPresentQueue);
        }
        else
        {
            _lastFlags = SwapChainFlags;
            result = _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                            ppPresentQueue);
        }
    }

#ifdef DXGI_DEBUG_ENABLED
    if (result != S_OK)
        ReadDxgiInfoQueue();
#endif

    if (result == S_OK && State::Instance().currentFeature == nullptr)
    {
        State::Instance().screenWidth = static_cast<float>(Width);
        State::Instance().screenHeight = static_cast<float>(Height);
        State::Instance().lastMipBias = 100.0f;
        State::Instance().lastMipBiasMax = -100.0f;
    }

    // Crude implementation of EndlesslyFlowering's AutoHDR-ReShade
    // https://github.com/EndlesslyFlowering/AutoHDR-ReShade
    if (Config::Instance()->ForceHDR.value_or_default())
    {
        LOG_INFO("Force HDR on");

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
    }

    State::Instance().SCbuffers.clear();
    UINT bc = BufferCount;
    if (bc == 0 && _real1 != nullptr)
    {
        DXGI_SWAP_CHAIN_DESC1 desc {};

        if (_real1->GetDesc1(&desc) == S_OK)
            bc = desc.BufferCount;
    }

    for (UINT i = 0; i < bc; i++)
    {
        IUnknown* buffer;

        if (_real->GetBuffer(i, IID_PPV_ARGS(&buffer)) == S_OK)
        {
            State::Instance().SCbuffers.push_back(buffer);
            buffer->Release();
        }
    }

    LOG_DEBUG("result: {0:X}", (UINT) result);

    if (State::Instance().activeFgOutput == FGOutput::FSRFG &&
        Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        LOG_TRACE("Releasing ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
        State::Instance().currentFG->Mutex.unlockThis(3);
    }

    return result;
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size,
                                                                 void* pMetaData)
{
    return _real4->SetHDRMetaData(Type, Size, pMetaData);
}

// ============================================================================
// DX11 Frame Generation Proxy
// ============================================================================

bool WrappedIDXGISwapChain4::InitDx11FGProxy(IDXGISwapChain* fgSwapChain, ID3D11Device* dx11Dev,
                                              ID3D12Device* dx12Dev, ID3D12CommandQueue* dx12Queue,
                                              DXGI_SWAP_CHAIN_DESC* desc)
{
    if (!fgSwapChain || !dx11Dev || !dx12Dev || !dx12Queue || !desc)
    {
        LOG_ERROR("DX11 FG Proxy: Invalid parameters for initialization");
        return false;
    }

    _fgSwapChain = fgSwapChain;       fgSwapChain->AddRef();
    _proxyDx11Device = dx11Dev;        dx11Dev->AddRef();
    _proxyDx12Device = dx12Dev;        dx12Dev->AddRef();
    _proxyDx12Queue = dx12Queue;       dx12Queue->AddRef();
    _proxyDesc = *desc;
    _proxyBufferCount = std::max(desc->BufferCount, 2u);
    if (_proxyBufferCount > 4)
        _proxyBufferCount = 4;

    // Create shared DX11 textures matching backbuffer format
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = desc->BufferDesc.Width;
    texDesc.Height = desc->BufferDesc.Height;
    texDesc.Format = desc->BufferDesc.Format;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.SampleDesc = {1, 0};
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    for (UINT i = 0; i < _proxyBufferCount; i++)
    {
        HRESULT hr = dx11Dev->CreateTexture2D(&texDesc, nullptr, &_proxyBuffers[i]);
        if (FAILED(hr) || !_proxyBuffers[i])
        {
            LOG_ERROR("DX11 FG Proxy: Failed to create shared texture {} ({:X})", i, (UINT)hr);
            CleanupDx11FGProxy();
            return false;
        }

        // Get NT shared handle
        IDXGIResource1* dxgiRes = nullptr;
        hr = _proxyBuffers[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes));
        if (FAILED(hr) || !dxgiRes)
        {
            LOG_ERROR("DX11 FG Proxy: Failed to get IDXGIResource1 for texture {}", i);
            CleanupDx11FGProxy();
            return false;
        }

        hr = dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &_proxySharedHandles[i]);
        dxgiRes->Release();
        if (FAILED(hr))
        {
            LOG_ERROR("DX11 FG Proxy: Failed to create shared handle for texture {} ({:X})", i, (UINT)hr);
            CleanupDx11FGProxy();
            return false;
        }

        // Open in DX12
        hr = dx12Dev->OpenSharedHandle(_proxySharedHandles[i], IID_PPV_ARGS(&_proxyDx12Resources[i]));
        if (FAILED(hr) || !_proxyDx12Resources[i])
        {
            LOG_ERROR("DX11 FG Proxy: Failed to open shared handle in DX12 for texture {} ({:X})", i, (UINT)hr);
            CleanupDx11FGProxy();
            return false;
        }

        LOG_INFO("DX11 FG Proxy: Shared texture {} created ({}x{}, fmt {})",
                 i, texDesc.Width, texDesc.Height, (UINT)texDesc.Format);
    }

    // Create shared fence (DX11 → DX12 sync)
    // ID3D11Device5 is needed for CreateFence
    ID3D11Device5* dev5 = nullptr;
    HRESULT hr = dx11Dev->QueryInterface(IID_PPV_ARGS(&dev5));
    if (FAILED(hr) || !dev5)
    {
        LOG_ERROR("DX11 FG Proxy: ID3D11Device5 not available for fence creation");
        CleanupDx11FGProxy();
        return false;
    }

    hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&_proxyDx11Fence));
    dev5->Release();
    if (FAILED(hr) || !_proxyDx11Fence)
    {
        LOG_ERROR("DX11 FG Proxy: Failed to create DX11 shared fence ({:X})", (UINT)hr);
        CleanupDx11FGProxy();
        return false;
    }

    hr = _proxyDx11Fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &_proxyFenceSharedHandle);
    if (FAILED(hr))
    {
        LOG_ERROR("DX11 FG Proxy: Failed to create fence shared handle ({:X})", (UINT)hr);
        CleanupDx11FGProxy();
        return false;
    }

    hr = dx12Dev->OpenSharedHandle(_proxyFenceSharedHandle, IID_PPV_ARGS(&_proxyDx12Fence));
    if (FAILED(hr) || !_proxyDx12Fence)
    {
        LOG_ERROR("DX11 FG Proxy: Failed to open fence in DX12 ({:X})", (UINT)hr);
        CleanupDx11FGProxy();
        return false;
    }

    // Create DX12 copy command infrastructure
    hr = dx12Dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_proxyCopyAllocator));
    if (FAILED(hr))
    {
        LOG_ERROR("DX11 FG Proxy: Failed to create copy command allocator ({:X})", (UINT)hr);
        CleanupDx11FGProxy();
        return false;
    }

    hr = dx12Dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _proxyCopyAllocator, nullptr,
                                     IID_PPV_ARGS(&_proxyCopyCmdList));
    if (FAILED(hr))
    {
        LOG_ERROR("DX11 FG Proxy: Failed to create copy command list ({:X})", (UINT)hr);
        CleanupDx11FGProxy();
        return false;
    }
    _proxyCopyCmdList->Close();

    _dx11FGProxy = true;

    // Store FG swapchain dimensions in State for UpscalerInputsDx11 to use.
    // In DX11 proxy mode, feature->DisplayWidth() may differ from the actual FG swapchain
    // (e.g., God of War: DLSS targets 3840x2160 internally, but swapchain is 1920x1080).
    State::Instance().fgSwapchainWidth = desc->BufferDesc.Width;
    State::Instance().fgSwapchainHeight = desc->BufferDesc.Height;

    LOG_INFO("DX11 FG Proxy: Initialized successfully ({} buffers, {}x{})",
             _proxyBufferCount, texDesc.Width, texDesc.Height);
    return true;
}

void WrappedIDXGISwapChain4::CleanupDx11FGProxy()
{
    _dx11FGProxy = false;

    if (_proxyCopyCmdList) { _proxyCopyCmdList->Release(); _proxyCopyCmdList = nullptr; }
    if (_proxyCopyAllocator) { _proxyCopyAllocator->Release(); _proxyCopyAllocator = nullptr; }

    // (waitable removed — Streamline's FG swapchain lacks FRAME_LATENCY_WAITABLE_OBJECT flag)

    if (_proxyDx12Fence) { _proxyDx12Fence->Release(); _proxyDx12Fence = nullptr; }
    if (_proxyFenceSharedHandle) { CloseHandle(_proxyFenceSharedHandle); _proxyFenceSharedHandle = nullptr; }
    if (_proxyDx11Fence) { _proxyDx11Fence->Release(); _proxyDx11Fence = nullptr; }

    for (UINT i = 0; i < 4; i++)
    {
        if (_proxyDx12Resources[i]) { _proxyDx12Resources[i]->Release(); _proxyDx12Resources[i] = nullptr; }
        if (_proxySharedHandles[i]) { CloseHandle(_proxySharedHandles[i]); _proxySharedHandles[i] = nullptr; }
        if (_proxyBuffers[i]) { _proxyBuffers[i]->Release(); _proxyBuffers[i] = nullptr; }
    }

    _proxyBufferCount = 0;
    _proxyCurrentBuffer = 0;
    if (_fgSwapChain) { _fgSwapChain->Release(); _fgSwapChain = nullptr; }
    if (_proxyDx11Device) { _proxyDx11Device->Release(); _proxyDx11Device = nullptr; }
    if (_proxyDx12Device) { _proxyDx12Device->Release(); _proxyDx12Device = nullptr; }
    if (_proxyDx12Queue) { _proxyDx12Queue->Release(); _proxyDx12Queue = nullptr; }
}

HRESULT WrappedIDXGISwapChain4::Dx11FGProxyPresent(UINT SyncInterval, UINT Flags)
{
    // Menu overlay is rendered in Present() BEFORE this function is called,
    // to keep ImGui's stack frames off the deep FG present call chain.
    // ReflexHooks::update() is also already called in Present() — do NOT call it
    // again here, as double-incrementing _updatesWithoutMarker causes FPS limit spam.

    // 1. Signal DX11 fence after game finished rendering to proxy texture
    ID3D11DeviceContext* ctx = nullptr;
    _proxyDx11Device->GetImmediateContext(&ctx);
    if (!ctx)
    {
        LOG_ERROR("DX11 FG Proxy: Failed to get immediate context");
        return E_FAIL;
    }

    ID3D11DeviceContext4* ctx4 = nullptr;
    ctx->QueryInterface(IID_PPV_ARGS(&ctx4));
    ctx->Release();
    if (!ctx4)
    {
        LOG_ERROR("DX11 FG Proxy: ID3D11DeviceContext4 not available");
        return E_FAIL;
    }

    _proxyFenceValue++;
    ctx4->Signal(_proxyDx11Fence, _proxyFenceValue);
    ctx4->Flush();
    ctx4->Release();

    // 2. DX12 queue waits for DX11 rendering to complete
    _proxyDx12Queue->Wait(_proxyDx12Fence, _proxyFenceValue);

    // 3. Copy shared proxy texture → FG swapchain backbuffer
    IDXGISwapChain3* fgSC3 = nullptr;
    _fgSwapChain->QueryInterface(IID_PPV_ARGS(&fgSC3));
    if (!fgSC3)
    {
        LOG_ERROR("DX11 FG Proxy: FG swapchain doesn't support IDXGISwapChain3");
        return E_FAIL;
    }

    UINT bbIndex = fgSC3->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = nullptr;
    HRESULT hr = fgSC3->GetBuffer(bbIndex, IID_PPV_ARGS(&backBuffer));
    fgSC3->Release();

    if (FAILED(hr) || !backBuffer)
    {
        LOG_ERROR("DX11 FG Proxy: Failed to get FG backbuffer {} ({:X})", bbIndex, (UINT)hr);
        return hr;
    }

    // DX11 game always renders to proxy buffer 0 (via GetBuffer(0) in BITBLT mode).
    // Always copy from buffer 0 — do NOT cycle _proxyCurrentBuffer.
    const UINT srcBuffer = 0;

    // Record copy commands
    _proxyCopyAllocator->Reset();
    _proxyCopyCmdList->Reset(_proxyCopyAllocator, nullptr);

    // Transition backbuffer: PRESENT → COPY_DEST
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    _proxyCopyCmdList->ResourceBarrier(1, &barrier);

    // Copy proxy texture → backbuffer
    // Shared resources from DX11 are in COMMON state (implicitly promoted)
    _proxyCopyCmdList->CopyResource(backBuffer, _proxyDx12Resources[srcBuffer]);

    // Transition backbuffer: COPY_DEST → PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    _proxyCopyCmdList->ResourceBarrier(1, &barrier);

    _proxyCopyCmdList->Close();

    ID3D12CommandList* cmdLists[] = {_proxyCopyCmdList};
    _proxyDx12Queue->ExecuteCommandLists(1, cmdLists);
    backBuffer->Release();

    _frameCounter++;

    // 4. Trigger present on the real DX12 swapchain
    bool fgEnabled = Config::Instance()->FGEnabled.value_or_default();
    HRESULT presentResult;
    if (fgEnabled)
    {
        _fgDisableCooldown = 5; // Reset cooldown while FG is active
        // Through FG hooks (hkFGPresent) which handles interpolation
        presentResult = _fgSwapChain->Present(SyncInterval, Flags);
        LOG_DEBUG("DX11 FG Proxy: FG Present result={:X}", (UINT)presentResult);
    }
    else if (_fgDisableCooldown > 0)
    {
        // Cooldown after FG disable: skip presenting on the FG swapchain
        // for a few frames to let Streamline fully process its deactivation.
        // CallOriginalPresent goes through Streamline's interposer which does
        // heavy work during shutdown (root signature + pipeline state creation),
        // adding enough stack depth to overflow the NVIDIA driver.
        _fgDisableCooldown--;
        LOG_DEBUG("DX11 FG Proxy: FG disable cooldown, {} frames remaining", _fgDisableCooldown);
        presentResult = S_OK;
    }
    else
    {
        // Streamline has fully drained — safe to present directly
        presentResult = FGHooks::CallOriginalPresent(_fgSwapChain, SyncInterval, Flags);
        LOG_DEBUG("DX11 FG Proxy: Direct Present result={:X}", (UINT)presentResult);
    }

    // Vsync frame pacing: Streamline overrides all DLSSG presents to SyncInterval=0
    // + DXGI_PRESENT_ALLOW_TEARING, removing all DXGI flip queue backpressure.
    // WaitForVBlank restores display-synchronized pacing independently — it blocks
    // until the display controller reaches vertical blank regardless of present mode.
    IDXGIOutput* output = nullptr;
    if (SUCCEEDED(_fgSwapChain->GetContainingOutput(&output)) && output)
    {
        output->WaitForVBlank();
        output->Release();
    }
    else
    {
        // Fallback if GetContainingOutput fails (e.g., minimized window)
        Sleep(1);
    }

    return presentResult;
}
