
cbuffer Marker : register(b1) {
    float pulse;
    float rim;
    float2 unused;
    float4 glow_color;
};

float4 ps_main(BridgerPixel i) : SV_TARGET {
    float3 n = normalize(i.normal);
    float3 to_eye = normalize(bridger_camera_position.xyz - i.world);
    float shade = bridger_simple_shade(n);
    float fresnel = pow(1.0 - saturate(dot(n, to_eye)), 3.0) * rim;
    float band = 0.5 + 0.5 * sin(i.world.z * 8.0 - bridger_time.x * 4.0);
    float3 c = i.color.rgb * shade + glow_color.rgb * (fresnel + band * pulse * 0.25);
    return float4(c, i.color.a);
}
