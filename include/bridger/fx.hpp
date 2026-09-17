#pragma once

#include "bridger/api.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef BRIDGER_MOD_EXPORT
#error "include bridger/mod.hpp, which pulls this header in after the loader API"
#endif

namespace bridger::fx {

using Handle = BridgerFxHandle;
using Vertex = BridgerFxVertex;
using Camera = BridgerFxCamera;
using Frame = BridgerFxFrame;

[[nodiscard]] inline const BridgerFx* table() {
    return api != nullptr && api->version >= 7 ? api->fx : nullptr;
}

struct UpscaleInfo { std::uint32_t width = 0, height = 0; float jitter[2]{}; bool available = false; };
inline UpscaleInfo upscale_info() {
    UpscaleInfo info;
    if (const auto* fx = table(); fx && api->version >= 9 && fx->upscale_info)
        info.available = fx->upscale_info(&info.width, &info.height, info.jitter);
    return info;
}
inline void revert_owned() {
    if (const auto* fx = table(); fx && api->version >= 9 && fx->revert_owned) fx->revert_owned();
}

constexpr std::uint32_t rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8)
         | (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(a) << 24);
}

inline std::uint32_t rgbaf(float r, float g, float b, float a = 1.0f) {
    auto channel = [](float v) {
        v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
    };
    return rgba(channel(r), channel(g), channel(b), channel(a));
}

inline std::uint32_t with_alpha(std::uint32_t color, float alpha) {
    const auto a = static_cast<std::uint32_t>((color >> 24) * (alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha));
    return (color & 0x00ffffffu) | (a << 24);
}

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
    const float len = length(a);
    return len > 1e-12f ? a * (1.0f / len) : Vec3{0.0f, 0.0f, 1.0f};
}
inline Vec3 to_float(Vec3d v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}
inline Vec3d to_double(Vec3 v) { return {v.x, v.y, v.z}; }
inline Vec3d operator+(Vec3d a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3d a, Vec3d b) {
    return {static_cast<float>(a.x - b.x), static_cast<float>(a.y - b.y), static_cast<float>(a.z - b.z)};
}

struct Mat4 {
    float m[16]{};

    static Mat4 identity() {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
    static Mat4 translation(Vec3 t) {
        Mat4 r = identity();
        r.m[3] = t.x;
        r.m[7] = t.y;
        r.m[11] = t.z;
        return r;
    }
    static Mat4 scale(Vec3 s) {
        Mat4 r = identity();
        r.m[0] = s.x;
        r.m[5] = s.y;
        r.m[10] = s.z;
        return r;
    }
    static Mat4 scale(float s) { return scale({s, s, s}); }
    static Mat4 rotation(Vec3 axis, float degrees) {
        const float a = degrees * 0.017453292519943295f;
        const float c = std::cos(a);
        const float s = std::sin(a);
        const float t = 1.0f - c;
        const Vec3 u = normalize(axis);
        Mat4 r = identity();
        r.m[0] = t * u.x * u.x + c;       r.m[1] = t * u.x * u.y - s * u.z; r.m[2] = t * u.x * u.z + s * u.y;
        r.m[4] = t * u.x * u.y + s * u.z; r.m[5] = t * u.y * u.y + c;       r.m[6] = t * u.y * u.z - s * u.x;
        r.m[8] = t * u.x * u.z - s * u.y; r.m[9] = t * u.y * u.z + s * u.x; r.m[10] = t * u.z * u.z + c;
        return r;
    }
    static Mat4 basis(Vec3 right, Vec3 forward, Vec3 up, Vec3 origin) {
        Mat4 r = identity();
        r.m[0] = right.x; r.m[1] = forward.x; r.m[2] = up.x; r.m[3] = origin.x;
        r.m[4] = right.y; r.m[5] = forward.y; r.m[6] = up.y; r.m[7] = origin.y;
        r.m[8] = right.z; r.m[9] = forward.z; r.m[10] = up.z; r.m[11] = origin.z;
        return r;
    }
    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += m[row * 4 + k] * o.m[k * 4 + col];
                }
                r.m[row * 4 + col] = sum;
            }
        }
        return r;
    }
    [[nodiscard]] Vec3 point(Vec3 p) const {
        return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
                m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
    }
};

inline Vertex vertex(Vec3 position, std::uint32_t color, Vec3 normal = {0.0f, 0.0f, 1.0f},
                     float u = 0.0f, float v = 0.0f) {
    Vertex out{};
    out.position[0] = position.x;
    out.position[1] = position.y;
    out.position[2] = position.z;
    out.normal[0] = normal.x;
    out.normal[1] = normal.y;
    out.normal[2] = normal.z;
    out.uv[0] = u;
    out.uv[1] = v;
    out.color = color;
    return out;
}

[[nodiscard]] inline Frame frame() {
    Frame f{};
    if (const auto* fx = table(); fx != nullptr) {
        fx->frame(&f);
    }
    return f;
}

[[nodiscard]] inline Camera camera() {
    return frame().camera;
}

inline bool project(Vec3d world, float& x, float& y, float* depth = nullptr) {
    const auto* fx = table();
    if (fx == nullptr) {
        return false;
    }
    const double w[3] = {world.x, world.y, world.z};
    float screen[2] = {0.0f, 0.0f};
    const bool ok = fx->project(w, screen, depth);
    x = screen[0];
    y = screen[1];
    return ok;
}

inline void camera_override(const Camera* camera) {
    if (const auto* fx = table(); fx != nullptr) {
        fx->set_camera_override(camera);
    }
}

[[nodiscard]] inline std::string directory() {
    const auto* fx = table();
    if (fx == nullptr) {
        return {};
    }
    char buffer[1024];
    fx->mod_directory(mod_id(), buffer, sizeof buffer);
    return buffer;
}

[[nodiscard]] inline std::string path(std::string_view relative) {
    if (relative.size() > 1 && (relative[1] == ':' || relative[0] == '\\' || relative[0] == '/')) {
        return std::string(relative);
    }
    const std::string base = directory();
    return base.empty() ? std::string(relative) : base + "\\" + std::string(relative);
}

struct ShaderOptions {
    const char* name = nullptr;
    const char* ps_entry = nullptr;
    const char* vs_entry = nullptr;
    std::vector<std::string> defines;
};

class Shader {
public:
    Shader() = default;
    Shader(BridgerFxShaderKind kind, std::string_view file, ShaderOptions options = {}) {
        load(kind, file, std::move(options));
    }
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& other) noexcept : handle_(std::exchange(other.handle_, 0)) {}
    Shader& operator=(Shader&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, 0);
        }
        return *this;
    }
    ~Shader() { destroy(); }

    bool load(BridgerFxShaderKind kind, std::string_view file, ShaderOptions options = {}) {
        const std::string full = path(file);
        return create(kind, nullptr, full.c_str(), options);
    }

    bool load_source(BridgerFxShaderKind kind, std::string_view hlsl, ShaderOptions options = {}) {
        const std::string text(hlsl);
        return create(kind, text.c_str(), nullptr, options);
    }

    void destroy() {
        if (handle_ != 0) {
            if (const auto* fx = table(); fx != nullptr) {
                fx->destroy_shader(handle_);
            }
            handle_ = 0;
        }
    }

    [[nodiscard]] Handle handle() const { return handle_; }
    [[nodiscard]] bool ready() const {
        const auto* fx = table();
        return fx != nullptr && handle_ != 0 && fx->shader_ready(handle_);
    }
    [[nodiscard]] std::string log() const {
        const auto* fx = table();
        if (fx == nullptr || handle_ == 0) {
            return {};
        }
        char buffer[4096];
        fx->shader_log(handle_, buffer, sizeof buffer);
        return buffer;
    }
    void reload() {
        if (const auto* fx = table(); fx != nullptr && handle_ != 0) {
            fx->reload_shader(handle_);
        }
    }
    explicit operator bool() const { return handle_ != 0; }

private:
    bool create(BridgerFxShaderKind kind, const char* source, const char* file,
                const ShaderOptions& options) {
        destroy();
        const auto* fx = table();
        if (fx == nullptr) {
            return false;
        }
        std::vector<const char*> defines;
        for (const auto& define : options.defines) {
            defines.push_back(define.c_str());
        }
        BridgerFxShaderDesc desc{};
        desc.name = options.name;
        desc.owner = mod_id();
        desc.source = source;
        desc.path = file;
        desc.vs_entry = options.vs_entry;
        desc.ps_entry = options.ps_entry;
        desc.defines = defines.empty() ? nullptr : defines.data();
        desc.define_count = static_cast<std::uint32_t>(defines.size());
        desc.kind = kind;
        handle_ = fx->create_shader(&desc);
        return handle_ != 0;
    }

    Handle handle_ = 0;
};

class Texture {
public:
    Texture() = default;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept : handle_(std::exchange(other.handle_, 0)) {}
    Texture& operator=(Texture&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, 0);
        }
        return *this;
    }
    ~Texture() { destroy(); }

    bool load(std::string_view file, const char* name = nullptr) {
        destroy();
        const auto* fx = table();
        if (fx == nullptr) {
            return false;
        }
        const std::string full = path(file);
        BridgerFxTextureDesc desc{};
        desc.name = name;
        desc.path = full.c_str();
        handle_ = fx->create_texture(&desc);
        return handle_ != 0;
    }

    bool create(std::uint32_t width, std::uint32_t height, BridgerFxFormat format,
                const void* pixels, std::uint32_t row_pitch = 0, const char* name = nullptr) {
        destroy();
        const auto* fx = table();
        if (fx == nullptr) {
            return false;
        }
        BridgerFxTextureDesc desc{};
        desc.name = name;
        desc.width = width;
        desc.height = height;
        desc.format = format;
        desc.pixels = pixels;
        desc.row_pitch = row_pitch;
        handle_ = fx->create_texture(&desc);
        return handle_ != 0;
    }

    bool update(const void* pixels, std::uint32_t row_pitch = 0) {
        const auto* fx = table();
        return fx != nullptr && handle_ != 0 && fx->update_texture(handle_, pixels, row_pitch);
    }

    void destroy() {
        if (handle_ != 0) {
            if (const auto* fx = table(); fx != nullptr) {
                fx->destroy_texture(handle_);
            }
            handle_ = 0;
        }
    }

    [[nodiscard]] Handle handle() const { return handle_; }
    explicit operator bool() const { return handle_ != 0; }

private:
    Handle handle_ = 0;
};

class RenderTarget {
public:
    RenderTarget() = default;
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;
    RenderTarget(RenderTarget&& other) noexcept : handle_(std::exchange(other.handle_, 0)) {}
    RenderTarget& operator=(RenderTarget&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, 0);
        }
        return *this;
    }
    ~RenderTarget() { destroy(); }

    bool create(float scale, BridgerFxFormat format = BRIDGER_FX_FORMAT_BACKBUFFER,
                bool with_depth = false, const char* name = nullptr) {
        BridgerFxRenderTargetDesc desc{};
        desc.name = name;
        desc.scale = scale;
        desc.format = format;
        desc.depth = with_depth;
        return create(desc);
    }

    bool create_fixed(std::uint32_t width, std::uint32_t height,
                      BridgerFxFormat format = BRIDGER_FX_FORMAT_RGBA8, bool with_depth = false,
                      const char* name = nullptr) {
        BridgerFxRenderTargetDesc desc{};
        desc.name = name;
        desc.width = width;
        desc.height = height;
        desc.format = format;
        desc.depth = with_depth;
        return create(desc);
    }

    void destroy() {
        if (handle_ != 0) {
            if (const auto* fx = table(); fx != nullptr) {
                fx->destroy_render_target(handle_);
            }
            handle_ = 0;
        }
    }

    [[nodiscard]] Handle handle() const { return handle_; }
    explicit operator bool() const { return handle_ != 0; }

private:
    bool create(const BridgerFxRenderTargetDesc& desc) {
        destroy();
        const auto* fx = table();
        if (fx == nullptr) {
            return false;
        }
        handle_ = fx->create_render_target(&desc);
        return handle_ != 0;
    }

    Handle handle_ = 0;
};

class Mesh {
public:
    Mesh() = default;
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept : handle_(std::exchange(other.handle_, 0)) {}
    Mesh& operator=(Mesh&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, 0);
        }
        return *this;
    }
    ~Mesh() { destroy(); }

    bool create(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices) {
        destroy();
        const auto* fx = table();
        if (fx == nullptr || vertices.empty()) {
            return false;
        }
        handle_ = fx->create_mesh(vertices.data(), static_cast<std::uint32_t>(vertices.size()),
                                  indices.empty() ? nullptr : indices.data(),
                                  static_cast<std::uint32_t>(indices.size()));
        return handle_ != 0;
    }

    void destroy() {
        if (handle_ != 0) {
            if (const auto* fx = table(); fx != nullptr) {
                fx->destroy_mesh(handle_);
            }
            handle_ = 0;
        }
    }

    [[nodiscard]] Handle handle() const { return handle_; }
    explicit operator bool() const { return handle_ != 0; }

private:
    Handle handle_ = 0;
};

class Effect {
public:
    Effect() = default;
    Effect(const Effect&) = delete;
    Effect& operator=(const Effect&) = delete;
    Effect(Effect&& other) noexcept : handle_(std::exchange(other.handle_, 0)) {}
    Effect& operator=(Effect&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, 0);
        }
        return *this;
    }
    ~Effect() { destroy(); }

    void destroy() {
        if (handle_ != 0) {
            if (const auto* fx = table(); fx != nullptr) {
                fx->destroy_effect(handle_);
            }
            handle_ = 0;
        }
    }

    [[nodiscard]] Handle handle() const { return handle_; }
    explicit operator bool() const { return handle_ != 0; }

    void enable(bool enabled) {
        if (const auto* fx = table(); fx != nullptr && handle_ != 0) {
            fx->set_enabled(handle_, enabled);
        }
    }
    [[nodiscard]] bool enabled() const {
        const auto* fx = table();
        return fx != nullptr && handle_ != 0 && fx->enabled(handle_);
    }
    void order(BridgerFxStage stage, int priority) {
        if (const auto* fx = table(); fx != nullptr && handle_ != 0) {
            fx->set_order(handle_, stage, priority);
        }
    }
    void shader(const Shader& shader) {
        if (const auto* fx = table(); fx != nullptr && handle_ != 0) {
            fx->set_shader(handle_, shader.handle());
        }
    }
    void output(Handle target) {
        if (const auto* fx = table(); fx != nullptr && handle_ != 0) {
            fx->set_output(handle_, target);
        }
    }
    void output(const RenderTarget& target) { output(target.handle()); }

    template <typename T>
    void constants(const T& block) {
        static_assert(sizeof(T) <= 4096, "constant block is limited to 4 KB");
        if (const auto* fx = table(); fx != nullptr && handle_ != 0) {
            fx->set_constants(handle_, &block, sizeof block);
        }
    }
    void texture(std::uint32_t slot, Handle resource) {
        if (const auto* fx = table(); fx != nullptr && handle_ != 0) {
            fx->set_texture(handle_, slot, resource);
        }
    }
    void texture(std::uint32_t slot, const Texture& value) { texture(slot, value.handle()); }
    void texture(std::uint32_t slot, const RenderTarget& value) { texture(slot, value.handle()); }

protected:
    Handle handle_ = 0;
};

struct PassOptions {
    BridgerFxStage stage = BRIDGER_FX_STAGE_POST;
    int priority = 0;
    Handle output = 0;
    bool clear_output = false;
    float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool history = false;
    BridgerFxBlend blend = BRIDGER_FX_BLEND_OPAQUE;
    bool enabled = true;
};

class Pass : public Effect {
public:
    Pass() = default;
    Pass(const char* name, const Shader& shader, PassOptions options = {}) {
        create(name, shader, options);
    }

    bool create(const char* name, const Shader& shader, PassOptions options = {}) {
        destroy();
        const auto* fx = table();
        if (fx == nullptr) {
            return false;
        }
        BridgerFxPassDesc desc{};
        desc.name = name;
        desc.owner = mod_id();
        desc.shader = shader.handle();
        desc.stage = options.stage;
        desc.priority = options.priority;
        desc.output = options.output;
        desc.clear_output = options.clear_output;
        std::memcpy(desc.clear_color, options.clear_color, sizeof desc.clear_color);
        desc.wants_history = options.history;
        desc.enabled = options.enabled;
        desc.blend = options.blend;
        handle_ = fx->create_pass(&desc);
        return handle_ != 0;
    }
};

struct DrawListOptions {
    Handle shader = 0;
    BridgerFxStage stage = BRIDGER_FX_STAGE_SCENE;
    int priority = 0;
    Handle output = 0;
    BridgerFxBlend blend = BRIDGER_FX_BLEND_ALPHA;
    BridgerFxDepth depth = BRIDGER_FX_DEPTH_NONE;
    BridgerFxCull cull = BRIDGER_FX_CULL_NONE;
    BridgerFxTopology topology = BRIDGER_FX_TOPOLOGY_TRIANGLES;
    BridgerFxFill fill = BRIDGER_FX_FILL_SOLID;
    bool enabled = true;
};

class DrawList : public Effect {
public:
    DrawList() = default;
    DrawList(const char* name, BridgerFxSpace space, DrawListOptions options = {}) {
        create(name, space, options);
    }

    bool create(const char* name, BridgerFxSpace space, DrawListOptions options = {}) {
        destroy();
        const auto* fx = table();
        if (fx == nullptr) {
            return false;
        }
        space_ = space;
        BridgerFxDrawListDesc desc{};
        desc.name = name;
        desc.owner = mod_id();
        desc.space = space;
        desc.shader = options.shader;
        desc.stage = options.stage;
        desc.priority = options.priority;
        desc.output = options.output;
        desc.blend = options.blend;
        desc.depth = options.depth;
        desc.cull = options.cull;
        desc.topology = options.topology;
        desc.fill = options.fill;
        desc.enabled = options.enabled;
        handle_ = fx->create_draw_list(&desc);
        return handle_ != 0;
    }

    void push(const Vertex* vertices, std::uint32_t vertex_count, const std::uint32_t* indices,
              std::uint32_t index_count) {
        if (const auto* fx = table(); fx != nullptr && handle_ != 0) {
            fx->push(handle_, vertices, vertex_count, indices, index_count);
        }
    }
    void push(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices) {
        push(vertices.data(), static_cast<std::uint32_t>(vertices.size()),
             indices.empty() ? nullptr : indices.data(), static_cast<std::uint32_t>(indices.size()));
    }
    void mesh(const Mesh& mesh, const Mat4& model = Mat4::identity(), std::uint32_t tint = 0xffffffffu) {
        if (const auto* fx = table(); fx != nullptr && handle_ != 0) {
            const float t[4] = {(tint & 0xff) / 255.0f, ((tint >> 8) & 0xff) / 255.0f,
                                ((tint >> 16) & 0xff) / 255.0f, ((tint >> 24) & 0xff) / 255.0f};
            fx->draw_mesh(handle_, mesh.handle(), model.m, t);
        }
    }

    void triangle(Vec3 a, Vec3 b, Vec3 c, std::uint32_t color) {
        const Vec3 n = normalize(cross(b - a, c - a));
        const Vertex v[3] = {vertex(a, color, n, 0.0f, 0.0f), vertex(b, color, n, 1.0f, 0.0f),
                             vertex(c, color, n, 0.0f, 1.0f)};
        push(v, 3, nullptr, 0);
    }
    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, std::uint32_t color) {
        const Vec3 n = normalize(cross(b - a, d - a));
        const Vertex v[4] = {vertex(a, color, n, 0.0f, 0.0f), vertex(b, color, n, 1.0f, 0.0f),
                             vertex(c, color, n, 1.0f, 1.0f), vertex(d, color, n, 0.0f, 1.0f)};
        static const std::uint32_t i[6] = {0, 1, 2, 0, 2, 3};
        push(v, 4, i, 6);
    }

    void line(Vec3 a, Vec3 b, std::uint32_t color, float width_px = 2.0f) {
        line(a, b, color, color, width_px);
    }

    void line(Vec3 a, Vec3 b, std::uint32_t color_a, std::uint32_t color_b, float width_px) {
        if (space_ == BRIDGER_FX_SPACE_SCREEN) {
            line2d(a.x, a.y, b.x, b.y, color_a, color_b, width_px, a.z);
            return;
        }
        const Frame f = frame();
        if (!f.camera.valid) {
            return;
        }
        const Vec3 eye = to_float(Vec3d{f.camera.position[0], f.camera.position[1], f.camera.position[2]});
        const Vec3 direction = normalize(b - a);
        const float per_pixel = world_per_pixel(f);
        const float core_px = std::max(width_px * 0.5f - 0.5f, 0.1f);
        const float edge_px = core_px + 1.0f;

        auto side_of = [&](Vec3 p, float& metres_per_pixel) {
            const Vec3 to_eye = p - eye;
            metres_per_pixel = per_pixel * length(to_eye);
            const Vec3 perpendicular = cross(direction, to_eye);
            if (length(perpendicular) < 1e-6f) {
                return Vec3{f.camera.up[0], f.camera.up[1], f.camera.up[2]};
            }
            return normalize(perpendicular);
        };
        float scale_a = 0.0f;
        float scale_b = 0.0f;
        const Vec3 sa = side_of(a, scale_a);
        const Vec3 sb = side_of(b, scale_b);
        const Vec3 n = normalize(eye - a);
        const std::uint32_t clear_a = with_alpha(color_a, 0.0f);
        const std::uint32_t clear_b = with_alpha(color_b, 0.0f);
        const Vertex v[8] = {
            vertex(a - sa * (edge_px * scale_a), clear_a, n, 0.0f, 0.0f),
            vertex(a - sa * (core_px * scale_a), color_a, n, 0.0f, 0.25f),
            vertex(a + sa * (core_px * scale_a), color_a, n, 0.0f, 0.75f),
            vertex(a + sa * (edge_px * scale_a), clear_a, n, 0.0f, 1.0f),
            vertex(b - sb * (edge_px * scale_b), clear_b, n, 1.0f, 0.0f),
            vertex(b - sb * (core_px * scale_b), color_b, n, 1.0f, 0.25f),
            vertex(b + sb * (core_px * scale_b), color_b, n, 1.0f, 0.75f),
            vertex(b + sb * (edge_px * scale_b), clear_b, n, 1.0f, 1.0f),
        };
        static const std::uint32_t i[18] = {0, 4, 5, 0, 5, 1, 1, 5, 6, 1, 6, 2, 2, 6, 7, 2, 7, 3};
        push(v, 8, i, 18);
    }
    void polyline(const Vec3* points, std::size_t count, std::uint32_t color, float width_px = 2.0f,
                  bool closed = false) {
        for (std::size_t i = 1; i < count; ++i) {
            line(points[i - 1], points[i], color, width_px);
        }
        if (closed && count > 2) {
            line(points[count - 1], points[0], color, width_px);
        }
    }
    void box(Vec3 min, Vec3 max, std::uint32_t color, float width_px = 2.0f) {
        const Vec3 c[8] = {{min.x, min.y, min.z}, {max.x, min.y, min.z}, {max.x, max.y, min.z},
                           {min.x, max.y, min.z}, {min.x, min.y, max.z}, {max.x, min.y, max.z},
                           {max.x, max.y, max.z}, {min.x, max.y, max.z}};
        static const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                         {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& edge : edges) {
            line(c[edge[0]], c[edge[1]], color, width_px);
        }
    }
    void solid_box(Vec3 center, Vec3 size, std::uint32_t color) {
        const Vec3 h = size * 0.5f;
        const Vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const Vec3 u[6] = {{0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
        for (int face = 0; face < 6; ++face) {
            const Vec3 normal = n[face];
            const Vec3 right = u[face];
            const Vec3 up = cross(normal, right);
            const Vec3 origin = center + Vec3{normal.x * h.x, normal.y * h.y, normal.z * h.z};
            const Vec3 r = Vec3{right.x * h.x, right.y * h.y, right.z * h.z};
            const Vec3 t = Vec3{up.x * h.x, up.y * h.y, up.z * h.z};
            const Vertex v[4] = {vertex(origin - r - t, color, normal, 0.0f, 0.0f),
                                 vertex(origin + r - t, color, normal, 1.0f, 0.0f),
                                 vertex(origin + r + t, color, normal, 1.0f, 1.0f),
                                 vertex(origin - r + t, color, normal, 0.0f, 1.0f)};
            static const std::uint32_t i[6] = {0, 1, 2, 0, 2, 3};
            push(v, 4, i, 6);
        }
    }
    void sphere(Vec3 center, float radius, std::uint32_t color, int segments = 16) {
        segments = segments < 4 ? 4 : segments;
        std::vector<Vertex> v;
        std::vector<std::uint32_t> idx;
        const int rings = segments;
        const int slices = segments * 2;
        for (int ring = 0; ring <= rings; ++ring) {
            const float phi = 3.14159265f * static_cast<float>(ring) / static_cast<float>(rings);
            for (int slice = 0; slice <= slices; ++slice) {
                const float theta = 6.2831853f * static_cast<float>(slice) / static_cast<float>(slices);
                const Vec3 n{std::sin(phi) * std::cos(theta), std::sin(phi) * std::sin(theta), std::cos(phi)};
                v.push_back(vertex(center + n * radius, color, n,
                                   static_cast<float>(slice) / slices, static_cast<float>(ring) / rings));
            }
        }
        for (int ring = 0; ring < rings; ++ring) {
            for (int slice = 0; slice < slices; ++slice) {
                const auto a = static_cast<std::uint32_t>(ring * (slices + 1) + slice);
                const auto b = a + static_cast<std::uint32_t>(slices + 1);
                idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        }
        push(v, idx);
    }
    void circle(Vec3 center, Vec3 normal, float radius, std::uint32_t color, float width_px = 2.0f,
                int segments = 48) {
        const Vec3 n = normalize(normal);
        const Vec3 reference = std::fabs(n.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
        const Vec3 u = normalize(cross(reference, n));
        const Vec3 w = cross(n, u);
        std::vector<Vec3> points;
        for (int i = 0; i < segments; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(segments);
            points.push_back(center + u * (std::cos(a) * radius) + w * (std::sin(a) * radius));
        }
        polyline(points.data(), points.size(), color, width_px, true);
    }
    void axes(Vec3 origin, float length_m = 1.0f, float width_px = 3.0f) {
        line(origin, origin + Vec3{length_m, 0, 0}, rgba(235, 80, 80), width_px);
        line(origin, origin + Vec3{0, length_m, 0}, rgba(90, 220, 90), width_px);
        line(origin, origin + Vec3{0, 0, length_m}, rgba(90, 140, 255), width_px);
    }
    void grid(Vec3 center, float extent, float step, std::uint32_t color, float width_px = 1.5f,
              int segments = 8) {
        if (step <= 0.0f || extent <= 0.0f) {
            return;
        }
        segments = segments < 1 ? 1 : segments;
        auto tint = [&](Vec3 p) {
            const float dx = p.x - center.x;
            const float dy = p.y - center.y;
            const float d = std::sqrt(dx * dx + dy * dy) / extent;
            return with_alpha(color, std::max(0.0f, 1.0f - d * d));
        };
        auto run = [&](Vec3 from, Vec3 to) {
            Vec3 previous = from;
            for (int i = 1; i <= segments; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(segments);
                const Vec3 next = from + (to - from) * t;
                line(previous, next, tint(previous), tint(next), width_px);
                previous = next;
            }
        };
        const float start_x = std::floor((center.x - extent) / step) * step;
        const float start_y = std::floor((center.y - extent) / step) * step;
        for (float x = start_x; x <= center.x + extent; x += step) {
            run({x, center.y - extent, center.z}, {x, center.y + extent, center.z});
        }
        for (float y = start_y; y <= center.y + extent; y += step) {
            run({center.x - extent, y, center.z}, {center.x + extent, y, center.z});
        }
    }
    void billboard(Vec3 center, float width_m, float height_m, std::uint32_t color) {
        const Frame f = frame();
        if (!f.camera.valid) {
            return;
        }
        const Vec3 right{f.camera.right[0], f.camera.right[1], f.camera.right[2]};
        const Vec3 up{f.camera.up[0], f.camera.up[1], f.camera.up[2]};
        const Vec3 r = right * (0.5f * width_m);
        const Vec3 u = up * (0.5f * height_m);
        const Vec3 n = -Vec3{f.camera.forward[0], f.camera.forward[1], f.camera.forward[2]};
        const Vertex v[4] = {vertex(center - r + u, color, n, 0.0f, 0.0f), vertex(center + r + u, color, n, 1.0f, 0.0f),
                             vertex(center + r - u, color, n, 1.0f, 1.0f), vertex(center - r - u, color, n, 0.0f, 1.0f)};
        static const std::uint32_t i[6] = {0, 1, 2, 0, 2, 3};
        push(v, 4, i, 6);
    }

    void rect(float x0, float y0, float x1, float y1, std::uint32_t color, float depth = 0.5f) {
        quad({x0, y0, depth}, {x1, y0, depth}, {x1, y1, depth}, {x0, y1, depth}, color);
    }
    void line2d(float x0, float y0, float x1, float y1, std::uint32_t color, float width_px = 2.0f,
                float depth = 0.5f) {
        line2d(x0, y0, x1, y1, color, color, width_px, depth);
    }
    void line2d(float x0, float y0, float x1, float y1, std::uint32_t color_a,
                std::uint32_t color_b, float width_px, float depth) {
        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f) {
            return;
        }
        const float core = std::max(width_px * 0.5f - 0.5f, 0.1f);
        const float edge = core + 1.0f;
        const float nx = -dy / len;
        const float ny = dx / len;
        const std::uint32_t clear_a = with_alpha(color_a, 0.0f);
        const std::uint32_t clear_b = with_alpha(color_b, 0.0f);
        const Vec3 n{0.0f, 0.0f, 1.0f};
        const Vertex v[8] = {
            vertex({x0 - nx * edge, y0 - ny * edge, depth}, clear_a, n, 0.0f, 0.0f),
            vertex({x0 - nx * core, y0 - ny * core, depth}, color_a, n, 0.0f, 0.25f),
            vertex({x0 + nx * core, y0 + ny * core, depth}, color_a, n, 0.0f, 0.75f),
            vertex({x0 + nx * edge, y0 + ny * edge, depth}, clear_a, n, 0.0f, 1.0f),
            vertex({x1 - nx * edge, y1 - ny * edge, depth}, clear_b, n, 1.0f, 0.0f),
            vertex({x1 - nx * core, y1 - ny * core, depth}, color_b, n, 1.0f, 0.25f),
            vertex({x1 + nx * core, y1 + ny * core, depth}, color_b, n, 1.0f, 0.75f),
            vertex({x1 + nx * edge, y1 + ny * edge, depth}, clear_b, n, 1.0f, 1.0f),
        };
        static const std::uint32_t i[18] = {0, 4, 5, 0, 5, 1, 1, 5, 6, 1, 6, 2, 2, 6, 7, 2, 7, 3};
        push(v, 8, i, 18);
    }
    void rect_outline(float x0, float y0, float x1, float y1, std::uint32_t color,
                      float width_px = 1.0f, float depth = 0.5f) {
        line2d(x0, y0, x1, y0, color, width_px, depth);
        line2d(x1, y0, x1, y1, color, width_px, depth);
        line2d(x1, y1, x0, y1, color, width_px, depth);
        line2d(x0, y1, x0, y0, color, width_px, depth);
    }

    [[nodiscard]] BridgerFxSpace space() const { return space_; }

private:
    static float world_per_pixel(const Frame& f) {
        const float tan_half_v = f.projection[5] != 0.0f ? 1.0f / f.projection[5] : 1.0f;
        return f.height > 0 ? 2.0f * tan_half_v / static_cast<float>(f.height) : 0.0f;
    }

    BridgerFxSpace space_ = BRIDGER_FX_SPACE_WORLD;
};

using PipelineInfo = BridgerFxPipelineInfo;
using Draw = BridgerFxDraw;

[[nodiscard]] inline const BridgerFx* pipeline_table() {
    const auto* fx = table();
    return fx != nullptr && api->version >= 10 ? fx : nullptr;
}

inline std::uint32_t pipeline_count() {
    const auto* fx = pipeline_table();
    return fx != nullptr ? fx->pipeline_count() : 0;
}
inline bool pipeline_at(std::uint32_t index, PipelineInfo& out) {
    const auto* fx = pipeline_table();
    return fx != nullptr && fx->pipeline_at(index, &out);
}
inline bool find_pipeline(std::uint64_t hash, PipelineInfo& out) {
    const auto* fx = pipeline_table();
    return fx != nullptr && fx->pipeline_find(hash, &out);
}
inline std::vector<PipelineInfo> pipelines() {
    std::vector<PipelineInfo> out;
    const auto count = pipeline_count();
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        PipelineInfo info{};
        if (pipeline_at(i, info)) out.push_back(info);
    }
    return out;
}
inline std::string dump_pipeline(std::uint64_t hash) {
    const auto* fx = pipeline_table();
    if (fx == nullptr) return {};
    std::string path(512, 0);
    const auto n = fx->pipeline_dump(hash, path.data(), path.size());
    path.resize(n < path.size() ? n : path.size() - 1);
    return path;
}

class PipelineHandle {
public:
    PipelineHandle() = default;
    PipelineHandle(const PipelineHandle&) = delete;
    PipelineHandle& operator=(const PipelineHandle&) = delete;
    PipelineHandle(PipelineHandle&& other) noexcept : handle_(std::exchange(other.handle_, 0)) {}
    PipelineHandle& operator=(PipelineHandle&& other) noexcept {
        if (this != &other) { revert(); handle_ = std::exchange(other.handle_, 0); }
        return *this;
    }
    ~PipelineHandle() { revert(); }

    void revert() {
        if (handle_ != 0) {
            if (const auto* fx = pipeline_table(); fx != nullptr) fx->pipeline_revert(handle_);
            handle_ = 0;
        }
    }
    [[nodiscard]] Handle handle() const { return handle_; }
    explicit operator bool() const { return handle_ != 0; }
    bool reload() {
        const auto* fx = pipeline_table();
        return fx != nullptr && handle_ != 0 && fx->pipeline_reload(handle_);
    }
    [[nodiscard]] std::string problem() const {
        const auto* fx = pipeline_table();
        if (fx == nullptr || handle_ == 0) return {};
        std::string text(4096, 0);
        const auto n = fx->pipeline_problem(handle_, text.data(), text.size());
        text.resize(n < text.size() ? n : text.size() - 1);
        return text;
    }

protected:
    Handle handle_ = 0;
};

class DrawHook : public PipelineHandle {
public:
    DrawHook() = default;
    DrawHook(std::uint64_t pipeline, BridgerFxDrawHook before, BridgerFxDrawHook after = nullptr, void* user = nullptr) {
        create(pipeline, before, after, user);
    }
    bool create(std::uint64_t pipeline, BridgerFxDrawHook before, BridgerFxDrawHook after = nullptr, void* user = nullptr) {
        revert();
        const auto* fx = pipeline_table();
        if (fx == nullptr) return false;
        handle_ = fx->pipeline_hook(pipeline, before, after, user);
        return handle_ != 0;
    }
};

class Replacement : public PipelineHandle {
public:
    Replacement() = default;
    Replacement(std::uint64_t pipeline, BridgerFxShaderStage stage, const char* path, const char* entry = "main",
                const std::vector<std::string>& defines = {}) {
        create(pipeline, stage, path, entry, defines);
    }
    bool create(std::uint64_t pipeline, BridgerFxShaderStage stage, const char* path, const char* entry = "main",
                const std::vector<std::string>& defines = {}) {
        revert();
        const auto* fx = pipeline_table();
        if (fx == nullptr) return false;
        std::vector<const char*> pointers;
        for (const auto& d : defines) pointers.push_back(d.c_str());
        BridgerFxReplaceDesc desc{};
        desc.pipeline = pipeline;
        desc.stage = stage;
        desc.path = path;
        desc.entry = entry;
        desc.defines = pointers.empty() ? nullptr : pointers.data();
        desc.define_count = static_cast<std::uint32_t>(pointers.size());
        handle_ = fx->pipeline_replace(&desc);
        return handle_ != 0;
    }
};

struct MaterialHookOptions {
    const char* entry = "bridger_material";
    std::vector<std::string> defines;
    std::uint32_t min_render_targets = 2;
    std::uint32_t max_render_targets = 0;
    std::uint64_t only_pipeline = 0;
};

class MaterialHook : public PipelineHandle {
public:
    MaterialHook() = default;
    MaterialHook(const char* path, const MaterialHookOptions& options = {}) { create(path, options); }
    bool create(const char* path, const MaterialHookOptions& options = {}) {
        revert();
        const auto* fx = pipeline_table();
        if (fx == nullptr) return false;
        std::vector<const char*> pointers;
        for (const auto& d : options.defines) pointers.push_back(d.c_str());
        BridgerFxMaterialHookDesc desc{};
        desc.path = path;
        desc.entry = options.entry;
        desc.defines = pointers.empty() ? nullptr : pointers.data();
        desc.define_count = static_cast<std::uint32_t>(pointers.size());
        desc.min_render_targets = options.min_render_targets;
        desc.max_render_targets = options.max_render_targets;
        desc.only_pipeline = options.only_pipeline;
        handle_ = fx->material_hook(&desc);
        return handle_ != 0;
    }

    bool constants(const void* data, std::size_t size) {
        const auto* fx = pipeline_table();
        return fx != nullptr && api->version >= 11 && handle_ != 0 && fx->material_constants(handle_, data, size);
    }
    template <typename T>
    bool constants(const T& block) { return constants(&block, sizeof(T)); }
    bool texture(std::uint32_t slot, const Texture& texture) {
        const auto* fx = pipeline_table();
        return fx != nullptr && api->version >= 11 && handle_ != 0 && fx->material_texture(handle_, slot, texture.handle());
    }
};

namespace geometry {

inline void box(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, Vec3 size,
                std::uint32_t color = 0xffffffffu) {
    const Vec3 h = size * 0.5f;
    const Vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const Vec3 u[6] = {{0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    for (int face = 0; face < 6; ++face) {
        const Vec3 normal = n[face];
        const Vec3 right = u[face];
        const Vec3 up = cross(normal, right);
        const Vec3 origin{normal.x * h.x, normal.y * h.y, normal.z * h.z};
        const Vec3 r{right.x * h.x, right.y * h.y, right.z * h.z};
        const Vec3 t{up.x * h.x, up.y * h.y, up.z * h.z};
        const auto base = static_cast<std::uint32_t>(vertices.size());
        vertices.push_back(vertex(origin - r - t, color, normal, 0.0f, 0.0f));
        vertices.push_back(vertex(origin + r - t, color, normal, 1.0f, 0.0f));
        vertices.push_back(vertex(origin + r + t, color, normal, 1.0f, 1.0f));
        vertices.push_back(vertex(origin - r + t, color, normal, 0.0f, 1.0f));
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

inline void sphere(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, float radius,
                   int segments = 16, std::uint32_t color = 0xffffffffu) {
    segments = segments < 4 ? 4 : segments;
    const int rings = segments;
    const int slices = segments * 2;
    const auto base = static_cast<std::uint32_t>(vertices.size());
    for (int ring = 0; ring <= rings; ++ring) {
        const float phi = 3.14159265f * static_cast<float>(ring) / static_cast<float>(rings);
        for (int slice = 0; slice <= slices; ++slice) {
            const float theta = 6.2831853f * static_cast<float>(slice) / static_cast<float>(slices);
            const Vec3 n{std::sin(phi) * std::cos(theta), std::sin(phi) * std::sin(theta), std::cos(phi)};
            vertices.push_back(vertex(n * radius, color, n, static_cast<float>(slice) / slices,
                                      static_cast<float>(ring) / rings));
        }
    }
    for (int ring = 0; ring < rings; ++ring) {
        for (int slice = 0; slice < slices; ++slice) {
            const auto a = base + static_cast<std::uint32_t>(ring * (slices + 1) + slice);
            const auto b = a + static_cast<std::uint32_t>(slices + 1);
            indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
}

inline void plane(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, float size,
                  std::uint32_t color = 0xffffffffu) {
    const float h = size * 0.5f;
    const auto base = static_cast<std::uint32_t>(vertices.size());
    vertices.push_back(vertex({-h, -h, 0.0f}, color, {0, 0, 1}, 0.0f, 0.0f));
    vertices.push_back(vertex({h, -h, 0.0f}, color, {0, 0, 1}, 1.0f, 0.0f));
    vertices.push_back(vertex({h, h, 0.0f}, color, {0, 0, 1}, 1.0f, 1.0f));
    vertices.push_back(vertex({-h, h, 0.0f}, color, {0, 0, 1}, 0.0f, 1.0f));
    indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

}

}
