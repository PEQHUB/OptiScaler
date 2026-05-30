#include "pch.h"
#include "ResTrack_dx12.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <menu/menu_overlay_dx.h>
#include <framewarp/FrameWarp.h>
#include <framewarp/FrameWarp_DLSSGPolicy.h>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <future>
#include <intrin.h>

#include <magic_enum_utility.hpp>
#include <include/d3dx/d3dx12.h>
#include <detours/detours.h>

#ifndef STDMETHODCALLTYPE
#include <Unknwn.h> // or <objbase.h> to get STDMETHODCALLTYPE
#endif

// Device hooks for FG
typedef void(STDMETHODCALLTYPE* PFN_CreateRenderTargetView)(ID3D12Device* This, ID3D12Resource* pResource,
                                                            D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_CreateShaderResourceView)(ID3D12Device* This, ID3D12Resource* pResource,
                                                              D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                                              D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_CreateUnorderedAccessView)(ID3D12Device* This, ID3D12Resource* pResource,
                                                               ID3D12Resource* pCounterResource,
                                                               D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
                                                               D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_CreateDepthStencilView)(ID3D12Device* This, ID3D12Resource* pResource,
                                                            const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_CreateConstantBufferView)(ID3D12Device* This,
                                                              const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
                                                              D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);

typedef void(STDMETHODCALLTYPE* PFN_CreateSampler)(ID3D12Device* This, const D3D12_SAMPLER_DESC* pDesc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDescriptorHeap)(ID3D12Device* This,
                                                             D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
                                                             REFIID riid, void** ppvHeap);
typedef ULONG(STDMETHODCALLTYPE* PFN_HeapRelease)(ID3D12DescriptorHeap* This);
typedef void(STDMETHODCALLTYPE* PFN_CopyDescriptors)(ID3D12Device* This, UINT NumDestDescriptorRanges,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                                                     UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
                                                     UINT* pSrcDescriptorRangeSizes,
                                                     D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType);
typedef void(STDMETHODCALLTYPE* PFN_CopyDescriptorsSimple)(ID3D12Device* This, UINT NumDescriptors,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
                                                           D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType);

// Command list hooks for FG
typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D12GraphicsCommandList* This,
                                                        UINT NumRenderTargetDescriptors,
                                                        D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
                                                        BOOL RTsSingleHandleToDescriptorRange,
                                                        D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_SetGraphicsRootDescriptorTable)(ID3D12GraphicsCommandList* This,
                                                                    UINT RootParameterIndex,
                                                                    D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_SetComputeRootDescriptorTable)(ID3D12GraphicsCommandList* This,
                                                                   UINT RootParameterIndex,
                                                                   D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_ResourceBarrier)(ID3D12GraphicsCommandList* This, UINT NumBarriers,
                                                     const D3D12_RESOURCE_BARRIER* pBarriers);
typedef void(STDMETHODCALLTYPE* PFN_CopyResource)(ID3D12GraphicsCommandList* This, ID3D12Resource* pDstResource,
                                                  ID3D12Resource* pSrcResource);
typedef void(STDMETHODCALLTYPE* PFN_CopyTextureRegion)(ID3D12GraphicsCommandList* This,
                                                       const D3D12_TEXTURE_COPY_LOCATION* pDst,
                                                       UINT DstX, UINT DstY, UINT DstZ,
                                                       const D3D12_TEXTURE_COPY_LOCATION* pSrc,
                                                       const D3D12_BOX* pSrcBox);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D12GraphicsCommandList* This, UINT IndexCountPerInstance,
                                                          UINT InstanceCount, UINT StartIndexLocation,
                                                          INT BaseVertexLocation, UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D12GraphicsCommandList* This, UINT VertexCountPerInstance,
                                                   UINT InstanceCount, UINT StartVertexLocation,
                                                   UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D12GraphicsCommandList* This, UINT ThreadGroupCountX,
                                              UINT ThreadGroupCountY, UINT ThreadGroupCountZ);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteBundle)(ID3D12GraphicsCommandList* This,
                                                   ID3D12GraphicsCommandList* pCommandList);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Close)(ID3D12GraphicsCommandList* This);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResetCommandList)(ID3D12GraphicsCommandList* This,
                                                        ID3D12CommandAllocator* pAllocator,
                                                        ID3D12PipelineState* pInitialState);

typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandLists)(ID3D12CommandQueue* This, UINT NumCommandLists,
                                                         ID3D12CommandList* const* ppCommandLists);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CommandQueueSignal)(ID3D12CommandQueue* This, ID3D12Fence* pFence,
                                                           UINT64 Value);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CommandQueueWait)(ID3D12CommandQueue* This, ID3D12Fence* pFence,
                                                         UINT64 Value);

typedef ULONG(STDMETHODCALLTYPE* PFN_Release)(ID3D12Resource* This);

// Original method calls for device
static PFN_CreateRenderTargetView o_CreateRenderTargetView = nullptr;
static PFN_CreateShaderResourceView o_CreateShaderResourceView = nullptr;
static PFN_CreateUnorderedAccessView o_CreateUnorderedAccessView = nullptr;
static PFN_CreateDepthStencilView o_CreateDepthStencilView = nullptr;
static PFN_CreateConstantBufferView o_CreateConstantBufferView = nullptr;
static PFN_CreateSampler o_CreateSampler = nullptr;

static PFN_CreateDescriptorHeap o_CreateDescriptorHeap = nullptr;
static PFN_HeapRelease o_HeapRelease = nullptr;
static PFN_CopyDescriptors o_CopyDescriptors = nullptr;
static PFN_CopyDescriptorsSimple o_CopyDescriptorsSimple = nullptr;

// Original method calls for command list
static PFN_Dispatch o_Dispatch = nullptr;
static PFN_DrawInstanced o_DrawInstanced = nullptr;
static PFN_DrawIndexedInstanced o_DrawIndexedInstanced = nullptr;
static PFN_ExecuteBundle o_ExecuteBundle = nullptr;
static PFN_Close o_Close = nullptr;
static PFN_ResetCommandList o_ResetCommandList = nullptr;
static PFN_ResourceBarrier o_ResourceBarrier = nullptr;
static PFN_CopyResource o_CopyResource = nullptr;
static PFN_CopyTextureRegion o_CopyTextureRegion = nullptr;

static PFN_ExecuteCommandLists o_ExecuteCommandLists = nullptr;
static PFN_CommandQueueSignal o_CommandQueueSignal = nullptr;
static PFN_CommandQueueWait o_CommandQueueWait = nullptr;
static PFN_Release o_Release = nullptr;

static PFN_OMSetRenderTargets o_OMSetRenderTargets = nullptr;
static PFN_SetGraphicsRootDescriptorTable o_SetGraphicsRootDescriptorTable = nullptr;
static PFN_SetComputeRootDescriptorTable o_SetComputeRootDescriptorTable = nullptr;

static std::mutex _hudlessTrackMutex;
static ankerl::unordered_dense::map<ID3D12GraphicsCommandList*,
                                    ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo>>
    fgPossibleHudless[BUFFER_COUNT];

// heaps section

// #define USE_SPINLOCK_MUTEX_FOR_HEAP_CREATION

#ifdef USE_SPINLOCK_MUTEX_FOR_HEAP_CREATION
static SpinLock _heapCreationMutex;
#else
static std::mutex _heapCreationMutex;
#endif

static std::vector<std::unique_ptr<HeapInfo>> fgHeaps;

static std::set<void*> _notFoundCmdLists;
static std::unordered_map<FG_ResourceType, void*> _resCmdList[BUFFER_COUNT];

static const char* VF2TraceModuleFromAddress(void* address, char (&moduleName)[MAX_PATH])
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

static bool VF2ShouldTraceDLSSGQueue()
{
    return FrameWarpDLSSG::ShouldTraceQueue();
}

static bool VF2ShouldLogDetailedTrace()
{
    return Config::Instance()->FrameWarpTimingAuditLog.value_or_default();
}

static void VF2LogQueueTrace(const char* op,
                             ID3D12CommandQueue* queue,
                             UINT numCommandLists,
                             ID3D12CommandList* const* commandLists,
                             ID3D12Fence* fence,
                             UINT64 fenceValue,
                             HRESULT result,
                             void* caller)
{
    if (!VF2ShouldTraceDLSSGQueue() || !VF2ShouldLogDetailedTrace())
        return;

    static uint64_t traceCount = 0;
    traceCount++;
    if (traceCount > 900 && traceCount % 30 != 0)
        return;

    char callerModule[MAX_PATH] = {};
    const char* moduleName = VF2TraceModuleFromAddress(caller, callerModule);
    auto& state = State::Instance();
    auto& status = state.frameWarpStatus;
    D3D12_COMMAND_QUEUE_DESC desc {};
    if (queue != nullptr)
        desc = queue->GetDesc();

    uintptr_t list0 = 0;
    uintptr_t list1 = 0;
    uintptr_t list2 = 0;
    if (commandLists != nullptr && numCommandLists > 0)
        list0 = reinterpret_cast<uintptr_t>(commandLists[0]);
    if (commandLists != nullptr && numCommandLists > 1)
        list1 = reinterpret_cast<uintptr_t>(commandLists[1]);
    if (commandLists != nullptr && numCommandLists > 2)
        list2 = reinterpret_cast<uintptr_t>(commandLists[2]);

    LOG_INFO("VF2QueueTrace #{} frame={} op={} caller={} callerAddr={:X} queue={:X} queueType={} queueFlags={:X} priority={} node={} numLists={} list0={:X} list1={:X} list2={:X} fence={:X} fenceValue={} result={:X} activeFg={} renderSnapshot={} rawSeq={} snapSeq={} sinceSnap={} slPresented={} nativeDetected={} nativeAttach={} nativePassthrough={}",
             traceCount,
             state.frameCount,
             op != nullptr ? op : "unknown",
             moduleName,
             reinterpret_cast<uintptr_t>(caller),
             reinterpret_cast<uintptr_t>(queue),
             static_cast<uint32_t>(desc.Type),
             static_cast<uint32_t>(desc.Flags),
             desc.Priority,
             desc.NodeMask,
             numCommandLists,
             list0,
             list1,
             list2,
             reinterpret_cast<uintptr_t>(fence),
             fenceValue,
             static_cast<uint32_t>(result),
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

struct VF2CommandListResourceSummary
{
    ID3D12Resource* resource = nullptr;
    UINT64 width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
    uint32_t reads = 0;
    uint32_t writes = 0;
    uint32_t barriers = 0;
    uint32_t copies = 0;
    uint32_t descriptors = 0;
    uint32_t rtvs = 0;
    uint32_t uavs = 0;
    uint32_t beforeStates = 0;
    uint32_t afterStates = 0;
    char lastOp[24] = {};
    char lastCaller[64] = {};
};

struct VF2CommandListTrace
{
    uint64_t events = 0;
    ankerl::unordered_dense::map<ID3D12Resource*, VF2CommandListResourceSummary> resources;
};

struct VF2DLSSGResourceWarpCandidate
{
    bool seen = false;
    bool accepted = false;
    ID3D12Resource* dst = nullptr;
    ID3D12Resource* src = nullptr;
    D3D12_RESOURCE_STATES dstState = D3D12_RESOURCE_STATE_COMMON;
    UINT64 width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    char reason[96] = {};
};

static std::mutex g_vf2CommandListTraceMutex;
static ankerl::unordered_dense::map<ID3D12GraphicsCommandList*, VF2CommandListTrace> g_vf2CommandListTraces;

static bool VF2TraceIsWriteState(D3D12_RESOURCE_STATES state)
{
    const uint32_t value = static_cast<uint32_t>(state);
    const uint32_t writeMask = static_cast<uint32_t>(D3D12_RESOURCE_STATE_RENDER_TARGET) |
                               static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
                               static_cast<uint32_t>(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
                               static_cast<uint32_t>(D3D12_RESOURCE_STATE_COPY_DEST) |
                               static_cast<uint32_t>(D3D12_RESOURCE_STATE_RESOLVE_DEST) |
                               static_cast<uint32_t>(D3D12_RESOURCE_STATE_STREAM_OUT);
    return (value & writeMask) != 0;
}

static bool VF2TraceIsReadState(D3D12_RESOURCE_STATES state)
{
    const uint32_t value = static_cast<uint32_t>(state);
    const uint32_t readMask = static_cast<uint32_t>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) |
                              static_cast<uint32_t>(D3D12_RESOURCE_STATE_INDEX_BUFFER) |
                              static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
                              static_cast<uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
                              static_cast<uint32_t>(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) |
                              static_cast<uint32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE) |
                              static_cast<uint32_t>(D3D12_RESOURCE_STATE_DEPTH_READ) |
                              static_cast<uint32_t>(D3D12_RESOURCE_STATE_RESOLVE_SOURCE) |
                              static_cast<uint32_t>(D3D12_RESOURCE_STATE_PREDICATION);
    return (value & readMask) != 0 || (value & static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) != 0;
}

static bool VF2TraceIsStreamlineModule(const char* moduleName)
{
    return FrameWarpDLSSG::IsStreamlineModule(moduleName);
}

static void VF2TraceCommandListResource(ID3D12GraphicsCommandList* commandList,
                                        const char* op,
                                        ID3D12Resource* resource,
                                        bool read,
                                        bool write,
                                        bool barrier,
                                        bool copy,
                                        bool descriptor,
                                        bool rtv,
                                        bool uav,
                                        D3D12_RESOURCE_STATES beforeState,
                                        D3D12_RESOURCE_STATES afterState,
                                        void* caller)
{
    if (!VF2ShouldTraceDLSSGQueue() || commandList == nullptr || resource == nullptr)
        return;

    char callerModule[MAX_PATH] = {};
    const char* moduleName = VF2TraceModuleFromAddress(caller, callerModule);

    std::lock_guard<std::mutex> lock(g_vf2CommandListTraceMutex);
    auto& trace = g_vf2CommandListTraces[commandList];
    trace.events++;

    constexpr size_t maxResourcesPerCommandList = 128;
    auto existing = trace.resources.find(resource);
    if (existing == trace.resources.end() && trace.resources.size() >= maxResourcesPerCommandList)
        return;

    auto& summary = trace.resources[resource];
    if (summary.resource == nullptr)
    {
        summary.resource = resource;
        auto desc = resource->GetDesc();
        summary.width = desc.Width;
        summary.height = desc.Height;
        summary.format = desc.Format;
        summary.flags = desc.Flags;
        summary.dimension = desc.Dimension;
    }

    if (read)
        summary.reads++;
    if (write)
        summary.writes++;
    if (barrier)
        summary.barriers++;
    if (copy)
        summary.copies++;
    if (descriptor)
        summary.descriptors++;
    if (rtv)
        summary.rtvs++;
    if (uav)
        summary.uavs++;

    summary.beforeStates |= static_cast<uint32_t>(beforeState);
    summary.afterStates |= static_cast<uint32_t>(afterState);
    strncpy_s(summary.lastOp, op != nullptr ? op : "unknown", _TRUNCATE);
    strncpy_s(summary.lastCaller, moduleName, _TRUNCATE);
}

static void VF2TraceDescriptor(ID3D12GraphicsCommandList* commandList,
                               const char* op,
                               ResourceInfo* resourceInfo,
                               void* caller)
{
    if (resourceInfo == nullptr || resourceInfo->buffer == nullptr)
        return;

    const bool isUav = resourceInfo->type == UAV;
    const bool isRtv = resourceInfo->type == RTV;
    const bool isSrv = resourceInfo->type == SRV;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    if (isUav)
        state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    else if (isRtv)
        state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    else if (isSrv)
        state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    VF2TraceCommandListResource(commandList,
                                op,
                                resourceInfo->buffer,
                                isSrv || isUav,
                                isRtv || isUav,
                                false,
                                false,
                                true,
                                isRtv,
                                isUav,
                                D3D12_RESOURCE_STATE_COMMON,
                                state,
                                caller);
}

static bool VF2StateMaskEquals(uint32_t states, D3D12_RESOURCE_STATES state)
{
    return states == static_cast<uint32_t>(state);
}

static bool VF2StateMaskCopySourceLike(uint32_t states)
{
    const uint32_t copySource = static_cast<uint32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE);
    const uint32_t uav = static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return states == copySource || states == (copySource | uav);
}

static bool VF2ShouldLogDLSSGResourceCandidate(uint64_t sequence)
{
    if (!VF2ShouldLogDetailedTrace())
        return sequence <= 10 || sequence % 300 == 0;

    return sequence <= 320 || sequence % 30 == 0;
}

static void VF2LogDLSSGResourceCandidate(const VF2DLSSGResourceWarpCandidate& candidate,
                                         ID3D12CommandQueue* queue,
                                         ID3D12GraphicsCommandList* commandList,
                                         const VF2CommandListTrace& trace,
                                         const char* callerModule,
                                         uint64_t sequence)
{
    if (!VF2ShouldLogDLSSGResourceCandidate(sequence))
        return;

    auto& state = State::Instance();
    auto& status = state.frameWarpStatus;
    LOG_INFO("VF2DLSSGResourceWarpCandidate #{} frame={} accepted={} reason={} policy={} confidence={} caller={} queue={:X} list={:X} resources={} events={} activeFg={} renderSnapshot={} rawSeq={} snapSeq={} sinceSnap={} slPresented={}",
             sequence,
             state.frameCount,
             candidate.accepted ? "true" : "false",
             candidate.reason[0] ? candidate.reason : "unknown",
             status.dlssgResourceWarpPolicyName[0] ? status.dlssgResourceWarpPolicyName : "unknown",
             status.dlssgResourceWarpCompatibilityConfidence,
             callerModule != nullptr ? callerModule : "unknown",
             reinterpret_cast<uintptr_t>(queue),
             reinterpret_cast<uintptr_t>(commandList),
             trace.resources.size(),
             trace.events,
             static_cast<uint32_t>(state.activeFgOutput),
             status.renderSnapshotCount,
             status.rawInputSequence,
             status.rawInputSnapshotSequence,
             status.rawInputSamplesSinceSnapshot,
             status.dlssgLastFramesPresented);
}

static VF2DLSSGResourceWarpCandidate VF2FindDLSSGResourceWarpCandidate(
    const VF2CommandListTrace& trace,
    ID3D12CommandQueue* queue,
    const char* callerModule,
    UINT numCommandLists)
{
    VF2DLSSGResourceWarpCandidate candidate {};
    strncpy_s(candidate.reason, "not checked", _TRUNCATE);

    auto& state = State::Instance();
    auto& status = state.frameWarpStatus;
    const auto policy = FrameWarpDLSSG::GetResourceWarpPolicy();
    status.dlssgResourceWarpPolicyMode = policy.mode;
    strncpy_s(status.dlssgResourceWarpPolicyName, policy.modeName, _TRUNCATE);

    auto setReason = [&](const char* reason, uint32_t confidence = 0) {
        strncpy_s(candidate.reason, reason != nullptr ? reason : "unknown", _TRUNCATE);
        status.dlssgResourceWarpCompatibilityConfidence = confidence;
        strncpy_s(status.dlssgResourceWarpCompatibilityReason, candidate.reason, _TRUNCATE);
    };

    setReason("not checked");

    if (!policy.resourceCopyEnabled)
    {
        setReason(policy.frameWarpEnabled ? "mode not resource-copy" : "framewarp disabled");
        return candidate;
    }
    if (state.activeFgOutput != FGOutput::DLSSG)
    {
        setReason("DLSSG inactive");
        return candidate;
    }
    if (!FrameWarpDLSSG::IsResourceCopyModule(callerModule))
    {
        setReason("caller not sl.common");
        return candidate;
    }
    if (queue == nullptr || numCommandLists != 1)
    {
        setReason("queue/list count mismatch", 10);
        return candidate;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = queue->GetDesc();
    if (!FrameWarpDLSSG::IsResourceCopyQueueShape(queueDesc))
    {
        setReason("queue shape mismatch", 20);
        return candidate;
    }
    if (!FrameWarpDLSSG::IsSimpleCopyTraceShape(trace.resources.size(), trace.events))
    {
        setReason("trace shape mismatch", 30);
        return candidate;
    }

    candidate.seen = true;
    state.frameWarpStatus.dlssgResourceWarpCandidateCount++;

    const VF2CommandListResourceSummary* dst = nullptr;
    const VF2CommandListResourceSummary* src = nullptr;
    for (const auto& [resource, summary] : trace.resources)
    {
        char textureReason[96] = {};
        const bool compatibleTexture =
            summary.resource != nullptr &&
            FrameWarpDLSSG::IsCompatibleOutputTexture(summary.dimension,
                                                      summary.width,
                                                      summary.height,
                                                      summary.format,
                                                      state.currentSwapchainDesc,
                                                      textureReason,
                                                      sizeof(textureReason));
        if (!compatibleTexture)
        {
            setReason(textureReason[0] ? textureReason : "resource shape mismatch", 40);
            return candidate;
        }

        const bool dstLike =
            summary.reads == 0 &&
            summary.writes > 0 &&
            summary.copies > 0 &&
            VF2StateMaskEquals(summary.beforeStates, D3D12_RESOURCE_STATE_COPY_DEST) &&
            VF2StateMaskEquals(summary.afterStates, D3D12_RESOURCE_STATE_COPY_DEST);
        const bool srcLike =
            summary.reads > 0 &&
            summary.copies > 0 &&
            VF2StateMaskCopySourceLike(summary.beforeStates) &&
            VF2StateMaskCopySourceLike(summary.afterStates);

        if (dstLike)
            dst = &summary;
        if (srcLike)
            src = &summary;
    }

    if (dst == nullptr || src == nullptr || dst->resource == src->resource)
    {
        setReason("copy endpoints not identified", 60);
        return candidate;
    }

    candidate.accepted = true;
    candidate.dst = dst->resource;
    candidate.src = src->resource;
    candidate.dstState = D3D12_RESOURCE_STATE_COPY_DEST;
    candidate.width = dst->width;
    candidate.height = dst->height;
    candidate.format = dst->format;
    setReason("matched sl.common resource-copy output", 100);
    state.frameWarpStatus.dlssgResourceWarpAcceptedCount++;
    return candidate;
}

static VF2DLSSGResourceWarpCandidate VF2FlushCommandListResourceTrace(ID3D12CommandQueue* queue,
                                                                      UINT numCommandLists,
                                                                      ID3D12CommandList* const* commandLists,
                                                                      const char* callerModule)
{
    VF2DLSSGResourceWarpCandidate candidate {};
    strncpy_s(candidate.reason, "no trace", _TRUNCATE);
    if (!VF2ShouldTraceDLSSGQueue() || commandLists == nullptr || numCommandLists == 0)
        return candidate;

    const bool logDetails = VF2ShouldLogDetailedTrace() && VF2TraceIsStreamlineModule(callerModule);
    static uint64_t traceCount = 0;

    std::lock_guard<std::mutex> lock(g_vf2CommandListTraceMutex);
    for (UINT i = 0; i < numCommandLists; i++)
    {
        auto* commandList = reinterpret_cast<ID3D12GraphicsCommandList*>(commandLists[i]);
        auto it = g_vf2CommandListTraces.find(commandList);
        if (it == g_vf2CommandListTraces.end())
            continue;

        auto& trace = it->second;
        auto currentCandidate = VF2FindDLSSGResourceWarpCandidate(trace, queue, callerModule, numCommandLists);
        if (currentCandidate.seen || currentCandidate.accepted)
        {
            candidate = currentCandidate;
            VF2LogDLSSGResourceCandidate(currentCandidate,
                                         queue,
                                         commandList,
                                         trace,
                                         callerModule,
                                         traceCount + 1);
        }

        traceCount++;
        const bool shouldLogThisList = logDetails && (traceCount <= 320 || traceCount % 30 == 0);

        if (shouldLogThisList)
        {
            auto& state = State::Instance();
            auto& status = state.frameWarpStatus;
            LOG_INFO("VF2CLTrace #{} frame={} caller={} queue={:X} list={:X} resources={} events={} activeFg={} renderSnapshot={} rawSeq={} snapSeq={} sinceSnap={} slPresented={}",
                     traceCount,
                     state.frameCount,
                     callerModule != nullptr ? callerModule : "unknown",
                     reinterpret_cast<uintptr_t>(queue),
                     reinterpret_cast<uintptr_t>(commandList),
                     trace.resources.size(),
                     trace.events,
                     static_cast<uint32_t>(state.activeFgOutput),
                     status.renderSnapshotCount,
                     status.rawInputSequence,
                     status.rawInputSnapshotSequence,
                     status.rawInputSamplesSinceSnapshot,
                     status.dlssgLastFramesPresented);

            uint32_t resourceIndex = 0;
            for (auto& [resource, summary] : trace.resources)
            {
                if (resourceIndex >= 32)
                    break;

                LOG_INFO("VF2CLResource #{} list={:X} idx={} res={:X} dim={} size={}x{} fmt={} flags={:X} reads={} writes={} barriers={} copies={} descriptors={} rtvs={} uavs={} before={:X} after={:X} lastOp={} lastCaller={}",
                         traceCount,
                         reinterpret_cast<uintptr_t>(commandList),
                         resourceIndex,
                         reinterpret_cast<uintptr_t>(resource),
                         static_cast<uint32_t>(summary.dimension),
                         summary.width,
                         summary.height,
                         static_cast<uint32_t>(summary.format),
                         static_cast<uint32_t>(summary.flags),
                         summary.reads,
                         summary.writes,
                         summary.barriers,
                         summary.copies,
                         summary.descriptors,
                         summary.rtvs,
                         summary.uavs,
                         summary.beforeStates,
                         summary.afterStates,
                         summary.lastOp,
                         summary.lastCaller);
                resourceIndex++;
            }
        }

        g_vf2CommandListTraces.erase(it);
    }

    return candidate;
}

struct HeapCacheTLS
{
    unsigned genSeen = 0;
    HeapInfo* heapPtr = nullptr;
    uint64_t heapVersion = 0;
};

static thread_local HeapCacheTLS cache;
static thread_local HeapCacheTLS cacheRTV;
static thread_local HeapCacheTLS cacheCBV;
static thread_local HeapCacheTLS cacheSRV;
static thread_local HeapCacheTLS cacheUAV;
static std::atomic<unsigned> gHeapGeneration { 1 };

static thread_local HeapCacheTLS cacheGR;
static thread_local HeapCacheTLS cacheCR;

bool ResTrack_Dx12::CheckResource(ID3D12Resource* resource)
{
    if (State::Instance().isShuttingDown)
        return false;

    auto resDesc = resource->GetDesc();

    if (resDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        return false;

    auto& s = State::Instance();

    if (resDesc.Height != s.currentSwapchainDesc.BufferDesc.Height ||
        resDesc.Width != s.currentSwapchainDesc.BufferDesc.Width)
    {
        auto result = Config::Instance()->FGRelaxedResolutionCheck.value_or_default() &&
                      resDesc.Height >= s.currentSwapchainDesc.BufferDesc.Height - 32 &&
                      resDesc.Height <= s.currentSwapchainDesc.BufferDesc.Height + 32 &&
                      resDesc.Width >= s.currentSwapchainDesc.BufferDesc.Width - 32 &&
                      resDesc.Width <= s.currentSwapchainDesc.BufferDesc.Width + 32;

        // LOG_TRACK("Resource: {}x{} ({}), Swapchain: {}x{} ({}), Relaxed Result: {}", resDesc.Width, resDesc.Height,
        //           (UINT) resDesc.Format, scDesc.BufferDesc.Width, scDesc.BufferDesc.Height,
        //           (UINT) scDesc.BufferDesc.Format, result);

        return result;
    }

    return true;
}

inline static IID streamlineRiid {};
inline static std::once_flag streamlineRiidInitFlag;

bool ResTrack_Dx12::CheckForRealObject(const std::string functionName, IUnknown* pObject, IUnknown** ppRealObject)
{
    std::call_once(streamlineRiidInitFlag,
                   []() { IIDFromString(L"{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}", &streamlineRiid); });

    auto qResult = pObject->QueryInterface(streamlineRiid, (void**) ppRealObject);

    if (qResult == S_OK && *ppRealObject != nullptr)
    {
        LOG_INFO("{} Streamline proxy found!", functionName);
        (*ppRealObject)->Release();
        return true;
    }

    return false;
}

#pragma region Resource methods

bool ResTrack_Dx12::CreateBufferResource(ID3D12Device* InDevice, ResourceInfo* InSource, D3D12_RESOURCE_STATES InState,
                                         ID3D12Resource** OutResource)
{
    if (InDevice == nullptr || InSource == nullptr || InSource->buffer == nullptr)
        return false;

    if (*OutResource != nullptr)
    {
        auto bufDesc = (*OutResource)->GetDesc();

        if (bufDesc.Width != (UINT64) (InSource->width) || bufDesc.Height != (UINT) (InSource->height) ||
            bufDesc.Format != InSource->format)
        {
            (*OutResource)->Release();
            (*OutResource) = nullptr;
        }
        else
            return true;
    }

    D3D12_HEAP_PROPERTIES heapProperties;
    D3D12_HEAP_FLAGS heapFlags;
    HRESULT hr = InSource->buffer->GetHeapProperties(&heapProperties, &heapFlags);

    if (hr != S_OK)
    {
        LOG_ERROR("GetHeapProperties result: {0:X}", (UINT64) hr);
        return false;
    }

    D3D12_RESOURCE_DESC texDesc = InSource->buffer->GetDesc();
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    hr = InDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &texDesc, InState, nullptr,
                                           IID_PPV_ARGS(OutResource));

    if (hr != S_OK)
    {
        LOG_ERROR("CreateCommittedResource result: {0:X}", (UINT64) hr);
        return false;
    }

    (*OutResource)->SetName(L"fgHudlessSCBufferCopy");
    return true;
}

void ResTrack_Dx12::ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                                    D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState)
{
    if (InBeforeState == InAfterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = InResource;
    barrier.Transition.StateBefore = InBeforeState;
    barrier.Transition.StateAfter = InAfterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    InCommandList->ResourceBarrier(1, &barrier);
}

#pragma endregion

#pragma region Heap helpers

SIZE_T ResTrack_Dx12::GetGPUHandle(ID3D12Device* This, SIZE_T cpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        auto val = fgHeaps[i].get();
        if (fgHeaps[i] != nullptr && val->active && val->cpuStart <= cpuHandle && val->cpuEnd > cpuHandle &&
            val->gpuStart != 0)
        {
            auto incSize = This->GetDescriptorHandleIncrementSize(type);
            auto addr = cpuHandle - val->cpuStart;
            auto index = addr / incSize;
            auto gpuAddr = val->gpuStart + (index * incSize);

            return gpuAddr;
        }
    }

    return NULL;
}

SIZE_T ResTrack_Dx12::GetCPUHandle(ID3D12Device* This, SIZE_T gpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        auto val = fgHeaps[i].get();
        if (fgHeaps[i] != nullptr && val->active && val->gpuStart <= gpuHandle && val->gpuEnd > gpuHandle &&
            val->cpuStart != 0)
        {
            auto incSize = This->GetDescriptorHandleIncrementSize(type);
            auto addr = gpuHandle - val->gpuStart;
            auto index = addr / incSize;
            auto cpuAddr = val->cpuStart + (index * incSize);

            return cpuAddr;
        }
    }

    return NULL;
}

HeapInfo* ResTrack_Dx12::GetHeapByCpuHandleCBV(SIZE_T cpuHandle)
{
    unsigned currentGen = gHeapGeneration.load(std::memory_order_acquire);
    if (cacheCBV.genSeen == currentGen && cacheCBV.heapPtr != nullptr &&
        cacheCBV.heapPtr->version == cacheCBV.heapVersion && cacheCBV.heapPtr->active &&
        cacheCBV.heapPtr->cpuStart <= cpuHandle && cpuHandle < cacheCBV.heapPtr->cpuEnd)
    {
        return cacheCBV.heapPtr;
    }

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        if (fgHeaps[i] != nullptr && fgHeaps[i]->active && fgHeaps[i]->cpuStart <= cpuHandle &&
            cpuHandle < fgHeaps[i]->cpuEnd)
        {
            cacheCBV.genSeen = currentGen;
            cacheCBV.heapPtr = fgHeaps[i].get();
            cacheCBV.heapVersion = cacheCBV.heapPtr->version;
            return cacheCBV.heapPtr;
        }
    }

    cacheCBV.heapVersion = 0;
    cacheCBV.heapPtr = nullptr;
    return nullptr;
}

HeapInfo* ResTrack_Dx12::GetHeapByCpuHandleRTV(SIZE_T cpuHandle)
{
    unsigned currentGen = gHeapGeneration.load(std::memory_order_acquire);
    if (cacheRTV.genSeen == currentGen && cacheRTV.heapPtr != nullptr &&
        cacheRTV.heapPtr->version == cacheRTV.heapVersion && cacheRTV.heapPtr->active &&
        cacheRTV.heapPtr->cpuStart <= cpuHandle && cpuHandle < cacheRTV.heapPtr->cpuEnd)
    {
        return cacheRTV.heapPtr;
    }

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        if (fgHeaps[i] != nullptr && fgHeaps[i]->active && fgHeaps[i]->cpuStart <= cpuHandle &&
            cpuHandle < fgHeaps[i]->cpuEnd)
        {
            cacheRTV.genSeen = currentGen;
            cacheRTV.heapPtr = fgHeaps[i].get();
            cacheRTV.heapVersion = cacheRTV.heapPtr->version;
            return cacheRTV.heapPtr;
        }
    }

    cacheRTV.heapVersion = 0;
    cacheRTV.heapPtr = nullptr;
    return nullptr;
}

HeapInfo* ResTrack_Dx12::GetHeapByCpuHandleSRV(SIZE_T cpuHandle)
{
    unsigned currentGen = gHeapGeneration.load(std::memory_order_acquire);
    if (cacheSRV.genSeen == currentGen && cacheSRV.heapPtr != nullptr &&
        cacheSRV.heapPtr->version == cacheSRV.heapVersion && cacheSRV.heapPtr->active &&
        cacheSRV.heapPtr->cpuStart <= cpuHandle && cpuHandle < cacheSRV.heapPtr->cpuEnd)
    {
        return cacheSRV.heapPtr;
    }

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        if (fgHeaps[i] != nullptr && fgHeaps[i]->active && fgHeaps[i]->cpuStart <= cpuHandle &&
            cpuHandle < fgHeaps[i]->cpuEnd)
        {
            cacheSRV.genSeen = currentGen;
            cacheSRV.heapPtr = fgHeaps[i].get();
            cacheSRV.heapVersion = cacheSRV.heapPtr->version;
            return cacheSRV.heapPtr;
        }
    }

    cacheSRV.heapVersion = 0;
    cacheSRV.heapPtr = nullptr;
    return nullptr;
}

HeapInfo* ResTrack_Dx12::GetHeapByCpuHandleUAV(SIZE_T cpuHandle)
{
    unsigned currentGen = gHeapGeneration.load(std::memory_order_acquire);
    if (cacheUAV.genSeen == currentGen && cacheUAV.heapPtr != nullptr &&
        cacheUAV.heapPtr->version == cacheUAV.heapVersion && cacheUAV.heapPtr->active &&
        cacheUAV.heapPtr->cpuStart <= cpuHandle && cpuHandle < cacheUAV.heapPtr->cpuEnd)
    {
        return cacheUAV.heapPtr;
    }

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        if (fgHeaps[i] != nullptr && fgHeaps[i]->active && fgHeaps[i]->cpuStart <= cpuHandle &&
            cpuHandle < fgHeaps[i]->cpuEnd)
        {
            cacheUAV.genSeen = currentGen;
            cacheUAV.heapPtr = fgHeaps[i].get();
            cacheUAV.heapVersion = cacheUAV.heapPtr->version;
            return cacheUAV.heapPtr;
        }
    }

    cacheUAV.heapVersion = 0;
    cacheUAV.heapPtr = nullptr;
    return nullptr;
}

HeapInfo* ResTrack_Dx12::GetHeapByCpuHandle(SIZE_T cpuHandle)
{
    unsigned currentGen = gHeapGeneration.load(std::memory_order_acquire);
    if (cache.genSeen == currentGen && cache.heapPtr != nullptr && cache.heapPtr->version == cache.heapVersion &&
        cache.heapPtr->active && cache.heapPtr->cpuStart <= cpuHandle && cpuHandle < cache.heapPtr->cpuEnd)
    {
        return cache.heapPtr;
    }

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        if (fgHeaps[i] != nullptr && fgHeaps[i]->active && fgHeaps[i]->cpuStart <= cpuHandle &&
            cpuHandle < fgHeaps[i]->cpuEnd)
        {
            cache.genSeen = currentGen;
            cache.heapPtr = fgHeaps[i].get();
            cache.heapVersion = cache.heapPtr->version;
            return cache.heapPtr;
        }
    }

    cache.heapVersion = 0;
    cache.heapPtr = nullptr;
    return nullptr;
}

HeapInfo* ResTrack_Dx12::GetHeapByGpuHandleGR(SIZE_T gpuHandle)
{
    if (gpuHandle == NULL)
        return nullptr;

    unsigned currentGen = gHeapGeneration.load(std::memory_order_acquire);
    if (cacheGR.genSeen == currentGen && cacheGR.heapPtr != nullptr &&
        cacheGR.heapPtr->version == cacheGR.heapVersion && cacheGR.heapPtr->active &&
        cacheGR.heapPtr->gpuStart <= gpuHandle && gpuHandle < cacheGR.heapPtr->gpuEnd)
    {
        return cacheGR.heapPtr;
    }

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        if (fgHeaps[i] != nullptr && fgHeaps[i]->active && fgHeaps[i]->gpuStart <= gpuHandle &&
            gpuHandle < fgHeaps[i]->gpuEnd)
        {
            cacheGR.genSeen = currentGen;
            cacheGR.heapPtr = fgHeaps[i].get();
            cacheGR.heapVersion = cacheGR.heapPtr->version;
            return cacheGR.heapPtr;
        }
    }

    cacheGR.heapVersion = 0;
    cacheGR.heapPtr = nullptr;
    return nullptr;
}

HeapInfo* ResTrack_Dx12::GetHeapByGpuHandleCR(SIZE_T gpuHandle)
{
    if (gpuHandle == NULL)
        return nullptr;

    unsigned currentGen = gHeapGeneration.load(std::memory_order_acquire);
    if (cacheCR.genSeen == currentGen && cacheCR.heapPtr != nullptr &&
        cacheCR.heapPtr->version == cacheCR.heapVersion && cacheCR.heapPtr->active &&
        cacheCR.heapPtr->gpuStart <= gpuHandle && gpuHandle < cacheCR.heapPtr->gpuEnd)
    {
        return cacheCR.heapPtr;
    }

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        if (fgHeaps[i] != nullptr && fgHeaps[i]->active && fgHeaps[i]->gpuStart <= gpuHandle &&
            gpuHandle < fgHeaps[i]->gpuEnd)
        {
            cacheCR.genSeen = currentGen;
            cacheCR.heapPtr = fgHeaps[i].get();
            cacheCR.heapVersion = cacheCR.heapPtr->version;
            return cacheCR.heapPtr;
        }
    }

    cacheCR.heapVersion = 0;
    cacheCR.heapPtr = nullptr;
    return nullptr;
}

#pragma endregion

#pragma region Hudless methods

void ResTrack_Dx12::FillResourceInfo(ID3D12Resource* resource, ResourceInfo* info)
{
    auto desc = resource->GetDesc();
    info->buffer = resource;
    info->width = desc.Width;
    info->height = desc.Height;
    info->format = desc.Format;
    info->flags = desc.Flags;
}

bool ResTrack_Dx12::IsHudFixActive()
{
    auto config = Config::Instance();
    auto& state = State::Instance();
    const bool fgHudfixConfig = config->FGEnabled.value_or_default() && config->FGHUDFix.value_or_default();
    const bool fgHudfixActive = fgHudfixConfig &&
        state.currentFG != nullptr &&
        state.currentFeature != nullptr &&
        !state.FGchanged &&
        state.currentFG->IsActive();
    const bool frameWarpNoFgHudfixActive =
        config->FrameWarpEnabled.value_or_default() &&
        config->FGHUDFix.value_or_default() &&
        state.currentFeature != nullptr &&
        state.currentD3D12Device != nullptr &&
        state.currentSwapchain != nullptr &&
        !fgHudfixActive;

    if (!fgHudfixActive && !frameWarpNoFgHudfixActive)
    {
        LOG_TRACK("HUDFix inactive for FG and FrameWarp standalone");
        return false;
    }

    if (!_presentDone)
    {
        LOG_TRACK("!_presentDone");
        return false;
    }

    if (Hudfix_Dx12::SkipHudlessChecks())
    {
        LOG_TRACK("!Hudfix_Dx12::SkipHudlessChecks()");
        return false;
    }

    if (!Hudfix_Dx12::IsResourceCheckActive())
    {
        // LOG_TRACK("!Hudfix_Dx12::IsResourceCheckActive()");
        return false;
    }

    return true;
}

#pragma endregion

#pragma region Resource input hooks

void ResTrack_Dx12::hkCreateRenderTargetView(ID3D12Device* This, ID3D12Resource* pResource,
                                             D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                             D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    // force hdr for swapchain buffer
    if (pResource != nullptr && pDesc != nullptr && Config::Instance()->ForceHDR.value_or_default())
    {
        for (size_t i = 0; i < State::Instance().SCbuffers.size(); i++)
        {
            if (State::Instance().SCbuffers[i] == pResource)
            {
                if (Config::Instance()->UseHDR10.value_or_default())
                    pDesc->Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                else
                    pDesc->Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

                break;
            }
        }
    }

    o_CreateRenderTargetView(This, pResource, pDesc, DestDescriptor);

    if (Config::Instance()->FGHudfixDisableRTV.value_or_default())
        return;

    if (pResource == nullptr || pDesc == nullptr || pDesc->ViewDimension != D3D12_RTV_DIMENSION_TEXTURE2D ||
        !CheckResource(pResource))
    {
        auto heap = GetHeapByCpuHandleRTV(DestDescriptor.ptr);

        if (heap != nullptr)
            heap->ClearByCpuHandle(DestDescriptor.ptr);

        return;
    }

    // if (!CheckResource(pResource))
    //     return;

    auto heap = GetHeapByCpuHandleRTV(DestDescriptor.ptr);
    if (heap != nullptr)
    {
        ResourceInfo resInfo {};
        FillResourceInfo(pResource, &resInfo);
        resInfo.type = RTV;
        resInfo.captureInfo = CaptureInfo::CreateRTV;
        heap->SetByCpuHandle(DestDescriptor.ptr, resInfo);
    }
    // else
    //{
    //     LOG_TRACK("Heap not found for RTV: {:X}", DestDescriptor.ptr);
    // }
}

void ResTrack_Dx12::hkCreateShaderResourceView(ID3D12Device* This, ID3D12Resource* pResource,
                                               D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                               D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    // force hdr for swapchain buffer
    if (pResource != nullptr && pDesc != nullptr && Config::Instance()->ForceHDR.value_or_default())
    {
        for (size_t i = 0; i < State::Instance().SCbuffers.size(); i++)
        {
            if (State::Instance().SCbuffers[i] == pResource)
            {
                if (Config::Instance()->UseHDR10.value_or_default())
                    pDesc->Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                else
                    pDesc->Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

                break;
            }
        }
    }

    o_CreateShaderResourceView(This, pResource, pDesc, DestDescriptor);

    if (Config::Instance()->FGHudfixDisableSRV.value_or_default())
        return;

    if (pResource == nullptr || pDesc == nullptr || pDesc->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D ||
        !CheckResource(pResource))
    {
        auto heap = GetHeapByCpuHandleSRV(DestDescriptor.ptr);

        if (heap != nullptr)
            heap->ClearByCpuHandle(DestDescriptor.ptr);

        return;
    }

    // if (!CheckResource(pResource))
    //     return;

    auto heap = GetHeapByCpuHandleSRV(DestDescriptor.ptr);
    if (heap != nullptr)
    {
        ResourceInfo resInfo {};
        FillResourceInfo(pResource, &resInfo);
        resInfo.type = SRV;
        resInfo.captureInfo = CaptureInfo::CreateSRV;
        heap->SetByCpuHandle(DestDescriptor.ptr, resInfo);
    }
    // else
    //{
    //     LOG_TRACK("Heap not found for SRV: {:X}", DestDescriptor.ptr);
    // }
}

void ResTrack_Dx12::hkCreateUnorderedAccessView(ID3D12Device* This, ID3D12Resource* pResource,
                                                ID3D12Resource* pCounterResource,
                                                D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    if (pResource != nullptr && pDesc != nullptr && Config::Instance()->ForceHDR.value_or_default())
    {
        for (size_t i = 0; i < State::Instance().SCbuffers.size(); i++)
        {
            if (State::Instance().SCbuffers[i] == pResource)
            {
                if (Config::Instance()->UseHDR10.value_or_default())
                    pDesc->Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                else
                    pDesc->Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

                break;
            }
        }
    }

    o_CreateUnorderedAccessView(This, pResource, pCounterResource, pDesc, DestDescriptor);

    if (Config::Instance()->FGHudfixDisableUAV.value_or_default())
        return;

    if (pResource == nullptr || pDesc == nullptr || pDesc->ViewDimension != D3D12_UAV_DIMENSION_TEXTURE2D ||
        !CheckResource(pResource))
    {
        auto heap = GetHeapByCpuHandleUAV(DestDescriptor.ptr);

        if (heap != nullptr)
            heap->ClearByCpuHandle(DestDescriptor.ptr);

        return;
    }

    // if (!CheckResource(pResource))
    //     return;

    auto heap = GetHeapByCpuHandleUAV(DestDescriptor.ptr);
    if (heap != nullptr)
    {
        ResourceInfo resInfo {};
        FillResourceInfo(pResource, &resInfo);
        resInfo.type = UAV;
        resInfo.captureInfo = CaptureInfo::CreateUAV;
        heap->SetByCpuHandle(DestDescriptor.ptr, resInfo);
    }
    // else
    //{
    //     LOG_TRACK("Heap not found for UAV: {:X}", DestDescriptor.ptr);
    // }
}

#pragma endregion

void ResTrack_Dx12::hkExecuteCommandLists(ID3D12CommandQueue* This, UINT NumCommandLists,
                                           ID3D12CommandList* const* ppCommandLists)
{
    void* caller = _ReturnAddress();
    char callerModule[MAX_PATH] = {};
    const char* vf2CallerModule = VF2TraceModuleFromAddress(caller, callerModule);
    VF2LogQueueTrace("ExecuteCommandLists", This, NumCommandLists, ppCommandLists, nullptr, 0, S_OK, caller);

    auto fg = State::Instance().currentFG;

    if (fg != nullptr && fg->IsActive() && !fg->IsPaused())
    {
        LOG_TRACK("NumCommandLists: {}", NumCommandLists);

        std::vector<FG_ResourceType> found;
        auto fIndex = fg->GetIndex();

        do
        {
            std::lock_guard<std::mutex> lock2(_resourceCommandListMutex);

            if (!_notFoundCmdLists.empty())
            {
                for (size_t i = 0; i < NumCommandLists; i++)
                {
                    if (_notFoundCmdLists.contains(ppCommandLists[i]))
                    {
                        LOG_WARN("Found last frames cmdList: {:X}", (size_t) ppCommandLists[i]);
                        _notFoundCmdLists.erase(ppCommandLists[i]);
                    }
                }
            }

            if (_resCmdList[fIndex].empty())
                break;

            for (size_t i = 0; i < NumCommandLists; i++)
            {
                LOG_TRACK("ppCommandLists[{}]: {:X}", i, (size_t) ppCommandLists[i]);

                for (const auto& pair : _resCmdList[fIndex])
                {
                    if (pair.second == ppCommandLists[i])
                    {
                        LOG_DEBUG("found {} cmdList: {:X}, queue: {:X}", (UINT) pair.first, (size_t) pair.second,
                                  (size_t) This);
                        fg->SetResourceReady(pair.first);
                        found.push_back(pair.first);
                    }
                }

                for (size_t i = 0; i < found.size(); i++)
                {
                    _resCmdList[fIndex].erase(found[i]);
                }

                if (_resCmdList[fIndex].empty())
                    break;
            }

        } while (false);

        if (!found.empty())
        {
            auto vf2Candidate = VF2FlushCommandListResourceTrace(This, NumCommandLists, ppCommandLists, vf2CallerModule);
            o_ExecuteCommandLists(This, NumCommandLists, ppCommandLists);
            if (vf2Candidate.accepted)
            {
                bool applied = FrameWarpRuntime::ApplyToResourceAfterDLSSGCopy(
                    This, vf2Candidate.dst, vf2Candidate.dstState, vf2Candidate.src, vf2Candidate.reason);
                if (!applied)
                    State::Instance().frameWarpStatus.dlssgResourceWarpLastApplied = false;
            }

            for (size_t i = 0; i < found.size(); i++)
            {
                fg->SetCommandQueue(found[i], This);
            }

            return;
        }
    }

    LOG_TRACK("Done NumCommandLists: {}", NumCommandLists);

    auto vf2Candidate = VF2FlushCommandListResourceTrace(This, NumCommandLists, ppCommandLists, vf2CallerModule);
    o_ExecuteCommandLists(This, NumCommandLists, ppCommandLists);
    if (vf2Candidate.accepted)
    {
        bool applied = FrameWarpRuntime::ApplyToResourceAfterDLSSGCopy(
            This, vf2Candidate.dst, vf2Candidate.dstState, vf2Candidate.src, vf2Candidate.reason);
        if (!applied)
            State::Instance().frameWarpStatus.dlssgResourceWarpLastApplied = false;
    }
}

HRESULT ResTrack_Dx12::hkCommandQueueSignal(ID3D12CommandQueue* This, ID3D12Fence* pFence, UINT64 Value)
{
    void* caller = _ReturnAddress();
    HRESULT result = o_CommandQueueSignal(This, pFence, Value);
    VF2LogQueueTrace("Signal", This, 0, nullptr, pFence, Value, result, caller);
    return result;
}

HRESULT ResTrack_Dx12::hkCommandQueueWait(ID3D12CommandQueue* This, ID3D12Fence* pFence, UINT64 Value)
{
    void* caller = _ReturnAddress();
    HRESULT result = o_CommandQueueWait(This, pFence, Value);
    VF2LogQueueTrace("Wait", This, 0, nullptr, pFence, Value, result, caller);
    return result;
}

#pragma region Heap hooks

static ULONG STDMETHODCALLTYPE hkHeapRelease(ID3D12DescriptorHeap* This)
{
    if (State::Instance().isShuttingDown)
        return o_HeapRelease(This);

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        auto& up = fgHeaps[i];

        if (up == nullptr || up->heap != This || !up->active)
            continue;

        This->AddRef();
        if (o_HeapRelease(This) <= 1)
        {
#ifdef USE_SPINLOCK_MUTEX_FOR_HEAP_CREATION
            std::lock_guard<SpinLock> lock(_heapCreationMutex);
#else
            std::lock_guard<std::mutex> lock(_heapCreationMutex);
#endif

            up->active = false;

            LOG_INFO("Heap released: {:X}", (size_t) This);

            // detach all slots from _trackedResources
            {
                std::scoped_lock lk(_trackedResourcesMutex);

                for (UINT j = 0; j < up->numDescriptors; ++j)
                {
                    auto& slot = up->info[j];

                    if (slot.buffer == nullptr)
                        continue;

                    if (auto it = _trackedResources.find(slot.buffer); it != _trackedResources.end())
                    {
                        auto& vec = it->second;
                        vec.erase(std::remove(vec.begin(), vec.end(), &slot), vec.end());
                        if (vec.empty())
                            _trackedResources.erase(it);
                    }

                    slot.buffer = nullptr;
                    slot.lastUsedFrame = 0;
                }
            }

            gHeapGeneration.fetch_add(1, std::memory_order_release); // invalidate caches
        }

        break;
    }

    return o_HeapRelease(This);
}

HRESULT ResTrack_Dx12::hkCreateDescriptorHeap(ID3D12Device* This, D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
                                              REFIID riid, void** ppvHeap)
{
    auto result = o_CreateDescriptorHeap(This, pDescriptorHeapDesc, riid, ppvHeap);

    if (State::Instance().skipHeapCapture)
        return result;

    // try to calculate handle ranges for heap
    if (result == S_OK && (pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
                           pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV))
    {
        auto heap = (ID3D12DescriptorHeap*) (*ppvHeap);

        if (!o_HeapRelease)
        {
            PVOID* vtbl = *(PVOID**) heap;
            o_HeapRelease = (PFN_HeapRelease) vtbl[2];
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&) o_HeapRelease, hkHeapRelease);
            DetourTransactionCommit();
        }

        auto increment = This->GetDescriptorHandleIncrementSize(pDescriptorHeapDesc->Type);
        auto numDescriptors = pDescriptorHeapDesc->NumDescriptors;
        auto cpuStart = (SIZE_T) (heap->GetCPUDescriptorHandleForHeapStart().ptr);
        auto cpuEnd = cpuStart + (increment * numDescriptors);
        auto gpuStart = (SIZE_T) (heap->GetGPUDescriptorHandleForHeapStart().ptr);
        auto gpuEnd = gpuStart + (increment * numDescriptors);
        auto type = (UINT) pDescriptorHeapDesc->Type;

        LOG_TRACE("Heap: {:X}, Heap type: {}, Cpu: {}-{}, Gpu: {}-{}, Desc count: {}", (size_t) *ppvHeap, type,
                  cpuStart, cpuEnd, gpuStart, gpuEnd, numDescriptors);
        {
#ifdef USE_SPINLOCK_MUTEX_FOR_HEAP_CREATION
            std::lock_guard<SpinLock> lock(_heapCreationMutex);
#else
            std::lock_guard<std::mutex> lock(_heapCreationMutex);
#endif
            size_t count = fgHeaps.size();
            bool foundEmpty = false;
            for (size_t i = 0; i < count; i++)
            {
                if (fgHeaps[i] != nullptr && !fgHeaps[i]->active)
                {

                    fgHeaps[i].reset();
                    fgHeaps[i] = std::make_unique<HeapInfo>(heap, cpuStart, cpuEnd, gpuStart, gpuEnd, numDescriptors,
                                                            increment, type);

                    gHeapGeneration.fetch_add(1, std::memory_order_release);
                    foundEmpty = true;
                    LOG_DEBUG("Reusing empty heap slot: {}", i);
                    break;
                }
            }

            if (!foundEmpty)
            {
                // Reallocate vector if needed
                if (fgHeaps.capacity() == fgHeaps.size())
                    fgHeaps.reserve(fgHeaps.size() + 65536);

                fgHeaps.push_back(std::make_unique<HeapInfo>(heap, cpuStart, cpuEnd, gpuStart, gpuEnd, numDescriptors,
                                                             increment, type));

                gHeapGeneration.fetch_add(1, std::memory_order_release);
                LOG_DEBUG("Adding new heap slot: {}", fgHeaps.size() - 1);
            }
        }
    }
    else
    {
        if (ppvHeap != nullptr && *ppvHeap != nullptr)
        {
            auto heap = (ID3D12DescriptorHeap*) (*ppvHeap);
            LOG_TRACE("Skipping, Heap type: {}, Cpu: {}, Gpu: {}", (UINT) pDescriptorHeapDesc->Type,
                      heap->GetCPUDescriptorHandleForHeapStart().ptr, heap->GetGPUDescriptorHandleForHeapStart().ptr);
        }
    }

    return result;
}

ULONG ResTrack_Dx12::hkRelease(ID3D12Resource* This)
{
    if (State::Instance().isShuttingDown)
        return o_Release(This);

    std::vector<ResourceInfo*> toClean;
    {
        std::lock_guard lock(_trackedResourcesMutex);

        This->AddRef();
        auto refCount = o_Release(This);

        if (refCount <= 1 && _trackedResources.contains(This))
        {
            toClean = _trackedResources[This]; // Copy vector
            _trackedResources.erase(This);
            State::Instance().CapturedHudlesses.erase(This);
        }
    }

    // Clean up outside lock
    for (auto* info : toClean)
    {
        if (info->buffer == This)
        {
            info->buffer = nullptr;
            info->lastUsedFrame = 0;
        }
    }

    return o_Release(This);
}

void ResTrack_Dx12::hkCopyDescriptors(ID3D12Device* This, UINT NumDestDescriptorRanges,
                                      D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                                      UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
                                      D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
                                      UINT* pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
    o_CopyDescriptors(This, NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
                      NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType);

    // Early exit conditions - consistent validation
    if (DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
        return;

    if (NumDestDescriptorRanges == 0 || pDestDescriptorRangeStarts == nullptr)
        return;

    if (!Config::Instance()->FGAlwaysTrackHeaps.value_or_default() && !IsHudFixActive())
        return;

    const UINT inc = This->GetDescriptorHandleIncrementSize(DescriptorHeapsType);

    // Validate that we have source descriptors to copy
    bool haveSources = (NumSrcDescriptorRanges > 0 && pSrcDescriptorRangeStarts != nullptr);

    // Track positions in both source and destination ranges
    UINT srcRangeIndex = 0;
    UINT srcOffsetInRange = 0;
    UINT destRangeIndex = 0;
    UINT destOffsetInRange = 0;

    // Cache for heap lookups to avoid repeated lookups within the same range
    HeapInfo* cachedDestHeap = nullptr;
    SIZE_T cachedDestRangeStart = 0;
    UINT cachedDestRangeSize = 0;
    HeapInfo* cachedSrcHeap = nullptr;
    SIZE_T cachedSrcRangeStart = 0;
    UINT cachedSrcRangeSize = 0;

    // Process all destination descriptors
    while (destRangeIndex < NumDestDescriptorRanges)
    {
        // Update destination heap cache if we've moved to a new range
        if (destOffsetInRange == 0)
        {
            cachedDestRangeStart = pDestDescriptorRangeStarts[destRangeIndex].ptr;
            cachedDestRangeSize =
                (pDestDescriptorRangeSizes == nullptr) ? 1 : pDestDescriptorRangeSizes[destRangeIndex];
            cachedDestHeap = GetHeapByCpuHandle(cachedDestRangeStart);
        }

        // Calculate current destination handle
        const SIZE_T destHandle = cachedDestRangeStart + (static_cast<SIZE_T>(destOffsetInRange) * inc);

        // Get or update source information
        ResourceInfo* srcInfo = nullptr;
        if (haveSources && srcRangeIndex < NumSrcDescriptorRanges)
        {
            // Update source heap cache if we've moved to a new range
            if (srcOffsetInRange == 0)
            {
                cachedSrcRangeStart = pSrcDescriptorRangeStarts[srcRangeIndex].ptr;
                cachedSrcRangeSize =
                    (pSrcDescriptorRangeSizes == nullptr) ? 1 : pSrcDescriptorRangeSizes[srcRangeIndex];
                cachedSrcHeap = GetHeapByCpuHandle(cachedSrcRangeStart);
            }

            // Calculate current source handle
            const SIZE_T srcHandle = cachedSrcRangeStart + (static_cast<SIZE_T>(srcOffsetInRange) * inc);

            // Get source resource info with proper synchronization
            if (cachedSrcHeap != nullptr)
            {
                // Access to heap info is synchronized through HeapInfo's const methods
                // which use _trackedResourcesMutex internally
                srcInfo = cachedSrcHeap->GetByCpuHandle(srcHandle);
            }

            // Advance source position
            srcOffsetInRange++;
            if (srcOffsetInRange >= cachedSrcRangeSize)
            {
                srcOffsetInRange = 0;
                srcRangeIndex++;
            }
        }

        // Update destination heap tracking with proper synchronization
        if (cachedDestHeap != nullptr)
        {
            // HeapInfo's Set/Clear methods use _trackedResourcesMutex internally
            if (srcInfo != nullptr && srcInfo->buffer != nullptr)
                cachedDestHeap->SetByCpuHandle(destHandle, *srcInfo);
            else
                cachedDestHeap->ClearByCpuHandle(destHandle);
        }

        // Advance destination position
        destOffsetInRange++;
        if (destOffsetInRange >= cachedDestRangeSize)
        {
            destOffsetInRange = 0;
            destRangeIndex++;
        }
    }
}

void ResTrack_Dx12::hkCopyDescriptorsSimple(ID3D12Device* This, UINT NumDescriptors,
                                            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
                                            D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
                                            D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
    o_CopyDescriptorsSimple(This, NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart,
                            DescriptorHeapsType);

    if (DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
        return;

    if (!Config::Instance()->FGAlwaysTrackHeaps.value_or_default() && !IsHudFixActive())
        return;

    auto size = This->GetDescriptorHandleIncrementSize(DescriptorHeapsType);

    for (size_t i = 0; i < NumDescriptors; i++)
    {
        HeapInfo* srcHeap = nullptr;
        SIZE_T srcHandle = 0;

        // source
        if (SrcDescriptorRangeStart.ptr != 0)
        {
            srcHandle = SrcDescriptorRangeStart.ptr + i * size;
            srcHeap = GetHeapByCpuHandle(srcHandle);
        }

        auto destHandle = DestDescriptorRangeStart.ptr + i * size;
        auto dstHeap = GetHeapByCpuHandle(destHandle);

        // destination
        if (dstHeap == nullptr)
            continue;

        if (srcHeap == nullptr)
        {
            dstHeap->ClearByCpuHandle(destHandle);
            continue;
        }

        auto buffer = srcHeap->GetByCpuHandle(srcHandle);

        if (buffer == nullptr)
        {
            dstHeap->ClearByCpuHandle(destHandle);
            continue;
        }

        dstHeap->SetByCpuHandle(destHandle, *buffer);
    }
}

#pragma endregion

#pragma region Shader input hooks

void ResTrack_Dx12::hkSetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* This, UINT RootParameterIndex,
                                                     D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    void* caller = _ReturnAddress();
    if (BaseDescriptor.ptr != 0)
    {
        auto traceHeap = GetHeapByGpuHandleGR(BaseDescriptor.ptr);
        if (traceHeap != nullptr)
        {
            auto capturedBuffer = traceHeap->GetByGpuHandle(BaseDescriptor.ptr);
            VF2TraceDescriptor(This, "set-gr", capturedBuffer, caller);
        }
    }

    // Consistent early exit - always call original function
    auto shouldTrack = !Config::Instance()->FGHudfixDisableSGR.value_or_default() && BaseDescriptor.ptr != 0 &&
                       IsHudFixActive() && !Hudfix_Dx12::SkipHudlessChecks() &&
                       This != MenuOverlayDx::MenuCommandList();

    if (!shouldTrack)
    {
        o_SetGraphicsRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    auto heap = GetHeapByGpuHandleGR(BaseDescriptor.ptr);
    if (heap == nullptr)
    {
        LOG_DEBUG_ONLY("No heap for handle: {:X}", BaseDescriptor.ptr);
        o_SetGraphicsRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    auto capturedBuffer = heap->GetByGpuHandle(BaseDescriptor.ptr);
    if (capturedBuffer == nullptr || capturedBuffer->buffer == nullptr)
    {
        LOG_DEBUG_ONLY("No resource at RootParameterIndex: {}, CommandList: {:X}, gpuHandle: {:X}", RootParameterIndex,
                       (SIZE_T) This, BaseDescriptor.ptr);
        o_SetGraphicsRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    LOG_DEBUG_ONLY("CommandList: {:X}, Resource: {:X}", (size_t) This, (size_t) capturedBuffer->buffer);

    // Only proceed with tracking if we have a valid buffer
    capturedBuffer->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    capturedBuffer->captureInfo = CaptureInfo::SetGR;

    // Track the resource
    bool capturedImmediately = false;
    if (Config::Instance()->FGImmediateCapture.value_or_default())
    {
        capturedImmediately = Hudfix_Dx12::CheckForHudless(This, capturedBuffer, capturedBuffer->state);
    }

    if (!capturedImmediately)
    {
        auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

        if (!_useShards)
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (!fgPossibleHudless[fIndex].contains(This))
            {
                ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                newMap.reserve(32);
                fgPossibleHudless[fIndex].insert_or_assign(This, std::move(newMap));
            }

            LOG_TRACK("Tracking Resource: {:X}, Desc: {:X}", (size_t) capturedBuffer->buffer, BaseDescriptor.ptr);
            fgPossibleHudless[fIndex][This].insert_or_assign(capturedBuffer->buffer, *capturedBuffer);
        }
        else
        {
            size_t shardIdx = GetShardIndex(This);
            auto& shard = _hudlessShards[fIndex][shardIdx];

#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            if (!shard.map.contains(This))
            {
                ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                newMap.reserve(32);
                shard.map.insert_or_assign(This, std::move(newMap));
            }

            LOG_TRACK("CmdList: {:X}, Tracking Resource: {:X}, Desc: {:X}, Format: {}", (size_t) This,
                      (size_t) capturedBuffer->buffer, BaseDescriptor.ptr, (UINT) capturedBuffer->format);

            shard.map[This].insert_or_assign(capturedBuffer->buffer, *capturedBuffer);
        }
    }

    o_SetGraphicsRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
}

#pragma endregion

#pragma region Shader output hooks

void ResTrack_Dx12::hkOMSetRenderTargets(ID3D12GraphicsCommandList* This, UINT NumRenderTargetDescriptors,
                                         D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
                                         BOOL RTsSingleHandleToDescriptorRange,
                                         D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor)
{
    void* caller = _ReturnAddress();
    if (NumRenderTargetDescriptors > 0 && pRenderTargetDescriptors != nullptr)
    {
        for (UINT i = 0; i < NumRenderTargetDescriptors; i++)
        {
            HeapInfo* traceHeap = nullptr;
            D3D12_CPU_DESCRIPTOR_HANDLE handle {};

            if (RTsSingleHandleToDescriptorRange)
            {
                traceHeap = GetHeapByCpuHandleRTV(pRenderTargetDescriptors[0].ptr);
                if (traceHeap == nullptr)
                    continue;

                handle.ptr = pRenderTargetDescriptors[0].ptr + (i * traceHeap->increment);
            }
            else
            {
                handle = pRenderTargetDescriptors[i];
                traceHeap = GetHeapByCpuHandleRTV(handle.ptr);
                if (traceHeap == nullptr)
                    continue;
            }

            auto capturedBuffer = traceHeap->GetByCpuHandle(handle.ptr);
            VF2TraceDescriptor(This, "om-rtv", capturedBuffer, caller);
        }
    }

    // Consistent early exit validation
    auto shouldTrack = !Config::Instance()->FGHudfixDisableOM.value_or_default() && NumRenderTargetDescriptors > 0 &&
                       pRenderTargetDescriptors != nullptr && IsHudFixActive() && !Hudfix_Dx12::SkipHudlessChecks() &&
                       This != MenuOverlayDx::MenuCommandList();

    if (!shouldTrack)
    {
        o_OMSetRenderTargets(This, NumRenderTargetDescriptors, pRenderTargetDescriptors,
                             RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
        return;
    }

    LOG_DEBUG_ONLY("NumRenderTargetDescriptors: {}", NumRenderTargetDescriptors);

    auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    // Process render targets
    for (size_t i = 0; i < NumRenderTargetDescriptors; i++)
    {
        HeapInfo* heap = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE handle {};

        // Get the appropriate handle
        if (RTsSingleHandleToDescriptorRange)
        {
            heap = GetHeapByCpuHandleRTV(pRenderTargetDescriptors[0].ptr);
            if (heap == nullptr)
            {
                LOG_DEBUG_ONLY("No heap at index: {}", i);
                continue;
            }

            handle.ptr = pRenderTargetDescriptors[0].ptr + (i * heap->increment);
        }
        else
        {
            handle = pRenderTargetDescriptors[i];
            heap = GetHeapByCpuHandleRTV(handle.ptr);
            if (heap == nullptr)
            {
                LOG_DEBUG_ONLY("No heap at index: {}", i);
                continue;
            }
        }

        auto capturedBuffer = heap->GetByCpuHandle(handle.ptr);
        if (capturedBuffer == nullptr || capturedBuffer->buffer == nullptr)
        {
            LOG_DEBUG_ONLY("No resource at index: {}, cpu: {:X}", i, handle.ptr);
            continue;
        }

        // Valid resource found, update state
        capturedBuffer->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        capturedBuffer->captureInfo = CaptureInfo::OMSetRTV;

        // Check for immediate capture
        bool capturedImmediately = false;
        if (Config::Instance()->FGImmediateCapture.value_or_default())
        {
            capturedImmediately = Hudfix_Dx12::CheckForHudless(This, capturedBuffer, capturedBuffer->state);
            if (capturedImmediately)
                break; // Early exit if captured
        }

        // Track for later processing
        if (!capturedImmediately)
        {
            if (!_useShards)
            {
                std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

                if (!fgPossibleHudless[fIndex].contains(This))
                {
                    ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                    newMap.reserve(32);
                    fgPossibleHudless[fIndex].insert_or_assign(This, std::move(newMap));
                }

                LOG_TRACK("Tracking Resource: {:X}, Desc: {:X}", (size_t) capturedBuffer->buffer, handle.ptr);
                fgPossibleHudless[fIndex][This].insert_or_assign(capturedBuffer->buffer, *capturedBuffer);
            }
            else
            {
                size_t shardIdx = GetShardIndex(This);
                auto& shard = _hudlessShards[fIndex][shardIdx];

#ifdef USE_SPINLOCK_MUTEX
                std::lock_guard<SpinLock> lock(shard.mutex);
#else
                std::lock_guard<std::mutex> lock(shard.mutex);
#endif

                if (!shard.map.contains(This))
                {
                    ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                    newMap.reserve(32);
                    shard.map.insert_or_assign(This, std::move(newMap));
                }

                LOG_TRACK("CmdList: {:X}, Tracking Resource: {:X}, Desc: {:X}, Format: {}", (size_t) This,
                          (size_t) capturedBuffer->buffer, handle.ptr, (UINT) capturedBuffer->format);

                shard.map[This].insert_or_assign(capturedBuffer->buffer, *capturedBuffer);
            }
        }
    }

    o_OMSetRenderTargets(This, NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange,
                         pDepthStencilDescriptor);
}

#pragma endregion

#pragma region Compute paramter hooks

void ResTrack_Dx12::hkSetComputeRootDescriptorTable(ID3D12GraphicsCommandList* This, UINT RootParameterIndex,
                                                    D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    void* caller = _ReturnAddress();
    if (BaseDescriptor.ptr != 0)
    {
        auto traceHeap = GetHeapByGpuHandleCR(BaseDescriptor.ptr);
        if (traceHeap != nullptr)
        {
            auto capturedBuffer = traceHeap->GetByGpuHandle(BaseDescriptor.ptr);
            VF2TraceDescriptor(This, "set-cr", capturedBuffer, caller);
        }
    }

    // Consistent early exit - always call original function
    auto shouldTrack = !Config::Instance()->FGHudfixDisableSCR.value_or_default() && BaseDescriptor.ptr != 0 &&
                       IsHudFixActive() && !Hudfix_Dx12::SkipHudlessChecks() &&
                       This != MenuOverlayDx::MenuCommandList();

    if (!shouldTrack)
    {
        o_SetComputeRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    auto heap = GetHeapByGpuHandleCR(BaseDescriptor.ptr);
    if (heap == nullptr)
    {
        LOG_DEBUG_ONLY("No heap for handle: {:X}", BaseDescriptor.ptr);
        o_SetComputeRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    auto capturedBuffer = heap->GetByGpuHandle(BaseDescriptor.ptr);
    if (capturedBuffer == nullptr || capturedBuffer->buffer == nullptr)
    {
        LOG_DEBUG_ONLY("No resource at RootParameterIndex: {}, CommandList: {:X}, gpuHandle: {:X}", RootParameterIndex,
                       (SIZE_T) This, BaseDescriptor.ptr);
        o_SetComputeRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    LOG_DEBUG_ONLY("CommandList: {:X}, Resource: {:X}", (size_t) This, (size_t) capturedBuffer->buffer);

    // Only proceed with tracking if we have a valid buffer
    if (capturedBuffer->type == UAV)
        capturedBuffer->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    else
        capturedBuffer->state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    capturedBuffer->captureInfo = CaptureInfo::SetCR;

    // Track the resource
    bool capturedImmediately = false;
    if (Config::Instance()->FGImmediateCapture.value_or_default())
    {
        capturedImmediately = Hudfix_Dx12::CheckForHudless(This, capturedBuffer, capturedBuffer->state);
    }

    if (!capturedImmediately)
    {
        auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

        if (!_useShards)
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (!fgPossibleHudless[fIndex].contains(This))
            {
                ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                newMap.reserve(32);
                fgPossibleHudless[fIndex].insert_or_assign(This, std::move(newMap));
            }

            LOG_TRACK("Tracking Resource: {:X}, Desc: {:X}", (size_t) capturedBuffer->buffer, BaseDescriptor.ptr);
            fgPossibleHudless[fIndex][This].insert_or_assign(capturedBuffer->buffer, *capturedBuffer);
        }
        else
        {
            size_t shardIdx = GetShardIndex(This);
            auto& shard = _hudlessShards[fIndex][shardIdx];

#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            if (!shard.map.contains(This))
            {
                ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                newMap.reserve(32);
                shard.map.insert_or_assign(This, std::move(newMap));
            }

            LOG_TRACK("CmdList: {:X}, Tracking Resource: {:X}, Desc: {:X}, Format: {}", (size_t) This,
                      (size_t) capturedBuffer->buffer, BaseDescriptor.ptr, (UINT) capturedBuffer->format);

            shard.map[This].insert_or_assign(capturedBuffer->buffer, *capturedBuffer);
        }
    }

    o_SetComputeRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
}

#pragma endregion

#pragma region Command list resource trace hooks

HRESULT ResTrack_Dx12::hkReset(ID3D12GraphicsCommandList* This, ID3D12CommandAllocator* pAllocator,
                               ID3D12PipelineState* pInitialState)
{
    if (This != nullptr)
    {
        std::lock_guard<std::mutex> lock(g_vf2CommandListTraceMutex);
        g_vf2CommandListTraces.erase(This);
    }

    return o_ResetCommandList(This, pAllocator, pInitialState);
}

void ResTrack_Dx12::hkResourceBarrier(ID3D12GraphicsCommandList* This, UINT NumBarriers,
                                      const D3D12_RESOURCE_BARRIER* pBarriers)
{
    void* caller = _ReturnAddress();
    if (pBarriers != nullptr)
    {
        for (UINT i = 0; i < NumBarriers; i++)
        {
            const auto& barrier = pBarriers[i];
            switch (barrier.Type)
            {
                case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
                {
                    const auto before = barrier.Transition.StateBefore;
                    const auto after = barrier.Transition.StateAfter;
                    VF2TraceCommandListResource(This,
                                                "barrier",
                                                barrier.Transition.pResource,
                                                VF2TraceIsReadState(before) || VF2TraceIsReadState(after),
                                                VF2TraceIsWriteState(before) || VF2TraceIsWriteState(after),
                                                true,
                                                false,
                                                false,
                                                false,
                                                false,
                                                before,
                                                after,
                                                caller);
                    break;
                }
                case D3D12_RESOURCE_BARRIER_TYPE_UAV:
                    VF2TraceCommandListResource(This,
                                                "uav-barrier",
                                                barrier.UAV.pResource,
                                                true,
                                                true,
                                                true,
                                                false,
                                                false,
                                                false,
                                                true,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                caller);
                    break;
                case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
                    VF2TraceCommandListResource(This,
                                                "alias-before",
                                                barrier.Aliasing.pResourceBefore,
                                                false,
                                                false,
                                                true,
                                                false,
                                                false,
                                                false,
                                                false,
                                                D3D12_RESOURCE_STATE_COMMON,
                                                D3D12_RESOURCE_STATE_COMMON,
                                                caller);
                    VF2TraceCommandListResource(This,
                                                "alias-after",
                                                barrier.Aliasing.pResourceAfter,
                                                false,
                                                false,
                                                true,
                                                false,
                                                false,
                                                false,
                                                false,
                                                D3D12_RESOURCE_STATE_COMMON,
                                                D3D12_RESOURCE_STATE_COMMON,
                                                caller);
                    break;
                default:
                    break;
            }
        }
    }

    o_ResourceBarrier(This, NumBarriers, pBarriers);
}

void ResTrack_Dx12::hkCopyResource(ID3D12GraphicsCommandList* This, ID3D12Resource* pDstResource,
                                   ID3D12Resource* pSrcResource)
{
    void* caller = _ReturnAddress();
    VF2TraceCommandListResource(This,
                                "copy-dst",
                                pDstResource,
                                false,
                                true,
                                false,
                                true,
                                false,
                                false,
                                false,
                                D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_STATE_COPY_DEST,
                                caller);
    VF2TraceCommandListResource(This,
                                "copy-src",
                                pSrcResource,
                                true,
                                false,
                                false,
                                true,
                                false,
                                false,
                                false,
                                D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_STATE_COPY_SOURCE,
                                caller);

    o_CopyResource(This, pDstResource, pSrcResource);
}

void ResTrack_Dx12::hkCopyTextureRegion(ID3D12GraphicsCommandList* This,
                                        const D3D12_TEXTURE_COPY_LOCATION* pDst,
                                        UINT DstX,
                                        UINT DstY,
                                        UINT DstZ,
                                        const D3D12_TEXTURE_COPY_LOCATION* pSrc,
                                        const D3D12_BOX* pSrcBox)
{
    void* caller = _ReturnAddress();
    VF2TraceCommandListResource(This,
                                "copytex-dst",
                                pDst != nullptr ? pDst->pResource : nullptr,
                                false,
                                true,
                                false,
                                true,
                                false,
                                false,
                                false,
                                D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_STATE_COPY_DEST,
                                caller);
    VF2TraceCommandListResource(This,
                                "copytex-src",
                                pSrc != nullptr ? pSrc->pResource : nullptr,
                                true,
                                false,
                                false,
                                true,
                                false,
                                false,
                                false,
                                D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_STATE_COPY_SOURCE,
                                caller);

    o_CopyTextureRegion(This, pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
}

#pragma endregion

#pragma region Shader finalizer hooks

// Capture if render target matches, wait for DrawIndexed
void ResTrack_Dx12::hkDrawInstanced(ID3D12GraphicsCommandList* This, UINT VertexCountPerInstance, UINT InstanceCount,
                                    UINT StartVertexLocation, UINT StartInstanceLocation)
{
    o_DrawInstanced(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);

    if (!IsHudFixActive())
    {
        LOG_TRACK("Skipping {:X}", (size_t) This);
        return;
    }

    LOG_TRACK("CmdList: {:X}", (size_t) This);

    auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    if (!_useShards)
    {
        if (This == MenuOverlayDx::MenuCommandList())
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);
            fgPossibleHudless[fIndex].erase(This);
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (fgPossibleHudless[fIndex].size() == 0 || !fgPossibleHudless[fIndex].contains(This))
                return;

            val0 = std::move(fgPossibleHudless[fIndex][This]);
            fgPossibleHudless[fIndex].erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDI.value_or_default())
                break;

            std::lock_guard<std::mutex> lock(_drawMutex);
            for (auto& [key, val] : val0)
            {
                val.captureInfo |= CaptureInfo::DrawInstanced;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }

        } while (false);
    }
    else
    {
        size_t shardIdx = GetShardIndex(This);
        auto& shard = _hudlessShards[fIndex][shardIdx];

        if (This == MenuOverlayDx::MenuCommandList() && shard.map.contains(This))
        {
#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            shard.map.erase(This);
            return;
        }

        // if can't find output skip
        if (shard.map.size() == 0)
        {
            LOG_DEBUG_ONLY("Early exit");
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {

#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            if (!shard.map.contains(This))
                return;

            val0 = std::move(shard.map[This]);
            shard.map.erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDI.value_or_default())
                break;

            for (auto& [key, val] : val0)
            {
                std::lock_guard<std::mutex> lock(_drawMutex);

                val.captureInfo |= CaptureInfo::DrawInstanced;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }

        } while (false);
    }
}

void ResTrack_Dx12::hkDrawIndexedInstanced(ID3D12GraphicsCommandList* This, UINT IndexCountPerInstance,
                                           UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation,
                                           UINT StartInstanceLocation)
{
    o_DrawIndexedInstanced(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                           StartInstanceLocation);

    if (!IsHudFixActive())
    {
        LOG_TRACK("Skipping CmdList: {:X}", (size_t) This);
        return;
    }

    LOG_TRACK("CmdList: {:X}", (size_t) This);

    auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    if (!_useShards)
    {
        if (This == MenuOverlayDx::MenuCommandList())
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);
            fgPossibleHudless[fIndex].erase(This);
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (fgPossibleHudless[fIndex].size() == 0 || !fgPossibleHudless[fIndex].contains(This))
                return;

            val0 = std::move(fgPossibleHudless[fIndex][This]);
            fgPossibleHudless[fIndex].erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDII.value_or_default())
                break;

            std::lock_guard<std::mutex> lock(_drawMutex);
            for (auto& [key, val] : val0)
            {
                val.captureInfo |= CaptureInfo::DrawIndexedInstanced;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }

        } while (false);
    }
    else
    {
        size_t shardIdx = GetShardIndex(This);
        auto& shard = _hudlessShards[fIndex][shardIdx];

        if (This == MenuOverlayDx::MenuCommandList() && shard.map.contains(This))
        {
#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            shard.map.erase(This);
            return;
        }

        // if can't find output skip
        if (shard.map.size() == 0)
        {
            LOG_DEBUG_ONLY("Early exit");
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {

#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            if (!shard.map.contains(This))
                return;

            val0 = std::move(shard.map[This]);
            shard.map.erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDII.value_or_default())
                break;

            for (auto& [key, val] : val0)
            {
                // LOG_DEBUG("Waiting _drawMutex {:X}", (size_t)val.buffer);
                std::lock_guard<std::mutex> lock(_drawMutex);

                val.captureInfo |= CaptureInfo::DrawIndexedInstanced;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }

        } while (false);
    }
}

void ResTrack_Dx12::hkExecuteBundle(ID3D12GraphicsCommandList* This, ID3D12GraphicsCommandList* pCommandList)
{
    LOG_FUNC();

    IFGFeature_Dx12* fg = State::Instance().currentFG;
    auto index = fg != nullptr ? fg->GetIndex() : 0;

    {
        std::lock_guard<std::mutex> lock(_resourceCommandListMutex);

        if (fg != nullptr && fg->IsActive() && (_resourceCommandList[index].size() > 0 || !_resCmdList[index].empty()))
        {
            if (_notFoundCmdLists.contains(pCommandList))
                LOG_WARN("Found last frames cmdList: {:X}", (size_t) This);

            auto& frameCmdList = _resourceCommandList[index];
            for (std::unordered_map<FG_ResourceType, ID3D12GraphicsCommandList*>::iterator it = frameCmdList.begin();
                 it != frameCmdList.end(); ++it)
            {
                if (it->second == pCommandList)
                    it->second = This;
            }

            for (std::unordered_map<FG_ResourceType, void*>::iterator it = _resCmdList[index].begin();
                 it != _resCmdList[index].end(); ++it)
            {
                if (it->second == pCommandList)
                    it->second = This;
            }
        }
    }

    o_ExecuteBundle(This, pCommandList);
}

HRESULT ResTrack_Dx12::hkClose(ID3D12GraphicsCommandList* This)
{
    auto fg = State::Instance().currentFG;
    auto index = fg != nullptr ? fg->GetIndex() : 0;

    if (fg != nullptr && fg->IsActive() && !fg->IsPaused() && _resourceCommandList[index].size() > 0)
    {
        LOG_TRACK("CmdList: {:X}", (size_t) This);

        std::lock_guard<std::mutex> lock(_resourceCommandListMutex);

        if (_notFoundCmdLists.contains(This))
            LOG_WARN("Found last frames cmdList: {:X}", (size_t) This);

        std::vector<FG_ResourceType> found;

        for (const auto& pair : _resourceCommandList[index])
        {
            if (This == pair.second)
            {
                if (!fg->IsResourceReady(pair.first))
                {
                    LOG_DEBUG("{} cmdList: {:X}", (UINT) pair.first, (size_t) This);
                    _resCmdList[index][pair.first] = pair.second;
                    found.push_back(pair.first);
                }
            }
        }

        for (size_t i = 0; i < found.size(); i++)
        {
            _resourceCommandList[index].erase(found[i]);
        }
    }

    return o_Close(This);
}

void ResTrack_Dx12::hkDispatch(ID3D12GraphicsCommandList* This, UINT ThreadGroupCountX, UINT ThreadGroupCountY,
                               UINT ThreadGroupCountZ)
{
    o_Dispatch(This, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);

    if (!IsHudFixActive())
    {
        LOG_TRACK("Skipping {:X}", (size_t) This);
        return;
    }

    LOG_TRACK("CmdList: {:X}", (size_t) This);

    auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    if (!_useShards)
    {
        if (This == MenuOverlayDx::MenuCommandList())
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);
            fgPossibleHudless[fIndex].erase(This);
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (fgPossibleHudless[fIndex].size() == 0 || !fgPossibleHudless[fIndex].contains(This))
                return;

            val0 = std::move(fgPossibleHudless[fIndex][This]);
            fgPossibleHudless[fIndex].erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDispatch.value_or_default())
                break;

            std::lock_guard<std::mutex> lock(_drawMutex);
            for (auto& [key, val] : val0)
            {
                val.captureInfo |= CaptureInfo::Dispatch;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }
        } while (false);
    }
    else
    {
        size_t shardIdx = GetShardIndex(This);
        auto& shard = _hudlessShards[fIndex][shardIdx];

        if (This == MenuOverlayDx::MenuCommandList() && shard.map.contains(This))
        {
#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            shard.map.erase(This);
            return;
        }

        // if can't find output skip
        if (shard.map.size() == 0)
        {
            LOG_DEBUG_ONLY("Early exit");
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {

#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            if (!shard.map.contains(This))
                return;

            val0 = std::move(shard.map[This]);
            shard.map.erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDispatch.value_or_default())
                break;

            for (auto& [key, val] : val0)
            {
                // LOG_DEBUG("Waiting _drawMutex {:X}", (size_t)val.buffer);
                std::lock_guard<std::mutex> lock(_drawMutex);

                val.captureInfo |= CaptureInfo::Dispatch;
                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                {
                    break;
                }
            }
        } while (false);
    }
}

#pragma endregion

void ResTrack_Dx12::HookResource(ID3D12Device* InDevice)
{
    if (o_Release != nullptr)
        return;

    ID3D12Resource* tmp = nullptr;
    auto d = CD3DX12_RESOURCE_DESC::Buffer(4);
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    HRESULT hr = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &d,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tmp));

    if (hr == S_OK)
    {
        PVOID* pVTable = *(PVOID**) tmp;
        o_Release = (PFN_Release) pVTable[2];

        if (o_Release != nullptr)
        {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&) o_Release, hkRelease);
            DetourTransactionCommit();

            o_Release(tmp); // drop temp
        }
        else
        {
            tmp->Release();
        }
    }
}

void ResTrack_Dx12::HookCommandList(ID3D12Device* InDevice)
{

    if (o_OMSetRenderTargets != nullptr)
        return;

    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12CommandAllocator* commandAllocator = nullptr;

    if (InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)) == S_OK)
    {
        if (InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, nullptr,
                                        IID_PPV_ARGS(&commandList)) == S_OK)
        {
            ID3D12GraphicsCommandList* realCL = nullptr;
            if (!CheckForRealObject(__FUNCTION__, commandList, (IUnknown**) &realCL))
                realCL = commandList;

            // Get the vtable pointer
            PVOID* pVTable = *(PVOID**) realCL;

            // hudless shader
            o_OMSetRenderTargets = (PFN_OMSetRenderTargets) pVTable[46];
            o_SetGraphicsRootDescriptorTable = (PFN_SetGraphicsRootDescriptorTable) pVTable[32];

            o_DrawInstanced = (PFN_DrawInstanced) pVTable[12];
            o_DrawIndexedInstanced = (PFN_DrawIndexedInstanced) pVTable[13];
            o_Dispatch = (PFN_Dispatch) pVTable[14];
            o_Close = (PFN_Close) pVTable[9];
            o_ResetCommandList = (PFN_ResetCommandList) pVTable[10];
            o_CopyTextureRegion = (PFN_CopyTextureRegion) pVTable[16];
            o_CopyResource = (PFN_CopyResource) pVTable[17];
            o_ResourceBarrier = (PFN_ResourceBarrier) pVTable[26];

            // hudless compute
            o_SetComputeRootDescriptorTable = (PFN_SetComputeRootDescriptorTable) pVTable[31];

            o_ExecuteBundle = (PFN_ExecuteBundle) pVTable[27];

            if (o_OMSetRenderTargets != nullptr)
            {
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());

                if (o_OMSetRenderTargets != nullptr)
                    DetourAttach(&(PVOID&) o_OMSetRenderTargets, hkOMSetRenderTargets);

                if (o_SetGraphicsRootDescriptorTable != nullptr)
                    DetourAttach(&(PVOID&) o_SetGraphicsRootDescriptorTable, hkSetGraphicsRootDescriptorTable);

                if (o_SetComputeRootDescriptorTable != nullptr)
                    DetourAttach(&(PVOID&) o_SetComputeRootDescriptorTable, hkSetComputeRootDescriptorTable);

                if (o_ResetCommandList != nullptr)
                    DetourAttach(&(PVOID&) o_ResetCommandList, hkReset);

                if (o_ResourceBarrier != nullptr)
                    DetourAttach(&(PVOID&) o_ResourceBarrier, hkResourceBarrier);

                if (o_CopyResource != nullptr)
                    DetourAttach(&(PVOID&) o_CopyResource, hkCopyResource);

                if (o_CopyTextureRegion != nullptr)
                    DetourAttach(&(PVOID&) o_CopyTextureRegion, hkCopyTextureRegion);

                // Only needed for hudfix
                if (State::Instance().activeFgInput == FGInput::Upscaler)
                {
                    if (o_DrawIndexedInstanced != nullptr)
                        DetourAttach(&(PVOID&) o_DrawIndexedInstanced, hkDrawIndexedInstanced);

                    if (o_DrawInstanced != nullptr)
                        DetourAttach(&(PVOID&) o_DrawInstanced, hkDrawInstanced);

                    if (o_Dispatch != nullptr)
                        DetourAttach(&(PVOID&) o_Dispatch, hkDispatch);
                }

                if (o_Close != nullptr)
                    DetourAttach(&(PVOID&) o_Close, hkClose);

                if (o_ExecuteBundle != nullptr)
                    DetourAttach(&(PVOID&) o_ExecuteBundle, hkExecuteBundle);

                DetourTransactionCommit();
            }

            commandList->Close();
            commandList->Release();
        }

        commandAllocator->Reset();
        commandAllocator->Release();
    }
}

void ResTrack_Dx12::HookToQueue(ID3D12Device* InDevice)
{
    if (o_ExecuteCommandLists != nullptr)
        return;

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

    auto hr = InDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));

    if (hr == S_OK)
    {
        ID3D12CommandQueue* realQueue = nullptr;
        if (!CheckForRealObject(__FUNCTION__, queue, (IUnknown**) &realQueue))
            realQueue = queue;

        // Get the vtable pointer
        PVOID* pVTable = *(PVOID**) realQueue;

        o_ExecuteCommandLists = (PFN_ExecuteCommandLists) pVTable[10];
        o_CommandQueueSignal = (PFN_CommandQueueSignal) pVTable[14];
        o_CommandQueueWait = (PFN_CommandQueueWait) pVTable[15];

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (o_ExecuteCommandLists != nullptr)
            DetourAttach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);
        if (o_CommandQueueSignal != nullptr)
            DetourAttach(&(PVOID&) o_CommandQueueSignal, hkCommandQueueSignal);
        if (o_CommandQueueWait != nullptr)
            DetourAttach(&(PVOID&) o_CommandQueueWait, hkCommandQueueWait);

        DetourTransactionCommit();

        queue->Release();
    }
}

void ResTrack_Dx12::HookDevice(ID3D12Device* device)
{
    if (o_CreateDescriptorHeap != nullptr || State::Instance().activeFgInput == FGInput::Nukems)
        return;

    if (device == nullptr)
        return;

    if (fgHeaps.capacity() < 65536)
    {
        _useShards = Config::Instance()->FGUseShards.value_or_default();
        _trackedResources.reserve(1024);
        fgHeaps.reserve(65536);
    }

    LOG_FUNC();

    ID3D12Device* realDevice = nullptr;
    if (!CheckForRealObject(__FUNCTION__, device, (IUnknown**) &realDevice))
        realDevice = device;

    // Get the vtable pointer
    PVOID* pVTable = *(PVOID**) realDevice;

    // Hudfix
    o_CreateDescriptorHeap = (PFN_CreateDescriptorHeap) pVTable[14];
    o_CreateShaderResourceView = (PFN_CreateShaderResourceView) pVTable[18];
    o_CreateUnorderedAccessView = (PFN_CreateUnorderedAccessView) pVTable[19];
    o_CreateRenderTargetView = (PFN_CreateRenderTargetView) pVTable[20];
    o_CreateSampler = (PFN_CreateSampler) pVTable[22];
    o_CopyDescriptors = (PFN_CopyDescriptors) pVTable[23];
    o_CopyDescriptorsSimple = (PFN_CopyDescriptorsSimple) pVTable[24];

    // o_CreateDepthStencilView = (PFN_CreateDepthStencilView) pVTable[21];
    // o_CreateConstantBufferView = (PFN_CreateConstantBufferView) pVTable[17];

    // Apply the detour

    if (o_CreateDescriptorHeap != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (o_CreateDescriptorHeap != nullptr)
            DetourAttach(&(PVOID&) o_CreateDescriptorHeap, hkCreateDescriptorHeap);

        if (o_CreateRenderTargetView != nullptr)
            DetourAttach(&(PVOID&) o_CreateRenderTargetView, hkCreateRenderTargetView);

        if (o_CreateShaderResourceView != nullptr)
            DetourAttach(&(PVOID&) o_CreateShaderResourceView, hkCreateShaderResourceView);

        if (o_CreateUnorderedAccessView != nullptr)
            DetourAttach(&(PVOID&) o_CreateUnorderedAccessView, hkCreateUnorderedAccessView);

        if (o_CopyDescriptors != nullptr)
            DetourAttach(&(PVOID&) o_CopyDescriptors, hkCopyDescriptors);

        if (o_CopyDescriptorsSimple != nullptr)
            DetourAttach(&(PVOID&) o_CopyDescriptorsSimple, hkCopyDescriptorsSimple);

        DetourTransactionCommit();
    }

    HookToQueue(device);
    HookCommandList(device);
    HookResource(device);
}

void ResTrack_Dx12::ReleaseDeviceHooks()
{
    LOG_DEBUG("");

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_CreateDescriptorHeap != nullptr)
        DetourDetach(&(PVOID&) o_CreateDescriptorHeap, hkCreateDescriptorHeap);

    if (o_CreateRenderTargetView != nullptr)
        DetourDetach(&(PVOID&) o_CreateRenderTargetView, hkCreateRenderTargetView);

    if (o_CreateShaderResourceView != nullptr)
        DetourDetach(&(PVOID&) o_CreateShaderResourceView, hkCreateShaderResourceView);

    if (o_CreateUnorderedAccessView != nullptr)
        DetourDetach(&(PVOID&) o_CreateUnorderedAccessView, hkCreateUnorderedAccessView);

    if (o_CopyDescriptors != nullptr)
        DetourDetach(&(PVOID&) o_CopyDescriptors, hkCopyDescriptors);

    if (o_CopyDescriptorsSimple != nullptr)
        DetourDetach(&(PVOID&) o_CopyDescriptorsSimple, hkCopyDescriptorsSimple);

    // Queue
    if (o_ExecuteCommandLists != nullptr)
        DetourDetach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);
    if (o_CommandQueueSignal != nullptr)
        DetourDetach(&(PVOID&) o_CommandQueueSignal, hkCommandQueueSignal);
    if (o_CommandQueueWait != nullptr)
        DetourDetach(&(PVOID&) o_CommandQueueWait, hkCommandQueueWait);

    // CommandList
    if (o_OMSetRenderTargets != nullptr)
        DetourDetach(&(PVOID&) o_OMSetRenderTargets, hkOMSetRenderTargets);

    if (o_SetGraphicsRootDescriptorTable != nullptr)
        DetourDetach(&(PVOID&) o_SetGraphicsRootDescriptorTable, hkSetGraphicsRootDescriptorTable);

    if (o_SetComputeRootDescriptorTable != nullptr)
        DetourDetach(&(PVOID&) o_SetComputeRootDescriptorTable, hkSetComputeRootDescriptorTable);

    if (o_ResetCommandList != nullptr)
        DetourDetach(&(PVOID&) o_ResetCommandList, hkReset);

    if (o_ResourceBarrier != nullptr)
        DetourDetach(&(PVOID&) o_ResourceBarrier, hkResourceBarrier);

    if (o_CopyResource != nullptr)
        DetourDetach(&(PVOID&) o_CopyResource, hkCopyResource);

    if (o_CopyTextureRegion != nullptr)
        DetourDetach(&(PVOID&) o_CopyTextureRegion, hkCopyTextureRegion);

    if (o_DrawIndexedInstanced != nullptr)
        DetourDetach(&(PVOID&) o_DrawIndexedInstanced, hkDrawIndexedInstanced);

    if (o_DrawInstanced != nullptr)
        DetourDetach(&(PVOID&) o_DrawInstanced, hkDrawInstanced);

    if (o_Dispatch != nullptr)
        DetourDetach(&(PVOID&) o_Dispatch, hkDispatch);

    if (o_Close != nullptr)
        DetourDetach(&(PVOID&) o_Close, hkClose);

    if (o_ExecuteBundle != nullptr)
        DetourDetach(&(PVOID&) o_ExecuteBundle, hkExecuteBundle);

    // Resource
    if (o_Release != nullptr)
        DetourDetach(&(PVOID&) o_Release, hkRelease);

    DetourTransactionCommit();

    // Device
    o_CreateDescriptorHeap = nullptr;
    o_CreateRenderTargetView = nullptr;
    o_CreateShaderResourceView = nullptr;
    o_CreateUnorderedAccessView = nullptr;
    o_CopyDescriptors = nullptr;
    o_CopyDescriptorsSimple = nullptr;

    // Queue
    o_ExecuteCommandLists = nullptr;
    o_CommandQueueSignal = nullptr;
    o_CommandQueueWait = nullptr;

    // CommandList
    o_OMSetRenderTargets = nullptr;
    o_SetGraphicsRootDescriptorTable = nullptr;
    o_SetComputeRootDescriptorTable = nullptr;
    o_ResetCommandList = nullptr;
    o_ResourceBarrier = nullptr;
    o_CopyResource = nullptr;
    o_CopyTextureRegion = nullptr;
    o_DrawIndexedInstanced = nullptr;
    o_DrawInstanced = nullptr;
    o_Dispatch = nullptr;
    o_Close = nullptr;
    o_ExecuteBundle = nullptr;

    // Resource
    o_Release = nullptr;
}

void ResTrack_Dx12::ReleaseHooks()
{
    LOG_DEBUG("");

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    // if (o_CreateDescriptorHeap != nullptr)
    //     DetourDetach(&(PVOID&) o_CreateDescriptorHeap, hkCreateDescriptorHeap);

    // if (o_CreateRenderTargetView != nullptr)
    //     DetourDetach(&(PVOID&) o_CreateRenderTargetView, hkCreateRenderTargetView);

    // if (o_CreateShaderResourceView != nullptr)
    //     DetourDetach(&(PVOID&) o_CreateShaderResourceView, hkCreateShaderResourceView);

    // if (o_CreateUnorderedAccessView != nullptr)
    //     DetourDetach(&(PVOID&) o_CreateUnorderedAccessView, hkCreateUnorderedAccessView);

    // if (o_CopyDescriptors != nullptr)
    //     DetourDetach(&(PVOID&) o_CopyDescriptors, hkCopyDescriptors);

    // if (o_CopyDescriptorsSimple != nullptr)
    //     DetourDetach(&(PVOID&) o_CopyDescriptorsSimple, hkCopyDescriptorsSimple);

    // o_CreateDescriptorHeap = nullptr;
    // o_CreateRenderTargetView = nullptr;
    // o_CreateShaderResourceView = nullptr;
    // o_CreateUnorderedAccessView = nullptr;
    // o_CopyDescriptors = nullptr;
    // o_CopyDescriptorsSimple = nullptr;

    // if (o_ExecuteCommandLists != nullptr)
    //     DetourAttach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);

    // o_ExecuteCommandLists = nullptr;

    // if (o_Release != nullptr)
    //     DetourAttach(&(PVOID&) o_Release, hkRelease);

    // o_Release = nullptr;

    if (o_OMSetRenderTargets != nullptr)
        DetourDetach(&(PVOID&) o_OMSetRenderTargets, hkOMSetRenderTargets);

    if (o_SetGraphicsRootDescriptorTable != nullptr)
        DetourDetach(&(PVOID&) o_SetGraphicsRootDescriptorTable, hkSetGraphicsRootDescriptorTable);

    if (o_SetComputeRootDescriptorTable != nullptr)
        DetourDetach(&(PVOID&) o_SetComputeRootDescriptorTable, hkSetComputeRootDescriptorTable);

    if (o_ResetCommandList != nullptr)
        DetourDetach(&(PVOID&) o_ResetCommandList, hkReset);

    if (o_ResourceBarrier != nullptr)
        DetourDetach(&(PVOID&) o_ResourceBarrier, hkResourceBarrier);

    if (o_CopyResource != nullptr)
        DetourDetach(&(PVOID&) o_CopyResource, hkCopyResource);

    if (o_CopyTextureRegion != nullptr)
        DetourDetach(&(PVOID&) o_CopyTextureRegion, hkCopyTextureRegion);

    if (o_DrawIndexedInstanced != nullptr)
        DetourDetach(&(PVOID&) o_DrawIndexedInstanced, hkDrawIndexedInstanced);

    if (o_DrawInstanced != nullptr)
        DetourDetach(&(PVOID&) o_DrawInstanced, hkDrawInstanced);

    if (o_Dispatch != nullptr)
        DetourDetach(&(PVOID&) o_Dispatch, hkDispatch);

    if (o_Close != nullptr)
        DetourDetach(&(PVOID&) o_Close, hkClose);

    if (o_ExecuteBundle != nullptr)
        DetourDetach(&(PVOID&) o_ExecuteBundle, hkExecuteBundle);

    o_OMSetRenderTargets = nullptr;
    o_SetGraphicsRootDescriptorTable = nullptr;
    o_SetComputeRootDescriptorTable = nullptr;
    o_ResetCommandList = nullptr;
    o_ResourceBarrier = nullptr;
    o_CopyResource = nullptr;
    o_CopyTextureRegion = nullptr;
    o_DrawIndexedInstanced = nullptr;
    o_DrawInstanced = nullptr;
    o_Dispatch = nullptr;
    o_Close = nullptr;
    o_ExecuteBundle = nullptr;

    DetourTransactionCommit();
}

void ResTrack_Dx12::ClearPossibleHudless()
{
    LOG_DEBUG("");

    auto hfIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    if (!_useShards)
    {
        std::lock_guard<std::mutex> lock(_hudlessTrackMutex);
        fgPossibleHudless[hfIndex].clear();
    }
    else
    {
        for (size_t i = 0; i < SHARD_COUNT; i++)
        {
            auto& shard = _hudlessShards[hfIndex][i];

#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            shard.map.clear();
        }
    }

    std::lock_guard<std::mutex> lock2(_resourceCommandListMutex);

    auto fg = State::Instance().currentFG;
    if (fg != nullptr)
    {
        auto fIndex = fg->GetIndex();

        if (_notFoundCmdLists.size() > 10)
            _notFoundCmdLists.clear();

        for (const auto& pair : _resourceCommandList[fIndex])
        {
            LOG_WARN("{} cmdList: {:X}, not closed!", (UINT) pair.first, (size_t) pair.second);
            _notFoundCmdLists.insert(pair.second);
        }

        _resourceCommandList[fIndex].clear();

        for (const auto& pair : _resCmdList[fIndex])
        {
            LOG_WARN("{} cmdList: {:X}, not executed!", (UINT) pair.first, (size_t) pair.second);
            _notFoundCmdLists.insert(pair.second);
        }

        _resCmdList[fIndex].clear();
    }
}

void ResTrack_Dx12::SetResourceCmdList(FG_ResourceType type, ID3D12GraphicsCommandList* cmdList)
{
    auto fg = State::Instance().currentFG;
    if (fg != nullptr && fg->IsActive())
    {
        auto index = fg->GetIndex();

        ID3D12GraphicsCommandList* realCmdList = nullptr;
        if (!CheckForRealObject(__FUNCTION__, cmdList, (IUnknown**) &realCmdList))
            realCmdList = cmdList;

        _resourceCommandList[index][type] = realCmdList;
        LOG_DEBUG("_resourceCommandList[{}][{}]: {:X}", index, magic_enum::enum_name(type), (size_t) realCmdList);
    }
}
