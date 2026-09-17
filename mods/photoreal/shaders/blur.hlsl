#include "common.hlsli"

cbuffer Blur : register(b1) {
    float2 direction;
    float2 unused;
};

float4 ps_main(BridgerScreenPixel i) : SV_TARGET {
    return float4(photoreal_blur9(bridger_texture0, i.uv, direction * bridger_resolution.zw), 1.0);
}
