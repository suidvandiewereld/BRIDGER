# The shader system

*Mods run their own HLSL on the game's frame.*

The `SCENE`, `POST` and `OVERLAY` stages run on the overlay's DX12 command list at Present. The optional `PRE_UPSCALE` stage runs inside the NGX evaluate call on the game's command list, before DLSS resolves the input colour.

Three kinds of work, freely mixed and ordered:

| Kind | What it is | Typical use |
|---|---|---|
| **Post-process pass** | a full-screen pixel shader over the frame, chained with the other passes in priority order | colour grading, bloom, fog, film effects |
| **World-space draw list** | geometry in engine metres (Z up), pushed every frame and transformed by the live camera | markers, debug lines, volumes, gizmos, meshes |
| **Screen-space draw list** | geometry in pixels | HUD elements, overlays, effects on a region of the screen |

From C++ everything is under `bridger::fx` in [include/bridger/fx.hpp](../include/bridger/fx.hpp), which `mod.hpp` includes. From any language it is the `BridgerFx` table in [include/bridger/api.h](../include/bridger/api.h). `examples/fxdemo` exercises all of it.

**Contents**

1. [A first pass](#a-first-pass)
2. [What every shader gets](#what-every-shader-gets)
3. [Conventions](#conventions)
4. [Stages and ordering](#stages-and-ordering)
5. [Draw lists](#draw-lists)
6. [The camera](#the-camera)
7. [Textures and render targets](#textures-and-render-targets)
8. [Game depth](#game-depth)
9. [The pre-upscale stage](#the-pre-upscale-stage)
10. [Temporal stability](#temporal-stability)
11. [The Shaders tab](#the-shaders-tab)
12. [Checking shaders without the game](#checking-shaders-without-the-game)
13. [Lifetime and threads](#lifetime-and-threads)
14. [The game's pipelines](#the-games-pipelines)
15. [Raytracing](#raytracing)
16. [Limits](#limits)
17. [C ABI](#c-abi)

---

## A first pass

`shaders/tint.hlsl`, next to the mod's DLL:

```hlsl
cbuffer Tint : register(b1) {
    float4 color;
};

float4 ps_main(BridgerScreenPixel i) : SV_TARGET {
    float3 c = bridger_input.SampleLevel(bridger_point, i.uv, 0).rgb;
    return float4(c * color.rgb, 1.0);
}
```

The mod:

```cpp
#include "bridger/mod.hpp"
namespace fx = bridger::fx;

fx::Shader g_shader;
fx::Pass g_pass;

struct alignas(16) Tint { float color[4]; };

bool bridger::on_load() {
    g_shader.load(BRIDGER_FX_SHADER_FULLSCREEN, "shaders/tint.hlsl");
    g_pass.create("tint", g_shader);
    g_pass.constants(Tint{{1.0f, 0.9f, 0.8f, 1.0f}});
    return true;
}
```

There is no vertex shader to write: the prelude supplies a full-screen triangle. `#include` resolves relative to the including file, then the mod folder, then `<game>/Bridger/shaders` for files shared between mods.

Save the file while the game runs and the pass picks up the change. A compile error keeps the previous pipeline and shows the compiler output in the **Shaders** tab and in `bridger.log`.

## What every shader gets

`bridger.hlsli` is prepended to every shader. `#include` it yourself for editor completion. It declares:

| Register | Name | Holds |
|---|---|---|
| `b0` | `BridgerFrameConstants` | the frame, see below |
| `b1` | yours | `cbuffer X : register(b1)` |
| `b2` | `BridgerObjectConstants` | `bridger_model`, `bridger_tint` |
| `t0` | `bridger_scene` | colour at the start of the current stage |
| `t1` | `bridger_scene_depth` | the game's depth buffer when captured, else a 1x1 "far" value |
| `t2` | `bridger_input` | the previous pass's output in the post chain; the scene for draw lists |
| `t3` | `bridger_history` | last frame's post result, when the pass asked for history |
| `t4` to `t7` | `bridger_texture0` to `3` | the effect's four texture slots |
| `t8` | `bridger_scene_motion` | captured NGX motion vectors, else zero |
| `s0` to `s3` | `bridger_linear`, `bridger_point`, `bridger_linear_wrap`, `bridger_point_wrap` | samplers |

**Frame constants:** `bridger_view`, `bridger_proj`, `bridger_view_proj`, `bridger_inv_view_proj`, `bridger_inv_proj`, `bridger_camera_position`, `bridger_camera_forward`, `bridger_camera_up`, `bridger_camera_right`, `bridger_resolution` (w, h, 1/w, 1/h), `bridger_time` (seconds, delta, frame), `bridger_depth` (near, far, reversed, game depth available) and `bridger_fov` (vertical rad, horizontal rad, tan halves).

**Object constants** apply to draw lists: `bridger_model` and `bridger_tint`, identity and white for pushed geometry, per draw for meshes.

### Vertex formats and default vertex shaders

```hlsl
struct BridgerVertex { float3 position; float3 normal; float2 uv; float4 color; };
struct BridgerPixel  { float4 position : SV_POSITION; float3 world; float3 normal; float2 uv; float4 color; };
struct BridgerScreenPixel { float4 position : SV_POSITION; float2 uv; };

BridgerScreenPixel bridger_fullscreen_vs(uint id : SV_VertexID);   // passes
BridgerPixel bridger_world_vs(BridgerVertex v);                     // world lists
BridgerPixel bridger_screen_vs(BridgerVertex v);                    // screen lists
```

A pass writes `ps_main(BridgerScreenPixel)`. A draw list shader writes `ps_main(BridgerPixel)`. Supply your own vertex stage with `ShaderOptions::vs_entry` when the defaults fall short; the input layout stays fixed to `BridgerVertex`.

### Helpers

| Helper | Gives |
|---|---|
| `bridger_linear_depth(d)` | device depth to metres under the frame's convention |
| `bridger_scene_linear_depth(uv)` | exact, point sampled |
| `bridger_scene_linear_depth_filtered(uv)` | four taps averaged in metres. Use this for soft masks. |
| `bridger_scene_depth_resolution()` | size of the captured depth buffer |
| `bridger_world_from_depth(uv, d)` | world position from a depth sample |
| `bridger_view_ray(uv)` | the camera ray through a pixel |
| `bridger_device_depth(world)` | world position to device depth |
| `bridger_luminance`, `bridger_srgb_to_linear`, `bridger_linear_to_srgb` | colour |
| `bridger_hash`, `bridger_noise` | noise |
| `bridger_simple_shade(normal)` | a quick lit look for debug geometry |

Matrices are row-major with column vectors: `mul(bridger_view_proj, float4(p, 1))`. The compiler runs with row-major packing, so a `float4x4` in your `b1` block matches a C++ `float[16]` written row by row. Keep constant blocks 16-byte aligned. They are limited to 4 KB.

## Conventions

| | |
|---|---|
| **World space** | the engine's: metres, right-handed, Z up. The camera basis is the `WorldTransform` rows: right, forward, up. |
| **View space** | right-handed, x right, y up, looking down -z |
| **Depth** | reverse-Z with an infinite far plane: 1 at the near plane, tending to 0 with distance. Draw lists that test depth use `GREATER_EQUAL`, and the mod depth buffer clears to 0 each frame. In screen space the vertex z is written as depth directly, so 1 is nearest. |
| **Front faces** | counter-clockwise. `DrawList` helpers wind their geometry that way. |
| **Colours** | `0xAABBGGRR` (`fx::rgba`, `fx::rgbaf`), unpacked to `float4` in the shader |

## Stages and ordering

Every effect has a **stage** and a **priority**. Stages run in order. Within a stage, effects run by ascending priority, then by creation order.

| Stage | Starts from | Meant for |
|---|---|---|
| `PRE_UPSCALE` | NGX input colour at render size | geometry and passes that DLSS should resolve |
| `SCENE` | the game's frame | world geometry that belongs "in" the scene, so post passes treat it like the game's pixels |
| `POST` | a copy of everything below | the post chain: each pass reads the previous one through `bridger_input` and the last one writes the backbuffer |
| `OVERLAY` | the post result | HUD-like screen-space work, drawn under the Bridger UI |

At the start of each stage that has work, the backbuffer is copied into `bridger_scene`, so `t0` is always the frame as it stood when the stage began.

Post passes with `output = 0` ping-pong between two internal targets, and the last one lands on the backbuffer. A pass with an `output` render target writes there instead and leaves the chain alone. That target can feed a later effect's texture slot, which is how `fxdemo` builds a bloom from four passes: extract to quarter resolution, blur horizontally, blur vertically, combine onto the chain.

Draw lists in the post stage draw onto wherever the chain currently lives. Draw lists with a render target output draw into it, cleared on first use each frame, together with its depth attachment if it has one.

`PassOptions::history` keeps last frame's post result in `bridger_history` for temporal effects: trails, feedback, reprojection experiments.

## Draw lists

```cpp
fx::DrawList g_world;
fx::DrawList g_hud;

bool bridger::on_load() {
    fx::DrawListOptions options;
    options.depth = BRIDGER_FX_DEPTH_TEST_WRITE;   // against other mod geometry this frame
    options.cull = BRIDGER_FX_CULL_BACK;
    g_world.create("markers", BRIDGER_FX_SPACE_WORLD, options);

    fx::DrawListOptions hud;
    hud.stage = BRIDGER_FX_STAGE_OVERLAY;
    g_hud.create("hud", BRIDGER_FX_SPACE_SCREEN, hud);
    bridger::tick(on_frame);
    return true;
}

void on_frame(float) {
    g_world.sphere({x, y, z}, 0.5f, fx::rgba(255, 140, 40));
    g_world.line(a, b, fx::rgba(255, 255, 255), 2.0f);      // 2 px wide, camera facing
    g_world.box(min, max, fx::rgba(80, 200, 255), 1.5f);     // wire
    g_world.grid({x, y, ground}, 30.0f, 1.0f, fx::rgba(120, 200, 230, 80));
    g_world.axes(origin);
    g_world.mesh(g_cube, fx::Mat4::translation(p) * fx::Mat4::rotation({0, 0, 1}, angle));

    float sx, sy;
    if (fx::project({x, y, z}, sx, sy)) {                     // world point to pixels
        g_hud.rect_outline(sx - 20, sy - 20, sx + 20, sy + 20, fx::rgba(255, 255, 255));
    }
}
```

Geometry is pushed from `bridger::tick` (the render thread, from Present) and consumed in the same call, so there is no locking and nothing to clear. Every frame starts empty.

`push()` takes raw `BridgerFxVertex` arrays with indices relative to the pushed vertices. Topology (triangles, lines, points), fill (solid, wireframe), blend, cull and depth mode are properties of the list. Line helpers build camera-facing triangle strips of constant pixel width, so they work on triangle lists with any shader. The `LINES` topology gives hairlines.

`fx::Mesh` uploads geometry once (`fx::geometry::box`, `sphere` and `plane` build the arrays) and draws it through `DrawList::mesh` with a model matrix and a tint, reachable in the shader as `bridger_model` and `bridger_tint`.

A list with no shader of its own draws vertex colour with the built-in shader. A `BRIDGER_FX_SHADER_WORLD` or `_SCREEN` shader receives `BridgerPixel` with the world position, normal, uv and colour, plus all the frame constants and textures.

## The camera

Every engine frame, from the game-thread tick, the core resolves the local player (`Player_sExportedGetLocalPlayer`) and its last activated camera entity (`Player_ExportedGetLastActivatedCamera`), then reads the entity's `Orientation` (`WorldTransform` at +200), `FOV` (+964), `NearPlane` (+1084) and `FarPlane` (+1088). The read runs under a fault guard. After five faults it stops and the Shaders tab says so.

Because the read happens after the frame's commit, any mod that overrides the camera through `Entity::SetOrientation` (the freecam) is seen automatically. A mod that knows better can push its own camera with `fx::camera_override(&camera)` and clear it with `nullptr`.

The engine's `FOV` is treated as a **horizontal** angle by default. If world geometry slides against the terrain when the camera turns, flip **FOV is horizontal** in the Shaders tab. The `fxdemo` ground grid is the quickest way to judge it.

`fx::frame()` returns the frame the shaders see: size, time, camera, the three matrices, and whether game depth is available. `fx::project()` maps a world point to pixels.

## Textures and render targets

```cpp
fx::Texture g_lut;      g_lut.load("textures/lut.dds");                       // .dds or .tga
fx::Texture g_dynamic;  g_dynamic.create(256, 256, BRIDGER_FX_FORMAT_RGBA8, pixels);
g_dynamic.update(pixels);                                                       // any thread
fx::RenderTarget g_half; g_half.create(0.5f, BRIDGER_FX_FORMAT_RGBA16F);       // follows resizes
fx::RenderTarget g_mask; g_mask.create_fixed(512, 512, BRIDGER_FX_FORMAT_R8, /*depth*/ true);
g_pass.texture(0, g_lut);                                                       // bridger_texture0
```

| | |
|---|---|
| **DDS** | uncompressed RGBA/BGRA, BC1 to BC7, DX10 headers, mip chains |
| **TGA** | uncompressed or RLE, 24 or 32 bit, greyscale, decoded to RGBA8 |
| **`create` formats** | 8-bit UNORM (R, RG, RGBA, BGRA, sRGB), 16 and 32-bit float (R, RG, RGBA), R11G11B10F, RGB10A2 |
| **Render targets** | the same uncompressed formats, or `BACKBUFFER` to match the swapchain (which is also what HDR output changes) |

You can create resources in `on_load`, before the swapchain is hooked. GPU objects materialise on the first frame the device is available, and again after a device change.

## Game depth

> [!NOTE]
> This feature is experimental. It reads a resource the game never shared. If the game misbehaves, turn it off.

D3D12 offers no way to enumerate another module's resources, so the core watches the API. A hook on `CreateDepthStencilView` learns which resource each depth view refers to, and a hook on `ClearDepthStencilView` records which of them are cleared each frame, with their size. With **Capture game depth** on in the Shaders tab, the chosen candidate is copied at Present into a texture bound at `t1`. A third hook on `ResourceBarrier`, installed only while capture is on, tracks the resource's state so the copy can transition it correctly.

Things to know, all visible in the tab:

- Depth buffers created before Bridger attached stay invisible until the game recreates them. Changing the resolution once does it.
- With upscaling the depth buffer is smaller than the swapchain, so sample it by uv.
- Use `bridger_scene_linear_depth_filtered` for anything that blends by depth. The buffer is both lower resolution than the frame and still jittered for the game's own temporal resolve, so a point-sampled mask stamps hard, crawling silhouettes onto an already-resolved image.
- The game's depth is reverse-Z (toggle in the tab if it looks wrong), and its projection differs slightly from the one the core builds, so `bridger_world_from_depth` on captured depth is approximate.

`bridger_depth.w` is 1 when a capture happened this frame. The fog in `fxdemo` checks it.

## The pre-upscale stage

*API 9.* Work in this stage renders at the game's internal resolution and goes through DLSS with the rest of the frame, so it gets the same temporal resolve as the game's own pixels.

### Turning it on

Enable **Pre upscale effects** in the Shaders tab's settings. Off, it stops every pre-upscale draw and all motion capture. **Bypass all effects** stops this work too. Without a live NGX evaluate call the stage never runs and motion falls back to zero.

### Using it

Set a pass or draw list's stage to `BRIDGER_FX_STAGE_PRE_UPSCALE`. Its enum value is 3, which keeps the older stage values stable; it still executes before `SCENE`.

The core builds the projection from the NGX input size and pixel jitter, renders into a private target with the input's format, then copies the result back into the NGX input colour. Before handing over to NGX it restores colour, depth and motion to `NON_PIXEL_SHADER_RESOURCE`. The output resource belongs to NGX and the stage never writes it.

Geometry comes from the latest completed render tick. The stage refreshes the camera and projection at evaluation time and does not run the render tick a second time, so push world coordinates and let the stage project them; it never replays mod callbacks on another thread. Shader compilation still happens at Present. Custom shaders may use the four texture slots and named output targets. Depth testing uses a private mod depth attachment, and the captured game depth is available for sampling.

`fx::upscale_info()` returns availability, render size and pixel jitter. The C table entry is `BridgerFx::upscale_info`. In Lua, `fx.upscale_info()` and `fx.pre_upscale_pass(hlsl_source)` do the same; Lua passes belong to their script and disappear on unload, and `fx.revert()` removes them by hand. `examples/fxdemo/pre_upscale.lua` shows it.

### Queues, ownership and faults

The core learns the game's real queue through the existing `ExecuteCommandLists` hook before it allows any write to the input, and it requires the same direct queue the overlay uses. Each recorded job holds its uploads, descriptors and GPU object references until that submission's fence completes. The Shaders tab lists job owners, command lists, fences, resources and draw counts.

Native resource and effect creation records the caller's return address for ownership. Unload and reload remove effects, and GPU references drain after use. `fx::revert_owned()` removes the calling mod's FX resources, and `bridger::safely` calls it after a fault. A fault during pre-upscale recording disables the stage and removes the participating mods' FX entries. Engine resource barriers run under fault guards.

### Motion vectors

While capture is enabled, `t8` carries motion in every stage. `bridger_motion.xy` converts the stored values to UV offsets using NGX's motion scale and render size. `.z` reports valid motion and `.w` reports a history reset. Motion points from the current pixel to its prior position. `bridger_history_uv(uv)` exposes that mapping for custom history buffers.

### History

The core reprojects `bridger_history` before effects sample it. Pre-upscale history is stored apart from the `POST` history. A reset or an out-of-bounds sample falls back to the current scene, and missing motion uses the same UV. `bridger_reproject_history(uv)` samples the prepared texture without applying motion twice. Use `PassOptions::history` to keep a prior result. The shader check case `fx_pre_upscale_history` covers `shaders/motion_history.hlsl` in fxdemo.

### The game check

Enable Shader Demo, then **World > Ground grid** and **World > Pre upscale grid**. The pre-upscale grid draws opaque strips with hard edges and no feathering. Enable **Pre upscale effects** and use DLSS. Turn the camera slowly and compare the grid with the switch off.

A BMP cannot show a temporal result, so this check happens in the running game: DLSS has to resolve the grid without crawling edges before the next phase starts.

### Known limits

- The first implementation needs a full input rectangle, one mip and one array slice, no MSAA, on a direct command list. It skips unsupported input shapes.
- It writes no engine depth or motion for new mod geometry, so floating or moving objects can still mismatch in DLSS.
- History reprojection has no disocclusion rejection.

`fx_upscale_test` validates the stage locally on Microsoft's software DX12 device: it checks motion reprojection pixels and named output targets, and removes a mod before the submitted GPU work executes. That is separate from the in-game check.

The resource state contract and the command list rules come from NVIDIA's [DLSS guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf) and [NGX guide](https://docs.nvidia.com/rtx/ngx/programming-guide/).

## Temporal stability

The frame handed to a pass at Present has already been through the game's temporal anti-aliasing and, on most settings, an upscaler. Death Stranding renders at a fraction of the output resolution and resolves up, which is why the captured depth buffer is smaller than the swapchain. Everything a mod adds afterwards sits outside that resolve, so it has no temporal filtering of its own, and anything a pass amplifies gets amplified without it too.

Three rules follow.

**Feather thin geometry.** A hard-edged sliver one or two pixels wide crawls as the camera moves. `DrawList::line`, `line2d` and the helpers built on them draw a solid core with a pixel of falloff on each side, so they need alpha blending (the default) to look right. Geometry you push yourself should fade its own edges the same way.

**Fade, never end.** `DrawList::grid` splits its lines into segments and fades them radially, because a wall of thin converging lines at the far edge of a grid is the worst case for shimmer. Distance fades cost nothing and remove the problem at the source.

**Downsample honestly.** A bright pass that reads a 4x4 block with four taps pulses as bright features drift between taps. `examples/fxdemo/shaders/bloom_extract.hlsl` shows the standard fix: thirteen taps grouped into five overlapping boxes, each weighted by 1 / (1 + luma) so one hot sample cannot carry the block, and a soft knee on the threshold so pixels fade in. Upsample with a tent filter, never a single bilinear tap.

If an effect still shimmers, `bridger_history` (`PassOptions::history`) gives last frame's post result, which is enough to blend a temporal filter of your own.

> [!TIP]
> When something looks wrong in the frame, **Bypass all effects** in the Shaders tab is the first thing to try. It skips every pass and draw list while leaving compiles and uploads running, so it separates "a mod did this" from "the game looks like this".

## The Shaders tab

| Group | Shows |
|---|---|
| **Pipeline** | device, frame, effects run, draw calls, vertices, copies, CPU and GPU cost, cached pipelines, depth status |
| **Camera** | source, position, lens, sample and fault counts |
| **Settings** | bypass, hot reload, GPU timings, FOV convention, depth capture and candidate |
| **Effects** | every pass and list with its owner, kind, stage and priority, draw counts, output, per-effect GPU time from timestamp queries, and any problem |
| **Shaders** | status, compile counts and times, file, reload button, compiler output |
| **Resources** | textures, render targets and meshes |

Effect toggles in the tab are live and unsaved. The core's settings persist in `Bridger/config/bridger.json`.

The tab's own layout can be reviewed without the game by rendering it against fabricated state:

```bash
cmake --build build --config Release --target ui_preview
```

```bash
build/Release/ui_preview.exe out.bmp 600 400 shaders
```

Add `:N` to the last argument to scroll N wheel notches first.

## Checking shaders without the game

### The shader compiler

`fx_check` compiles files against the prelude with the core's compiler flags:

```bash
cmake --build build --config Release --target fx_check
```

```bash
build/Release/fx_check.exe fullscreen examples/fxdemo/shaders/grade.hlsl
```

```bash
build/Release/fx_check.exe world mods/my_mod/shaders/marker.hlsl
```

`ctest -C Release` runs it over the demo shaders together with the camera maths test.

### Golden frames

The Shaders tab has **Capture effects for golden check**. One click copies two consecutive frames, before and after mod effects and before the Bridger UI, into `Bridger/dumps`. Files are named `golden-<batch>-1-effects.bmp` and `golden-<batch>-2-effects.bmp`, with matching `-before.bmp` files. The copies wait for the overlay GPU fence before readback. The button works without DLSS and with effects bypassed.

`tools/fx_check/golden.py` accepts a real fxdemo frame into `captures/golden`, then compares each new phase against it. It checks size, channel error and the fraction of pixels outside tolerance, and it refuses to replace a baseline. Follow the capture steps in `captures/golden/README.md` to keep settings and camera fixed. No baseline exists until someone captures it in the game; the tool's unit tests use generated BMPs only to check the comparison logic.

## Lifetime and threads

- `Shader`, `Texture`, `RenderTarget`, `Mesh`, `Pass` and `DrawList` are RAII handles. They destroy what they own, and the loader drops whatever a mod still holds when it unloads. GPU objects retire after the frames in flight have passed.
- Creating resources and effects, setting constants and textures, and reading `fx::frame()` are safe from any thread. Pushing geometry belongs to the render tick.
- Shader compilation happens on the render thread when a shader is new or changed. A big shader costs a few milliseconds once. Pipelines are cached per (shader, state, target format).
- A pass with no shader, a list with a fullscreen shader, an output that is not a render target, a texture slot bound to the target being written: each is reported per effect in the tab. None of them crash.

## The game's pipelines

*API 10.* Everything above draws on top of or inside the game's frame. This reaches into the game's own rendering. Every pipeline the engine creates is recorded, its draws are counted and can be wrapped or skipped, one of its shaders can be swapped for yours, and a function of yours can be spliced into every pixel shader that matches a filter.

The game compiles its shaders with FXC (shader model 5, DXBC containers), and D3D12 refuses a container whose checksum is wrong, so the core carries a DXBC reader, writer and checksum in `src/fx/dxbc.cpp`, proven against the runtime by `fx_dxbc_test`.

### Hashes

A pipeline is named by a 64-bit hash of its shaders' own DXBC hashes, so the same pipeline has the same name across runs and machines. The Shaders tab's **Game pipelines** group lists the busiest each frame with their colour target count: geometry passes in a deferred renderer write several targets, post and UI passes write one.

**dump** writes the shaders, their disassembly (with the RDEF block that names every binding) and the description to `Bridger/dumps/pipelines/<hash>/`. From C++, `fx::pipelines()`, `fx::find_pipeline` and `fx::dump_pipeline` do the same. From Lua, `fx.pipelines()`, `fx.pipeline(hex)` and `fx.pipeline_dump(hex)` take the hash as a 16-digit hex string.

### Draw hooks

`fx::DrawHook hook(hash, before, after, user)` runs `before` and `after` around every draw and dispatch with that pipeline, on the game's render thread, with the game's command list in the `BridgerFxDraw` it receives. Returning false from `before` skips the draw, which is how a mod hides everything a pipeline renders.

Keep hooks short. They run thousands of times per frame. A hook that faults is disabled and logged.

### Replacement

`fx::Replacement r(hash, BRIDGER_FX_STAGE_PS, "shaders/water.hlsl")` compiles the file with the host's own profile and creates a twin pipeline with that stage swapped. The twin is substituted whenever the game binds the original. The file hot reloads. Your shader must use the game's bindings for that pipeline, which is what the dumped RDEF block is for. `problem()` returns compiler output.

### The material hook

`fx::MaterialHook hook("shaders/material.hlsl")` takes a file with

```hlsl
float4 bridger_material(float4 color : COLOR0, float4 position : SV_Position) : SV_Target0
```

and splices it into the pixel shader of every pipeline with at least two colour targets, which is the deferred geometry passes. `MaterialHookOptions` changes the filter or limits it to one pipeline.

The function receives what the shader was about to write to target 0, and its output is written instead, before every return in the host. Temporaries, arithmetic and control flow are all fine. Compile values in through defines, edit the file and save: the core recompiles it and rebuilds affected pipelines as the game next binds them, which can stall for a moment the first time. One material hook per mod.

### Material bindings

*API 11.* With `"fx.material_bindings": true` in `bridger.json` (read at boot), the core extends every root signature the game creates with slots in register space 60 and reserves four descriptors at the end of every shader-visible heap. A hook may then read:

```hlsl
cbuffer MyValues : register(b0, space60) { float4 tint; float4 grade; };   // hook.constants(block)
Texture2D detail : register(t0, space60);                                  // hook.texture(0, tex), t0..t3
SamplerState linear_wrap : register(s2, space60);   // s0 linear clamp, s1 point clamp, s2 linear wrap, s3 point wrap
```

`fx::MaterialHook::constants` copies up to 4096 bytes, uploaded at the end of the frame, so a slider moves the game's materials with no recompile. Textures are plain `fx::Texture`s; render targets are not accepted here.

The game's shaders are shader model 5.1, which is what space 60 needs. A hook that binds resources fails to compile for 5.0 hosts, and those keep the game's shader. Before every draw of a twin that reads space 60 the core binds its constant buffer and texture table. On a command list whose root signature or heap it never extended, the game's own pipeline goes back in for that draw, so nothing draws with unbound slots.

While bindings are on, the game's cached pipeline blobs are dropped (they were built against the unextended signatures), which lengthens the first load. The log reports extended signatures, reserved heaps, bound draws and fallbacks every 600 frames.

### Switches

**Intercept draws** in the Game pipelines group is the kill switch. Off, every call passes straight through and no twin is substituted. **Retain shader bytecode** keeps a copy of each distinct shader, a few tens of megabytes for this game; without it nothing can be replaced or dumped. Both persist in `bridger.json`.

### Still to come

GPU timings per game pass are not taken yet. Replacement shaders do not get the space 60 bindings; only material hooks do. This game creates almost every pipeline through `ID3D12Device2::CreatePipelineState`. Those streams are retained and twinned through the same call, except when a stream carries a subobject type this build does not know, which the pipeline's row reports.

## Raytracing

`"fx.raytracing": true` in `bridger.json` (read at boot) starts Bridger's own DX12 raytracing runtime, `src/fx/rt.cpp`. Nothing in the game knows about acceleration structures, so the runtime builds them from what the game already draws.

1. **Capture.** Every deferred geometry pipeline (vertex and pixel shader only, two or more colour targets, triangles) gets a twin with a stream-output declaration on `SV_Position`, and the root signatures extended at boot carry `ALLOW_STREAM_OUTPUT`. The switch turns material bindings on. Before each such draw, the draw detour binds a region of a core-owned buffer as the stream-output target, so the draw renders exactly as before and also writes every post-transform vertex it produced: skinning, wind and instancing resolved by the game's own vertex shader.
2. **Unproject.** At the DLSS seam, a compute pass turns those clip-space vertices back into world space from clip x, y and w alone. w is view depth, x and y divide out the focal terms and the frame's jitter, and the inverse view matrix does the rest, so the projection's z row never matters.
3. **Build and trace.** One bottom-level structure, one geometry per draw, rebuilt every frame under a single-instance top-level structure. A compute shader with DXR 1.1 inline ray queries (`cs_6_5`) reads the game's depth for the shading position, derives a normal, and traces ambient occlusion, a sun shadow, or the debug view.
4. **Composite.** A fullscreen pass writes the result into the upscaler's input, so DLSS resolves it with the frame.

The Shaders tab's Settings page has the switches: mode, rays per pixel, radius, strength, bias, sun direction. Diagnostics shows the runtime state, regions, vertices and memory.

Shaders compile at runtime with `dxcompiler.dll` and are signed by `dxil.dll`, both deployed beside `bridger.dll` from the Windows SDK. `fx.rt_budget_mb` (default 96, per frame in flight) bounds the stream-out memory; draws beyond it count as skipped. The stream-out buffers are never freed while the process lives, because game command lists recorded on other threads name them.

### The debug view

Mode **debug: primary rays** traces a camera ray per pixel and colours it by whether the traced distance agrees with the depth buffer:

| Colour | Meaning |
|---|---|
| green | agrees |
| red | the traced hit is in front of the depth |
| yellow | the traced hit is behind the depth |
| magenta | the depth has a surface the trace missed |
| blue | traced geometry where the depth says sky |

A correctly placed scene is mostly green. The other colours locate the draws the capture gets wrong or misses. In this view the trace texture's alpha is the hit distance.

## Limits

- Constant blocks: 4 KB per effect. Texture slots: 4 per effect. Effects per frame: 512.
- Geometry rings grow on demand. Meshes live in upload heaps, which is fine for mod-scale geometry.
- One render target per pass. No compute shaders, no MSAA, no stencil.
- Draw lists in the post stage read `bridger_scene`, never the chain.

## C ABI

`BridgerApi::fx` is the `BridgerFx` table, introduced in API 7 and extended since. Handles are `uint32_t`, and 0 is never valid. See `api.h` for the descriptor structs; `fx.hpp` is a thin layer over them and the reference for how to fill them.

| API | Adds |
|---|---|
| 7 | `create_shader`, `create_texture`, `create_render_target`, `create_mesh`, `create_pass`, `create_draw_list`, the `set_*` calls, `push`, `draw_mesh`, `frame`, `project`, `set_camera_override`, `mod_directory` |
| 9 | `upscale_info`, `revert_owned` |
| 10 | `pipeline_count`, `pipeline_at`, `pipeline_find`, `pipeline_dump`, `pipeline_hook`, `pipeline_replace`, `material_hook`, `pipeline_reload`, `pipeline_problem`, `pipeline_revert` |

Deploy the matching core and rebuilt mods together.
