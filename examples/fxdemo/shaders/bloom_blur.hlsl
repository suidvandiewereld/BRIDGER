
cbuffer Bloom : register(b1) {
    float threshold;
    float intensity;
    float radius;
    float unused;
    float2 direction;
    float2 unused2;
};

static const float weights[5] = {0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162};

float4 ps_main(BridgerScreenPixel i) : SV_TARGET {
    float width;
    float height;
    bridger_texture0.GetDimensions(width, height);
    float2 step = direction * radius / float2(width, height);

    float3 c = bridger_texture0.SampleLevel(bridger_linear, i.uv, 0).rgb * weights[0];
    [unroll]
    for (int k = 1; k < 5; ++k) {
        float2 offset = step * k;
        c += bridger_texture0.SampleLevel(bridger_linear, i.uv + offset, 0).rgb * weights[k];
        c += bridger_texture0.SampleLevel(bridger_linear, i.uv - offset, 0).rgb * weights[k];
    }
    return float4(c, 1.0);
}
