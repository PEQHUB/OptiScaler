$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$dx12 = [IO.File]::ReadAllText((Join-Path $repoRoot 'OptiScaler/gpu_time/FgGpuTime_Dx12.cpp'))
$vulkan = [IO.File]::ReadAllText((Join-Path $repoRoot 'OptiScaler/gpu_time/FgGpuTime_Vk.cpp'))

function Read-SourceBlock([string]$source, [string]$start, [string]$end) {
    $first = $source.IndexOf($start, [StringComparison]::Ordinal)
    if ($first -lt 0) { throw "Missing collector function: $start" }
    $last = $source.IndexOf($end, $first, [StringComparison]::Ordinal)
    if ($last -lt 0) { throw "Missing collector boundary: $end" }
    return $source.Substring($first, $last - $first)
}

# Compile the production functions against CPU fixtures, without loading the DLL or a graphics runtime.
# Missing/renamed boundaries fail the test instead of silently testing an obsolete copied implementation.
$blocks = @{
    ROOT = $repoRoot.Replace('\', '/')
    VK_TYPES = Read-SourceBlock $vulkan 'struct SemaphoreState' 'struct PoolState'
    VK_FIND_QUEUE = Read-SourceBlock $vulkan 'DeviceState* FindQueue(' 'DeviceState* FindBuffer('
    VK_FIND_BUFFER = Read-SourceBlock $vulkan 'DeviceState* FindBuffer(' 'template <typename T> const T* FindNext('
    VK_READBACK = Read-SourceBlock $vulkan 'bool Completed(' 'void RetireDevice('
    VK_RETIREMENT = Read-SourceBlock $vulkan 'void InvalidateRecording(' 'struct Signal'
    VK_SIGNALS = Read-SourceBlock $vulkan 'struct Signal' 'struct SubmissionRecords'
    VK_PENDING = Read-SourceBlock $vulkan 'struct SubmissionRecords' 'void InvalidateUnobservedSignal('
    VK_UNOBSERVED = Read-SourceBlock $vulkan 'void InvalidateUnobservedSignal(' 'VKAPI_ATTR VkResult VKAPI_CALL HookCreateCommandPool('
    VK_SUBMIT2 = Read-SourceBlock $vulkan 'VkResult ObserveQueueSubmit2(' 'VKAPI_ATTR VkResult VKAPI_CALL HookQueueSubmit2('
    DX_TYPES = Read-SourceBlock $dx12 'struct QueueTimeline' 'struct AllocatorGeneration'
    DX_ALLOCATOR = Read-SourceBlock $dx12 'struct AllocatorGeneration' 'class PrivateData'
    DX_RECORDING = Read-SourceBlock $dx12 'class RecordingData' 'class AllocatorData'
    DX_EXECUTE = Read-SourceBlock $dx12 'void ExecuteRecordings(' 'bool HookList('
    DX_OWNER = (Read-SourceBlock $dx12 'void FgGpuTime_Dx12::SubmitOwnedRecording(' 'void FgGpuTime_Dx12::Poll(').Replace('void FgGpuTime_Dx12::SubmitOwnedRecording(', 'void SubmitOwnedRecording(')
    DX_POLL = (Read-SourceBlock ($dx12 + '// COLLECTOR_TEST_END') 'void FgGpuTime_Dx12::Poll(' '// COLLECTOR_TEST_END').Replace('void FgGpuTime_Dx12::Poll(', 'void Poll(')
}
$fixture = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'FgTimingCollectorTests.cpp.in'))
foreach ($block in $blocks.GetEnumerator()) {
    $marker = '@@' + $block.Key + '@@'
    if ($block.Key -ne 'ROOT') { $marker = '// ' + $marker }
    $fixture = $fixture.Replace($marker, $block.Value)
}
if ($fixture -match '@@[A-Z_0-9]+@@') { throw 'Unexpanded collector test fixture' }

$output = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$sourcePath = Join-Path $output 'FgTimingCollectorTests.cpp'
$objectPath = Join-Path $output 'FgTimingCollectorTests.obj'
$exePath = Join-Path $output 'FgTimingCollectorTests.exe'
[IO.File]::WriteAllText($sourcePath, $fixture, [Text.UTF8Encoding]::new($false))
& cl.exe /nologo /std:c++20 /EHsc /W4 /WX $sourcePath "/Fo:$objectPath" "/Fe:$exePath"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $exePath
exit $LASTEXITCODE
