#include "pch.h"
#include "Upscaler_Inputs_Dx11.h"

void UpscalerInputsDx11::Init(ID3D11Device* dx11Device, ID3D12Device* dx12Device)
{
    if (State::Instance().activeFgInput != FGInput::Upscaler)
        return;

    // Clean up any previous state
    Cleanup();

    _dx11Device = dx11Device;
    _dx12Device = dx12Device;

    if (_dx11Device)
        _dx11Device->AddRef();
    if (_dx12Device)
        _dx12Device->AddRef();

    LOG_INFO("UpscalerInputsDx11::Init - DX11 device: {:X}, DX12 device: {:X}",
             (size_t)_dx11Device, (size_t)_dx12Device);
}

void UpscalerInputsDx11::Cleanup()
{
    for (int i = 0; i < BUFFER_COUNT; i++)
    {
        ReleaseSharedTexture(_sharedMV[i]);
        ReleaseSharedTexture(_sharedDepth[i]);
    }

    if (_dx11Device)
    {
        _dx11Device->Release();
        _dx11Device = nullptr;
    }

    if (_dx12Device)
    {
        _dx12Device->Release();
        _dx12Device = nullptr;
    }
}

void UpscalerInputsDx11::ReleaseSharedTexture(SharedResource& shared)
{
    if (shared.dx12Res)
    {
        shared.dx12Res->Release();
        shared.dx12Res = nullptr;
    }

    if (shared.handle)
    {
        CloseHandle(shared.handle);
        shared.handle = nullptr;
    }

    if (shared.dx11Tex)
    {
        shared.dx11Tex->Release();
        shared.dx11Tex = nullptr;
    }

    shared.width = 0;
    shared.height = 0;
    shared.format = DXGI_FORMAT_UNKNOWN;
}

bool UpscalerInputsDx11::EnsureSharedTexture(ID3D11Resource* source, SharedResource& shared)
{
    if (source == nullptr || _dx11Device == nullptr || _dx12Device == nullptr)
        return false;

    // Get source dimensions
    ID3D11Texture2D* srcTex = nullptr;
    HRESULT hr = source->QueryInterface(IID_PPV_ARGS(&srcTex));
    if (FAILED(hr) || srcTex == nullptr)
    {
        LOG_ERROR("UpscalerInputsDx11::EnsureSharedTexture - Failed to QI source as Texture2D: {:X}", (UINT)hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc {};
    srcTex->GetDesc(&srcDesc);
    srcTex->Release();

    // Determine shared texture format
    DXGI_FORMAT sharedFormat = srcDesc.Format;

    // Handle depth formats that can't be shared directly
    if (sharedFormat == DXGI_FORMAT_R24G8_TYPELESS || sharedFormat == DXGI_FORMAT_D24_UNORM_S8_UINT)
        sharedFormat = DXGI_FORMAT_R32_FLOAT;
    else if (sharedFormat == DXGI_FORMAT_R32G8X24_TYPELESS || sharedFormat == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
        sharedFormat = DXGI_FORMAT_R32_FLOAT;
    else if (sharedFormat == DXGI_FORMAT_D32_FLOAT)
        sharedFormat = DXGI_FORMAT_R32_FLOAT;
    else if (sharedFormat == DXGI_FORMAT_D16_UNORM)
        sharedFormat = DXGI_FORMAT_R16_FLOAT;

    // Check if existing shared texture is still valid
    if (shared.dx11Tex != nullptr && shared.dx12Res != nullptr &&
        shared.width == srcDesc.Width && shared.height == srcDesc.Height &&
        shared.format == sharedFormat)
    {
        return true; // Reuse existing
    }

    // Need to create new shared texture
    LOG_INFO("UpscalerInputsDx11::EnsureSharedTexture - Creating {}x{} format={} (src format={})",
             srcDesc.Width, srcDesc.Height, (UINT)sharedFormat, (UINT)srcDesc.Format);

    ReleaseSharedTexture(shared);

    // Create DX11 shared texture
    D3D11_TEXTURE2D_DESC sharedDesc {};
    sharedDesc.Width = srcDesc.Width;
    sharedDesc.Height = srcDesc.Height;
    sharedDesc.MipLevels = 1;
    sharedDesc.ArraySize = 1;
    sharedDesc.Format = sharedFormat;
    sharedDesc.SampleDesc.Count = 1;
    sharedDesc.SampleDesc.Quality = 0;
    sharedDesc.Usage = D3D11_USAGE_DEFAULT;
    sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sharedDesc.CPUAccessFlags = 0;
    sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    hr = _dx11Device->CreateTexture2D(&sharedDesc, nullptr, &shared.dx11Tex);
    if (FAILED(hr))
    {
        LOG_ERROR("UpscalerInputsDx11::EnsureSharedTexture - CreateTexture2D failed: {:X}", (UINT)hr);
        return false;
    }

    // Get shared handle via IDXGIResource1
    IDXGIResource1* dxgiResource = nullptr;
    hr = shared.dx11Tex->QueryInterface(IID_PPV_ARGS(&dxgiResource));
    if (FAILED(hr))
    {
        LOG_ERROR("UpscalerInputsDx11::EnsureSharedTexture - QI IDXGIResource1 failed: {:X}", (UINT)hr);
        ReleaseSharedTexture(shared);
        return false;
    }

    hr = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &shared.handle);
    dxgiResource->Release();

    if (FAILED(hr) || shared.handle == nullptr)
    {
        LOG_ERROR("UpscalerInputsDx11::EnsureSharedTexture - CreateSharedHandle failed: {:X}", (UINT)hr);
        ReleaseSharedTexture(shared);
        return false;
    }

    // Open on DX12 side
    hr = _dx12Device->OpenSharedHandle(shared.handle, IID_PPV_ARGS(&shared.dx12Res));
    if (FAILED(hr))
    {
        LOG_ERROR("UpscalerInputsDx11::EnsureSharedTexture - OpenSharedHandle failed: {:X}", (UINT)hr);
        ReleaseSharedTexture(shared);
        return false;
    }

    shared.width = srcDesc.Width;
    shared.height = srcDesc.Height;
    shared.format = sharedFormat;

    LOG_INFO("UpscalerInputsDx11::EnsureSharedTexture - Created shared texture: DX11={:X}, DX12={:X}, handle={:X}",
             (size_t)shared.dx11Tex, (size_t)shared.dx12Res, (size_t)shared.handle);

    return true;
}

void UpscalerInputsDx11::UpscaleStart(ID3D11DeviceContext* InDevCtx, NVSDK_NGX_Parameter* InParameters,
                                      IFeature_Dx11* feature)
{
    auto fg = State::Instance().currentFG;

    if (fg == nullptr || State::Instance().activeFgInput != FGInput::Upscaler ||
        !State::Instance().dx11FGMode || _dx11Device == nullptr || _dx12Device == nullptr)
        return;

    // --- Camera values (mirrors Upscaler_Inputs_Dx12.cpp lines 33-85) ---
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float cameraVFov = 0.0f;
    float meterFactor = 0.0f;
    float mvScaleX = 0.0f;
    float mvScaleY = 0.0f;
    float jitterX = 0.0f;
    float jitterY = 0.0f;

    float tempCameraNear = 0.0f;
    float tempCameraFar = 0.0f;
    InParameters->Get("FSR.cameraNear", &tempCameraNear);
    InParameters->Get("FSR.cameraFar", &tempCameraFar);

    if (!Config::Instance()->FsrUseFsrInputValues.value_or_default() ||
        (tempCameraNear == 0.0f && tempCameraFar == 0.0f))
    {
        if (feature->DepthInverted())
        {
            cameraFar = Config::Instance()->FsrCameraNear.value_or_default();
            cameraNear = Config::Instance()->FsrCameraFar.value_or_default();
        }
        else
        {
            cameraFar = Config::Instance()->FsrCameraFar.value_or_default();
            cameraNear = Config::Instance()->FsrCameraNear.value_or_default();
        }
    }
    else
    {
        cameraNear = tempCameraNear;
        cameraFar = tempCameraFar;
    }

    if (!Config::Instance()->FsrUseFsrInputValues.value_or_default() ||
        InParameters->Get("FSR.cameraFovAngleVertical", &cameraVFov) != NVSDK_NGX_Result_Success)
    {
        if (Config::Instance()->FsrVerticalFov.has_value())
            cameraVFov = Config::Instance()->FsrVerticalFov.value() * 0.0174532925199433f;
        else if (Config::Instance()->FsrHorizontalFov.value_or_default() > 0.0f)
            cameraVFov = 2.0f * atan((tan(Config::Instance()->FsrHorizontalFov.value() * 0.0174532925199433f) * 0.5f) /
                                     (float) feature->TargetHeight() * (float) feature->TargetWidth());
        else
            cameraVFov = 1.0471975511966f;
    }

    if (!Config::Instance()->FsrUseFsrInputValues.value_or_default())
        InParameters->Get("FSR.viewSpaceToMetersFactor", &meterFactor);

    State::Instance().lastFsrCameraFar = cameraFar;
    State::Instance().lastFsrCameraNear = cameraNear;

    // --- Build FG_Constants and call EvaluateState ---
    // Use the actual FG swapchain dimensions, NOT feature->DisplayWidth/Height.
    // In DX11 proxy mode, the FG swapchain may be at a different resolution than the
    // DLSS feature's display size (e.g., God of War: DLSS targets 3840x2160 internally,
    // but the FG swapchain is 1920x1080). Passing 3840x2160 to FG_Constants causes
    // Streamline to allocate 3840x2160 DLFG outputs for a 1920x1080 swapchain → TDR.
    UINT fgDisplayWidth = State::Instance().fgSwapchainWidth;
    UINT fgDisplayHeight = State::Instance().fgSwapchainHeight;

    // Guard: skip FG calls if dimensions are invalid (e.g., proxy not initialized yet)
    if (fgDisplayWidth == 0 || fgDisplayHeight == 0)
    {
        LOG_WARN("UpscalerInputsDx11: Skipping FG — FG swapchain dimensions are 0x0 (feature={}x{}, fgSC={}x{})",
                 feature->DisplayWidth(), feature->DisplayHeight(),
                 fgDisplayWidth, fgDisplayHeight);
        return;
    }

    LOG_DEBUG("UpscalerInputsDx11: FG swapchain={}x{}, feature display={}x{}, render={}x{}",
              fgDisplayWidth, fgDisplayHeight,
              feature->DisplayWidth(), feature->DisplayHeight(),
              feature->RenderWidth(), feature->RenderHeight());

    FG_Constants fgConstants {};
    fgConstants.displayWidth = fgDisplayWidth;
    fgConstants.displayHeight = fgDisplayHeight;

    if (feature->IsHdr())
        fgConstants.flags |= FG_Flags::Hdr;

    if (feature->DepthInverted())
        fgConstants.flags |= FG_Flags::InvertedDepth;

    if (feature->JitteredMV())
        fgConstants.flags |= FG_Flags::JitteredMVs;

    if (!feature->LowResMV())
        fgConstants.flags |= FG_Flags::DisplayResolutionMVs;

    if (Config::Instance()->FGAsync.value_or_default())
        fgConstants.flags |= FG_Flags::Async;

    fg->EvaluateState(_dx12Device, fgConstants);

    // After EvaluateState (which handles Activate/Deactivate transitions),
    // skip the rest if FG is not enabled. Calling StartNewFrame/SetResource
    // after Deactivate confuses Streamline's internal state.
    // NOTE: Do NOT check fg->IsActive() here — DLSSG needs StartNewFrame +
    // SetCameraValues to transition from "created" to "active". The IsActive()
    // gate is only on SetResource (MV/Depth sharing) further below.
    bool fgEnabled = Config::Instance()->FGEnabled.value_or_default();
    if (!fgEnabled)
    {
        LOG_DEBUG("UpscalerInputsDx11: FG disabled, skipping per-frame setup");
        return;
    }

    // --- Read NVNGX parameters ---
    int reset = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Reset, &reset);

    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);

    // --- FG per-frame setup ---
    fg->StartNewFrame();

    auto aspectRatio = (float) fgDisplayWidth / (float) fgDisplayHeight;
    fg->SetCameraValues(cameraNear, cameraFar, cameraVFov, aspectRatio, meterFactor);
    fg->SetFrameTimeDelta(State::Instance().lastFGFrameTime);
    fg->SetMVScale(mvScaleX, mvScaleY);
    fg->SetJitter(jitterX, jitterY);
    fg->SetReset(reset);
    fg->SetInterpolationRect(fgDisplayWidth, fgDisplayHeight);

    // --- Optional NVNGX matrix parameters ---
    {
        void* clipToPrevClipPtr = nullptr;
        if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_CLIP_TO_PREV_CLIP_MATRIX, &clipToPrevClipPtr) ==
                NVSDK_NGX_Result_Success &&
            clipToPrevClipPtr != nullptr)
        {
            fg->SetClipToPrevClipMatrix(static_cast<const float*>(clipToPrevClipPtr));
            static bool loggedOnce = false;
            if (!loggedOnce)
            {
                LOG_INFO("UpscalerInputsDx11: Got ClipToPrevClipMatrix from NVNGX parameters");
                loggedOnce = true;
            }
        }

        void* invViewProjPtr = nullptr;
        if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_INV_VIEW_PROJECTION_MATRIX, &invViewProjPtr) ==
                NVSDK_NGX_Result_Success &&
            invViewProjPtr != nullptr)
        {
            fg->SetInvViewProjMatrix(static_cast<const float*>(invViewProjPtr));
            static bool loggedOnce = false;
            if (!loggedOnce)
            {
                LOG_INFO("UpscalerInputsDx11: Got InvViewProjectionMatrix from NVNGX parameters");
                loggedOnce = true;
            }
        }
    }

    // --- FG Resource Setup: Share MV and Depth from DX11 to DX12 ---
    // Use per-frame-index shared textures to avoid GPU race conditions:
    // DX12 Dispatch reads from slot[N] while DX11 CopyResource writes to slot[N+1].
    if (!State::Instance().isShuttingDown && fg->IsActive() && Config::Instance()->FGEnabled.value_or_default() &&
        State::Instance().currentSwapchain != nullptr)
    {
        int fIndex = fg->GetIndex();
        LOG_DEBUG("UpscalerInputsDx11: FG active, fIndex={}, preparing MV and Depth resources", fIndex);

        // Motion Vectors
        ID3D11Resource* paramVelocity = nullptr;
        if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &paramVelocity) != NVSDK_NGX_Result_Success)
            InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, (void**)&paramVelocity);

        if (paramVelocity != nullptr)
        {
            if (EnsureSharedTexture(paramVelocity, _sharedMV[fIndex]))
            {
                // Copy game MV to shared DX11 texture (per-frame-index slot)
                InDevCtx->CopyResource(_sharedMV[fIndex].dx11Tex, paramVelocity);

                // Set on FG backend via DX12 resource
                Dx12Resource setResource {};
                setResource.type = FG_ResourceType::Velocity;
                setResource.resource = _sharedMV[fIndex].dx12Res;
                setResource.state = D3D12_RESOURCE_STATE_COMMON;
                setResource.validity = FG_ResourceValidity::UntilPresent;
                setResource.cmdList = nullptr;

                if (feature->LowResMV())
                {
                    setResource.width = feature->RenderWidth();
                    setResource.height = feature->RenderHeight();
                }
                else
                {
                    setResource.width = feature->TargetWidth();
                    setResource.height = feature->TargetHeight();
                }

                fg->SetResource(&setResource);
                LOG_DEBUG("UpscalerInputsDx11: Set Velocity[{}] resource {}x{} dx12={:X}",
                          fIndex, setResource.width, setResource.height, (size_t)setResource.resource);
            }
            else
            {
                LOG_WARN("UpscalerInputsDx11: Failed to create shared texture for MV[{}]", fIndex);
            }
        }
        else
        {
            LOG_WARN("UpscalerInputsDx11: MotionVectors parameter is null");
        }

        // Depth
        ID3D11Resource* paramDepth = nullptr;
        if (InParameters->Get(NVSDK_NGX_Parameter_Depth, &paramDepth) != NVSDK_NGX_Result_Success)
            InParameters->Get(NVSDK_NGX_Parameter_Depth, (void**)&paramDepth);

        if (paramDepth != nullptr)
        {
            if (EnsureSharedTexture(paramDepth, _sharedDepth[fIndex]))
            {
                // Copy game depth to shared DX11 texture (per-frame-index slot)
                InDevCtx->CopyResource(_sharedDepth[fIndex].dx11Tex, paramDepth);

                // Set on FG backend via DX12 resource
                Dx12Resource setResource {};
                setResource.type = FG_ResourceType::Depth;
                setResource.resource = _sharedDepth[fIndex].dx12Res;
                setResource.width = feature->RenderWidth();
                setResource.height = feature->RenderHeight();
                setResource.state = D3D12_RESOURCE_STATE_COMMON;
                setResource.validity = FG_ResourceValidity::UntilPresent;
                setResource.cmdList = nullptr;

                fg->SetResource(&setResource);
                LOG_DEBUG("UpscalerInputsDx11: Set Depth[{}] resource {}x{} dx12={:X}",
                          fIndex, setResource.width, setResource.height, (size_t)setResource.resource);
            }
            else
            {
                LOG_WARN("UpscalerInputsDx11: Failed to create shared texture for Depth[{}]", fIndex);
            }
        }
        else
        {
            LOG_WARN("UpscalerInputsDx11: Depth parameter is null");
        }
    }
}

void UpscalerInputsDx11::UpscaleEnd(ID3D11DeviceContext* InDevCtx, NVSDK_NGX_Parameter* InParameters,
                                     IFeature_Dx11* feature)
{
    // No DX11 HUDfix support yet - stub for future use
}
