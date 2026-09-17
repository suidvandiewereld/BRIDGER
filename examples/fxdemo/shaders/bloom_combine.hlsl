
cbuffer Bloom : register(b1) {
    float threshold;
    float intensity;
    float radius;
    float unused;
    float2 direction;
    float2 unused2;
};

float3 tent(float2 uv) {
    float width;
    float height;
    bridger_texture0.GetDimensions(width, height);
    float2 t = radius / float2(width, height);

    float3 sum = bridger_texture0.SampleLevel(bridger_linear, uv, 0).rgb * 4.0;
    sum += bridger_texture0.SampleLevel(bridger_linear, uv + float2(-t.x, 0.0), 0).rgb * 2.0;
    sum += bridger_texture0.SampleLevel(bridger_linear, uv + float2( t.x, 0.0), 0).rgb * 2.0;
    sum += bridger_texture0.SampleLevel(bridger_linear, uv + float2(0.0, -t.y), 0).rgb * 2.0;
    sum += bridger_texture0.SampleLevel(bridger_linear, uv + float2(0.0,  t.y), 0).rgb * 2.0;
    sum += bridger_texture0.SampleLevel(bridger_linear, uv + float2(-t.x, -t.y), 0).rgb;
    sum += bridger_texture0.SampleLevel(bridger_linear, uv + float2( t.x, -t.y), 0).rgb;
    sum += bridger_texture0.SampleLevel(bridger_linear, uv + float2(-t.x,  t.y), 0).rgb;
    sum += bridger_texture0.SampleLevel(bridger_linear, uv + float2( t.x,  t.y), 0).rgb;
    return sum * (1.0 / 16.0);
}

float4 ps_main(BridgerScreenPixel i) : SV_TARGET {
    float3 scene = bridger_input.SampleLevel(bridger_point, i.uv, 0).rgb;
    return float4(scene + tent(i.uv) * intensity, 1.0);
}
