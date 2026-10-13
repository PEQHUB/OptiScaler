#pragma once

#include <d3d12.h>

#include <cstdint>
#include <memory>

#include <gpu_time/FgTiming.h>

// A GPU interval on an already-recording DIRECT or COMPUTE command list. The
// result is published only after the submitting queue completes and the originating allocator
// generation retires. SDK recordings are observed through hooks and skip the first recording until
// Reset reveals the allocator. Owned recordings bind their lifecycle and submission explicitly.
class ScopedFgGpuTime_Dx12
{
    struct Impl;
    std::unique_ptr<Impl> _impl;

  public:
    enum class RecordingMode
    {
        Observed,
        Owned
    };

    ScopedFgGpuTime_Dx12(ID3D12GraphicsCommandList* commandList, const std::shared_ptr<FgTiming::Channel>& channel,
                         uint64_t frameId, uint32_t generatedFrames = 0, RecordingMode mode = RecordingMode::Observed);
    ~ScopedFgGpuTime_Dx12();

    ScopedFgGpuTime_Dx12(const ScopedFgGpuTime_Dx12&) = delete;
    ScopedFgGpuTime_Dx12& operator=(const ScopedFgGpuTime_Dx12&) = delete;

    // Failed/cancelled dispatches still close the marker pair, but do not
    // publish. An omitted End is treated as a failed dispatch.
    void End(bool succeeded = true);
};

class FgGpuTime_Dx12
{
  public:
    static void Poll();
    static bool HookQueue(ID3D12CommandQueue* commandQueue);

    // Owned producers notify a successful allocator + list Reset, then submit that closed recording
    // exactly once here. Their scopes skip API hook installation and queue implementation probes.
    static bool BindOwnedRecording(ID3D12GraphicsCommandList* commandList, ID3D12CommandAllocator* allocator);
    static void SubmitOwnedRecording(ID3D12CommandQueue* commandQueue, ID3D12GraphicsCommandList* commandList);
};
