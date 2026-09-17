
#include "common.hlsli"

cbuffer Photoreal : register(b1) {
    float exposure;
    float temperature;
    float shoulder;
    float toe;

    float film_contrast;
    float saturation;
    float sharpness;
    float distance_sharpen;

    float clarity;
    float haze_density;
    float haze_start;
    float haze_brightness;

    float haze_desaturate;
    float glare;
    float highlights;
    float bloom_threshold;

    float vignette;
    float aberration;
    float grain;
    float depth_available;
};

float4 ps_main(BridgerScreenPixel i) : SV_TARGET {
    const float2 uv = i.uv;
    const float2 px = bridger_resolution.zw;
    const float2 dir = uv - 0.5;
    const float r2 = dot(dir, dir);

    const float d = depth_available > 0.5 ? bridger_scene_depth01(uv) : 0.0;
    const float far_weight = smoothstep(0.01, 0.25, d);
    const float sharp = saturate(sharpness + distance_sharpen * far_weight * (1.0 - sharpness));

    const float2 ca = dir * r2 * aberration * 0.006;
    float3 c;
    c.r = bridger_input.SampleLevel(bridger_linear, uv + ca, 0).r;
    c.g = bridger_input.SampleLevel(bridger_linear, uv, 0).g;
    c.b = bridger_input.SampleLevel(bridger_linear, uv - ca, 0).b;

    const float3 n = bridger_input.SampleLevel(bridger_linear, uv - float2(0.0, px.y), 0).rgb;
    const float3 s = bridger_input.SampleLevel(bridger_linear, uv + float2(0.0, px.y), 0).rgb;
    const float3 w = bridger_input.SampleLevel(bridger_linear, uv - float2(px.x, 0.0), 0).rgb;
    const float3 e = bridger_input.SampleLevel(bridger_linear, uv + float2(px.x, 0.0), 0).rgb;
    const float3 lo = min(min(min(n, s), min(w, e)), c);
    const float3 hi = max(max(max(n, s), max(w, e)), c);
    const float3 amplitude = sqrt(saturate(min(lo, 2.0 - hi) / max(hi, 0.0001)));
    const float3 weight = amplitude * (-1.0 / lerp(8.0, 5.0, sharp));
    c = saturate((weight * (n + s + w + e) + c) / (4.0 * weight + 1.0));

    const float3 low = bridger_texture0.SampleLevel(bridger_linear, uv, 0).rgb;
    const float luma_high = bridger_luminance(c);
    const float luma_low = bridger_luminance(low);
    const float k = 1.0 + clarity * (luma_high - luma_low) / (luma_low + 0.15);
    c *= clamp(k, 0.6, 1.5);

    if (depth_available > 0.5) {
        float3 sky = bridger_texture0.SampleLevel(bridger_linear, float2(0.25, 0.2), 0).rgb;
        sky += bridger_texture0.SampleLevel(bridger_linear, float2(0.5, 0.2), 0).rgb;
        sky += bridger_texture0.SampleLevel(bridger_linear, float2(0.75, 0.2), 0).rgb;
        sky *= 0.3333;
        float haze = 1.0 - exp(-max(d - haze_start, 0.0) * haze_density * 2.0);
        haze *= (d > 0.998) ? 0.0 : 1.0;
        haze = saturate(haze);
        const float luma = bridger_luminance(c);
        c = lerp(c, luma.xxx, haze * haze_desaturate);
        c = lerp(c, sky * haze_brightness, haze);
    }

    c += low * glare;
    const float3 bright = max(low - bloom_threshold, 0.0) / (1.0 - bloom_threshold + 0.0001);
    c += bright * highlights * 0.5;

    c *= exp2(exposure);
    const float3 warm = float3(1.06, 1.0, 0.92);
    const float3 cool = float3(0.92, 0.98, 1.06);
    c *= lerp(float3(1.0, 1.0, 1.0), temperature > 0.0 ? warm : cool, abs(temperature));

    c = c * (1.0 + shoulder) / (1.0 + shoulder * c);
    c = c * (1.0 - toe) + toe;
    c = lerp(c, c * c * (3.0 - 2.0 * c), film_contrast);
    const float luma = bridger_luminance(c);
    c = lerp(luma.xxx, c, saturation);

    const float cosa = 1.0 / sqrt(1.0 + r2 * 1.6);
    const float cos4 = cosa * cosa * cosa * cosa;
    c *= lerp(1.0, cos4, vignette);

    const float t = bridger_time.x;
    float g = bridger_hash(i.position.xy + frac(t * 7.31) * 1000.0)
            + bridger_hash(i.position.yx * 1.7 + frac(t * 3.17) * 777.0) - 1.0;
    c += g * grain * (0.35 + 0.65 * (1.0 - luma));
    c += (bridger_hash(i.position.xy) - 0.5) / 255.0;

    return float4(saturate(c), 1.0);
}
