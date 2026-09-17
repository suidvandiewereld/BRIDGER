#include "common.hlsli"

cbuffer Grade : register(b1) {
    float exposure;
    float saturation;
    float temperature;
    float vignette;
    float grain;
    float fog_density;
    float fog_start;
    float unused;
    float4 fog_color;
};

float4 ps_main(BridgerScreenPixel i) : SV_TARGET {
    float3 c = bridger_input.SampleLevel(bridger_point, i.uv, 0).rgb;

    c *= exp2(exposure);
    c = fxdemo_temperature(c, temperature);
    float luma = bridger_luminance(c);
    c = lerp(luma.xxx, c, saturation);

    if (bridger_depth.w > 0.5 && fog_density > 0.0) {
        float metres = bridger_scene_linear_depth_filtered(i.uv);
        float amount = 1.0 - exp(-max(metres - fog_start, 0.0) * fog_density);
        c = lerp(c, fog_color.rgb, saturate(amount) * fog_color.a);
    }

    float2 p = i.uv * 2.0 - 1.0;
    c *= saturate(1.0 - dot(p, p) * vignette * 0.5);

    float shade = 1.0 - saturate(bridger_luminance(c));
    float noise = bridger_hash(i.uv * bridger_resolution.xy + bridger_time.x * 61.7) - 0.5;
    c += noise * grain * shade * shade;

    return float4(c, 1.0);
}
