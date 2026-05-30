#pragma once
#include "upscalers/IFeature.h"
#include "framegen/IFGFeature_Dx12.h"
#include <inputs/FG/Streamline_Inputs_Dx12.h>
#include "misc/Quirks.h"


#include <set>
#include <deque>
#include <vulkan/vulkan.h>
#include <ankerl/unordered_dense.h>
#include <mutex>

typedef enum API
{
    NotSelected = 0,
    DX11,
    DX12,
    Vulkan,
} API;

enum class FGPreset : uint32_t
{
    NoFG,
    OptiFG,
    Nukems,
};

enum class FrameTimeSource : uint32_t
{
    Input,
    Opti,
    Zero,
};

enum class FGInput : uint32_t
{
    NoFG,
    Nukems,
    FSRFG,
    DLSSG, // technically Streamline inputs
    XeFG,
    Upscaler, // OptiFG
    FSRFG30,
};

enum class FGOutput : uint32_t
{
    NoFG,
    Nukems,
    FSRFG,
    DLSSG,
    XeFG
};

enum class DLSSGNativeMode : uint32_t
{
    Auto,
    Legacy,
    Attach,
    Passthrough,
};

enum class WorkingMode : uint32_t
{
    Dxgi,
    D3d12,
    Nvngx,
    Other,
};

typedef struct CapturedHudlessInfo
{
    UINT64 usageCount = 1;
    UINT captureInfo = 0;
    bool enabled = true;
} captured_hudless_info;

class State
{
  public:
    static State& Instance()
    {
        static State instance;
        return instance;
    }

    std::string GameName;
    std::string GameExe;
    ankerl::unordered_dense::map<void*, std::string> DeviceAdapterNames;

    bool NvngxDx11Inited = false;
    bool NvngxDx12Inited = false;
    bool NvngxVkInited = false;

    flag_set<GameQuirk> gameQuirks;
    bool isOptiPatcherSucceed = false;

    // Reseting on creation of new feature
    std::optional<bool> AutoExposure;

    // FG
    UINT64 FGLastFrame = 0;

    // DLSSG
    bool NukemsFilesAvailable = false;
    bool DLSSGDebugView = false;
    bool DLSSGInterpolatedOnly = false;
    uint32_t delayMenuRenderBy = 0;
    UINT64 DLSSGLastFrame = 0;

    // FSR Common
    float lastFsrCameraNear = 0.0f;
    float lastFsrCameraFar = 0.0f;

    // Frame Generation
    FGInput activeFgInput = FGInput::NoFG;
    FGOutput activeFgOutput = FGOutput::NoFG;

    // Streamline FG inputs
    Sl_Inputs_Dx12 slFGInputs = {};

    // OptiFG
    bool FGPresentIsCalled = false;
    bool FGonlyGenerated = false;
    bool FGHudlessCompare = false;
    bool FGchanged = false;
    bool SCchanged = false;
    bool skipHeapCapture = false;

    bool FGcaptureResources = false;
    size_t FGcapturedResourceCount = false;
    bool FGresetCapturedResources = false;
    bool FGonlyUseCapturedResources = false;

    bool FSRFGFTPchanged = false;
    bool FSRFGInputActive = false;

    bool FGResizing = false;

    ankerl::unordered_dense::map<void*, CapturedHudlessInfo> CapturedHudlesses;
    bool ClearCapturedHudlesses = false;

    // NVNGX init parameters
    uint64_t NVNGX_ApplicationId = 1337;
    std::wstring NVNGX_ApplicationDataPath;
    std::string NVNGX_ProjectId;
    NVSDK_NGX_Version NVNGX_Version {};
    const NVSDK_NGX_FeatureCommonInfo* NVNGX_FeatureInfo = nullptr;
    std::vector<std::wstring> NVNGX_FeatureInfo_Paths;
    NVSDK_NGX_LoggingInfo NVNGX_Logger { nullptr, NVSDK_NGX_LOGGING_LEVEL_OFF, false };
    NVSDK_NGX_EngineType NVNGX_Engine = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    std::string NVNGX_EngineVersion;
    std::optional<std::wstring> NVNGX_DLSS_Path;
    std::optional<std::wstring> NVNGX_DLSSD_Path;
    std::optional<std::wstring> NVNGX_DLSSG_Path;

    // NGX OTA
    std::string NGX_OTA_Dlss;
    std::string NGX_OTA_Dlssd;

    feature_version streamlineVersion = { 0, 0, 0 };

    API api = API::NotSelected;
    API swapchainApi = API::NotSelected;

    // Framerate
    bool reflexLimitsFps = false;
    bool reflexShowWarning = false;
    bool rtssReflexInjection = false;
    UINT64 reflexFrameId = 0;
    UINT64 frameCount = 0;

    // for realtime changes
    ankerl::unordered_dense::map<unsigned int, bool> changeBackend;
    std::string newBackend = "";

    // XeSS debug stuff
    bool xessDebug = false;
    int xessDebugFrames = 5;
    float lastMipBias = 100.0f;
    float lastMipBiasMax = -100.0f;

    int xefgMaxInterpolationCount = 1;
    bool WAR_xefgRequestFGToggle = false;

    // DLSS-G Output
    bool SLFilesAvailable = false;
    uint32_t DLSSGMaxFramesToGenerate = 1;
    bool dlssgNativeStreamlineDetected = false;
    bool dlssgNativeAttachActive = false;
    bool dlssgNativePassthroughActive = false;
    uint64_t dlssgNativeInterposerLoadCount = 0;
    uint64_t dlssgNativeCommonLoadCount = 0;
    uint64_t dlssgNativeDlssgLoadCount = 0;
    uint64_t dlssgNativeEvaluateCount = 0;
    uint64_t dlssgNativeLastEvaluateFrame = 0;
    uint64_t dlssgNativeSetTagCount = 0;
    uint64_t dlssgNativeSetConstantsCount = 0;
    uint64_t dlssgNativeDepthTagCount = 0;
    uint64_t dlssgNativeHudlessTagCount = 0;
    uint64_t dlssgNativeUiTagCount = 0;
    char dlssgNativeLastModule[32] = {};
    char dlssgNativeLastPath[MAX_PATH] = {};

    // DLSS
    bool dlssPresetsOverriddenExternally = false;
    bool dlssPresetsOverridenByOpti = false;
    uint32_t dlssRenderPresetExternal = 0;
    uint32_t dlssRenderPresetDLAA = 0;
    uint32_t dlssRenderPresetUltraQuality = 0;
    uint32_t dlssRenderPresetQuality = 0;
    uint32_t dlssRenderPresetBalanced = 0;
    uint32_t dlssRenderPresetPerformance = 0;
    uint32_t dlssRenderPresetUltraPerformance = 0;

    // DLSSD
    bool dlssdPresetsOverriddenExternally = false;
    bool dlssdPresetsOverridenByOpti = false;
    uint32_t dlssdRenderPresetExternal = 0;
    uint32_t dlssdRenderPresetDLAA = 0;
    uint32_t dlssdRenderPresetUltraQuality = 0;
    uint32_t dlssdRenderPresetQuality = 0;
    uint32_t dlssdRenderPresetBalanced = 0;
    uint32_t dlssdRenderPresetPerformance = 0;
    uint32_t dlssdRenderPresetUltraPerformance = 0;

    // Spoofing
    bool skipSpoofing = false;
    // For DXVK, it calls DXGI which cause softlock
    bool skipDxgiLoadChecks = false;
    bool skipParentWrapping = false;

    // quirks
    std::vector<std::string> detectedQuirks {};

    // FFX
    std::vector<const char*> ffxUpscalerVersionNames {};
    std::vector<uint64_t> ffxUpscalerVersionIds {};
    std::vector<const char*> ffxFGVersionNames {};
    std::vector<uint64_t> ffxFGVersionIds {};
    std::optional<uint32_t> currentFsr4Model {};

    // Linux checks
    bool isRunningOnLinux = false;
    bool isRunningOnDXVK = false;

    // Other checks
    bool isRunningOnNvidia = false;
    bool nativeLowLatencyAvailable = false;

    // Frame Warp - status-only struct for menu/debug display.
    // The FrameWarp_Dx12 instance is owned by FG_Hooks lifecycle code, not by State.
    struct FrameWarpStatus {
        bool available = false;          // FrameWarp runtime initialized
        bool enabled = false;            // Config enabled
        bool rawInputSeen = false;       // WM_INPUT mouse deltas forwarded
        bool rawInputRegistrationSeen = false; // Game registered a raw mouse target
        bool rawInputDataHookSeen = false; // GetRawInputData hook observed mouse input
        bool rawInputBufferHookSeen = false; // GetRawInputBuffer hook observed mouse input
        bool rawInputSubclassSeen = false; // Raw-input target HWND subclass observed input
        bool lastWarpApplied = false;    // Warp was applied this frame
        float lastDeltaYaw = 0.0f;       // Last warp yaw delta (radians)
        float lastDeltaPitch = 0.0f;     // Last warp pitch delta (radians)
        float lastPreparedPixelShift = 0.0f;
        float lastPreparedDeltaYaw = 0.0f;
        float lastPreparedDeltaPitch = 0.0f;
        char lastPreparedStatus[16] = {};
        char lastPreparedReason[64] = {};
        float lastAppliedPixelShift = 0.0f;
        float lastAppliedDeltaYaw = 0.0f;
        float lastAppliedDeltaPitch = 0.0f;
        uint64_t lastAppliedPresentSeq = 0;
        uint64_t lastRenderSnapshotAtPresent = 0;
        int64_t lastVf2SubmitQpc = 0;
        int64_t lastVf2FenceCompleteQpc = 0;
        bool lastVf2PreviousFenceComplete = true;
        float lastVf2PreviousSubmitAgeMs = 0.0f;
        float lastVFovRadians = 0.0f;    // FOV used by ray-space warp
        float lastAspectRatio = 0.0f;
        float lastApproxPixelShift = 0.0f;
        float lastDisocclusionPixels = 0.0f;
        float lastMouseDeltaDx = 0.0f;   // Last mouse delta since render snapshot
        float lastMouseDeltaDy = 0.0f;
        float lastAccumDx = 0.0f;        // Current accumulated raw mouse counts
        float lastAccumDy = 0.0f;
        float lastSnapshotDx = 0.0f;     // Accumulated counts at render snapshot
        float lastSnapshotDy = 0.0f;
        float lastRenderMouseStepDx = 0.0f; // Raw count delta between render snapshots
        float lastRenderMouseStepDy = 0.0f;
        float lastSnapshotAgeMs = 0.0f;  // Age of render snapshot at warp attempt
        bool lastDepthAwareEnabled = false;
        bool lastDepthResourceValid = false;
        bool lastDepthUsed = false;
        uint32_t lastDepthWidth = 0;
        uint32_t lastDepthHeight = 0;
        uint32_t lastDepthFormat = 0;
        bool depthInfillCopySuccess = false;
        bool depthInfillInverted = false;
        bool depthInfillHistoryAvailable = false;
        bool depthInfillHistoryUsed = false;
        uint32_t depthInfillSourceFormat = 0;
        uint32_t depthInfillCopyFormat = 0;
        uint32_t depthInfillSrvFormat = 0;
        uint32_t depthInfillSourceState = 0;
        uint32_t depthInfillWidth = 0;
        uint32_t depthInfillHeight = 0;
        float depthInfillAgeFrames = 0.0f;
        float depthInfillMaskCoveragePct = 0.0f;
        LONG lastRawInputDx = 0;
        LONG lastRawInputDy = 0;
        uint64_t rawInputSampleCount = 0;
        uint64_t rawInputMessageCount = 0;
        uint64_t rawInputForwardedCount = 0;
        uint64_t rawInputDuplicateCount = 0;
        uint64_t rawInputIgnoredCount = 0;
        uint64_t rawInputDroppedNoCallbackCount = 0;
        uint64_t rawInputInactiveSourceCount = 0;
        uint64_t rawInputUnidentifiedCount = 0;
        uint64_t rawInputDataMessageCount = 0;
        uint64_t rawInputBufferMessageCount = 0;
        uint64_t rawInputSubclassMessageCount = 0;
        uint64_t rawInputDataForwardedCount = 0;
        uint64_t rawInputBufferForwardedCount = 0;
        uint64_t rawInputSubclassForwardedCount = 0;
        uint64_t rawInputSequence = 0;
        uint64_t rawInputSnapshotSequence = 0;
        uint32_t rawInputSamplesSinceSnapshot = 0;
        float rawInputSampleHz = 0.0f;
        float rawInputIntervalMinMs = 0.0f;
        float rawInputIntervalMaxMs = 0.0f;
        float rawInputIntervalMeanMs = 0.0f;
        float rawInputIntervalStdDevMs = 0.0f;
        float lastInputSampleAgeMs = 0.0f;
        float lastPredictedMouseDx = 0.0f;
        float lastPredictedMouseDy = 0.0f;
        float lastFinalMouseDeltaDx = 0.0f;
        float lastFinalMouseDeltaDy = 0.0f;
        float lastFinalMouseDeltaStepDx = 0.0f;
        float lastFinalMouseDeltaStepDy = 0.0f;
        float lastPoseJitterYaw = 0.0f;
        float lastPoseJitterPitch = 0.0f;
        bool lastInputPredictionApplied = false;
        bool lastPoseClampApplied = false;
        uint64_t renderSnapshotCount = 0;
        char lastRawInputSource[32] = {};
        char activeRawInputSource[32] = {};
        char lastSnapshotSource[32] = {};
        char lastFovSource[32] = {};
        char lastCalibrationSource[32] = {};
        uint64_t sensitivityAuditCount = 0;
        bool sensitivityAuditAvailable = false;
        bool sensitivityAuditCopyPending = false;
        uint32_t sensitivityAuditMvWidth = 0;
        uint32_t sensitivityAuditMvHeight = 0;
        uint32_t sensitivityAuditMvFormat = 0;
        float sensitivityAuditMvScaleX = 0.0f;
        float sensitivityAuditMvScaleY = 0.0f;
        float sensitivityAuditCurrentRadPerCount = 0.0f;
        float sensitivityAuditRenderMouseDx = 0.0f;
        float sensitivityAuditRenderMouseDy = 0.0f;
        float sensitivityAuditGameMedianPxX = 0.0f;
        float sensitivityAuditGameMedianPxY = 0.0f;
        float sensitivityAuditGameAbsPx = 0.0f;
        float sensitivityAuditPredPxX = 0.0f;
        float sensitivityAuditPredPxY = 0.0f;
        float sensitivityAuditPredAbsPx = 0.0f;
        float sensitivityAuditRatio = 0.0f;
        float sensitivityAuditRecommendedRadPerCount = 0.0f;
        float sensitivityAuditConfidence = 0.0f;
        char sensitivityAuditSource[32] = {};
        char sensitivityAuditReason[64] = {};
        char lastSkipReason[64] = {};    // Human-readable reason warp was skipped
        char lastDepthReason[64] = {};
        char depthInfillSource[32] = {};
        char depthInfillReason[64] = {};
        uint32_t lastPresentationOwner = 0;
        bool lastOwnerFgRequested = false;
        bool lastOwnerHasCurrentFG = false;
        bool lastOwnerFgActive = false;
        bool lastOwnerFgPaused = false;
        bool fsrfgPresentCallbackConfigured = false;
        bool fsrfgPresentCallbackSeen = false;
        uint64_t fsrfgPresentCallbackCount = 0;
        uint64_t fsrfgPresentCallbackFrameID = 0;
        bool fsrfgPresentCallbackGenerated = false;
        uint64_t fsrfgPresentCallbackOrdinal = 0;
        float fsrfgPresentIntervalMs = 0.0f;
        float fsrfgPresentIntervalMeanMs = 0.0f;
        float fsrfgPresentIntervalStdDevMs = 0.0f;
        uint64_t fsrfgLastSourceResource = 0;
        uint64_t fsrfgLastDestinationResource = 0;
        bool fsrfgLastReuseFramePose = false;
        bool xefgPresentStatusSeen = false;
        uint64_t xefgPresentStatusCount = 0;
        uint32_t xefgLastFramesPresented = 0;
        int32_t xefgLastFrameGenResult = 0;
        int32_t xefgLastPresentStatusResult = 0;
        bool xefgLastFrameGenEnabled = false;
        uint64_t xefgFinalPresentCount = 0;
        uint64_t xefgFinalPresentInternalCount = 0;
        uint64_t xefgFinalPresentAsyncCount = 0;
        uint64_t xefgFinalWarpAttemptCount = 0;
        uint64_t xefgFinalWarpAppliedCount = 0;
        uint64_t xefgFinalWarpSkippedCount = 0;
        bool xefgFinalPresentLastInternal = false;
        uint32_t dlssgFrameWarpMode = 0;
        uint64_t dlssgDistortionAttemptCount = 0;
        uint64_t dlssgDistortionInjectedCount = 0;
        uint64_t dlssgDistortionSkippedCount = 0;
        bool dlssgDistortionLastInjected = false;
        uint32_t dlssgDistortionLastFrameIndex = 0;
        uint32_t dlssgLastRequestedFrames = 0;
        uint32_t dlssgLastMaxFrames = 0;
        uint32_t dlssgLastFramesPresented = 0;
        uint32_t dlssgLastStatus = 0;
        int32_t dlssgLastStateResult = 0;
        uint64_t dlssgPhaseClassifyCount = 0;
        uint64_t dlssgPhaseClassifyBoundaryCount = 0;
        uint64_t dlssgPhaseClassifySameSnapshotCount = 0;
        uint64_t dlssgPhaseClassifyHeuristicCount = 0;
        uint64_t dlssgPhaseClassifyUnknownCount = 0;
        uint64_t dlssgPhaseClassifyPresentSeq = 0;
        uint64_t dlssgPhaseClassifyRenderSnapshot = 0;
        uint64_t dlssgPhaseClassifyPreviousRenderSnapshot = 0;
        bool dlssgPhaseClassifySnapshotChanged = false;
        bool dlssgPhaseClassifyFutureEligible = false;
        bool dlssgPhaseClassifySlHeuristic = false;
        float dlssgPhaseClassifyConfidence = 0.0f;
        uint32_t dlssgPhaseClassifySlBefore = 0;
        uint32_t dlssgPhaseClassifySlAfter = 0;
        int32_t dlssgPhaseClassifySlResultBefore = 0;
        int32_t dlssgPhaseClassifySlResultAfter = 0;
        char dlssgPhaseClassifyLabel[32] = {};
        char dlssgPhaseClassifyReason[96] = {};
        char dlssgDistortionLastReason[64] = {};
        uint64_t dlssgLatePresentAttemptCount = 0;
        uint64_t dlssgLatePresentAppliedCount = 0;
        uint64_t dlssgLatePresentSkippedCount = 0;
        uint64_t dlssgLatePresentZeroPoseCount = 0;
        uint64_t dlssgLatePresentInFlightSkipCount = 0;
        uint64_t dlssgLatePresentSubmittedCount = 0;
        bool dlssgLatePresentLastApplied = false;
        bool dlssgLatePresentSafetySuppressed = false;
        char dlssgLatePresentLastReason[64] = {};
        uint64_t dlssgResourceWarpCandidateCount = 0;
        uint64_t dlssgResourceWarpAcceptedCount = 0;
        uint64_t dlssgResourceWarpSubmittedCount = 0;
        uint64_t dlssgResourceWarpAppliedCount = 0;
        uint64_t dlssgResourceWarpSkippedCount = 0;
        uint64_t dlssgResourceWarpInFlightSkipCount = 0;
        bool dlssgResourceWarpLastApplied = false;
        uint64_t dlssgResourceWarpLastDst = 0;
        uint64_t dlssgResourceWarpLastSrc = 0;
        uint64_t dlssgResourceWarpLastQueue = 0;
        uint32_t dlssgResourceWarpLastWidth = 0;
        uint32_t dlssgResourceWarpLastHeight = 0;
        uint32_t dlssgResourceWarpLastFormat = 0;
        uint32_t dlssgResourceWarpLastBeforeState = 0;
        uint32_t dlssgResourceWarpLastAfterState = 0;
        uint32_t dlssgResourceWarpPolicyMode = 0;
        uint32_t dlssgResourceWarpCompatibilityConfidence = 0;
        char dlssgResourceWarpPolicyName[32] = {};
        char dlssgResourceWarpCompatibilityReason[96] = {};
        char dlssgResourceWarpLastReason[96] = {};
        bool dlssgHudlessSubmitReady = false;
        uint64_t dlssgHudlessSubmitFrameID = 0;
        uint64_t dlssgHudlessSubmitFrame = 0;
        char dlssgHudlessSubmitLastReason[96] = {};
        char lastStandaloneWarpPath[32] = {};
        bool lastStableUiValid = false;
        uint64_t lastStableUiFrameID = 0;
        uint64_t lastStableUiAge = 0;
        bool lastStableUiCleanSceneValid = false;
        bool lastStableUiSuppressUnderlay = false;
        bool lastStableUiUseWarpTransform = false;
        bool frameWarpWithFgConfigured = false;
        bool frameWarpWithFgRawValue = false;
        bool frameWarpWithFgResolved = false;
        char frameWarpWithFgSource[16] = {};
        char fsrfgLastPhase[16] = {};
        char fsrfgLastPolicy[32] = {};
        char lastStableUiSource[32] = {};
        char lastStableUiRepairSource[32] = {};
        char lastPresentationOwnerName[32] = {};
        char lastPresentationOwnerReason[64] = {};
        uint32_t distortionFrameIndex = 0; // Which distortion buffer was used
    } frameWarpStatus;

    // Callback for forwarding raw mouse input to FrameWarp.
    // Set by FG_Hooks when FrameWarp is initialized, cleared on shutdown.
    // This avoids exposing the FrameWarp instance pointer through State.
    using FrameWarpRawMouseCallback = void(*)(LONG dx, LONG dy, const char* source, int64_t timestampQpc);
    FrameWarpRawMouseCallback frameWarpRawMouseCallback = nullptr;
    bool frameWarpDebugOverlay = false; // Show debug overlay text
	bool frameWarpDebugMaskReady = false; // True when debug mask texture is available for ImGui
	ID3D12Resource* frameWarpDebugMaskResource = nullptr; // Resource for barrier/transitions
    bool frameWarpReceivingWmInput = false; // True if WM_INPUT messages are being received
    HWND frameWarpHwnd = nullptr; // Window handle for raw input registration
    uint32_t gpuVendorId = 0;
    std::optional<bool> isRunningOnRDNA4;
    bool isPascalOrOlder = false;
    WorkingMode workingMode = WorkingMode::Other;

    // Vulkan stuff
    bool vulkanCreatingSC = false;
    bool creatingD3DDevice = false;
    bool vulkanSkipHooks = false;
    VkInstance VulkanInstance = nullptr;

    // Framegraph
    std::deque<double> upscaleTimes;
    std::deque<double> frameTimes;
    double lastFGFrameTime = 0.0;
    double presentFrameTime = 0.0;
    std::mutex frameTimeMutex;

    // Version check
    std::mutex versionCheckMutex;
    bool versionCheckInProgress = false;
    bool versionCheckCompleted = false;
    bool updateAvailable = false;
    std::string latestVersionTag;
    std::string latestVersionUrl;
    std::string versionCheckError;

    // Swapchain info
    float screenWidth = 800.0;
    float screenHeight = 450.0;
    bool realExclusiveFullscreen = false;
    bool SCExclusiveFullscreen = false;
    bool SCAllowTearing = false;
    UINT SCLastFlags = 0;

    // HDR
    std::vector<IUnknown*> SCbuffers;
    bool isHdrActive = false;

    std::string setInputApiName;
    std::string currentInputApiName;

    bool isShuttingDown = false;
    std::set<PVOID> modulesToFree;

    // menu warnings
    bool fgSettingsChanged = false;
    bool nvngxIniDetected = false;

    bool nvngxExists = false;
    std::optional<std::wstring> nvngxReplacement = std::nullopt;
    bool libxessExists = false;
    bool fsrHooks = false;

    IFeature* currentFeature = nullptr;
    IFGFeature_Dx12* currentFG = nullptr;
    IDXGISwapChain* currentSwapchain = nullptr;
    IDXGISwapChain* currentWrappedSwapchain = nullptr;
    IDXGISwapChain* currentRealSwapchain = nullptr;
    IDXGISwapChain* currentFGSwapchain = nullptr;
    ID3D12Device* currentD3D12Device = nullptr;
    DXGI_ADAPTER_DESC currentD3D12AdepterDesc = {};
    ID3D11Device* currentD3D11Device = nullptr;
    DXGI_ADAPTER_DESC currentD3D11AdepterDesc = {};
    ID3D12CommandQueue* currentCommandQueue = nullptr;
    VkDevice currentVkDevice = nullptr;
    DXGI_SWAP_CHAIN_DESC currentSwapchainDesc {};

    // DX11 Frame Generation interop
    ID3D12Device* dx12DeviceForDx11FG = nullptr;
    ID3D12CommandQueue* dx12QueueForDx11FG = nullptr;
    bool dx11FGMode = false;
    UINT fgSwapchainWidth = 0;   // Actual FG swapchain dimensions (for FG_Constants)
    UINT fgSwapchainHeight = 0;  // May differ from feature->DisplayWidth in DX11 proxy mode

    std::vector<ID3D12Device*> d3d12Devices;
    std::vector<ID3D11Device*> d3d11Devices;
    std::unordered_map<UINT64, std::string> adapterDescs;

    // Moved checks here to prevent circular includes
    /// <summary>
    /// Enables skipping of LoadLibrary checks
    /// </summary>
    /// <param name="dllName">Lower case dll name without `.dll` at the end. Leave blank for skipping all dll's</param>
    static void DisableChecks(UINT owner, std::string dllName = "")
    {
        if (_skipOwner == 0)
        {
            _skipOwner = owner;
            _skipChecks = true;
            _skipDllName = dllName;
        }
        else
        {
            _skipDllName = ""; // Hack for multiple skip calls
        }
    };

    static void EnableChecks(UINT owner)
    {
        if (_skipOwner == 0 || _skipOwner == owner)
        {
            _skipChecks = false;
            _skipDllName = "";
            _skipOwner = 0;
        }
    };

    static void DisableServeOriginal(UINT owner)
    {
        if (_serveOwner == 0 || _serveOwner == owner)
        {
            _serveOriginal = false;
            _skipOwner = 0;
        }
    };

    static void EnableServeOriginal(UINT owner)
    {
        if (_serveOwner == 0 || _serveOwner == owner)
        {
            _serveOriginal = true;
            _skipOwner = owner;
        }
    };

    static bool SkipDllChecks() { return _skipChecks; }
    static std::string SkipDllName() { return _skipDllName; }
    static bool ServeOriginal() { return _serveOriginal; }

  private:
    inline static bool _skipChecks = false;
    inline static std::string _skipDllName = "";
    inline static UINT _skipOwner = 0;

    inline static bool _serveOriginal = false;
    inline static UINT _serveOwner = 0;

    State() = default;
};

class ScopedSkipSpoofing
{
  private:
    bool previousState;

  public:
    ScopedSkipSpoofing()
    {
        previousState = State::Instance().skipSpoofing;
        State::Instance().skipSpoofing = true;
    }

    ~ScopedSkipSpoofing() { State::Instance().skipSpoofing = previousState; }
};

class ScopedSkipDxgiLoadChecks
{
  private:
    bool previousState;

  public:
    ScopedSkipDxgiLoadChecks()
    {
        previousState = State::Instance().skipDxgiLoadChecks;
        State::Instance().skipDxgiLoadChecks = true;
    }

    ~ScopedSkipDxgiLoadChecks() { State::Instance().skipDxgiLoadChecks = previousState; }
};

class ScopedSkipParentWrapping
{
  private:
    bool previousState;

  public:
    ScopedSkipParentWrapping()
    {
        previousState = State::Instance().skipParentWrapping;
        State::Instance().skipParentWrapping = true;
    }

    ~ScopedSkipParentWrapping() { State::Instance().skipParentWrapping = previousState; }
};

class ScopedSkipHeapCapture
{
  private:
    bool previousState;

  public:
    ScopedSkipHeapCapture()
    {
        previousState = State::Instance().skipHeapCapture;
        State::Instance().skipHeapCapture = true;
    }

    ~ScopedSkipHeapCapture() { State::Instance().skipHeapCapture = previousState; }
};

class ScopedSkipVulkanHooks
{
  private:
    bool previousState;

  public:
    ScopedSkipVulkanHooks()
    {
        previousState = State::Instance().vulkanSkipHooks;
        State::Instance().vulkanSkipHooks = true;
    }
    ~ScopedSkipVulkanHooks() { State::Instance().vulkanSkipHooks = previousState; }
};

class ScopedVulkanCreatingSC
{
  private:
    bool previousState;

  public:
    ScopedVulkanCreatingSC()
    {
        previousState = State::Instance().vulkanCreatingSC;
        State::Instance().vulkanCreatingSC = true;
    }
    ~ScopedVulkanCreatingSC() { State::Instance().vulkanCreatingSC = previousState; }
};

class ScopedCreatingD3DDevice
{
  private:
    bool previousState;

  public:
    ScopedCreatingD3DDevice()
    {
        previousState = State::Instance().creatingD3DDevice;
        State::Instance().creatingD3DDevice = true;
    }
    ~ScopedCreatingD3DDevice() { State::Instance().creatingD3DDevice = previousState; }
};
