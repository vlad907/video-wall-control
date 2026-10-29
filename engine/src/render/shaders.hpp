// Shader source shared by the Metal and Direct3D 11 backends.
//
// kShaderCommon is written in the subset of syntax HLSL and MSL share
// (float2/float3/float4, uint, saturate, abs, floor, fmod...). The MSL
// prelude maps the two HLSL-only names it uses (lerp, frac). Each backend
// wraps it with its own entry points and resource bindings, so the crop /
// scale / rotation maths and the test pattern are identical on both.
#pragma once

namespace mosaic::shaders {

inline constexpr const char* kShaderCommon = R"SHADER(
struct Uniforms {
    float4 dstRect;   // viewport x, y, w, h (target pixels)
    float4 crop;      // sampled region x, y, w, h (source pixels)
    float4 sizes;     // source w, source h, dst w, dst h
    float4 bg;        // background colour
    float4 fparams;   // time, tile w, tile h, -
    uint4  iparams;   // frame id, rotation, scale mode, flags
};

// ---------------------------------------------------------------------------
// Output mapping
// ---------------------------------------------------------------------------
// uv: position in the output (0..1, y down). Returns the position inside the
// crop rectangle (xy, 0..1) and whether it is inside the picture (z).
// Rotation is clockwise: rotating a crop 90 degrees makes its left edge the top.
float3 mapOutputUV(float2 uv, uint rot, uint mode, float2 cropSize, float2 outSize) {
    bool sideways = (rot == 90u || rot == 270u);
    float contentAspect = sideways ? cropSize.y / cropSize.x : cropSize.x / cropSize.y;
    float outAspect = outSize.x / outSize.y;
    float2 p = uv;
    if (mode == 1u) {
        // fit: whole picture visible, letterbox / pillarbox
        if (contentAspect > outAspect) p.y = (uv.y - 0.5) * (contentAspect / outAspect) + 0.5;
        else                           p.x = (uv.x - 0.5) * (outAspect / contentAspect) + 0.5;
    } else if (mode == 2u) {
        // fill: cover the output, cut off the overflow
        if (contentAspect > outAspect) p.x = (uv.x - 0.5) * (outAspect / contentAspect) + 0.5;
        else                           p.y = (uv.y - 0.5) * (contentAspect / outAspect) + 0.5;
    }
    float inside = (p.x >= 0.0 && p.x <= 1.0 && p.y >= 0.0 && p.y <= 1.0) ? 1.0 : 0.0;
    float2 c = p;
    if (rot == 90u)       c = float2(p.y, 1.0 - p.x);
    else if (rot == 180u) c = float2(1.0 - p.x, 1.0 - p.y);
    else if (rot == 270u) c = float2(1.0 - p.y, p.x);
    return float3(c.x, c.y, inside);
}

// ---------------------------------------------------------------------------
// Test pattern
// ---------------------------------------------------------------------------
float3 hue(float h) {
    float3 k = abs(frac(float3(h, h, h) + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0;
    return saturate(k);
}

float segDist(float2 p, float2 a, float2 b) {
    float2 pa = p - a;
    float2 ba = b - a;
    float t = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * t);
}

uint digitMask(uint d) {
    return d == 0u ? 0x3Fu : d == 1u ? 0x06u : d == 2u ? 0x5Bu : d == 3u ? 0x4Fu : d == 4u ? 0x66u :
           d == 5u ? 0x6Du : d == 6u ? 0x7Du : d == 7u ? 0x07u : d == 8u ? 0x7Fu : 0x6Fu;
}

// Seven-segment digit; p is in digit space (x 0..1, y 0..2, y down).
float digit(uint d, float2 p) {
    uint m = digitMask(d);
    float dist = 10.0;
    if ((m & 1u)  != 0u) dist = min(dist, segDist(p, float2(0.0, 0.0), float2(1.0, 0.0)));
    if ((m & 2u)  != 0u) dist = min(dist, segDist(p, float2(1.0, 0.0), float2(1.0, 1.0)));
    if ((m & 4u)  != 0u) dist = min(dist, segDist(p, float2(1.0, 1.0), float2(1.0, 2.0)));
    if ((m & 8u)  != 0u) dist = min(dist, segDist(p, float2(0.0, 2.0), float2(1.0, 2.0)));
    if ((m & 16u) != 0u) dist = min(dist, segDist(p, float2(0.0, 1.0), float2(0.0, 2.0)));
    if ((m & 32u) != 0u) dist = min(dist, segDist(p, float2(0.0, 0.0), float2(0.0, 1.0)));
    if ((m & 64u) != 0u) dist = min(dist, segDist(p, float2(0.0, 1.0), float2(1.0, 1.0)));
    return dist < 0.13 ? 1.0 : 0.0;
}

float numberWidth(float h, uint count) { return h * 0.8 * float(count) - h * 0.3; }

// `count` digits of `value`, top-left at origin, glyph height h pixels.
float number(float2 px, float2 origin, float h, uint value, uint count) {
    float adv = h * 0.8;
    float2 q = px - origin;
    if (q.x < -h * 0.1 || q.y < -h * 0.1 || q.x > adv * float(count) || q.y > h * 1.1) return 0.0;
    float idx = floor(q.x / adv);
    uint skip = count - 1u - uint(idx);
    uint v = value;
    for (uint i = 0u; i < skip; i++) v = v / 10u;
    float2 local = float2(q.x - idx * adv, q.y) / (h * 0.5);
    return digit(v % 10u, local);
}

bool inBox(float2 p, float2 lo, float2 hi) { return p.x >= lo.x && p.y >= lo.y && p.x < hi.x && p.y < hi.y; }

// px: pixel position in the source; size: source size; tile: tile size.
float4 testPattern(float2 px, float2 size, float2 tile, uint frameId, float time) {
    float2 t = floor(px / tile);
    float cols = ceil(size.x / tile.x);
    uint tileId = uint(t.y * cols + t.x) + 1u;
    float2 local = px - t * tile;
    float3 base = hue(frac(float(tileId) * 0.161 + 0.03));
    float3 col = base * 0.32;

    // 60 px grid, brighter every 240 px (count squares to check scaling)
    float2 m60 = fmod(local, 60.0);
    float2 m240 = fmod(local, 240.0);
    if (m60.x < 1.0 || m60.y < 1.0) col = lerp(col, float3(1.0, 1.0, 1.0), 0.25);
    if (m240.x < 2.0 || m240.y < 2.0) col = lerp(col, float3(1.0, 1.0, 1.0), 0.55);

    // tile border in the tile's colour
    if (local.x < 6.0 || local.y < 6.0 || local.x >= tile.x - 6.0 || local.y >= tile.y - 6.0) col = base;

    // corner markers: top-left red, top-right green, bottom-left blue, bottom-right white
    float cm = 96.0;
    float inset = 12.0;
    if (inBox(local, float2(inset, inset), float2(inset + cm, inset + cm))) col = float3(1.0, 0.0, 0.0);
    if (inBox(local, float2(tile.x - inset - cm, inset), float2(tile.x - inset, inset + cm))) col = float3(0.0, 1.0, 0.0);
    if (inBox(local, float2(inset, tile.y - inset - cm), float2(inset + cm, tile.y - inset))) col = float3(0.1, 0.35, 1.0);
    if (inBox(local, float2(tile.x - inset - cm, tile.y - inset - cm), float2(tile.x - inset, tile.y - inset))) col = float3(1.0, 1.0, 1.0);

    // "up" arrow
    float2 a = local - float2(tile.x * 0.5, 40.0);
    if (a.y >= 0.0 && a.y <= 150.0 && abs(a.x) <= a.y * 0.55) col = float3(1.0, 0.85, 0.0);

    // big tile number
    float h = tile.y * 0.3;
    uint nd = tileId >= 100u ? 3u : (tileId >= 10u ? 2u : 1u);
    float2 nOrigin = float2(tile.x * 0.5 - numberWidth(h, nd) * 0.5, tile.y * 0.5 - h * 0.62);
    if (number(local, nOrigin, h, tileId, nd) > 0.5) col = float3(1.0, 1.0, 1.0);

    // tile origin in source pixels: x left, y right
    float sh = tile.y * 0.05;
    float cy = tile.y * 0.5 + h * 0.55;
    uint ox = uint(t.x * tile.x);
    uint oy = uint(t.y * tile.y);
    float cw = numberWidth(sh, 5u);
    if (number(local, float2(tile.x * 0.5 - cw - sh, cy), sh, ox, 5u) > 0.5) col = float3(0.75, 1.0, 0.75);
    if (number(local, float2(tile.x * 0.5 + sh, cy), sh, oy, 5u) > 0.5) col = float3(0.75, 0.85, 1.0);

    // frame counter (same number on every output rendered from one frame)
    float fh = tile.y * 0.06;
    float fy = tile.y - 150.0 - fh;
    if (number(local, float2(tile.x * 0.5 - numberWidth(fh, 7u) * 0.5, fy), fh, frameId % 10000000u, 7u) > 0.5)
        col = float3(1.0, 1.0, 0.4);

    // frame id as 24 bits (LSB on the right), machine-readable
    float bs = 22.0;
    float2 bOrigin = float2(tile.x * 0.5 - 12.0 * (bs + 4.0), tile.y - 110.0);
    float2 bq = local - bOrigin;
    if (bq.y >= 0.0 && bq.y < bs && bq.x >= 0.0 && bq.x < 24.0 * (bs + 4.0)) {
        float bi = floor(bq.x / (bs + 4.0));
        if (bq.x - bi * (bs + 4.0) < bs) {
            uint bit = (frameId >> (23u - uint(bi))) & 1u;
            col = bit != 0u ? float3(1.0, 1.0, 1.0) : float3(0.12, 0.12, 0.12);
        }
    }

    // circle centred on the whole source (stays round only if aspect is preserved)
    float r = min(size.x, size.y) * 0.42;
    if (abs(length(px - size * 0.5) - r) < 3.0) col = float3(0.95, 0.95, 0.95);

    // moving bar: crosses the whole source every 4 s (motion / sync check)
    float barX = frac(time / 4.0) * size.x;
    if (abs(px.x - barX) < 16.0) col = lerp(col, float3(1.0, 1.0, 1.0), 0.8);

    return float4(col, 1.0);
}
)SHADER";

// ---------------------------------------------------------------------------
// Metal Shading Language wrapper
// ---------------------------------------------------------------------------
inline constexpr const char* kMetalPrelude = R"SHADER(
#include <metal_stdlib>
using namespace metal;
#define lerp mix
#define frac fract
)SHADER";

inline constexpr const char* kMetalEntryPoints = R"SHADER(
struct VOut { float4 pos [[position]]; };

vertex VOut vs_main(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));
    VOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

fragment float4 ps_pattern(VOut in [[stage_in]], constant Uniforms& u [[buffer(0)]]) {
    return testPattern(in.pos.xy - u.dstRect.xy, u.dstRect.zw, u.fparams.yz, u.iparams.x, u.fparams.x);
}

fragment float4 ps_textured(VOut in [[stage_in]], constant Uniforms& u [[buffer(0)]],
                            texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    if ((u.iparams.w & 1u) != 0u) return float4(u.bg.rgb, 1.0);  // solid colour
    float2 uv = (in.pos.xy - u.dstRect.xy) / u.dstRect.zw;
    float3 m = mapOutputUV(uv, u.iparams.y, u.iparams.z, u.crop.zw, u.dstRect.zw);
    float2 suv = (u.crop.xy + m.xy * u.crop.zw) / u.sizes.xy;
    if (m.z < 0.5 || suv.x < 0.0 || suv.y < 0.0 || suv.x > 1.0 || suv.y > 1.0) return float4(u.bg.rgb, 1.0);
    return float4(tex.sample(smp, suv).rgb, 1.0);
}
)SHADER";

// ---------------------------------------------------------------------------
// HLSL (Shader Model 5) wrapper
// ---------------------------------------------------------------------------
inline constexpr const char* kHlslEntryPoints = R"SHADER(
cbuffer CB : register(b0) { Uniforms u; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);

float4 vs_main(uint vid : SV_VertexID) : SV_Position {
    float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}

float4 ps_pattern(float4 pos : SV_Position) : SV_Target {
    return testPattern(pos.xy - u.dstRect.xy, u.dstRect.zw, u.fparams.yz, u.iparams.x, u.fparams.x);
}

float4 ps_textured(float4 pos : SV_Position) : SV_Target {
    if ((u.iparams.w & 1u) != 0u) return float4(u.bg.rgb, 1.0);
    float2 uv = (pos.xy - u.dstRect.xy) / u.dstRect.zw;
    float3 m = mapOutputUV(uv, u.iparams.y, u.iparams.z, u.crop.zw, u.dstRect.zw);
    float2 suv = (u.crop.xy + m.xy * u.crop.zw) / u.sizes.xy;
    if (m.z < 0.5 || suv.x < 0.0 || suv.y < 0.0 || suv.x > 1.0 || suv.y > 1.0) return float4(u.bg.rgb, 1.0);
    return float4(tex.Sample(smp, suv).rgb, 1.0);
}
)SHADER";

}  // namespace mosaic::shaders
