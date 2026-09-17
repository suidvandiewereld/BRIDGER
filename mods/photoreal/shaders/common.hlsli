#ifndef PHOTOREAL_COMMON
#define PHOTOREAL_COMMON

static const float3 kLumaWeights = float3(0.2126, 0.7152, 0.0722);

float3 photoreal_blur9(Texture2D source, float2 uv, float2 direction) {
    static const float weights[5] = {0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216};
    float3 acc = source.SampleLevel(bridger_linear, uv, 0).rgb * weights[0];
    [unroll]
    for (int i = 1; i < 5; ++i) {
        acc += source.SampleLevel(bridger_linear, uv + direction * i, 0).rgb * weights[i];
        acc += source.SampleLevel(bridger_linear, uv - direction * i, 0).rgb * weights[i];
    }
    return acc;
}

#endif
