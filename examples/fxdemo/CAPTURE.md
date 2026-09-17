# Capture the shader demo

Use the [golden frame steps](../../captures/golden/README.md) to capture bloom, grade and
the ground grid before changes to the render pipeline. Keep the scene and settings fixed.
The Shaders tab captures after the demo draws and before the Bridger UI draws.

The baseline is a real game image. Shader compilation and generated test BMPs cannot
confirm that the grid aligns with the terrain or that DLSS resolves it without shimmer.

For phase 1, enable `Pre upscale effects` in Shaders, then `Pre upscale grid` under
Shader Demo's World group. This uses opaque world strips with no feathered edges.
Check slow camera turns in DLSS mode, then capture once for the two frame BMP check.
Turn off `Pre upscale effects` to stop the injected work at once.
