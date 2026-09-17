
cbuffer Bloom : register(b1) {
    float threshold;
    float intensity;
    float radius;
    float unused;
    float2 direction;
    float2 unused2;
};

float3 tap(float2 uv) {
    return bridger_input.SampleLevel(bridger_linear, uv, 0).rgb;
}

float3 karis_average(float3 a, float3 b, float3 c, float3 d) {
    float wa = 1.0 / (1.0 + bridger_luminance(a));
    float wb = 1.0 / (1.0 + bridger_luminance(b));
    float wc = 1.0 / (1.0 + bridger_luminance(c));
    float wd = 1.0 / (1.0 + bridger_luminance(d));
    return (a * wa + b * wb + c * wc + d * wd) / max(wa + wb + wc + wd, 1e-5);
}

float4 ps_main(BridgerScreenPixel i) : SV_TARGET {
    float2 t = bridger_resolution.zw * 2.0;

    float3 a = tap(i.uv + t * float2(-2.0, -2.0));
    float3 b = tap(i.uv + t * float2( 0.0, -2.0));
    float3 c = tap(i.uv + t * float2( 2.0, -2.0));
    float3 d = tap(i.uv + t * float2(-1.0, -1.0));
    float3 e = tap(i.uv + t * float2( 1.0, -1.0));
    float3 f = tap(i.uv + t * float2(-2.0,  0.0));
    float3 g = tap(i.uv);
    float3 h = tap(i.uv + t * float2( 2.0,  0.0));
    float3 j = tap(i.uv + t * float2(-1.0,  1.0));
    float3 k = tap(i.uv + t * float2( 1.0,  1.0));
    float3 l = tap(i.uv + t * float2(-2.0,  2.0));
    float3 m = tap(i.uv + t * float2( 0.0,  2.0));
    float3 n = tap(i.uv + t * float2( 2.0,  2.0));

    float3 colour = karis_average(d, e, j, k) * 0.5;
    colour += karis_average(a, b, f, g) * 0.125;
    colour += karis_average(b, c, g, h) * 0.125;
    colour += karis_average(f, g, l, m) * 0.125;
    colour += karis_average(g, h, m, n) * 0.125;

    float luma = bridger_luminance(colour);
    float knee = max(threshold * 0.5, 1e-4);
    float soft = clamp(luma - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float keep = max(soft, luma - threshold) / max(luma, 1e-4);

    return float4(colour * keep, 1.0);
}
