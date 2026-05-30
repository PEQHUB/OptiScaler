#pragma once

#include <framegen/IFGFeature_Dx12.h>
#include <Util.h>

#include <proxies/SL_Proxy.h>

#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>

#include <DirectXMath.h>
#include <shaders/Shader_Dx12Utils.h>
using namespace DirectX;

class DLSSG_Dx12 : public virtual IFGFeature_Dx12
{
  private:
    // SL state tracking
    bool _slInitialized = false;
    bool _deviceRegistered = false;
    bool _dlssgFeatureReady = false;
    bool _objectsCreated = false;

    // MFG state
    uint32_t _numFramesToGenerateMax = 1;
    uint32_t _numFramesToGenerate = 1;
    bool _nameStale = true;

    // SL frame management
    uint64_t _slFrameIndex = 0;
    const sl::FrameToken* _currentFrameToken = nullptr;

    // Previous-frame view*projection tracking for clipToPrevClip computation (per ring-buffer index)
    XMMATRIX _prevViewProj[BUFFER_COUNT] {};
    XMMATRIX _prevViewProjInv[BUFFER_COUNT] {};
    bool _hasPrevProjection[BUFFER_COUNT] {};
    XMMATRIX _prevReflexWorldToView[BUFFER_COUNT] {};
    XMMATRIX _prevReflexViewToClip[BUFFER_COUNT] {};
    bool _hasPrevReflexCamera[BUFFER_COUNT] {};

    // Tagging command lists
    ID3D12GraphicsCommandList* _tagCommandList[BUFFER_COUNT] {};
    ID3D12CommandAllocator* _tagCommandAllocator[BUFFER_COUNT] {};

    // GPU fence for tag command list synchronization
    ID3D12Fence* _tagFence[BUFFER_COUNT] {};
    UINT64 _tagFenceValue[BUFFER_COUNT] {};
    HANDLE _tagFenceEvent = nullptr;

    // Experimental DLSSG UI recomposition input synthesized from same-frame
    // final-with-UI and HUD-less color. This avoids using a generated-vs-real
    // difference mask, which cancels VibeFlex's resource-level warp.
    ID3D12RootSignature* _uiExtractRootSignature = nullptr;
    ID3D12PipelineState* _uiExtractPipelineState = nullptr;
    FrameDescriptorHeap _uiExtractHeaps[BUFFER_COUNT] {};
    ID3D12Resource* _synthUiResource[BUFFER_COUNT] {};
    D3D12_RESOURCE_STATES _synthUiResourceState[BUFFER_COUNT] {};

    // Internal helpers
    bool InitStreamline(ID3D12Device* device);
    void ShutdownStreamline();

    bool TagResources(int fIndex, uint64_t willDispatchFrame);
    void SetSLConstants(int fIndex);
    bool EnsureUiExtractionPipeline(ID3D12Device* device);
    bool EnsureSynthUiResource(int fIndex, const D3D12_RESOURCE_DESC& desc);
    ID3D12Resource* SynthesizeUiColorAlpha(
        int fIndex,
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* hudless,
        D3D12_RESOURCE_STATES hudlessState,
        UINT width,
        UINT height,
        DXGI_FORMAT format,
        D3D12_RESOURCE_STATES* outState);
    void ProbeLatewarpCapability(ID3D12Device* device);
    void SubmitReflexCameraData(int fIndex, const XMMATRIX& worldToView, const XMMATRIX& viewToClip, bool haveView);
    void LogReflexState(const char* source);
    bool Dispatch();

    sl::Resource MakeSLResource(ID3D12Resource* d3dResource, D3D12_RESOURCE_STATES state);

  protected:
    void ReleaseObjects() override final;
    void CreateObjects(ID3D12Device* InDevice) override final;

  public:
    // IFGFeature
    const char* Name() override final;
    feature_version Version() override final;
    HWND Hwnd() override final;

    // IFGFeature_Dx12
    bool CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                         IDXGISwapChain** swapChain, bool readyToRelease) override final;
    bool CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd, DXGI_SWAP_CHAIN_DESC1* desc,
                          DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGISwapChain1** swapChain,
                          bool readyToRelease) override final;
    bool ReleaseSwapchain(HWND hwnd) override final;

    void CreateContext(ID3D12Device* device, FG_Constants& fgConstants) override final;
    void Activate() override final;
    void Deactivate() override final;
    void DestroyFGContext() override final;
    bool Shutdown() override final;

    void EvaluateState(ID3D12Device* device, FG_Constants& fgConstants) override final;

    bool Present() override final;
    bool PrepareHudlessBackbufferForPresent();

    bool SetResource(Dx12Resource* inputResource) override final;
    void SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) override final;

    void* FrameGenerationContext() override final;
    void* SwapchainContext() override final;

    // MFG accessors
    uint32_t GetCurrentFramesToGenerate() const { return _numFramesToGenerate; }
    uint32_t GetMaxFramesToGenerate() const { return _numFramesToGenerateMax; }
    int GetCurrentMultiplier() const { return _numFramesToGenerate + 1; }

    // PCL marker helpers — called from FG_Hooks around the actual Present call
    void SetPCLPresentStart();
    void SetPCLPresentEnd();
    void SetPCLLateWarpPresentStart();
    void SetPCLLateWarpPresentEnd();
    void SetPCLCameraConstructed();

    // Native Reflex frame pacing — replaces OptiScaler's software frame limiter
    void CallReflexSleep();
    void UpdateReflexFrameLimit(float fpsCap);

    DLSSG_Dx12(UINT framesToInterpolate = 1) : IFGFeature_Dx12(), IFGFeature()
    {
        _framesToInterpolate = framesToInterpolate;
        _numFramesToGenerate = framesToInterpolate;
        if (SLProxy::Module() == nullptr)
            SLProxy::InitSL();
    }

    ~DLSSG_Dx12();

    // Inherited via IFGFeature_Dx12
    bool SetInterpolatedFrameCount(UINT interpolatedFrameCount) override;
};
