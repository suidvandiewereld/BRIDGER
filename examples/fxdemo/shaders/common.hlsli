#ifndef FXDEMO_COMMON
#define FXDEMO_COMMON

float3 fxdemo_aces(float3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 fxdemo_temperature(float3 c, float amount) {
    float3 warm = float3(1.0, 0.94, 0.84);
    float3 cool = float3(0.84, 0.94, 1.0);
    float3 tint = amount > 0.0 ? warm : cool;
    return c * lerp(float3(1.0, 1.0, 1.0), tint, abs(amount));
}

#endif
