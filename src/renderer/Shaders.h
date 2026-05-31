#pragma once

/// Inline HLSL source — compiled at runtime via D3DCompile.
/// Keeping shaders inline (not separate .hlsl files) means:
///  - no shader file deployment;
///  - shader source ships with the binary;
///  - quick-iteration with /D defines from C++.

namespace shaders {

// View mode constants. Keep in sync with C++ ViewMode enum (DX11Widget.h).
constexpr int VIEW_LIT          = 0;
constexpr int VIEW_MASK_OVERLAY = 1;
constexpr int VIEW_MASK_ONLY    = 2;
constexpr int VIEW_UV_CHECKER   = 3;
constexpr int VIEW_MSTC_PREVIEW = 4;   // L2 reserved, not implemented yet

// Channel view constants. Keep in sync with editor::ChannelView.
// Determines how the mask is decomposed before tinting.
constexpr int CH_VIEW_RGB      = 0;   // composite: tint each of R, G, B by its own color
constexpr int CH_VIEW_R        = 1;   // only the R channel (with R's tint)
constexpr int CH_VIEW_G        = 2;   // only the G channel
constexpr int CH_VIEW_B        = 3;   // only the B channel
constexpr int CH_VIEW_SEMANTIC = 4;   // alias of RGB for the 3D viewport (kind-specific tinting)

// ─────────────────────────────────────────────────────────────────
// Lambert + Mask Overlay shader
//   t0: diffuse texture
//   t1: per-group mask texture (bound by host per draw call)
//   gViewMode  : view mode (one of VIEW_*)
//   gActiveKind: which TextureKind the mask represents
//   gChannelTints[4]: semantic colors for the mask channels (R, G, B, A)
//   _reservedMstc[2]: L2 placeholder (shadow matrix / morph params)
// ─────────────────────────────────────────────────────────────────

inline constexpr const char* kBasicLambertHLSL = R"(

cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
    float3   gLightDir;
    float    _pad0;
    float3   gEyePos;
    float    _pad1;

    // ── Phase 5 additions ───────────────────────────────────
    uint   gViewMode;     // 0 Lit, 1 MaskOverlay, 2 MaskOnly, 3 UvChecker, 4 MstcPreview
    uint   gActiveKind;   // 0 ShadowRate, 1 SubLight, 2 Edge, 3 SDF
    uint   gChannelView;  // 0 RGB, 1 R, 2 G, 3 B, 4 Semantic (alias of RGB)
    uint   _padPF1;

    float4 gChannelTints[4];   // RGB tint per channel; .a unused
    float4 _reservedMstc[2];   // L2 hooks (shadow matrix slots / morph params)

    // ── Phase 6 additions — 3D brush cursor ring ────────────
    float3 gBrushHitWorldPos;
    float  gBrushRadiusWorld;
    uint   gBrushVisible;
    uint   _padPF6_0;
    uint   _padPF6_1;
    uint   _padPF6_2;

    // ── Orbit pivot marker ──────────────────────────────────
    // SCREEN-SPACE dot drawn at the pivot's projected screen position. We
    // use a screen-space radius (in pixels) instead of a world-space radius
    // so the marker stays the same size regardless of zoom / distance, and
    // doesn't bleed onto multiple mesh faces near the pivot.
    float3 gPivotWorldPos;
    float  gPivotRadiusPx;       // marker radius in pixels
    uint   gPivotVisible;
    uint   _padPivot0;
    float2 gViewportSize;        // (width, height) in pixels — for NDC→pixel
};

cbuffer PerMaterial : register(b1)
{
    float4 gDiffuse;
    float3 gAmbient;
    float  _pad2;
    float3 gSpecular;
    float  gShininess;
};

cbuffer PerObject : register(b2)
{
    float4x4 gWorld;
};

Texture2D    tDiffuse  : register(t0);
Texture2D    tMask     : register(t1);
SamplerState sLinear   : register(s0);

struct VSIn
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
};

struct VSOut
{
    float4 svPos     : SV_POSITION;
    float3 worldPos  : POSITION;
    float3 worldNorm : NORMAL;
    float2 uv        : TEXCOORD0;
};

VSOut vs_main(VSIn vin)
{
    VSOut vout;
    float4 worldPos = mul(float4(vin.pos, 1.0), gWorld);
    vout.svPos     = mul(worldPos, gViewProj);
    vout.worldPos  = worldPos.xyz;
    vout.worldNorm = normalize(mul(vin.normal, (float3x3)gWorld));
    vout.uv        = vin.uv;
    return vout;
}

// ── Helpers ────────────────────────────────────────────────────

float3 computeLit(float3 worldNorm, float3 diffRgb)
{
    float3 N = normalize(worldNorm);
    float3 L = normalize(gLightDir);
    float ndl = saturate(dot(N, L));
    float wrapped = ndl * 0.5 + 0.5;     // half-Lambert
    return diffRgb * (gAmbient + (1.0 - gAmbient) * wrapped);
}

float3 uvCheckerColor(float2 uv)
{
    float2 cells = floor(uv * 16.0);
    float odd = fmod(cells.x + cells.y, 2.0);
    float3 a = float3(0.18, 0.20, 0.24);
    float3 b = float3(0.44, 0.46, 0.52);
    return lerp(a, b, odd);
}

float4 ps_main(VSOut pin) : SV_TARGET
{
    float4 diff = tDiffuse.Sample(sLinear, pin.uv) * gDiffuse;
    float3 lit  = computeLit(pin.worldNorm, diff.rgb);

    // Sample the per-group mask. Texture is RGBA8 with alpha forced to 255.
    float4 maskRaw = tMask.Sample(sLinear, pin.uv);

    // Compose mask tint + alpha based on the active channel view. The 3D
    // viewport's channel filter mirrors the 2D canvas's:
    //   RGB      → raw mask values as color (red stays red, green stays green)
    //   R/G/B    → isolate one channel, tinted with that channel's semantic color
    //   Semantic → all channels combined with their semantic tints
    //
    // Color storage uses "full intensity color + separate alpha" semantics
    // (NOT alpha-premultiplied). This is essential for MaskOverlay to look
    // right at brush edges: a pixel painted at half-strength G shows as
    // "half-coverage of bright green" rather than "full coverage of dark
    // green". The latter would mix with the underlying cyan hair to produce
    // a muddy / dark halo around every brush stamp.
    float3 maskColor;   // full-intensity color, never premultiplied
    float  maskA;       // coverage / brush strength

    if (gChannelView == 0u)         // RGB — pass through raw stored color
    {
        maskA     = max(maskRaw.r, max(maskRaw.g, maskRaw.b));
        // Normalize so the dimmest non-zero pixel still displays as fully
        // saturated. Avoids the dark-halo artifact at brush edges.
        maskColor = maskA > 0.001 ? maskRaw.rgb / maskA : float3(0, 0, 0);
    }
    else if (gChannelView == 1u)    // R only
    {
        maskA     = maskRaw.r;
        maskColor = gChannelTints[0].rgb;
    }
    else if (gChannelView == 2u)    // G only
    {
        maskA     = maskRaw.g;
        maskColor = gChannelTints[1].rgb;
    }
    else if (gChannelView == 3u)    // B only
    {
        maskA     = maskRaw.b;
        maskColor = gChannelTints[2].rgb;
    }
    else                            // Semantic — full composite with tints
    {
        float3 tinted = maskRaw.r * gChannelTints[0].rgb
                      + maskRaw.g * gChannelTints[1].rgb
                      + maskRaw.b * gChannelTints[2].rgb;
        maskA     = max(maskRaw.r, max(maskRaw.g, maskRaw.b));
        maskColor = maskA > 0.001 ? tinted / maskA : float3(0, 0, 0);
    }

    // For MaskOnly mode the user expects intensity gradients (so half-
    // strength G shows as dimmer green), so re-premultiply for that path.
    // MaskOverlay uses maskColor + maskA directly — no premultiplication —
    // so the lerp blends real color with real coverage, no muddy edges.
    float3 maskRgbPremul = maskColor * maskA;

    // ── Compute body color per view mode ───────────────────
    float3 bodyRgb;
    float  bodyAlpha = diff.a;
    if (gViewMode == 0u)        // Lit
    {
        bodyRgb = lit;
    }
    else if (gViewMode == 2u)   // Mask Only — premultiplied so dim brush
    {                            // shows as dim, preserving intensity gradient
        bodyRgb   = maskRgbPremul;
        bodyAlpha = 1.0;
    }
    else if (gViewMode == 3u)   // UV Checker — checker as diffuse, with lighting
    {
        float3 chk = uvCheckerColor(pin.uv);
        bodyRgb    = computeLit(pin.worldNorm, chk);
        bodyAlpha  = 1.0;
    }
    else                        // 1 = Mask Overlay (default); 4 = MstcPreview (L2 reserved)
    {
        // Use full-intensity maskColor + maskA. At brush edges the alpha
        // fades but the color stays saturated, so the lerp produces a clean
        // smooth fade from cyan hair → bright green, no dark halo.
        bodyRgb = lerp(lit, maskColor, maskA * 0.85);
    }

    // ── Phase 6 brush cursor ring ───────────────────────────
    // Draw a thin white ring at world-space distance = brush radius from the
    // last hit point. Approximated on the model surface, so the ring "wraps"
    // the geometry exactly where strokes land.
    if (gBrushVisible != 0u && gBrushRadiusWorld > 0.0)
    {
        float d = length(pin.worldPos - gBrushHitWorldPos);
        float ringHalf = max(gBrushRadiusWorld * 0.04, 0.001);
        float ringDist = abs(d - gBrushRadiusWorld);
        if (ringDist < ringHalf)
        {
            // Soft anti-aliased ring (fade by distance from ring centerline)
            float t = 1.0 - (ringDist / ringHalf);
            bodyRgb = lerp(bodyRgb, float3(1.0, 1.0, 1.0), t * 0.85);
        }
    }

    // ── Orbit pivot marker (screen-space dot, depth-tested) ─
    // Project the pivot's world position to screen pixels and check this
    // fragment's screen position against it. Independent of mesh geometry
    // around the pivot — the dot always appears at exactly the right place
    // on the visible surface, never bleeding onto adjacent faces. Depth
    // test hides the dot when the pivot is occluded by geometry between
    // it and the camera (e.g., you orbited to the far side of the model).
    if (gPivotVisible != 0u && gPivotRadiusPx > 0.0)
    {
        float4 pivotClip = mul(float4(gPivotWorldPos, 1.0), gViewProj);
        // Only draw when pivot is in front of the camera (w > 0). Skip
        // when behind us or at the eye plane (would divide by ~0).
        if (pivotClip.w > 0.0001)
        {
            float  pivotDepth  = pivotClip.z / pivotClip.w;   // NDC depth [0,1]
            float2 pivotNdc    = pivotClip.xy / pivotClip.w;
            float2 pivotScreen;
            pivotScreen.x = (pivotNdc.x * 0.5 + 0.5) * gViewportSize.x;
            pivotScreen.y = (1.0 - (pivotNdc.y * 0.5 + 0.5)) * gViewportSize.y;

            float2 d  = pin.svPos.xy - pivotScreen;
            float  ds = length(d);
            // Depth test: this pixel's NDC depth must be >= pivot's depth
            // (with small epsilon for the on-surface case). If pixelDepth <
            // pivotDepth, this pixel's mesh is in FRONT of the pivot →
            // pivot is occluded → don't draw.
            if (ds < gPivotRadiusPx && pin.svPos.z >= pivotDepth - 0.0002)
            {
                float edgeFade = saturate((gPivotRadiusPx - ds)
                                          / max(1.0, gPivotRadiusPx * 0.25));
                float3 pivotColor = float3(1.0, 0.55, 0.10);   // warm orange
                bodyRgb = lerp(bodyRgb, pivotColor, edgeFade * 0.95);
            }
        }
    }

    return float4(bodyRgb, bodyAlpha);
}

)";

} // namespace shaders
