#pragma once

#include <d3d12.h>
#include <dxgi.h>
#include <nvapi/NvApiTypes.h>

enum class NativeLatencyBackend : uint32_t
{
    None = 0,
    Reflex,       // NVIDIA native Reflex (pass-through)
    AntiLag2,     // AMD Anti-Lag 2 via SDK
    XeLL,         // Intel Xe Low Latency via libxell.dll
    LatencyFlex,  // Cross-vendor via fakenvapi
};

// User-facing config values for LatencyReductionMethod
enum class LatencyMethod : uint32_t
{
    Auto = 0,
    Reflex = 1,
    AntiLag2 = 2,
    XeLL = 3,
    LatencyFlex = 4,
    Off = 5,
};

class NativeLowLatency
{
    inline static NativeLatencyBackend _backend = NativeLatencyBackend::None;
    inline static bool _initialized = false;
    inline static bool _enabled = false;
    inline static unsigned int _maxFPS = 0;
    inline static uint32_t _gpuVendorId = 0;
    inline static uint64_t _currentFrameId = 0;

    // Anti-Lag 2 context is stored as raw bytes to avoid including the SDK header here.
    // AMD::AntiLag2DX12::Context is { IAmdExtAntiLagApi* m_pAntiLagAPI; bool m_enabled; unsigned int m_maxFPS; }
    // We allocate it dynamically in the cpp file.
    inline static void* _antiLag2Context = nullptr;

    static uint32_t DetectGPUVendor(ID3D12Device* device);

  public:
    // Initialize native latency reduction for the given D3D12 device.
    // userChoice maps to LatencyMethod enum from config.
    // Returns true if a native backend was successfully initialized.
    static bool Initialize(ID3D12Device* device, uint32_t userChoice);

    // Deinitialize and clean up
    static void DeInitialize();

    // Query state
    static NativeLatencyBackend GetBackend();
    static bool IsAvailable();
    static bool IsInitialized();

    // Check if a backend can be initialized on this system
    static bool CanInitBackend(NativeLatencyBackend backend);

    // Reflex API translation
    static NvAPI_Status SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* params);
    static NvAPI_Status Sleep(IUnknown* pDev);
    static NvAPI_Status SetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* params);
    static NvAPI_Status GetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* params);

    // Unified FPS cap that converts to the right units per backend
    static void SetFPSCap(float fps);

    // Frame generation integration
    static void ReportFGPresent(IDXGISwapChain* pSwapChain, bool fg_state, bool frame_interpolated);

    // Get the Anti-Lag 2 context pointer for FSR FG integration (returns nullptr if not AntiLag2)
    static void* GetAntiLag2Context();
};
