#include "common.hlsli"

float4 ps_main(BridgerScreenPixel i) : SV_TARGET {
    const float2 px = bridger_resolution.zw;
    float3 c = bridger_input.SampleLevel(bridger_linear, i.uv + px * float2(-1.0, -1.0), 0).rgb;
    c += bridger_input.SampleLevel(bridger_linear, i.uv + px * float2(1.0, -1.0), 0).rgb;
    c += bridger_input.SampleLevel(bridger_linear, i.uv + px * float2(-1.0, 1.0), 0).rgb;
    c += bridger_input.SampleLevel(bridger_linear, i.uv + px * float2(1.0, 1.0), 0).rgb;
    return float4(c * 0.25, 1.0);
}
