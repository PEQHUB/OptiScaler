#pragma once

#include "SysUtils.h"
#include <d3dcompiler.h>
#include <DirectXMath.h>

using namespace DirectX;

// Constants for the frame warp compute shader
struct alignas(256) FrameWarpShaderConstants
{
    XMMATRIX clipToClipWarp;       // Clip-to-clip warp matrix
    XMMATRIX clipToClipWarpInv;    // Inverse warp for sampling
    UINT width;                    // Output width
    UINT height;                   // Output height
    float warpStrength;            // 0..1 blend factor
    UINT depthAware;               // 0 = homography, 1 = per-pixel depth
    float tanHalfFovX;             // tan(horizontal FOV / 2)
    float tanHalfFovY;             // tan(vertical FOV / 2)
    float deltaYaw;                // Late yaw delta, radians
    float deltaPitch;              // Late pitch delta, radians
    UINT stableUiSceneHasUnderlay; // 0=none, 1=spatial repair, 2=clean HUD-less repair
    UINT depthInverted;            // 0=normal Z, 1=reversed/inverted Z
    UINT infillHistoryAvailable;   // 0=no history, 1=history color bound
    float infillMaskCoverageEstimate;
};

// Constants for the motion vector correction shader
struct alignas(256) FrameWarpMVConstants
{
    XMMATRIX clipToClipWarp;       // Forward warp matrix (clip-to-clip)
    XMMATRIX clipToClipWarpInv;    // Inverse warp matrix
    UINT mvWidth;                  // MV buffer width
    UINT mvHeight;                 // MV buffer height
    UINT depthWidth;               // Depth buffer width (for depth coordinate mapping)
    UINT depthHeight;              // Depth buffer height
    float mvScaleX;                // MV scale X (converts MV texels to pixel displacement)
    float mvScaleY;                // MV scale Y (converts MV texels to pixel displacement)
    UINT depthAware;               // 0 = homography, 1 = per-pixel depth
    float pad1;
    float pad2;
};

// Constants for the combined warp + MV correction shader
struct alignas(256) FrameWarpCombinedConstants
{
    XMMATRIX clipToClipWarp;        // Clip-to-clip warp matrix
    XMMATRIX clipToClipWarpInv;     // Inverse warp matrix
    UINT width;                     // Color/MV buffer width (must be same for combined path)
    UINT height;                    // Color/MV buffer height (must be same for combined path)
    float warpStrength;             // 0..1 blend factor for color warp
    UINT depthAware;                // 0 = homography, 1 = per-pixel depth
    float mvScaleX;                 // MV scale X (converts texels to pixel displacement)
    float mvScaleY;                 // MV scale Y (converts texels to pixel displacement)
};

// (FrameWarpMVConstants2 and FrameWarpDebugVisConstants removed -
//  those were for the MV-based warp path which is no longer used.
//  Mouse-based warp reuses FrameWarpShaderConstants for all shaders.)

// ============================================================================
// Simple Ray-Space Mouse Warp Shader
// ============================================================================
// Applies a camera-correct rotational late warp. Each output pixel is converted
// to a view ray using the current FOV, rotated back to the source frame by the
// late mouse delta, then projected back to source UVs. This avoids the old
// z=0 clip-plane rotation, which produced expansion/stretch artifacts.
inline static std::string frameWarpSimpleCode = R"(
cbuffer WarpParams : register(b0)
{
    float4x4 clipToClipWarp;
    float4x4 clipToClipWarpInv;
    uint width;
    uint height;
    float warpStrength;
    uint depthAware;
    float tanHalfFovX;
    float tanHalfFovY;
    float deltaYaw;
    float deltaPitch;
    uint stableUiSceneHasUnderlay;
    uint depthInverted;
    uint infillHistoryAvailable;
    float infillMaskCoverageEstimate;
};

// Input: HUD-less color buffer
Texture2D<float4> SourceColor : register(t0);
// Output: Warped color buffer
RWTexture2D<float4> WarpedOutput : register(u0);
SamplerState PointClamp : register(s0);

float3 RotateRayToSource(float3 ray)
{
    float sy = sin(deltaYaw);
    float cy = cos(deltaYaw);
    float sp = sin(deltaPitch);
    float cp = cos(deltaPitch);

    // Positive mouse dx is a camera turn to the right. For inverse sampling,
    // the output center should sample from the old frame's right side.
    float3 pitched = float3(ray.x, ray.y * cp - ray.z * sp, ray.y * sp + ray.z * cp);
    return float3(pitched.x * cy + pitched.z * sy, pitched.y, -pitched.x * sy + pitched.z * cy);
}

float2 SourceUVFromOutputPixel(float2 pixelCenter)
{
    float2 ndc;
    ndc.x = (pixelCenter.x / width) * 2.0 - 1.0;
    ndc.y = 1.0 - (pixelCenter.y / height) * 2.0;

    float safeTanX = max(abs(tanHalfFovX), 0.0001);
    float safeTanY = max(abs(tanHalfFovY), 0.0001);
    float3 outputRay = normalize(float3(ndc.x * safeTanX, ndc.y * safeTanY, 1.0));
    float3 sourceRay = RotateRayToSource(outputRay);

    if (sourceRay.z <= 0.0001)
        return pixelCenter / float2(width, height);

    float2 sourceNdc;
    sourceNdc.x = (sourceRay.x / sourceRay.z) / safeTanX;
    sourceNdc.y = (sourceRay.y / sourceRay.z) / safeTanY;

    return float2((sourceNdc.x + 1.0) * 0.5, 1.0 - (sourceNdc.y + 1.0) * 0.5);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;

    float2 pixelCenter = float2(dispatchThreadID.xy) + 0.5;
    float2 sourceUV = SourceUVFromOutputPixel(pixelCenter);

    // Clamp source UV to texture edge instead of outputting black.
    sourceUV = clamp(sourceUV, float2(0.0, 0.0), float2(1.0, 1.0));

    int2 sourcePixel = int2(sourceUV * float2(width, height));
    sourcePixel = clamp(sourcePixel, int2(0, 0), int2(width - 1, height - 1));
    float4 warpedColor = SourceColor.Load(int3(sourcePixel, 0));

    float4 originalColor = SourceColor.Load(int3(dispatchThreadID.xy, 0));
    float4 result = lerp(originalColor, warpedColor, warpStrength);
    WarpedOutput[dispatchThreadID.xy] = result;
}
)";

// ============================================================================
// Stable UI Composite Shader
// ============================================================================
// Extracts UI from the original final frame by comparing it against the
// original HUD-less frame, then adds that UI back over the warped HUD-less
// scene. This keeps HUD/menu elements screen-stable while FrameWarp moves only
// the scene underneath.
inline static std::string frameWarpStableUiCompositeCode = R"(
cbuffer WarpParams : register(b0)
{
    float4x4 clipToClipWarp;
    float4x4 clipToClipWarpInv;
    uint width;
    uint height;
    float warpStrength;
    uint depthAware;
    float tanHalfFovX;
    float tanHalfFovY;
    float deltaYaw;
    float deltaPitch;
    uint stableUiSceneHasUnderlay;
    float pad0;
    float pad1;
    float pad2;
};

Texture2D<float4> OriginalHudless : register(t0);
Texture2D<float4> OriginalPresent : register(t1);
Texture2D<float4> WarpedHudless : register(t2);
RWTexture2D<float4> StableOutput : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;

    int3 pixel = int3(dispatchThreadID.xy, 0);
    float4 originalHudless = OriginalHudless.Load(pixel);
    float4 originalPresent = OriginalPresent.Load(pixel);
    float4 warpedHudless = WarpedHudless.Load(pixel);

    float3 diff = abs(originalPresent.rgb - originalHudless.rgb);
    float delta = max(max(diff.r, diff.g), diff.b);

    // Use a conservative mask and replace detected UI pixels with the original
    // final-frame sample. Additive compositing can double-apply faint UI that is
    // still present in imperfect HUD-less captures, producing a ghost overlay.
    float uiMask = smoothstep(0.04, 0.10, delta);
    float4 result = lerp(warpedHudless, originalPresent, uiMask);
    result.a = originalPresent.a;
    StableOutput[dispatchThreadID.xy] = result;
}
)";

// ============================================================================
// Stable UI Layer Extract Shader
// ============================================================================
// Extracts a reusable UI layer from a same-frame final-with-UI and HUD-less
// scene pair. The alpha channel stores the confidence mask so generated-frame
// callbacks can reuse the layer without deriving a mask from generated content.
inline static std::string frameWarpUiLayerExtractCode = R"(
cbuffer WarpParams : register(b0)
{
    float4x4 clipToClipWarp;
    float4x4 clipToClipWarpInv;
    uint width;
    uint height;
    float warpStrength;
    uint depthAware;
    float tanHalfFovX;
    float tanHalfFovY;
    float deltaYaw;
    float deltaPitch;
    uint stableUiSceneHasUnderlay;
    float pad0;
    float pad1;
    float pad2;
};

Texture2D<float4> OriginalHudless : register(t0);
Texture2D<float4> OriginalPresent : register(t1);
RWTexture2D<float4> UiLayer : register(u0);

float PixelDelta(int2 p)
{
    p = clamp(p, int2(0, 0), int2(width - 1, height - 1));
    float3 a = OriginalPresent.Load(int3(p, 0)).rgb;
    float3 b = OriginalHudless.Load(int3(p, 0)).rgb;
    float3 d = abs(a - b);
    return max(max(d.r, d.g), d.b);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;

    int2 p = int2(dispatchThreadID.xy);
    float localMax = 0.0;

    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            localMax = max(localMax, PixelDelta(p + int2(x, y)));
        }
    }

    // DLSSG resource-copy UI extraction compares a real HUD-less capture against
    // a generated output. Use a strict threshold there so generated scene deltas
    // do not get classified as UI and erase the resource-level warp.
    bool dlssgStrictMask = stableUiSceneHasUnderlay == 3;
    float low = dlssgStrictMask ? 0.25 : 0.025;
    float high = dlssgStrictMask ? 0.55 : 0.075;
    float uiMask = smoothstep(low, high, localMax);
    float4 ui = OriginalPresent.Load(int3(p, 0));
    UiLayer[p] = float4(ui.rgb, uiMask);
}
)";

// ============================================================================
// Stable UI Layer Composite Shader
// ============================================================================
// Composites a cached or explicit UI layer over an already warped scene.
// Generated FSRFG inputs may already contain UI, so t2 optionally carries the
// cached clean HUD-less scene from the nearest validated real frame.
inline static std::string frameWarpUiLayerCompositeCode = R"(
cbuffer WarpParams : register(b0)
{
    float4x4 clipToClipWarp;
    float4x4 clipToClipWarpInv;
    uint width;
    uint height;
    float warpStrength;
    uint depthAware;
    float tanHalfFovX;
    float tanHalfFovY;
    float deltaYaw;
    float deltaPitch;
    uint stableUiSceneHasUnderlay; // 0=none, 1=spatial fallback, 2=clean HUD-less repair
    float pad0;
    float pad1;
    float pad2;
};

Texture2D<float4> WarpedScene : register(t0);
Texture2D<float4> UiLayer : register(t1);
Texture2D<float4> CleanScene : register(t2);
RWTexture2D<float4> StableOutput : register(u0);

float3 RotateRayToSource(float3 ray)
{
    float sy = sin(deltaYaw);
    float cy = cos(deltaYaw);
    float sp = sin(deltaPitch);
    float cp = cos(deltaPitch);

    float3 pitched = float3(ray.x, ray.y * cp - ray.z * sp, ray.y * sp + ray.z * cp);
    return float3(pitched.x * cy + pitched.z * sy, pitched.y, -pitched.x * sy + pitched.z * cy);
}

int2 ClampPixel(int2 p)
{
    return clamp(p, int2(0, 0), int2(width - 1, height - 1));
}

float MaskAt(int2 p)
{
    p = ClampPixel(p);
    float m = 0.0;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            m = max(m, UiLayer.Load(int3(ClampPixel(p + int2(x, y)), 0)).a);
        }
    }

    return saturate(m);
}

float2 SourceUvForOutputPixel(int2 p)
{
    float2 uv = (float2(p) + 0.5) / float2(width, height);
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;

    float safeTanX = max(abs(tanHalfFovX), 0.0001);
    float safeTanY = max(abs(tanHalfFovY), 0.0001);
    float3 outputRay = normalize(float3(ndc.x * safeTanX, ndc.y * safeTanY, 1.0));
    float3 sourceRay = RotateRayToSource(outputRay);

    if (sourceRay.z <= 0.0001)
        return uv;

    float2 sourceNdc;
    sourceNdc.x = (sourceRay.x / sourceRay.z) / safeTanX;
    sourceNdc.y = (sourceRay.y / sourceRay.z) / safeTanY;

    float2 sourceUv = sourceNdc * 0.5 + 0.5;
    sourceUv.y = 1.0 - sourceUv.y;
    return sourceUv;
}

float UnderlayMaskAtOutputPixel(int2 p)
{
    if (stableUiSceneHasUnderlay == 0)
        return 0.0;

    float2 sourceUv = SourceUvForOutputPixel(p);
    int2 sourcePixel = int2(sourceUv * float2(width, height));
    return MaskAt(sourcePixel);
}

float3 CleanSceneAtOutputPixel(int2 p, float3 fallback)
{
    if (stableUiSceneHasUnderlay != 2)
        return fallback;

    float2 sourceUv = SourceUvForOutputPixel(p);
    if (sourceUv.x < 0.0 || sourceUv.x > 1.0 || sourceUv.y < 0.0 || sourceUv.y > 1.0)
        sourceUv = saturate(sourceUv);

    int2 sourcePixel = ClampPixel(int2(sourceUv * float2(width, height)));
    return CleanScene.Load(int3(sourcePixel, 0)).rgb;
}

float3 FillSceneAroundUi(int2 p, float3 fallback)
{
    float3 sum = 0.0;
    float weight = 0.0;

    [unroll]
    for (int ring = 0; ring < 4; ++ring)
    {
        int radius = 3 + ring * 4;
        int2 offsets[8] = {
            int2(radius, 0), int2(-radius, 0), int2(0, radius), int2(0, -radius),
            int2(radius, radius), int2(-radius, radius), int2(radius, -radius), int2(-radius, -radius)
        };

        [unroll]
        for (int i = 0; i < 8; ++i)
        {
            int2 q = ClampPixel(p + offsets[i]);
            float stableMask = MaskAt(q);
            float movedMask = UnderlayMaskAtOutputPixel(q);
            float sampleWeight = saturate(1.0 - max(stableMask, movedMask));
            sum += WarpedScene.Load(int3(q, 0)).rgb * sampleWeight;
            weight += sampleWeight;
        }
    }

    return weight > 0.001 ? (sum / weight) : fallback;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;

    int2 p = int2(dispatchThreadID.xy);
    int3 pixel = int3(p, 0);
    float4 scene = WarpedScene.Load(pixel);
    float4 ui = UiLayer.Load(pixel);
    float stableMask = saturate(ui.a);
    float underlayMask = UnderlayMaskAtOutputPixel(p);

    // Generated FSRFG callbacks can give us a UI-bearing generated frame.
    // Repair both the stable UI footprint and the warped/moved underlay before
    // restoring the UI exactly once.
    float repairMask = saturate(max(stableMask, underlayMask));
    float3 cleanScene = scene.rgb;
    if (stableUiSceneHasUnderlay == 2 && repairMask > 0.01)
    {
        float3 clean = CleanSceneAtOutputPixel(p, scene.rgb);
        cleanScene = lerp(scene.rgb, clean, repairMask);
    }
    else if (stableUiSceneHasUnderlay == 1 && underlayMask > 0.01)
    {
        float3 fill = FillSceneAroundUi(p, scene.rgb);
        cleanScene = lerp(scene.rgb, fill, underlayMask);
    }

    float3 rgb = lerp(cleanScene, ui.rgb, stableMask);
    StableOutput[dispatchThreadID.xy] = float4(rgb, scene.a);
}
)";

// ============================================================================
// Depth-Aware Warp Shader
// ============================================================================
// Rotational late warp with depth-derived confidence. Rotation still controls
// source UVs, while depth is transformed through the same output/source ray
// mapping to flag disocclusion, foreground tears, and depthless areas. Invalid
// pixels are filled with an edge-aware spatial pull instead of blind full-frame
// stretching.
inline static std::string frameWarpDepthAwareCode = R"(
cbuffer WarpParams : register(b0)
{
    float4x4 clipToClipWarp;
    float4x4 clipToClipWarpInv;
    uint width;
    uint height;
    float warpStrength;
    uint depthAware;
    float tanHalfFovX;
    float tanHalfFovY;
    float deltaYaw;
    float deltaPitch;
};

// Input: HUD-less color buffer
Texture2D<float4> SourceColor : register(t0);
// Input: Depth buffer
Texture2D<float> SourceDepth : register(t1);
// Output: Warped color buffer
RWTexture2D<float4> WarpedOutput : register(u0);
SamplerState PointClamp : register(s0);

float2 ClampUV(float2 uv)
{
    return clamp(uv, float2(0.0, 0.0), float2(1.0, 1.0));
}

float3 RotateRayToSource(float3 ray)
{
    float sy = sin(deltaYaw);
    float cy = cos(deltaYaw);
    float sp = sin(deltaPitch);
    float cp = cos(deltaPitch);
    float3 pitched = float3(ray.x, ray.y * cp - ray.z * sp, ray.y * sp + ray.z * cp);
    return float3(pitched.x * cy + pitched.z * sy, pitched.y, -pitched.x * sy + pitched.z * cy);
}

float2 SourceUVFromOutputPixel(float2 pixelCenter)
{
    float2 ndc;
    ndc.x = (pixelCenter.x / width) * 2.0 - 1.0;
    ndc.y = 1.0 - (pixelCenter.y / height) * 2.0;
    float safeTanX = max(abs(tanHalfFovX), 0.0001);
    float safeTanY = max(abs(tanHalfFovY), 0.0001);
    float3 outputRay = normalize(float3(ndc.x * safeTanX, ndc.y * safeTanY, 1.0));
    float3 sourceRay = RotateRayToSource(outputRay);
    if (sourceRay.z <= 0.0001)
        return pixelCenter / float2(width, height);
    float2 sourceNdc;
    sourceNdc.x = (sourceRay.x / sourceRay.z) / safeTanX;
    sourceNdc.y = (sourceRay.y / sourceRay.z) / safeTanY;
    return float2((sourceNdc.x + 1.0) * 0.5, 1.0 - (sourceNdc.y + 1.0) * 0.5);
}

float DepthAtUV(float2 uv)
{
    uint depthWidth;
    uint depthHeight;
    SourceDepth.GetDimensions(depthWidth, depthHeight);
    float2 safeUV = ClampUV(uv);
    int2 p = int2(safeUV * float2(depthWidth, depthHeight));
    p = clamp(p, int2(0, 0), int2(int(depthWidth) - 1, int(depthHeight) - 1));
    return SourceDepth.Load(int3(p, 0));
}

float DepthGradientAtUV(float2 uv)
{
    uint depthWidth;
    uint depthHeight;
    SourceDepth.GetDimensions(depthWidth, depthHeight);
    float2 texel = 1.0 / max(float2(depthWidth, depthHeight), float2(1.0, 1.0));
    float c = DepthAtUV(uv);
    float l = DepthAtUV(uv + float2(-texel.x, 0.0));
    float r = DepthAtUV(uv + float2( texel.x, 0.0));
    float u = DepthAtUV(uv + float2(0.0, -texel.y));
    float d = DepthAtUV(uv + float2(0.0,  texel.y));
    return max(max(abs(c - l), abs(c - r)), max(abs(c - u), abs(c - d)));
}

float4 EdgeAwareSpatialFill(int2 pixel, float refDepth, float4 refColor)
{
    float4 bestColor = refColor;
    float bestScore = 1000000.0;

    [unroll]
    for (int dirIndex = 0; dirIndex < 8; ++dirIndex)
    {
        int2 dir = int2(0, 0);
        if (dirIndex == 0) dir = int2(1, 0);
        if (dirIndex == 1) dir = int2(-1, 0);
        if (dirIndex == 2) dir = int2(0, 1);
        if (dirIndex == 3) dir = int2(0, -1);
        if (dirIndex == 4) dir = int2(1, 1);
        if (dirIndex == 5) dir = int2(-1, 1);
        if (dirIndex == 6) dir = int2(1, -1);
        if (dirIndex == 7) dir = int2(-1, -1);

        [unroll]
        for (int step = 1; step <= 8; ++step)
        {
            int2 samplePixel = clamp(pixel + dir * step, int2(0, 0), int2(width - 1, height - 1));
            float2 sampleUV = (float2(samplePixel) + 0.5) / float2(width, height);
            float sampleDepth = DepthAtUV(sampleUV);
            float4 sampleColor = SourceColor.Load(int3(samplePixel, 0));

            float depthScore = abs(sampleDepth - refDepth) * 120.0;
            float colorScore = length(sampleColor.rgb - refColor.rgb) * 0.20;
            float distanceScore = float(step) * 0.015;
            float score = depthScore + colorScore + distanceScore;

            if (score < bestScore)
            {
                bestScore = score;
                bestColor = sampleColor;
            }
        }
    }

    return bestColor;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;

    // Output pixel center, inverse-sampled through the ray-space camera warp.
    float2 pixelCenter = float2(dispatchThreadID.xy) + 0.5;
    float2 sourceUV = SourceUVFromOutputPixel(pixelCenter);
    // Read depth at current pixel — this IS NDC Z (clipZ/clipW)
     // Clamp source UV to texture edge instead of outputting black.
 // This produces edge smearing at disocclusion boundaries instead of
 // visible black holes. For small warp angles (< 2 degrees), the
 // smeared strip is typically 10-30px wide and barely noticeable.
 sourceUV = clamp(sourceUV, float2(0.0, 0.0), float2(1.0, 1.0));

 int2 sourcePixel = int2(sourceUV * float2(width, height));
 sourcePixel = clamp(sourcePixel, int2(0, 0), int2(width - 1, height - 1));
 float4 warpedColor = SourceColor.Load(int3(sourcePixel, 0));

 // Blend with original based on warp strength
 float4 originalColor = SourceColor.Load(int3(dispatchThreadID.xy, 0));

 float2 outputUV = pixelCenter / float2(width, height);
 float2 rawSourceUV = SourceUVFromOutputPixel(pixelCenter);
 float outOfBounds =
     (rawSourceUV.x < 0.0 || rawSourceUV.x > 1.0 || rawSourceUV.y < 0.0 || rawSourceUV.y > 1.0) ? 1.0 : 0.0;

 float outputDepth = DepthAtUV(outputUV);
 float sourceDepth = DepthAtUV(sourceUV);
 float outputGrad = DepthGradientAtUV(outputUV);
 float sourceGrad = DepthGradientAtUV(sourceUV);
 float depthEdge = max(outputGrad, sourceGrad);
 float depthTolerance = max(0.0025, depthEdge * 2.0);
 float depthConflict = saturate((abs(sourceDepth - outputDepth) - depthTolerance) * 80.0);
 float foregroundEdge = saturate((depthEdge - 0.0015) * 180.0);
 float invalid = saturate(max(outOfBounds, max(depthConflict, foregroundEdge * 0.55)));

 float4 spatialFill = EdgeAwareSpatialFill(int2(dispatchThreadID.xy), outputDepth, originalColor);
 float4 repairedColor = lerp(warpedColor, spatialFill, invalid * 0.85);

 float effectiveStrength = saturate(warpStrength * (1.0 - invalid * 0.15));
 float4 result = lerp(originalColor, repairedColor, effectiveStrength);
 result.a = originalColor.a;
 WarpedOutput[dispatchThreadID.xy] = result;
}
)";

// ============================================================================
// Conservative Depth-Infill Warp Shader
// ============================================================================
// The main camera warp stays a ray-space rotational late warp. Depth is used
// only to build a conservative hole/foreground-edge infill mask.
inline static std::string frameWarpDepthInfillCode = R"(
cbuffer WarpParams : register(b0)
{
    float4x4 clipToClipWarp;
    float4x4 clipToClipWarpInv;
    uint width;
    uint height;
    float warpStrength;
    uint depthAware;
    float tanHalfFovX;
    float tanHalfFovY;
    float deltaYaw;
    float deltaPitch;
    uint stableUiSceneHasUnderlay;
    uint depthInverted;
    uint infillHistoryAvailable;
    float infillMaskCoverageEstimate;
};

Texture2D<float4> SourceColor : register(t0);
Texture2D<float> SourceDepth : register(t1);
Texture2D<float4> HistoryColor : register(t2);
RWTexture2D<float4> WarpedOutput : register(u0);
SamplerState PointClamp : register(s0);

float3 RotateRayToSource(float3 ray)
{
    float sy = sin(deltaYaw);
    float cy = cos(deltaYaw);
    float sp = sin(deltaPitch);
    float cp = cos(deltaPitch);
    float3 pitched = float3(ray.x, ray.y * cp - ray.z * sp, ray.y * sp + ray.z * cp);
    return float3(pitched.x * cy + pitched.z * sy, pitched.y, -pitched.x * sy + pitched.z * cy);
}

float2 SourceUVFromOutputPixel(float2 pixelCenter)
{
    float2 ndc;
    ndc.x = (pixelCenter.x / width) * 2.0 - 1.0;
    ndc.y = 1.0 - (pixelCenter.y / height) * 2.0;
    float safeTanX = max(abs(tanHalfFovX), 0.0001);
    float safeTanY = max(abs(tanHalfFovY), 0.0001);
    float3 outputRay = normalize(float3(ndc.x * safeTanX, ndc.y * safeTanY, 1.0));
    float3 sourceRay = RotateRayToSource(outputRay);
    if (sourceRay.z <= 0.0001)
        return pixelCenter / float2(width, height);
    float2 sourceNdc;
    sourceNdc.x = (sourceRay.x / sourceRay.z) / safeTanX;
    sourceNdc.y = (sourceRay.y / sourceRay.z) / safeTanY;
    return float2((sourceNdc.x + 1.0) * 0.5, 1.0 - (sourceNdc.y + 1.0) * 0.5);
}

float2 ClampUV(float2 uv)
{
    return clamp(uv, float2(0.0, 0.0), float2(1.0, 1.0));
}

int2 ClampPixel(int2 p)
{
    return clamp(p, int2(0, 0), int2(width - 1, height - 1));
}

float DepthAtUV(float2 uv)
{
    uint depthWidth;
    uint depthHeight;
    SourceDepth.GetDimensions(depthWidth, depthHeight);
    if (depthWidth == 0 || depthHeight == 0)
        return depthInverted != 0 ? 0.0 : 1.0;
    float2 safeUV = ClampUV(uv);
    int2 p = int2(safeUV * float2(depthWidth, depthHeight));
    p = clamp(p, int2(0, 0), int2(int(depthWidth) - 1, int(depthHeight) - 1));
    return SourceDepth.Load(int3(p, 0));
}

bool DepthFartherThan(float sampleDepth, float refDepth)
{
    return depthInverted != 0 ? sampleDepth < refDepth : sampleDepth > refDepth;
}

float LocalDepthEdgeMask(float2 outputUV, float2 displacementPixels)
{
    float displacementLength = length(displacementPixels);
    if (displacementLength < 0.35)
        return 0.0;

    float2 dirPixels = displacementPixels / max(displacementLength, 1e-3);
    float2 texel = 1.0 / float2(width, height);
    float2 dirUv = dirPixels * texel;

    float center = DepthAtUV(outputUV);
    float forward2 = DepthAtUV(outputUV + dirUv * 2.0);
    float back2 = DepthAtUV(outputUV - dirUv * 2.0);
    float forward5 = DepthAtUV(outputUV + dirUv * 5.0);
    float back5 = DepthAtUV(outputUV - dirUv * 5.0);
    float edge = max(max(abs(center - forward2), abs(center - back2)),
                     max(abs(center - forward5), abs(center - back5)));

    float threshold = max(0.0015, abs(center) * 0.0015);
    float motionGate = saturate((displacementLength - 0.75) / 12.0);
    return saturate((edge - threshold) * 120.0) * motionGate;
}

float4 DirectionalBackgroundFill(int2 pixel, float2 displacementPixels, float refDepth, float4 fallback)
{
    float displacementLength = max(length(displacementPixels), 1.0);
    float2 dir = displacementPixels / displacementLength;
    float4 bestColor = fallback;
    float bestScore = 100000.0;

    [unroll]
    for (int step = 1; step <= 12; ++step)
    {
        float radius = 2.0 + float(step) * 2.0;
        float2 baseOffset = -dir * radius;
        float2 perp = float2(-dir.y, dir.x);

        [unroll]
        for (int tap = 0; tap < 3; ++tap)
        {
            float side = tap == 0 ? 0.0 : (tap == 1 ? 1.0 : -1.0);
            int2 q = ClampPixel(int2(round(float2(pixel) + baseOffset + perp * side * radius * 0.45)));
            float2 qUv = (float2(q) + 0.5) / float2(width, height);
            float sampleDepth = DepthAtUV(qUv);
            float4 sampleColor = SourceColor.Load(int3(q, 0));
            float farBias = DepthFartherThan(sampleDepth, refDepth) ? -0.35 : 0.15;
            float score = abs(sampleDepth - refDepth) * 24.0 + float(step) * 0.035 +
                length(sampleColor.rgb - fallback.rgb) * 0.08 + farBias;
            if (score < bestScore)
            {
                bestScore = score;
                bestColor = sampleColor;
            }
        }
    }

    return bestColor;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;

    float2 pixelCenter = float2(dispatchThreadID.xy) + 0.5;
    float2 outputUV = pixelCenter / float2(width, height);
    float2 rawSourceUV = SourceUVFromOutputPixel(pixelCenter);
    float2 sourceUV = ClampUV(rawSourceUV);
    float2 displacementPixels = (rawSourceUV - outputUV) * float2(width, height);

    int2 p = int2(dispatchThreadID.xy);
    int2 sourcePixel = ClampPixel(int2(sourceUV * float2(width, height)));
    float4 warpedColor = SourceColor.Load(int3(sourcePixel, 0));
    float4 originalColor = SourceColor.Load(int3(p, 0));

    float outOfBounds =
        (rawSourceUV.x < 0.0 || rawSourceUV.x > 1.0 || rawSourceUV.y < 0.0 || rawSourceUV.y > 1.0) ? 1.0 : 0.0;
    float edgeMask = LocalDepthEdgeMask(outputUV, displacementPixels);
    float infillMask = saturate(max(outOfBounds, edgeMask * 0.65));

    float outputDepth = DepthAtUV(outputUV);
    float4 directionalFill = DirectionalBackgroundFill(p, displacementPixels, outputDepth, warpedColor);

    int2 historyPixel = ClampPixel(int2(round(float2(p) - displacementPixels)));
    float4 historyFill = HistoryColor.Load(int3(historyPixel, 0));
    float historyWeight = infillHistoryAvailable != 0 ? 0.55 : 0.0;
    float4 fillColor = lerp(directionalFill, historyFill, historyWeight);

    float4 repairedColor = lerp(warpedColor, fillColor, infillMask * 0.75);
    float effectiveStrength = saturate(warpStrength * (1.0 - infillMask * 0.05));
    float4 result = lerp(originalColor, repairedColor, effectiveStrength);
    result.a = originalColor.a;
    WarpedOutput[p] = result;
}
)";

// ============================================================================
// Motion Vector Correction Shader
// ============================================================================
// After warping the color buffer, motion vectors must be corrected so that
// DLSSG/FSRFG/XeFG interpolation remains accurate.
//
// The math is simple addition:
//     corrected_mv = original_mv + warp_displacement
//
// where warp_displacement = (warped_pixel_pos - original_pixel_pos) in the
// same unit space as the original MVs.
//
// The warp displacement is computed per-pixel from the warp matrix, same as
// the color warp shader. For the simple (homographic) case, we use Z=0.
// For the depth-aware case, we read depth and use it for proper parallax.
//
// MV format: typically R16G16_FLOAT or R32G32_FLOAT, 2-channel.
//
// MV unit space convention:
//   Game MVs in the buffer are in "pixel displacement" units:
//     pixel_displacement = MV_texel_value * MV_Scale
//   where MV_Scale converts from the stored texel value to actual pixel motion.
//   For DLSSG: MV_Scale is typically 1.0 (texels ARE pixel displacement)
//   For FSRFG: MV_Scale = motionVectorScale (may differ from 1.0)
//   For NVNGX upscaler path: MV_Scale = MV_Scale_X/Y from the upscaler
//
// We compute the warp displacement in texels, then convert to pixel-displacement
// units using mvScale so it matches the original MV unit space:
//     warpDisplacementPixels = warpDisplacementTexels * float2(mvScaleX, mvScaleY)
//     correctedMV = originalMV + warpDisplacementPixels
//
// Y-flip convention:
//   The -0.5 factor on Y assumes game MVs use Y-down (texel-space) convention,
//   which is standard for DLSSG and FSRFG. If a game uses Y-up MVs, the
//   displacement Y sign would need to be flipped. Currently no known FG backend
//   uses Y-up MVs, so this assumption is safe.
//
// Depth-aware Z convention (approximation):
//   We construct float4(ndcX, ndcY, depth, 1.0) where depth is NDC Z.
//   This treats NDC Z as clip Z with w=1, which is an approximation.
//   For perspective projections, clip Z = ndcZ * clipW, and clipW = viewZ.
//   Setting w=1 assumes clipW=1, which is wrong, but the x/y result after
//   perspective divide is approximately correct for small warps because the
//   rotation dominates. The Z error only affects parallax displacement at
//   depth discontinuities. For typical warp angles (< 3 degrees), this
//   approximation is acceptable. For very large depth ranges (near=0.1,
//   far=10000), the approximation degrades slightly for distant objects.
inline static std::string frameWarpMVCorrectCode = R"(
cbuffer MVCorrectParams : register(b0)
{
    float4x4 clipToClipWarp;
    float4x4 clipToClipWarpInv;
    uint mvWidth;                  // MV buffer width
    uint mvHeight;                 // MV buffer height
    uint depthWidth;               // Depth buffer width (for depth coordinate mapping)
    uint depthHeight;              // Depth buffer height
    float mvScaleX;                // MV scale X (converts texels to pixel displacement)
    float mvScaleY;                // MV scale Y (converts texels to pixel displacement)
    uint depthAware;               // 0 = homography, 1 = per-pixel depth
    float pad1;
    float pad2;
};

// Input: Original motion vectors (2-channel)
Texture2D<float2> SourceMV : register(t0);
// Input: Depth buffer (only used when depthAware = 1)
Texture2D<float> SourceDepth : register(t1);
// Output: Corrected motion vectors
RWTexture2D<float2> CorrectedMV : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= mvWidth || dispatchThreadID.y >= mvHeight)
        return;

    // Read original MV at this pixel
    float2 originalMV = SourceMV.Load(int3(dispatchThreadID.xy, 0));

    // Compute clip-space position of this MV pixel
    // MV buffer may be at a different resolution than color/depth buffers
    float2 mvPixelCenter = float2(dispatchThreadID.xy) + 0.5;
    float2 clipPos;
    clipPos.x = (mvPixelCenter.x / mvWidth) * 2.0 - 1.0;
    clipPos.y = 1.0 - (mvPixelCenter.y / mvHeight) * 2.0;

    // Construct clip-space position for warp
    float z = 0.0; // Default: homographic (Z=0, near plane)
    if (depthAware != 0)
    {
        // Sample depth at the corresponding depth buffer pixel.
        // Depth buffer may be at a different resolution than the MV buffer.
        // Use depthWidth/depthHeight (not colorWidth/colorHeight) because
        // depth is typically at render resolution, which may differ from both
        // MV resolution and display/color resolution.
        int2 depthPixel = int2(mvPixelCenter.x * depthWidth / mvWidth,
                               mvPixelCenter.y * depthHeight / mvHeight);
        depthPixel = clamp(depthPixel, int2(0, 0), int2(depthWidth - 1, depthHeight - 1));
        z = SourceDepth.Load(int3(depthPixel, 0));
    }

    // Apply forward warp to get the warped clip position
    float4 warpedClip = mul(clipToClipWarp, float4(clipPos, z, 1.0));

    // Perspective divide to get warped NDC
    float2 warpedNDC;
    if (warpedClip.w != 0.0)
    {
        warpedNDC = warpedClip.xy / warpedClip.w;
    }
    else
    {
        warpedNDC = clipPos;
    }

    // Compute warp displacement in MV texel space
    // NDC [-1,1] -> pixel displacement in MV buffer
    // delta_ndc * (mvWidth/2) = pixel displacement in MV texels
    float2 warpDisplacementTexels;
    warpDisplacementTexels.x = (warpedNDC.x - clipPos.x) * (mvWidth * 0.5);
    warpDisplacementTexels.y = (warpedNDC.y - clipPos.y) * (mvHeight * -0.5);  // Flip Y: NDC Y-up -> texel Y-down

    // Convert warp displacement from texels to the same unit space as the
    // original MVs. Game MVs store pixel_displacement = MV_texel * MV_Scale.
    // We must match this unit space so the addition is meaningful:
    //     correctedMV = originalMV + warpDisplacementTexels * mvScale
    // When mvScale is 1.0 (common for DLSSG), texels == pixel displacement.
    // When mvScale differs (FSRFG, NVNGX upscaler path), this converts correctly.
    float2 warpDisplacementPixels = warpDisplacementTexels * float2(mvScaleX, mvScaleY);

    float2 correctedMV = originalMV + warpDisplacementPixels;

    CorrectedMV[dispatchThreadID.xy] = correctedMV;
}
)";

// ============================================================================
// Combined Warp + MV Correction Shader (single-pass, homographic only)
// ============================================================================
// For the common case (homographic warp, no depth), we can warp color and
// correct MVs in a single dispatch. This saves one command list submission.
//
// IMPORTANT: This shader is ONLY valid when the MV buffer and color buffer
// are the SAME resolution. When they differ (e.g., render-res MVs with
// display-res color), the separate warp + MV correct passes must be used
// instead, because the MV displacement must be computed in MV texel space
// (using mvWidth/mvHeight), not color texel space.
//
// Output 0: Warped color (RGBA)
// Output 1: Corrected motion vectors (RG)
inline static std::string frameWarpCombinedCode = R"(
cbuffer WarpParams : register(b0)
{
    float4x4 clipToClipWarp;
    float4x4 clipToClipWarpInv;
    uint width;                    // Color buffer width (== MV buffer width for combined path)
    uint height;                   // Color buffer height (== MV buffer height for combined path)
    float warpStrength;
    uint depthAware;
    float mvScaleX;                // MV scale X (converts texels to pixel displacement)
    float mvScaleY;                // MV scale Y (converts texels to pixel displacement)
};

Texture2D<float4> SourceColor : register(t0);
Texture2D<float2> SourceMV : register(t1);
RWTexture2D<float4> WarpedOutput : register(u0);
RWTexture2D<float2> CorrectedMV : register(u1);
SamplerState PointClamp : register(s0);

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;

    // --- Color warp ---
    float2 pixelCenter = float2(dispatchThreadID.xy) + 0.5;
    float2 clipPos;
    clipPos.x = (pixelCenter.x / width) * 2.0 - 1.0;
    clipPos.y = 1.0 - (pixelCenter.y / height) * 2.0;

    float4 sourceClip = mul(clipToClipWarpInv, float4(clipPos, 0.0, 1.0));
    float2 sourceUV;
    if (sourceClip.w != 0.0)
    {
        sourceUV.x = (sourceClip.x / sourceClip.w + 1.0) * 0.5;
        sourceUV.y = 1.0 - (sourceClip.y / sourceClip.w + 1.0) * 0.5;
    }
    else
    {
        sourceUV = pixelCenter / float2(width, height);
    }

    // Clamp source UV to texture edge instead of outputting black
    sourceUV = clamp(sourceUV, float2(0.0, 0.0), float2(1.0, 1.0));
    int2 sourcePixel = int2(sourceUV * float2(width, height));
    sourcePixel = clamp(sourcePixel, int2(0, 0), int2(width - 1, height - 1));
    float4 warpedColor = SourceColor.Load(int3(sourcePixel, 0));
    float4 originalColor = SourceColor.Load(int3(dispatchThreadID.xy, 0));
    WarpedOutput[dispatchThreadID.xy] = lerp(originalColor, warpedColor, warpStrength);

    // --- MV correction ---
    // NOTE: In combined mode, width == mvWidth and height == mvHeight.
    // This shader must NOT be used when MV and color resolutions differ.
    float2 originalMV = SourceMV.Load(int3(dispatchThreadID.xy, 0));

    // Compute forward warp displacement
    float4 warpedClip = mul(clipToClipWarp, float4(clipPos, 0.0, 1.0));
    float2 warpedNDC = (warpedClip.w != 0.0) ? warpedClip.xy / warpedClip.w : clipPos;

    // Displacement in texels (same as MV buffer dimensions since combined
    // mode requires color and MV buffers to be the same resolution)
    float2 warpDisplacementTexels;
    warpDisplacementTexels.x = (warpedNDC.x - clipPos.x) * (width * 0.5);
    warpDisplacementTexels.y = (warpedNDC.y - clipPos.y) * (height * -0.5);  // Flip Y

    // Convert texels to pixel-displacement units to match original MV space
    float2 warpDisplacementPixels = warpDisplacementTexels * float2(mvScaleX, mvScaleY);

    CorrectedMV[dispatchThreadID.xy] = originalMV + warpDisplacementPixels;
}
)";



// ============================================================================
// Distortion Field Generation Shader
// ============================================================================

// Generates a per-pixel UV displacement texture (distortion field) for
// consumption by FSRFG's frame interpolation pipeline. FSRFG uses this
// field in its disocclusion detection, depth reconstruction, and MV field
// passes, providing proper inpainting at disoccluded regions.
//
// The distortion field encodes: warpedUV - originalUV in UV space [0,1].
// FSR3 samples this with bilinear filtering and multiplies by RenderSize()
// to get pixel offsets. UV-space values are resolution-independent.
//
// Format: R16G16_FLOAT for sub-pixel precision (R8G8_UNORM only gives 1/255).
// V1: Homographic only (depthAware = 0). Depth-aware mode deferred to V2.

inline static std::string frameWarpDistortionFieldCode = R"(
cbuffer DistortionFieldParams : register(b0)
{
    float4x4 clipToClipWarp;
    float4x4 clipToClipWarpInv;
    uint width;
    uint height;
    float warpStrength;
    uint depthAware;
    float tanHalfFovX;
    float tanHalfFovY;
    float deltaYaw;
    float deltaPitch;
};

// Optional depth input (V1: not bound, depthAware is always 0)
Texture2D<float> SourceDepth : register(t0);
// Output: RG16F distortion field (UV displacement)
RWTexture2D<float2> DistortionOutput : register(u0);

float3 RotateRayToOutput(float3 ray)
{
    float sy = sin(-deltaYaw);
    float cy = cos(-deltaYaw);
    float sp = sin(-deltaPitch);
    float cp = cos(-deltaPitch);
    float3 pitched = float3(ray.x, ray.y * cp - ray.z * sp, ray.y * sp + ray.z * cp);
    return float3(pitched.x * cy + pitched.z * sy, pitched.y, -pitched.x * sy + pitched.z * cy);
}

float2 ProjectRayToUV(float3 ray, float2 fallbackUV)
{
    float safeTanX = max(abs(tanHalfFovX), 0.0001);
    float safeTanY = max(abs(tanHalfFovY), 0.0001);
    if (ray.z <= 0.0001)
        return fallbackUV;
    float2 ndc;
    ndc.x = (ray.x / ray.z) / safeTanX;
    ndc.y = (ray.y / ray.z) / safeTanY;
    return float2((ndc.x + 1.0) * 0.5, 1.0 - (ndc.y + 1.0) * 0.5);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= width || dtid.y >= height)
        return;

    // Output pixel center in UV space [0, 1]
    float2 uv = (float2(dtid.xy) + 0.5) / float2(width, height);

    // Convert to clip space [-1, 1]
    float2 clipPos;
    clipPos.x = uv.x * 2.0 - 1.0;
    clipPos.y = 1.0 - uv.y * 2.0;

    float safeTanX = max(abs(tanHalfFovX), 0.0001);
    float safeTanY = max(abs(tanHalfFovY), 0.0001);
    float3 sourceRay = normalize(float3(clipPos.x * safeTanX, clipPos.y * safeTanY, 1.0));
    float3 warpedRay = RotateRayToOutput(sourceRay);
    float2 warpedUV = ProjectRayToUV(warpedRay, uv);

    // Distortion = UV displacement (warped - original)
    // FSR3 expects: "UV after distortion - UV before distortion"
    DistortionOutput[dtid.xy] = warpedUV - uv;
}
)";

// Streamline/DLSSG bidirectional distortion field.
//
// Streamline expects 4 channels in normalized [0,1] pixel space:
//   RG = distorted pixel -> undistorted pixel displacement
//   BA = undistorted pixel -> distorted pixel displacement
//
// Keep this separate from the FSRFG distortion field above. FSRFG consumes
// a 2-channel UV displacement, while Streamline's DLSSG path explicitly
// wants both directions in one R16G16B16A16_FLOAT texture.
inline static std::string frameWarpStreamlineDistortionFieldCode = R"(
cbuffer DistortionFieldParams : register(b0)
{
    float4x4 clipToClipWarp;
    float4x4 clipToClipWarpInv;
    uint width;
    uint height;
    float warpStrength;
    uint depthAware;
    float tanHalfFovX;
    float tanHalfFovY;
    float deltaYaw;
    float deltaPitch;
};

Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float4> DistortionOutput : register(u0);

float3 RotateRay(float3 ray, float yaw, float pitch)
{
    float sy = sin(yaw);
    float cy = cos(yaw);
    float sp = sin(pitch);
    float cp = cos(pitch);
    float3 pitched = float3(ray.x, ray.y * cp - ray.z * sp, ray.y * sp + ray.z * cp);
    return float3(pitched.x * cy + pitched.z * sy, pitched.y, -pitched.x * sy + pitched.z * cy);
}

float2 ProjectRayToUV(float3 ray, float2 fallbackUV)
{
    float safeTanX = max(abs(tanHalfFovX), 0.0001);
    float safeTanY = max(abs(tanHalfFovY), 0.0001);
    if (ray.z <= 0.0001)
        return fallbackUV;
    float2 ndc;
    ndc.x = (ray.x / ray.z) / safeTanX;
    ndc.y = (ray.y / ray.z) / safeTanY;
    return float2((ndc.x + 1.0) * 0.5, 1.0 - (ndc.y + 1.0) * 0.5);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= width || dtid.y >= height)
        return;

    float2 uv = (float2(dtid.xy) + 0.5) / float2(width, height);

    float2 clipPos;
    clipPos.x = uv.x * 2.0 - 1.0;
    clipPos.y = 1.0 - uv.y * 2.0;

    float safeTanX = max(abs(tanHalfFovX), 0.0001);
    float safeTanY = max(abs(tanHalfFovY), 0.0001);
    float3 ray = normalize(float3(clipPos.x * safeTanX, clipPos.y * safeTanY, 1.0));

    float2 undistortedToDistortedUV = ProjectRayToUV(RotateRay(ray, -deltaYaw, -deltaPitch), uv);
    float2 distortedToUndistortedUV = ProjectRayToUV(RotateRay(ray, deltaYaw, deltaPitch), uv);

    float2 distortedToUndistorted = distortedToUndistortedUV - uv;
    float2 undistortedToDistorted = undistortedToDistortedUV - uv;
    DistortionOutput[dtid.xy] = float4(distortedToUndistorted, undistortedToDistorted);
}
)";


// ============================================================================
// Mouse-Warp Debug Visualization Shader
// ============================================================================
// Shows the per-pixel warp displacement and disocclusion mask for the
// mouse-based frame warp. This is the primary debug view for Frame Warp.
//
// Visualization:
// - Red channel: horizontal displacement (pixels)
// - Green channel: vertical displacement (pixels)
// - Blue channel: disocclusion mask (1.0 = pixel samples outside frame)
// - Background: dimmed original frame (30% opacity)
// - Overlay: displacement heatmap (70% opacity)
//
// The disocclusion mask identifies pixels whose warped source UV falls
// outside the [0,1] range. These are "holes" in the warped frame where
// new content should appear but we have no data. FSRFG handles these
// via inpainting; standalone warp uses edge clamping (visible smearing).
inline static std::string frameWarpDebugDisplacementCode = R"(
cbuffer WarpParams : register(b0)
{
 float4x4 clipToClipWarp;
 float4x4 clipToClipWarpInv;
 uint width;
 uint height;
 float warpStrength;
 uint depthAware;
 float tanHalfFovX;
 float tanHalfFovY;
 float deltaYaw;
 float deltaPitch;
};

Texture2D<float4> SourceColor : register(t0);
Texture2D<float> SourceDepth : register(t1);
RWTexture2D<float4> DebugOutput : register(u0);
SamplerState PointClamp : register(s0);

float3 RotateRayToSource(float3 ray)
{
 float sy = sin(deltaYaw);
 float cy = cos(deltaYaw);
 float sp = sin(deltaPitch);
 float cp = cos(deltaPitch);
 float3 pitched = float3(ray.x, ray.y * cp - ray.z * sp, ray.y * sp + ray.z * cp);
 return float3(pitched.x * cy + pitched.z * sy, pitched.y, -pitched.x * sy + pitched.z * cy);
}

float2 SourceUVFromPixel(float2 pixelCenter)
{
 float2 ndc;
 ndc.x = (pixelCenter.x / width) * 2.0 - 1.0;
 ndc.y = 1.0 - (pixelCenter.y / height) * 2.0;
 float safeTanX = max(abs(tanHalfFovX), 0.0001);
 float safeTanY = max(abs(tanHalfFovY), 0.0001);
 float3 outputRay = normalize(float3(ndc.x * safeTanX, ndc.y * safeTanY, 1.0));
 float3 sourceRay = RotateRayToSource(outputRay);
 if (sourceRay.z <= 0.0001)
 return pixelCenter / float2(width, height);
 float2 sourceNdc;
 sourceNdc.x = (sourceRay.x / sourceRay.z) / safeTanX;
 sourceNdc.y = (sourceRay.y / sourceRay.z) / safeTanY;
 return float2((sourceNdc.x + 1.0) * 0.5, 1.0 - (sourceNdc.y + 1.0) * 0.5);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
 if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
 return;

 float2 pixelCenter = float2(dispatchThreadID.xy) + 0.5;
 float2 sourceUV = SourceUVFromPixel(pixelCenter);

 // Compute displacement in pixels
 float2 originalUV = pixelCenter / float2(width, height);
 float2 displacementPixels = (sourceUV - originalUV) * float2(width, height);

 // Disocclusion mask: pixels whose source UV falls outside [0,1]
 // These are the "holes" that need inpainting
 bool isDisoccluded = (sourceUV.x < 0.0 || sourceUV.x > 1.0 ||
 sourceUV.y < 0.0 || sourceUV.y > 1.0);

 // Scale displacement for visibility (warp is typically < 10 pixels)
 float2 visDisp = displacementPixels * 0.2; // Scale factor for visibility

 // Build the debug color
 float3 warpColor = float3(
 saturate(abs(visDisp.x) * 2.0), // Red: horizontal displacement
 saturate(abs(visDisp.y) * 2.0), // Green: vertical displacement
 isDisoccluded ? 1.0 : saturate(length(visDisp) * 0.5) // Blue: disocclusion or magnitude
 );

 // Original frame as dimmed background
 float4 originalColor = SourceColor.Load(int3(dispatchThreadID.xy, 0));
 float3 dimmedOriginal = originalColor.rgb * 0.3;

 // Blend: 70% warp visualization + 30% dimmed original
 float3 result = lerp(dimmedOriginal, warpColor, 0.7);

 // Add a bright border at disocclusion edges for visibility
 // Check if any neighbor is disoccluded while this pixel is not
 // (this highlights the disocclusion boundary)
 if (!isDisoccluded)
 {
 // Check 4-connected neighbors
 float2 neighborUV;
 bool anyNeighborDisoccluded = false;

 // Right neighbor
 neighborUV = SourceUVFromPixel(float2(pixelCenter.x + 1.0, pixelCenter.y));
 anyNeighborDisoccluded = anyNeighborDisoccluded ||
 (neighborUV.x < 0.0 || neighborUV.x > 1.0 || neighborUV.y < 0.0 || neighborUV.y > 1.0);

 // Bottom neighbor
 neighborUV = SourceUVFromPixel(float2(pixelCenter.x, pixelCenter.y + 1.0));
 anyNeighborDisoccluded = anyNeighborDisoccluded ||
 (neighborUV.x < 0.0 || neighborUV.x > 1.0 || neighborUV.y < 0.0 || neighborUV.y > 1.0);

 if (anyNeighborDisoccluded)
 result = float3(1.0, 1.0, 0.0); // Yellow border at disocclusion edge
 }

DebugOutput[dispatchThreadID.xy] = float4(result, 1.0);
}
)";

inline static std::string frameWarpDebugInfillMaskCode = R"(
cbuffer WarpParams : register(b0)
{
 float4x4 clipToClipWarp;
 float4x4 clipToClipWarpInv;
 uint width;
 uint height;
 float warpStrength;
 uint depthAware;
 float tanHalfFovX;
 float tanHalfFovY;
 float deltaYaw;
 float deltaPitch;
 uint stableUiSceneHasUnderlay;
 uint depthInverted;
 uint infillHistoryAvailable;
 float infillMaskCoverageEstimate;
};

Texture2D<float4> SourceColor : register(t0);
Texture2D<float> SourceDepth : register(t1);
RWTexture2D<float4> DebugOutput : register(u0);

float3 RotateRayToSource(float3 ray)
{
 float sy = sin(deltaYaw);
 float cy = cos(deltaYaw);
 float sp = sin(deltaPitch);
 float cp = cos(deltaPitch);
 float3 pitched = float3(ray.x, ray.y * cp - ray.z * sp, ray.y * sp + ray.z * cp);
 return float3(pitched.x * cy + pitched.z * sy, pitched.y, -pitched.x * sy + pitched.z * cy);
}

float2 SourceUVFromPixel(float2 pixelCenter)
{
 float2 ndc;
 ndc.x = (pixelCenter.x / width) * 2.0 - 1.0;
 ndc.y = 1.0 - (pixelCenter.y / height) * 2.0;
 float safeTanX = max(abs(tanHalfFovX), 0.0001);
 float safeTanY = max(abs(tanHalfFovY), 0.0001);
 float3 outputRay = normalize(float3(ndc.x * safeTanX, ndc.y * safeTanY, 1.0));
 float3 sourceRay = RotateRayToSource(outputRay);
 if (sourceRay.z <= 0.0001)
  return pixelCenter / float2(width, height);
 float2 sourceNdc;
 sourceNdc.x = (sourceRay.x / sourceRay.z) / safeTanX;
 sourceNdc.y = (sourceRay.y / sourceRay.z) / safeTanY;
 return float2((sourceNdc.x + 1.0) * 0.5, 1.0 - (sourceNdc.y + 1.0) * 0.5);
}

float DepthAtUV(float2 uv)
{
 uint depthWidth;
 uint depthHeight;
 SourceDepth.GetDimensions(depthWidth, depthHeight);
 if (depthAware == 0 || depthWidth == 0 || depthHeight == 0)
  return depthInverted != 0 ? 0.0 : 1.0;
 float2 safeUV = clamp(uv, float2(0.0, 0.0), float2(1.0, 1.0));
 int2 p = int2(safeUV * float2(depthWidth, depthHeight));
 p = clamp(p, int2(0, 0), int2(int(depthWidth) - 1, int(depthHeight) - 1));
 return SourceDepth.Load(int3(p, 0));
}

float LocalDepthEdgeMask(float2 outputUV, float2 displacementPixels)
{
 float displacementLength = length(displacementPixels);
 if (depthAware == 0 || displacementLength < 0.35)
  return 0.0;
 float2 dirPixels = displacementPixels / max(displacementLength, 1e-3);
 float2 dirUv = dirPixels / float2(width, height);
 float center = DepthAtUV(outputUV);
 float forward2 = DepthAtUV(outputUV + dirUv * 2.0);
 float back2 = DepthAtUV(outputUV - dirUv * 2.0);
 float forward5 = DepthAtUV(outputUV + dirUv * 5.0);
 float back5 = DepthAtUV(outputUV - dirUv * 5.0);
 float edge = max(max(abs(center - forward2), abs(center - back2)),
                  max(abs(center - forward5), abs(center - back5)));
 float threshold = max(0.0015, abs(center) * 0.0015);
 float motionGate = saturate((displacementLength - 0.75) / 12.0);
 return saturate((edge - threshold) * 120.0) * motionGate;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
 if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
  return;

 float2 pixelCenter = float2(dispatchThreadID.xy) + 0.5;
 float2 outputUV = pixelCenter / float2(width, height);
 float2 sourceUV = SourceUVFromPixel(pixelCenter);
 float2 displacementPixels = (sourceUV - outputUV) * float2(width, height);
 float outOfBounds =
  (sourceUV.x < 0.0 || sourceUV.x > 1.0 || sourceUV.y < 0.0 || sourceUV.y > 1.0) ? 1.0 : 0.0;
 float edgeMask = LocalDepthEdgeMask(outputUV, displacementPixels);
 float infillMask = saturate(max(outOfBounds, edgeMask * 0.65));

 float4 originalColor = SourceColor.Load(int3(dispatchThreadID.xy, 0));
 float3 dimmedOriginal = originalColor.rgb * 0.25;
 float3 warpColor = float3(saturate(abs(displacementPixels.x) * 0.08),
                           saturate(abs(displacementPixels.y) * 0.08),
                           infillMask);
 float3 result = lerp(dimmedOriginal, warpColor, 0.78);
 if (infillMask > 0.35)
  result = lerp(result, float3(0.1, 0.55, 1.0), 0.65);
 DebugOutput[dispatchThreadID.xy] = float4(result, 1.0);
}
)";

// Distortion field visualization shader - reads R16G16_FLOAT distortion field
// and renders it as a color visualization (R=horizontal, G=vertical, B=magnitude)
inline static std::string frameWarpDistortionVisCode = R"(
cbuffer DistortionVisParams : register(b0)
{
    uint width;
    uint height;
    float maxDisplacement; // Scale factor for visualization (pixels)
    uint pad;
};

Texture2D<float2> DistortionField : register(t0);
RWTexture2D<float4> VisOutput : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;

    // Read distortion field value (in pixels)
    float2 distortion = DistortionField.Load(int3(dispatchThreadID.xy, 0));

    // Scale for visibility - distortion values are typically < 10 pixels
    float2 visDist = distortion / max(maxDisplacement, 1.0f);

    // Build visualization color
    float3 color = float3(
        saturate(abs(visDist.x) * 2.0),  // Red: horizontal displacement
        saturate(abs(visDist.y) * 2.0),  // Green: vertical displacement
        saturate(length(visDist) * 0.5)   // Blue: magnitude
    );

    // Disocclusion: pixels with very large displacement
    bool isDisoccluded = length(distortion) > maxDisplacement * 0.8;
    if (isDisoccluded)
        color = float3(1.0, 0.3, 0.3); // Red tint for disoccluded regions

    // Dim background where there's no displacement
    float magnitude = length(distortion);
    if (magnitude < 0.5)
        color *= 0.15; // Very dim for near-zero displacement

    VisOutput[dispatchThreadID.xy] = float4(color, 1.0);
}
)";

// Shader compilation helper
inline static ID3DBlob* FrameWarp_CompileShader(
    const char* shaderCode,
    const char* entryPoint,
    const char* target)
{
    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    HRESULT hr = D3DCompile(
        shaderCode,
        strlen(shaderCode),
        nullptr,
        nullptr,
        nullptr,
        entryPoint,
        target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shaderBlob,
        &errorBlob
    );

    if (FAILED(hr))
    {
        LOG_ERROR("FrameWarp shader compilation failed");
        if (errorBlob)
        {
            LOG_ERROR("Shader compile error: {}", (char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        if (shaderBlob) shaderBlob->Release();
        return nullptr;
    }

    if (errorBlob) errorBlob->Release();
    return shaderBlob;
}
