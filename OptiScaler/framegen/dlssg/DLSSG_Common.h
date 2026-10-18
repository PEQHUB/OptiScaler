#pragma once

#include <framegen/IFGFeature.h>
#include <framegen/DynamicMfg.h>

#include <proxies/Streamline_Proxy.h>

// The Streamline side of the DLSSG output, shared by the D3D12 and Vulkan ones
class DLSSG_Common : public virtual IFGFeature
{
  private:
    // OptiScaler's own Dynamic MFG where Streamline has no eDynamic (Vulkan)
    DynamicMfgController _dynamicMfg;
    bool _dynamicMfgRunning = false;
    double _dynamicMfgLastFrameMs = -1.0;
    double _dynamicMfgLastLogMs = 0.0;
    bool _dynamicMfgOwnsCap = false;
    float _dynamicMfgCap = 0.0f;               // The FPS limit it set
    std::optional<float> _capBeforeDynamicMfg; // The FPS limit to restore
    int _refreshRate = 0;
    double _refreshCheckMs = -1.0;

    bool UseOptiDynamicMfg() const;
    // Interpolation count for this frame, fixedCount until the controller has decided
    int UpdateOptiDynamicMfg(int fixedCount);
    void StopOptiDynamicMfg();
    void ReleaseDynamicMfgCap();
    float DynamicMfgTarget(double nowMs, bool& autoTarget);
    void PublishDynamicMfg(float targetFps, bool autoTarget, bool costsAvailable);

  protected:
    sl::ViewportHandle _viewport { 0 };
    sl::FrameToken* _frameToken = nullptr;

    // Max interpolation count and DMFG support, once the swapchain exists
    void QueryState();

    // DLSSG and Reflex options for the next present, Reflex optimizes with the game's markers when it has them
    void SetOptions(bool gameMarkersReachStreamline);

    // Fills _frameToken and sends the frame's constants, pauses FG when Streamline refuses them
    bool SetConstants(int fIndex, uint32_t frameId, uint32_t mvWidth, uint32_t mvHeight);

    void PauseAfterError();

  public:
    // IFGFeature
    const char* Name() override final { return "DLSSG"; }
    feature_version Version() override final;
    void Activate() override final;
    void Deactivate() override final;
    bool SetInterpolatedFrameCount(UINT interpolatedFrameCount) override final { return true; }
};
