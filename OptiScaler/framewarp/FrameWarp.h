#pragma once

#include "SysUtils.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <chrono>
#include <atomic>
#include <array>
#include <mutex>
#include <shaders/Shader_Dx12Utils.h>

using namespace DirectX;

struct FrameWarpShaderConstants;

enum class FrameWarpPresentationOwner : uint32_t
{
	Disabled = 0,
	StandaloneNoFG,
	FsrfgPending,
	FsrfgCallback,
	FsrfgUnavailable,
	XeFGPending,
	XeFGInputFrame,
	DLSSGDistortionField,
	DLSSGLatePresent,
	NativeDLSSGLatePresent,
};

// ============================================================================
// MouseTracker - Tracks raw mouse input for frame warp camera prediction
// ============================================================================

class MouseTracker
{
public:
	struct MouseSample
	{
		LONG dx = 0;
		LONG dy = 0;
		int64_t timestampQpc = 0;
		uint64_t sequence = 0;
		double accumulatedDx = 0.0;
		double accumulatedDy = 0.0;
		char source[16] = {};
	};

	struct MouseState
	{
		double accumulatedDx = 0.0;
		double accumulatedDy = 0.0;
		float velocityX = 0.0f;
		float velocityY = 0.0f;
		int64_t timestampQpc = 0;
		uint64_t sequence = 0;
	};

private:
	static constexpr size_t SAMPLE_HISTORY = 128;
	std::array<MouseSample, SAMPLE_HISTORY> _sampleHistory {};
	size_t _sampleIndex = 0;
	size_t _sampleCount = 0;
	uint64_t _sampleSequence = 0;

	std::atomic<double> _accumulatedDx{0.0};
	std::atomic<double> _accumulatedDy{0.0};

	MouseState _renderMouseState {};
	bool _hasRenderSnapshot = false;
	MouseState _dlssgResourceAnchorMouseState {};
	MouseState _dlssgResourcePendingAnchorMouseState {};
	bool _hasDLSSGResourceAnchor = false;
	bool _hasDLSSGResourcePendingAnchor = false;

	float _sensitivityX = 0.0022f;
	float _sensitivityY = 0.0022f;

	bool _rawInputCaptured = false;
	mutable std::mutex _mutex;

	friend class FrameWarp_Dx12;

	static int64_t GetTimestampQpc();
	static double QpcToSeconds(int64_t qpcDelta);
	static double QpcToMilliseconds(int64_t qpcDelta);

public:
	MouseTracker() = default;

	void OnRawMouseInput(LONG dx, LONG dy, const char* source = nullptr, int64_t timestampQpc = 0);
	void SnapshotRenderState();
	void SnapshotDLSSGResourcePendingAnchor();
	void PromoteDLSSGResourcePendingAnchor();
	MouseState GetCurrentState() const;
	bool GetWarpDelta(float& outDeltaYaw, float& outDeltaPitch) const;
	bool GetWarpDeltaFromDLSSGResourceAnchor(float& outDeltaYaw, float& outDeltaPitch) const;
	void ResetAccumulator();

	void SetSensitivity(float sensX, float sensY)
	{
		_sensitivityX = sensX;
		_sensitivityY = sensY;
	}
	float GetSensitivityX() const { return _sensitivityX; }
	float GetSensitivityY() const { return _sensitivityY; }
	bool HasRenderSnapshot() const { return _hasRenderSnapshot; }

	void AutoDetectSensitivity(float cameraDeltaYaw, float cameraDeltaPitch);
};

// ============================================================================
// CameraPredictor - Converts mouse deltas to camera rotation predictions
// ============================================================================

class CameraPredictor
{
public:
	struct WarpParams
	{
		XMMATRIX renderViewProj;
		XMMATRIX predictedViewProj;
		float warpStrength = 1.0f;
		bool depthAware = false;
	};

	struct WarpResult
	{
		XMMATRIX clipToClip;
		XMMATRIX clipToClipInverse;
		bool isValid = false;
		float deltaYaw = 0.0f;
		float deltaPitch = 0.0f;
	};

private:
	XMMATRIX _renderViewProj {};
	bool _hasRenderViewProj = false;

	XMMATRIX _prevViewProj {};
	bool _hasPrevViewProj = false;

	friend class FrameWarp_Dx12;

	float _angularVelocityYaw = 0.0f;
	float _angularVelocityPitch = 0.0f;
	float _warpStrength = 1.0f;
	float _maxWarpAngle = 0.05f;

public:
	CameraPredictor() = default;

	void RecordRenderCamera(const XMMATRIX& viewProj);

	WarpResult PredictFromMouseDelta(
		float mouseDeltaYaw,
		float mouseDeltaPitch,
		const XMMATRIX& renderViewProj,
		float warpStrength = 1.0f
	);

	WarpResult PredictFromVelocity(
		float deltaTimeSeconds,
		const XMMATRIX& renderViewProj,
		float warpStrength = 1.0f
	);

	static WarpResult ComputeWarpMatrix(
		const XMMATRIX& renderViewProj,
		const XMMATRIX& predictedViewProj
	);

	void SetWarpStrength(float strength) { _warpStrength = strength; }
	void SetMaxWarpAngle(float maxAngle) { _maxWarpAngle = maxAngle; }
	float GetWarpStrength() const { return _warpStrength; }
	float GetMaxWarpAngle() const { return _maxWarpAngle; }
	bool HasRenderViewProj() const { return _hasRenderViewProj; }
};

// ============================================================================
// FrameWarp_Dx12 - D3D12 implementation of the frame warp shader
// ============================================================================

class FrameWarp_Dx12
{
public:
	// Use FrameWarpShaderConstants from FrameWarp_Common.h for constant buffer layout.
	// The old FrameWarpConstants struct was a duplicate and has been removed.

	static constexpr size_t NUM_HEAPS = 4; // 2 dispatches per frame × 2 frames double-buffered
	static constexpr size_t DEPTH_INFILL_HISTORY_COUNT = 4;
	static constexpr UINT THREAD_GROUP_X = 16;
	static constexpr UINT THREAD_GROUP_Y = 16;

private:
	struct CameraContext
	{
		float vFovRadians = 1.0471975512f;
		float aspectRatio = 16.0f / 9.0f;
		bool valid = false;
		char source[32] = "default";
	};

	bool _initialized = false;
	bool _enabled = false;
	int _heapIndex = 0;
	ID3D12Device* _device = nullptr;

	// Pipeline state
	ID3D12RootSignature* _rootSignature = nullptr;
	ID3D12PipelineState* _pipelineStateSimple = nullptr;
	ID3D12PipelineState* _pipelineStateDepthAware = nullptr;
	ID3D12PipelineState* _pipelineStateMVCorrect = nullptr;
	ID3D12PipelineState* _pipelineStateCombined = nullptr;
	ID3D12PipelineState* _pipelineStateDistortionField = nullptr;
	ID3D12PipelineState* _pipelineStateStreamlineDistortionField = nullptr;
	ID3D12PipelineState* _pipelineStateDebugDisplacement = nullptr;
	ID3D12PipelineState* _pipelineStateDistortionVis = nullptr;
	ID3D12PipelineState* _pipelineStateStableUiComposite = nullptr;
	ID3D12PipelineState* _pipelineStateUiLayerExtract = nullptr;
	ID3D12PipelineState* _pipelineStateUiLayerComposite = nullptr;
	ID3D12Resource* _distortionVisOutput = nullptr;
	char _uiLayerExtractPipelineReason[64] = "not initialized";

	// Root signatures
	ID3D12RootSignature* _rootSignatureMV = nullptr;
	ID3D12RootSignature* _rootSignatureCombined = nullptr;

	// Constant buffers
	ID3D12Resource* _constantBuffers[NUM_HEAPS] = {};
	ID3D12Resource* _mvConstantBuffers[NUM_HEAPS] = {};

	// Output resources
	ID3D12Resource* _warpedOutput = nullptr;
	ID3D12Resource* _warpedPresentOutput = nullptr;
	ID3D12Resource* _stableUiOutput = nullptr;
	ID3D12Resource* _uiLayerCache[NUM_HEAPS] = {};
	D3D12_RESOURCE_STATES _uiLayerCacheStates[NUM_HEAPS] = {};
	ID3D12Resource* _uiCleanSceneCache[NUM_HEAPS] = {};
	D3D12_RESOURCE_STATES _uiCleanSceneCacheStates[NUM_HEAPS] = {};
	ID3D12Resource* _correctedMVOutput = nullptr;
	ID3D12Resource* _distortionFieldOutputs[NUM_HEAPS] = {};
	ID3D12Resource* _distortionFieldOutput = nullptr;
	D3D12_RESOURCE_STATES _distortionFieldStates[NUM_HEAPS] = {};
	D3D12_RESOURCE_STATES _lastDistortionFieldState = D3D12_RESOURCE_STATE_COMMON;
	int _currentDistortionFieldIndex = 0;
	ID3D12Resource* _streamlineDistortionFieldOutputs[NUM_HEAPS] = {};
	ID3D12Resource* _streamlineDistortionFieldOutput = nullptr;
	D3D12_RESOURCE_STATES _streamlineDistortionFieldStates[NUM_HEAPS] = {};
	D3D12_RESOURCE_STATES _lastStreamlineDistortionFieldState = D3D12_RESOURCE_STATE_COMMON;
	int _currentStreamlineDistortionFieldIndex = 0;
	DXGI_FORMAT _outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI_FORMAT _sourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI_FORMAT _mvFormat = DXGI_FORMAT_R16G16_FLOAT;
	DXGI_FORMAT _distortionFormat = DXGI_FORMAT_R16G16_FLOAT;
	DXGI_FORMAT _streamlineDistortionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	UINT _outputWidth = 0;
	UINT _outputHeight = 0;
	UINT _mvWidth = 0;
	UINT _mvHeight = 0;

	// Descriptor heaps
	FrameDescriptorHeap _frameHeaps[NUM_HEAPS];

	// Camera tracking
	MouseTracker _mouseTracker;
	CameraPredictor _cameraPredictor;
	CameraContext _cameraContext {};

	bool _canUseCombinedPath = false;
	CameraPredictor::WarpResult _lastWarpResult {};

	// Stats
	uint64_t _warpFrameCount = 0;
	bool _warpApplied = false;
	bool _mvCorrected = false;

	// Standalone/no-FG path command infrastructure.
	ID3D12Resource* _standaloneInputCopy = nullptr;
	D3D12_RESOURCE_STATES _standaloneInputCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
	ID3D12Resource* _stableUiPresentCopy = nullptr;
	D3D12_RESOURCE_STATES _stableUiPresentCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
	ID3D12Resource* _hudlessInputCopy = nullptr;
	D3D12_RESOURCE_STATES _hudlessInputCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
	bool _hudlessInputCopyValid = false;
	UINT _hudlessInputCopyWidth = 0;
	UINT _hudlessInputCopyHeight = 0;
	DXGI_FORMAT _hudlessInputCopyFormat = DXGI_FORMAT_UNKNOWN;
	ID3D12Resource* _trackedDepthInput = nullptr;
	D3D12_RESOURCE_STATES _trackedDepthInputState = D3D12_RESOURCE_STATE_COMMON;
	bool _trackedDepthInputValid = false;
	UINT _trackedDepthInputWidth = 0;
	UINT _trackedDepthInputHeight = 0;
	DXGI_FORMAT _trackedDepthInputFormat = DXGI_FORMAT_UNKNOWN;
	ID3D12Resource* _trackedMotionVectorInput = nullptr;
	D3D12_RESOURCE_STATES _trackedMotionVectorInputState = D3D12_RESOURCE_STATE_COMMON;
	bool _trackedMotionVectorInputValid = false;
	UINT _trackedMotionVectorInputWidth = 0;
	UINT _trackedMotionVectorInputHeight = 0;
	DXGI_FORMAT _trackedMotionVectorInputFormat = DXGI_FORMAT_UNKNOWN;
	float _trackedMotionVectorScaleX = 0.0f;
	float _trackedMotionVectorScaleY = 0.0f;
	bool _trackedMotionVectorScalePreMultiplied = false;
	char _trackedMotionVectorSource[32] = {};
	ID3D12Resource* _depthSnapshot = nullptr;
	D3D12_RESOURCE_STATES _depthSnapshotState = D3D12_RESOURCE_STATE_COMMON;
	bool _depthSnapshotValid = false;
	bool _depthSnapshotInverted = false;
	UINT _depthSnapshotWidth = 0;
	UINT _depthSnapshotHeight = 0;
	DXGI_FORMAT _depthSnapshotSourceFormat = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT _depthSnapshotCopyFormat = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT _depthSnapshotSrvFormat = DXGI_FORMAT_UNKNOWN;
	D3D12_RESOURCE_STATES _depthSnapshotSourceState = D3D12_RESOURCE_STATE_COMMON;
	UINT64 _depthSnapshotSerial = 0;
	UINT64 _depthSnapshotFrame = 0;
	char _depthSnapshotSource[32] = {};
	char _depthSnapshotReason[64] = {};
	ID3D12CommandAllocator* _standaloneCommandAllocator = nullptr;
	ID3D12GraphicsCommandList* _standaloneCommandList = nullptr;
	ID3D12Fence* _standaloneFence = nullptr;
	HANDLE _standaloneFenceEvent = nullptr;
	UINT64 _standaloneFenceValue = 0;

	static constexpr UINT MV_CALIBRATION_ROI = 64;
	ID3D12Resource* _mvCalibrationReadback = nullptr;
	UINT64 _mvCalibrationReadbackSize = 0;
	UINT _mvCalibrationRowPitch = 0;
	UINT _mvCalibrationWidth = 0;
	UINT _mvCalibrationHeight = 0;
	DXGI_FORMAT _mvCalibrationFormat = DXGI_FORMAT_UNKNOWN;
	bool _mvCalibrationPending = false;
	bool _mvCalibrationCopyScheduled = false;
	UINT64 _mvCalibrationFenceValue = 0;
	UINT64 _mvCalibrationFrame = 0;
	float _mvCalibrationScaleX = 0.0f;
	float _mvCalibrationScaleY = 0.0f;
	float _mvCalibrationSensitivity = 0.0f;
	float _mvCalibrationRenderMouseDx = 0.0f;
	float _mvCalibrationRenderMouseDy = 0.0f;
	float _mvCalibrationPredPxX = 0.0f;
	float _mvCalibrationPredPxY = 0.0f;
	char _mvCalibrationSource[32] = {};

	struct PendingStableUiFrame
	{
		bool valid = false;
		int frameIndex = -1;
		ID3D12Resource* originalHudless = nullptr;
		D3D12_RESOURCE_STATES originalHudlessState = D3D12_RESOURCE_STATE_COMMON;
		ID3D12Resource* warpedHudless = nullptr;
		D3D12_RESOURCE_STATES warpedHudlessState = D3D12_RESOURCE_STATE_COMMON;
		UINT width = 0;
		UINT height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	};

	PendingStableUiFrame _pendingStableUi {};

	struct CachedUiLayer
	{
		bool valid = false;
		UINT64 frameID = 0;
		UINT width = 0;
		UINT height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		bool cleanSceneValid = false;
		char source[32] = {};
	};

	CachedUiLayer _cachedUiLayers[NUM_HEAPS] {};
	UINT64 _stableUiSerial = 0;
	UINT64 _hudlessInputCopySerial = 0;
	UINT64 _lastHistoryHudlessSerial = 0;
	UINT64 _lastHistoryDepthSerial = 0;
	bool _dlssgHudlessSubmitUiValid = false;
	UINT64 _dlssgHudlessSubmitUiFrameID = 0;
	UINT64 _dlssgHudlessSubmitUiFrame = 0;

	struct DepthInfillHistoryFrame
	{
		bool valid = false;
		ID3D12Resource* color = nullptr;
		ID3D12Resource* depth = nullptr;
		D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
		UINT width = 0;
		UINT height = 0;
		DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
		UINT64 hudlessSerial = 0;
		UINT64 depthSerial = 0;
		UINT64 frame = 0;
		bool invertedDepth = false;
	};

	DepthInfillHistoryFrame _depthInfillHistory[DEPTH_INFILL_HISTORY_COUNT] {};
	size_t _depthInfillHistoryIndex = 0;

public:
	FrameWarp_Dx12() = default;
	~FrameWarp_Dx12();

	// Lifecycle
	bool Initialize(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format);
	void Shutdown();
	bool Resize(UINT width, UINT height, DXGI_FORMAT format);
	bool ResizeMV(UINT mvWidth, UINT mvHeight, DXGI_FORMAT mvFormat);

	// Per-frame operations
	void OnFrameRenderStart(const XMMATRIX& viewProj);

	enum class PresentResult
	{
		Invalid,
		ZeroPose,
		Warped
	};

	PresentResult OnPrePresentEx(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* colorInput,
		ID3D12Resource* depthInput,
		ID3D12Resource* uiInput,
		ID3D12Resource** output,
		D3D12_RESOURCE_STATES colorInputState = D3D12_RESOURCE_STATE_RENDER_TARGET,
		ID3D12Resource* outputOverride = nullptr,
		D3D12_RESOURCE_STATES depthInputState = D3D12_RESOURCE_STATE_DEPTH_READ,
		const char* contextLabel = nullptr,
		bool reuseLastWarpResult = false
	);

	bool OnPrePresent(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* colorInput,
		ID3D12Resource* depthInput,
		ID3D12Resource* uiInput,
		ID3D12Resource** output,
		D3D12_RESOURCE_STATES colorInputState = D3D12_RESOURCE_STATE_RENDER_TARGET,
		ID3D12Resource* outputOverride = nullptr,
		D3D12_RESOURCE_STATES depthInputState = D3D12_RESOURCE_STATE_DEPTH_READ,
		const char* contextLabel = nullptr,
		bool reuseLastWarpResult = false
	);

	bool CorrectMotionVectors(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* mvInput,
		ID3D12Resource* depthInput,
		float mvScaleX,
		float mvScaleY,
		UINT depthWidth,
		UINT depthHeight,
		ID3D12Resource** output
	);

	// Mouse input
	void OnRawMouseInput(LONG dx, LONG dy, const char* source = nullptr, int64_t timestampQpc = 0);

	// Configuration
	void SetEnabled(bool enabled) { _enabled = enabled; }
	bool IsEnabled() const { return _enabled; }
	bool IsInitialized() const { return _initialized; }
	void SetWarpStrength(float strength);
	void SetSensitivity(float sensX, float sensY);
	void SetDepthAware(bool depthAware);
	void SetMaxWarpAngle(float maxAngle);
	void SetCameraContext(float vFovRadians, float aspectRatio, const char* source);

	// Diagnostics
	bool WasWarpApplied() const { return _warpApplied; }
	bool WasMVCorrected() const { return _mvCorrected; }
	const CameraPredictor::WarpResult& GetLastWarpResult() const { return _lastWarpResult; }
	uint64_t GetWarpFrameCount() const { return _warpFrameCount; }

	// Accessors
	const MouseTracker& GetMouseTracker() const { return _mouseTracker; }
	MouseTracker& GetMouseTracker() { return _mouseTracker; }
	CameraPredictor& GetCameraPredictor() { return _cameraPredictor; }
	const CameraPredictor& GetCameraPredictor() const { return _cameraPredictor; }

	// Output resources
	ID3D12Resource* GetWarpedOutput() const { return _warpedOutput; }
	ID3D12Resource* GetWarpedPresentOutput() const { return _warpedPresentOutput; }
	ID3D12Resource* GetCorrectedMVOutput() const { return _correctedMVOutput; }

	bool CaptureHudlessSource(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* hudlessInput,
		D3D12_RESOURCE_STATES hudlessInputState,
		UINT width,
		UINT height,
		DXGI_FORMAT format,
		const char* source = nullptr
	);

	bool TrackDepthSource(
		ID3D12Resource* depthInput,
		D3D12_RESOURCE_STATES depthInputState,
		UINT width,
		UINT height,
		DXGI_FORMAT format,
		const char* source = nullptr
	);

	bool TrackMotionVectorSource(
		ID3D12Resource* motionVectorInput,
		D3D12_RESOURCE_STATES motionVectorInputState,
		UINT width,
		UINT height,
		DXGI_FORMAT format,
		float mvScaleX,
		float mvScaleY,
		bool scalePreMultiplied = false,
		const char* source = nullptr
	);

	bool CaptureDepthSource(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* depthInput,
		D3D12_RESOURCE_STATES depthInputState,
		UINT width,
		UINT height,
		DXGI_FORMAT format,
		bool invertedDepth,
		const char* source = nullptr
	);

	void RegisterStableUiFrame(
		int frameIndex,
		ID3D12Resource* originalHudless,
		D3D12_RESOURCE_STATES originalHudlessState,
		ID3D12Resource* warpedHudless,
		D3D12_RESOURCE_STATES warpedHudlessState,
		UINT width,
		UINT height,
		DXGI_FORMAT format
	);

	bool ApplyStableUiComposite(
		int frameIndex,
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* presentWithUi,
		D3D12_RESOURCE_STATES presentState
	);

	bool CompositeStableUiFrame(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* originalHudless,
		D3D12_RESOURCE_STATES originalHudlessState,
		ID3D12Resource* originalPresent,
		D3D12_RESOURCE_STATES originalPresentState,
		ID3D12Resource* warpedHudless,
		D3D12_RESOURCE_STATES warpedHudlessState,
		ID3D12Resource* outputPresent,
		D3D12_RESOURCE_STATES outputPresentState
	);

	bool ExtractUiLayerFromHudless(
		ID3D12GraphicsCommandList* cmdList,
		UINT64 frameID,
		ID3D12Resource* originalHudless,
		D3D12_RESOURCE_STATES originalHudlessState,
		ID3D12Resource* originalPresent,
		D3D12_RESOURCE_STATES originalPresentState,
		const char* source
	);

	bool CompositeCachedUiLayer(
		ID3D12GraphicsCommandList* cmdList,
		UINT64 frameID,
		UINT64 maxFrameAge,
		ID3D12Resource* warpedScene,
		D3D12_RESOURCE_STATES warpedSceneState,
		ID3D12Resource* outputPresent,
		D3D12_RESOURCE_STATES outputPresentState,
		const char* phase,
		bool suppressWarpedUiUnderlay = false,
		bool useWarpTransformForUnderlay = false
	);
	bool PrepareDLSSGHudlessSubmit(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* hudlessInput,
		D3D12_RESOURCE_STATES hudlessInputState,
		ID3D12Resource* finalWithUi,
		D3D12_RESOURCE_STATES finalWithUiState
	);
	void InvalidateDLSSGHudlessSubmit(const char* reason = nullptr);

	// Debug visualization: render mouse-warp displacement and disocclusion mask
	// Shows per-pixel displacement (R=horizontal, G=vertical, B=disocclusion)
	// with dimmed original frame as background.
	bool GenerateDebugDisplacement(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* colorInput,
		ID3D12Resource* depthInput,
		ID3D12Resource** output,
		D3D12_RESOURCE_STATES colorInputState = D3D12_RESOURCE_STATE_RENDER_TARGET
	);

	// Generate distortion field visualization (reads distortion field, writes RGBA vis)
	bool GenerateDistortionVis(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource** output
	);

	// Get the distortion vis output resource
	ID3D12Resource* GetDistortionVisOutput() const { return _distortionVisOutput; }

	// Generate distortion field for FG consumption
	bool GenerateDistortionField(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource** output
	);

	// Generate Streamline/DLSSG bidirectional distortion field.
	bool GenerateStreamlineDistortionField(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource** output
	);

	ID3D12Resource* GetDistortionFieldOutput() const { return _distortionFieldOutput; }
	ID3D12Resource* GetStreamlineDistortionFieldOutput() const { return _streamlineDistortionFieldOutput; }
	UINT GetOutputWidth() const { return _outputWidth; }
	UINT GetOutputHeight() const { return _outputHeight; }
	DXGI_FORMAT GetOutputFormat() const { return _outputFormat; }
	DXGI_FORMAT GetSourceFormat() const { return _sourceFormat; }
	ID3D12Device* GetDevice() const { return _device; }

	// Standalone backbuffer warp for no-FG latency reduction.
	bool ApplyStandaloneWarp(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue);
	bool LatchDLSSGResourceCopyWarpPose();
	void BeginDLSSGResourceFrameAnchor(float vFovRadians, float aspectRatio, const char* source);
	bool ApplyDLSSGResourceCopyWarp(
		ID3D12CommandQueue* commandQueue,
		ID3D12Resource* targetResource,
		D3D12_RESOURCE_STATES targetState,
		ID3D12Resource* sourceResource,
		const char* reason
	);

private:
	enum class PreparedWarpStatus
	{
		Invalid,
		ZeroPose,
		Ready
	};

	struct PreparedWarp
	{
		PreparedWarpStatus status = PreparedWarpStatus::Invalid;
		CameraPredictor::WarpResult warpResult {};
		float magnitude = 0.0f;
		float approxPixelShift = 0.0f;
		bool reusedLastResult = false;
		char reason[64] = {};
	};

	bool CreatePipelineStates();
	bool CreateConstantBuffers();
	bool CreateOutputResource(UINT width, UINT height, DXGI_FORMAT format);
	bool CreateMVOutputResource(UINT mvWidth, UINT mvHeight, DXGI_FORMAT mvFormat);
	bool CreateDistortionFieldResource(UINT width, UINT height);
	bool CreateStreamlineDistortionFieldResource(UINT width, UINT height);
	CameraContext ResolveCameraContext() const;
	void FillShaderConstants(FrameWarpShaderConstants& constants, const CameraPredictor::WarpResult& warpResult) const;
	void UpdateFrameWarpDiagnostics(const CameraContext& camera, const CameraPredictor::WarpResult& warpResult) const;
	bool EnsureStableUiPresentCopy(const D3D12_RESOURCE_DESC& desc);
	bool EnsureHudlessInputCopy(const D3D12_RESOURCE_DESC& desc);
	bool EnsureUiLayerCacheResources(const D3D12_RESOURCE_DESC& desc);
	bool EnsureUiCleanSceneCacheResources(const D3D12_RESOURCE_DESC& desc);
	bool StoreDepthInfillHistory(ID3D12GraphicsCommandList* cmdList);
	int FindDepthInfillHistory(UINT width, UINT height, DXGI_FORMAT colorFormat) const;
	PreparedWarp PrepareWarpForPresent(const char* contextLabel, bool reuseLastWarpResult);
	PresentResult OnPrePresentPrepared(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* colorInput,
		ID3D12Resource* depthInput,
		ID3D12Resource* uiInput,
		ID3D12Resource** output,
		D3D12_RESOURCE_STATES colorInputState,
		ID3D12Resource* outputOverride,
		D3D12_RESOURCE_STATES depthInputState,
		const char* contextLabel,
		const PreparedWarp& preparedWarp
	);
	int FindCachedUiLayer(UINT64 frameID, UINT64 maxFrameAge) const;
	bool CompositeUiLayerToPresent(
		ID3D12GraphicsCommandList* cmdList,
		int cacheIndex,
		ID3D12Resource* warpedScene,
		D3D12_RESOURCE_STATES warpedSceneState,
		ID3D12Resource* outputPresent,
		D3D12_RESOURCE_STATES outputPresentState,
		const char* phase,
		bool suppressWarpedUiUnderlay,
		bool useWarpTransformForUnderlay
	);
	bool CompositeStableUiToPresent(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* originalHudless,
		D3D12_RESOURCE_STATES originalHudlessState,
		ID3D12Resource* originalPresent,
		D3D12_RESOURCE_STATES originalPresentState,
		ID3D12Resource* warpedHudless,
		D3D12_RESOURCE_STATES warpedHudlessState,
		ID3D12Resource* outputPresent,
		D3D12_RESOURCE_STATES outputPresentState
	);
	bool CompositeDLSSGHudlessSubmitUi(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* sceneInput,
		D3D12_RESOURCE_STATES sceneInputState,
		ID3D12Resource* outputPresent,
		D3D12_RESOURCE_STATES outputPresentState
	);

	void AddInputBarriers(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* colorInput,
		ID3D12Resource* depthInput,
		ID3D12Resource* mvInput,
		D3D12_RESOURCE_STATES colorInputState = D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATES depthInputState = D3D12_RESOURCE_STATE_DEPTH_READ
	);

	bool EnsureStandaloneCommandObjects(ID3D12CommandQueue* commandQueue);
	bool WaitStandaloneFence();
	bool IsStandaloneFenceComplete() const;
	bool CaptureMotionVectorCalibrationSample(ID3D12GraphicsCommandList* cmdList);
	void ProcessMotionVectorCalibrationReadback();
};

namespace FrameWarpRuntime
{
	bool EnsureInitialized(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format);
	void Shutdown();
	FrameWarp_Dx12* Get();
	void SyncConfig();
	const char* PresentationOwnerName(FrameWarpPresentationOwner owner);
	FrameWarpPresentationOwner ResolvePresentationOwner(const char* caller = nullptr, bool logDecision = false);
	bool IsStandalonePresentationOwner(const char* caller = nullptr, bool logDecision = false);
	void ReportFsrfgDispatchSkipped(const char* reason);
	void ReportFsrfgPresentCallbackConfigured(bool configured, const char* reason = nullptr);
	void ReportFsrfgPresentCallbackSeen(UINT64 frameID, bool generated);
	void ReportXeFGPresentStatus(uint32_t framesPresented, int32_t frameGenResult, bool frameGenEnabled, int32_t queryResult);
	void MarkFrameRenderStart(const char* source = nullptr, float vFovRadians = 0.0f, float aspectRatio = 0.0f);
	void BeginDLSSGResourceFrameAnchor(float vFovRadians, float aspectRatio, const char* source = nullptr);
	void OnRawMouseInput(LONG dx, LONG dy, const char* source = nullptr, int64_t timestampQpc = 0);
	bool ApplyStandalone(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue);
	bool LatchDLSSGResourceCopyWarpPose(IDXGISwapChain* swapChain);
	bool ApplyToResourceAfterDLSSGCopy(
		ID3D12CommandQueue* commandQueue,
		ID3D12Resource* targetResource,
		D3D12_RESOURCE_STATES targetState,
		ID3D12Resource* sourceResource,
		const char* reason = nullptr
	);
	bool PrepareDLSSGHudlessSubmit(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* hudlessInput,
		D3D12_RESOURCE_STATES hudlessInputState,
		ID3D12Resource* finalWithUi,
		D3D12_RESOURCE_STATES finalWithUiState
	);
	void InvalidateDLSSGHudlessSubmit(const char* reason = nullptr);
	bool IsFsrfgPresentCallbackRequested();
	bool CaptureHudlessSource(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* hudlessInput,
		D3D12_RESOURCE_STATES hudlessInputState,
		UINT width,
		UINT height,
		DXGI_FORMAT format,
		const char* source = nullptr
	);
	bool TrackDepthSource(
		ID3D12Resource* depthInput,
		D3D12_RESOURCE_STATES depthInputState,
		UINT width,
		UINT height,
		DXGI_FORMAT format,
		const char* source = nullptr
	);
	bool TrackMotionVectorSource(
		ID3D12Resource* motionVectorInput,
		D3D12_RESOURCE_STATES motionVectorInputState,
		UINT width,
		UINT height,
		DXGI_FORMAT format,
		float mvScaleX,
		float mvScaleY,
		bool scalePreMultiplied = false,
		const char* source = nullptr
	);
	bool CaptureDepthSource(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* depthInput,
		D3D12_RESOURCE_STATES depthInputState,
		UINT width,
		UINT height,
		DXGI_FORMAT format,
		bool invertedDepth,
		const char* source = nullptr
	);
	void RegisterStableUiFrame(
		int frameIndex,
		ID3D12Resource* originalHudless,
		D3D12_RESOURCE_STATES originalHudlessState,
		ID3D12Resource* warpedHudless,
		D3D12_RESOURCE_STATES warpedHudlessState,
		UINT width,
		UINT height,
		DXGI_FORMAT format
	);
	bool ApplyStableUiComposite(
		int frameIndex,
		ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* presentWithUi,
		D3D12_RESOURCE_STATES presentState
	);
}
