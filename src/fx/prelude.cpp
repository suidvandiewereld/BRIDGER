#include "fx/prelude.h"

namespace bridger::fx {

const char* prelude_source() {
    return R"HLSL(
#ifndef BRIDGER_PRELUDE
#define BRIDGER_PRELUDE 1
// bridger.hlsli - prepended to every shader compiled through the Bridger shader system.
//
// Matrices are row-major (the compiler is invoked with row-major packing), with column vector
// semantics: clip = mul(bridger_view_proj, float4(world, 1)). World space is the engine's:
// metres, Z up, right-handed. Depth is reverse-Z with an infinite far plane: 1 at the near
// plane, tending to 0 with distance.
//
// Register map
//   b0  frame constants (below)             t0  scene colour at the start of the stage
//   b1  yours: declare `cbuffer X : register(b1)`   t1  game depth (when captured), else 1x1
//   b2  object constants (below)            t2  chain input: previous pass output
//   s0  linear clamp    s1 point clamp      t3  previous frame's final image (if requested)
//   s2  linear wrap     s3 point wrap       t4..t7  the effect's four texture slots

cbuffer BridgerFrameConstants : register(b0) {
    float4x4 bridger_view;
    float4x4 bridger_proj;
    float4x4 bridger_view_proj;
    float4x4 bridger_inv_view_proj;
    float4x4 bridger_inv_proj;
    float4 bridger_camera_position;   // xyz world metres; w = 1 when a camera was available
    float4 bridger_camera_forward;
    float4 bridger_camera_up;
    float4 bridger_camera_right;
    float4 bridger_resolution;        // width, height, 1 / width, 1 / height
    float4 bridger_time;              // seconds, frame delta, frame index, unused
    float4 bridger_depth;             // near, far (0 = infinite), reversed (1/0), game depth available (1/0)
    float4 bridger_fov;               // vertical radians, horizontal radians, tan(v / 2), tan(h / 2)
    float4 bridger_motion;            // motion to UV scale, valid, reset
};

cbuffer BridgerObjectConstants : register(b2) {
    float4x4 bridger_model;
    float4 bridger_tint;
};

Texture2D bridger_scene : register(t0);
Texture2D<float> bridger_scene_depth : register(t1);
Texture2D bridger_input : register(t2);
Texture2D bridger_history : register(t3);
Texture2D bridger_texture0 : register(t4);
Texture2D bridger_texture1 : register(t5);
Texture2D bridger_texture2 : register(t6);
Texture2D bridger_texture3 : register(t7);
Texture2D<float2> bridger_scene_motion : register(t8);

SamplerState bridger_linear : register(s0);
SamplerState bridger_point : register(s1);
SamplerState bridger_linear_wrap : register(s2);
SamplerState bridger_point_wrap : register(s3);

float2 bridger_history_uv(float2 uv) {
    return uv + (bridger_motion.z > 0.5
        ? bridger_scene_motion.SampleLevel(bridger_point, uv, 0) * bridger_motion.xy : float2(0, 0));
}
float4 bridger_reproject_history(float2 uv) {
    // The core has already reprojected t3. Do not apply motion a second time.
    return bridger_history.SampleLevel(bridger_linear, uv, 0);
}

struct BridgerVertex {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct BridgerPixel {
    float4 position : SV_POSITION;
    float3 world : TEXCOORD1;     // world metres (world lists) or pixels (screen lists)
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct BridgerScreenPixel {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;        // 0..1 across the target, origin top left
};

// Full-screen triangle, no vertex buffer. The default vertex stage of a post-process pass.
BridgerScreenPixel bridger_fullscreen_vs(uint id : SV_VertexID) {
    BridgerScreenPixel o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// World-space geometry through the live camera.
BridgerPixel bridger_world_vs(BridgerVertex v) {
    BridgerPixel o;
    float4 world = mul(bridger_model, float4(v.position, 1.0));
    o.world = world.xyz;
    o.position = mul(bridger_view_proj, world);
    o.normal = normalize(mul((float3x3)bridger_model, v.normal));
    o.uv = v.uv;
    o.color = v.color * bridger_tint;
    return o;
}

// Screen-space geometry in pixels; z is written as depth (1 = nearest under reverse-Z).
BridgerPixel bridger_screen_vs(BridgerVertex v) {
    BridgerPixel o;
    float4 p = mul(bridger_model, float4(v.position, 1.0));
    o.world = p.xyz;
    float2 ndc = p.xy * bridger_resolution.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);
    o.position = float4(ndc, saturate(p.z), 1.0);
    o.normal = v.normal;
    o.uv = v.uv;
    o.color = v.color * bridger_tint;
    return o;
}

// View-space distance (metres) from a device depth value under the frame's depth convention.
float bridger_linear_depth(float device_depth) {
    float near = bridger_depth.x;
    float far = bridger_depth.y;
    bool reversed = bridger_depth.z > 0.5;
    if (far <= 0.0) {
        // infinite far plane
        return reversed ? near / max(device_depth, 1e-7) : near / max(1.0 - device_depth, 1e-7);
    }
    if (reversed) {
        return near * far / max(near + device_depth * (far - near), 1e-7);
    }
    return near * far / max(far - device_depth * (far - near), 1e-7);
}

// Game depth at a screen uv, as a device value and as metres. Only meaningful when
// bridger_depth.w is 1 (capture enabled in the Shaders tab and a depth buffer was found).
float bridger_scene_device_depth(float2 uv) {
    return bridger_scene_depth.SampleLevel(bridger_point, uv, 0);
}
float bridger_scene_linear_depth(float2 uv) {
    return bridger_linear_depth(bridger_scene_device_depth(uv));
}

// Resolution of the captured depth buffer, which is the resolution the game *renders* at. On
// any upscaling setting that is smaller than the frame you are writing to.
float2 bridger_scene_depth_resolution() {
    float width;
    float height;
    bridger_scene_depth.GetDimensions(width, height);
    return float2(width, height);
}

// Distance in metres, as a 3x3 tent over the depth buffer's own texels.
//
// Use this, not the point-sampled version, whenever depth drives a soft mask: fog, depth of
// field, a distance fade. The depth the game leaves behind is lower resolution than the frame
// and still carries the sub-pixel jitter its own temporal resolve removes from the colour, so
// point sampling it stamps hard, stair-stepped, crawling edges onto an image that had none.
// Averaging in metres spreads the mask over about three render pixels, which is wide enough
// that the jitter moves it by a fraction of its own falloff instead of by its whole width.
// A mask is not a measurement: when you need the actual distance at a pixel, for reconstruction
// or a depth test, use bridger_scene_linear_depth and accept the hard edges.
float bridger_scene_linear_depth_filtered(float2 uv) {
    float2 t = 1.0 / bridger_scene_depth_resolution();
    float sum = 0.0;
    float weight = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
            sum += bridger_linear_depth(bridger_scene_device_depth(uv + t * float2(x, y))) * w;
            weight += w;
        }
    }
    return sum / weight;
}

// Distance as a 0..1 number over a fixed 1000 metre range, saturating beyond it.
//
// This is the convention ReShade effects are written against, so a shader ported from one keeps
// its tuning: a threshold of 0.02 still means twenty metres, and the sky still reads as 1.
float bridger_scene_depth01(float2 uv) {
    return saturate(bridger_scene_linear_depth_filtered(uv) * 0.001);
}

// The same, unfiltered, for a shader that needs the exact value at a pixel.
float bridger_scene_depth01_point(float2 uv) {
    return saturate(bridger_scene_linear_depth(uv) * 0.001);
}

// World position of a pixel from its uv and device depth, through this frame's camera.
float3 bridger_world_from_depth(float2 uv, float device_depth) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 h = mul(bridger_inv_view_proj, float4(ndc, device_depth, 1.0));
    return h.xyz / h.w;
}

// Normalised view ray for a pixel, in world space.
float3 bridger_view_ray(float2 uv) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float3 dir = bridger_camera_forward.xyz
               + bridger_camera_right.xyz * (ndc.x * bridger_fov.w)
               + bridger_camera_up.xyz * (ndc.y * bridger_fov.z);
    return normalize(dir);
}

// Reverse-Z device depth of a world point through this frame's camera, for comparing your own
// geometry against bridger_scene_depth.
float bridger_device_depth(float3 world) {
    float4 clip = mul(bridger_view_proj, float4(world, 1.0));
    return clip.z / max(clip.w, 1e-7);
}

float bridger_luminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float3 bridger_srgb_to_linear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 bridger_linear_to_srgb(float3 c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float bridger_hash(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float bridger_noise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(bridger_hash(i), bridger_hash(i + float2(1, 0)), u.x),
                lerp(bridger_hash(i + float2(0, 1)), bridger_hash(i + float2(1, 1)), u.x), u.y);
}

// Simple lambert term with a fixed key light from above and behind the camera, for
// world-space geometry that wants some shape without a lighting model of its own.
float bridger_simple_shade(float3 normal) {
    float3 key = normalize(float3(0.3, -0.2, 1.0));
    float3 fill = -bridger_camera_forward.xyz;
    float n = saturate(dot(normal, key)) * 0.7 + saturate(dot(normal, fill)) * 0.3;
    return 0.35 + 0.65 * n;
}
#endif
)HLSL";
}

const char* builtin_world_source() {
    return R"HLSL(
float4 ps_main(BridgerPixel i) : SV_TARGET {
    return i.color;
}
)HLSL";
}

const char* builtin_screen_source() {
    return R"HLSL(
float4 ps_main(BridgerPixel i) : SV_TARGET {
    return i.color;
}
)HLSL";
}

const char* builtin_fullscreen_source() {
    return R"HLSL(
float4 ps_main(BridgerScreenPixel i) : SV_TARGET {
    return bridger_input.SampleLevel(bridger_point, i.uv, 0);
}
)HLSL";
}

}
