#pragma once
#include "SysUtils.h"
#include <NVNGX_Parameter.h>
#include <upscalers/IFeature_Dx11.h>

#include <d3d11_1.h>
#include <d3d12.h>

class UpscalerInputsDx11
{
  private:
    inline static ID3D11Device* _dx11Device = nullptr;
    inline static ID3D12Device* _dx12Device = nullptr;

    struct SharedResource
    {
        ID3D11Texture2D* dx11Tex = nullptr;
        ID3D12Resource* dx12Res = nullptr;
        HANDLE handle = nullptr;
        UINT width = 0;
        UINT height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };

    // Per-frame-index shared textures to avoid GPU race conditions:
    // DX12 Dispatch reads from slot N while DX11 CopyResource writes to slot N+1.
    inline static SharedResource _sharedMV[BUFFER_COUNT] {};
    inline static SharedResource _sharedDepth[BUFFER_COUNT] {};

    static bool EnsureSharedTexture(ID3D11Resource* source, SharedResource& shared);
    static void ReleaseSharedTexture(SharedResource& shared);

  public:
    static void Init(ID3D11Device* dx11Device, ID3D12Device* dx12Device);
    static void UpscaleStart(ID3D11DeviceContext* InDevCtx, NVSDK_NGX_Parameter* InParameters,
                             IFeature_Dx11* feature);
    static void UpscaleEnd(ID3D11DeviceContext* InDevCtx, NVSDK_NGX_Parameter* InParameters,
                           IFeature_Dx11* feature);
    static void Cleanup();
};
