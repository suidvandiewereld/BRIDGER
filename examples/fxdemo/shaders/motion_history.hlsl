float4 ps_main(BridgerScreenPixel i) : SV_TARGET {
    float4 current = bridger_input.SampleLevel(bridger_linear, i.uv, 0);
    if (bridger_motion.z < 0.5 || bridger_motion.w > 0.5) return current;
    float4 previous = bridger_history.SampleLevel(bridger_linear, i.uv, 0);
    return lerp(current, previous, 0.25);
}
