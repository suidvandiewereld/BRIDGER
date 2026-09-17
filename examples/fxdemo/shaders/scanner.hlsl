
cbuffer Scanner : register(b1) {
    float radius;
    float strength;
    float2 unused;
    float2 center;
    float2 unused2;
};

float4 ps_main(BridgerPixel i) : SV_TARGET {
    float2 uv = i.position.xy * bridger_resolution.zw;
    float aspect = bridger_resolution.x / bridger_resolution.y;
    float2 d = (uv - center) * float2(aspect, 1.0);
    float r = length(d);
    float inside = 1.0 - smoothstep(radius - 0.004, radius, r);
    if (inside <= 0.0) {
        discard;
    }

    float ripple = sin(r * 60.0 - bridger_time.x * 4.0);
    float2 offset = normalize(d + 1e-5) * (0.006 * strength) * ripple / float2(aspect, 1.0);
    float3 c;
    c.r = bridger_scene.SampleLevel(bridger_linear, uv + offset, 0).r;
    c.g = bridger_scene.SampleLevel(bridger_linear, uv, 0).g;
    c.b = bridger_scene.SampleLevel(bridger_linear, uv - offset, 0).b;

    float scan = 0.85 + 0.15 * sin(i.position.y * 1.5 + bridger_time.x * 20.0);
    float edge = smoothstep(radius - 0.02, radius - 0.003, r);
    float sweep = saturate(1.0 - abs(frac(bridger_time.x * 0.5) * 2.0 * radius - r) * 40.0);
    c = c * scan * float3(0.75, 1.0, 0.92) + (edge * 0.6 + sweep * 0.35) * float3(0.4, 1.0, 0.85);
    return float4(c, inside);
}
