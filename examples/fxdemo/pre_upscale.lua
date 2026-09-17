local info = fx.upscale_info()
print("NGX render size", info.width, info.height)
fx.pre_upscale_pass([[
float4 ps_main(BridgerScreenPixel i) : SV_TARGET {
    float4 c = bridger_input.SampleLevel(bridger_linear, i.uv, 0);
    float2 motion = bridger_scene_motion.SampleLevel(bridger_point, i.uv, 0);
    float movement = saturate(length(motion * bridger_motion.xy) * 10.0);
    c.rgb *= lerp(float3(1.0, 0.98, 0.96), float3(0.96, 0.98, 1.0), movement);
    return c;
}
]])
