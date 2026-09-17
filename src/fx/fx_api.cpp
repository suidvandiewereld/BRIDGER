#include "fx/fx.h"

#include <algorithm>
#include <cstring>
#include <intrin.h>

#include "core/log.h"
#include "core/guard.h"
#include "fx/ngx.h"
#include "fx/pipeline.h"
#include "fx/fx_internal.h"
#include "loader/registry.h"

namespace bridger::fx {
namespace {

std::string owner_of(const char* owner, const void* address = nullptr) {
    const auto caller = loader::owner_of_address(address);
    if (!caller.empty()) return caller;
    if (owner != nullptr && *owner != 0) {
        return owner;
    }
    return loader::Registry::instance().active_mod();
}

std::string text_or(const char* value, const char* fallback) {
    return value != nullptr ? value : fallback;
}

template <typename Map>
typename Map::mapped_type* lookup(Map& map, Handle handle) {
    const auto found = map.find(handle);
    return found == map.end() ? nullptr : &found->second;
}

Handle api_create_shader(const BridgerFxShaderDesc* desc) {
    if (desc == nullptr || (desc->source == nullptr && desc->path == nullptr)) {
        log::error("fx: create_shader needs a source or a path");
        return 0;
    }
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    Shader shader;
    shader.handle = s.next_handle++;
    shader.owner = owner_of(desc->owner, _ReturnAddress());
    shader.kind = desc->kind;
    if (desc->path != nullptr) {
        shader.path = std::filesystem::path(desc->path);
        shader.name = text_or(desc->name, shader.path.filename().string().c_str());
    } else {
        shader.source = desc->source;
        shader.name = text_or(desc->name, "shader");
    }
    shader.vs_entry = text_or(desc->vs_entry, "");
    shader.ps_entry = text_or(desc->ps_entry, "");
    for (std::uint32_t i = 0; desc->defines != nullptr && i < desc->define_count; ++i) {
        if (desc->defines[i] != nullptr) {
            shader.defines.emplace_back(desc->defines[i]);
        }
    }
    shader.dirty = true;
    const Handle handle = shader.handle;
    log::info("fx: {} registered shader {} ({})", shader.owner.empty() ? "core" : shader.owner,
              shader.name, shader.path.empty() ? "inline" : shader.path.string());
    s.shaders[handle] = std::move(shader);
    return handle;
}

void api_destroy_shader(Handle handle) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    Shader* shader = lookup(s.shaders, handle);
    if (shader == nullptr || shader->builtin) {
        return;
    }
    detail::destroy_shader_objects(*shader);
    s.shaders.erase(handle);
}

bool api_reload_shader(Handle handle) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    Shader* shader = lookup(s.shaders, handle);
    if (shader == nullptr) {
        return false;
    }
    shader->dirty = true;
    return true;
}

bool api_shader_ready(Handle handle) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const Shader* shader = lookup(s.shaders, handle);
    return shader != nullptr && shader->ready;
}

std::size_t copy_out(const std::string& value, char* buffer, std::size_t capacity) {
    if (buffer != nullptr && capacity > 0) {
        const auto length = std::min(value.size(), capacity - 1);
        std::memcpy(buffer, value.data(), length);
        buffer[length] = 0;
    }
    return value.size();
}

std::size_t api_shader_log(Handle handle, char* buffer, std::size_t capacity) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const Shader* shader = lookup(s.shaders, handle);
    return copy_out(shader != nullptr ? shader->log : std::string(), buffer, capacity);
}

Handle api_create_texture(const BridgerFxTextureDesc* desc) {
    if (desc == nullptr) {
        return 0;
    }
    Texture texture;
    std::string error;
    if (desc->path != nullptr) {
        texture.path = desc->path;
        if (!load_image(texture.path, texture.pending, error)) {
            log::error("fx: texture {}: {}", texture.path.string(), error);
            return 0;
        }
        texture.name = text_or(desc->name, texture.path.filename().string().c_str());
    } else {
        const DXGI_FORMAT format = detail::dxgi_format(desc->format);
        if (format == DXGI_FORMAT_UNKNOWN || desc->format == BRIDGER_FX_FORMAT_BACKBUFFER) {
            log::error("fx: create_texture: unsupported format");
            return 0;
        }
        if (!image_from_pixels(texture.pending, desc->width, desc->height, format, desc->pixels,
                               desc->row_pitch, error)) {
            log::error("fx: create_texture: {}", error);
            return 0;
        }
        texture.name = text_or(desc->name, "texture");
    }
    texture.has_pending = true;
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    texture.handle = s.next_handle++;
    texture.owner = owner_of(nullptr, _ReturnAddress());
    const Handle handle = texture.handle;
    s.textures[handle] = std::move(texture);
    return handle;
}

bool api_update_texture(Handle handle, const void* pixels, std::uint32_t row_pitch) {
    if (pixels == nullptr) {
        return false;
    }
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    Texture* texture = lookup(s.textures, handle);
    if (texture == nullptr || texture->is_target) {
        return false;
    }
    std::uint32_t width = texture->pending.width;
    std::uint32_t height = texture->pending.height;
    DXGI_FORMAT format = texture->pending.format;
    if (texture->color.valid()) {
        width = texture->color.width;
        height = texture->color.height;
        format = texture->color.format;
    }
    if (width == 0 || height == 0) {
        return false;
    }
    std::string error;
    Image image;
    if (!image_from_pixels(image, width, height, format, pixels, row_pitch, error)) {
        log::error("fx: update_texture {}: {}", texture->name, error);
        return false;
    }
    texture->pending = std::move(image);
    texture->has_pending = true;
    return true;
}

void api_destroy_texture(Handle handle) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    Texture* texture = lookup(s.textures, handle);
    if (texture == nullptr) {
        return;
    }
    detail::destroy_texture_objects(*texture);
    s.textures.erase(handle);
    for (auto& [id, effect] : s.effects) {
        (void)id;
        if (effect.output == handle) {
            effect.output = 0;
        }
        for (auto& slot : effect.textures) {
            if (slot == handle) {
                slot = 0;
            }
        }
    }
}

Handle api_create_render_target(const BridgerFxRenderTargetDesc* desc) {
    if (desc == nullptr) {
        return 0;
    }
    if (desc->scale <= 0.0f && (desc->width == 0 || desc->height == 0)) {
        log::error("fx: create_render_target needs a size or a scale");
        return 0;
    }
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    Texture texture;
    texture.handle = s.next_handle++;
    texture.owner = owner_of(nullptr, _ReturnAddress());
    texture.name = text_or(desc->name, "render target");
    texture.is_target = true;
    texture.scale = desc->scale;
    texture.width = desc->width;
    texture.height = desc->height;
    texture.format = desc->format;
    texture.with_depth = desc->depth;
    const Handle handle = texture.handle;
    s.textures[handle] = std::move(texture);
    return handle;
}

Handle api_create_mesh(const BridgerFxVertex* vertices, std::uint32_t vertex_count,
                       const std::uint32_t* indices, std::uint32_t index_count) {
    if (vertices == nullptr || vertex_count == 0) {
        return 0;
    }
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    Mesh mesh;
    mesh.handle = s.next_handle++;
    mesh.owner = owner_of(nullptr, _ReturnAddress());
    mesh.vertices.assign(vertices, vertices + vertex_count);
    if (indices != nullptr && index_count > 0) {
        mesh.indices.assign(indices, indices + index_count);
        for (auto index : mesh.indices) {
            if (index >= vertex_count) {
                log::error("fx: create_mesh: index {} out of range", index);
                return 0;
            }
        }
    } else {
        mesh.indices.resize(vertex_count);
        for (std::uint32_t i = 0; i < vertex_count; ++i) {
            mesh.indices[i] = i;
        }
    }
    const Handle handle = mesh.handle;
    s.meshes[handle] = std::move(mesh);
    return handle;
}

void api_destroy_mesh(Handle handle) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    Mesh* mesh = lookup(s.meshes, handle);
    if (mesh == nullptr) {
        return;
    }
    detail::destroy_mesh_objects(*mesh);
    s.meshes.erase(handle);
}

Handle api_create_pass(const BridgerFxPassDesc* desc) {
    if (desc == nullptr) {
        return 0;
    }
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    Effect effect;
    effect.handle = s.next_handle++;
    effect.name = text_or(desc->name, "pass");
    effect.owner = owner_of(desc->owner, _ReturnAddress());
    effect.is_pass = true;
    effect.stage = desc->stage;
    effect.priority = desc->priority;
    effect.shader = desc->shader;
    effect.output = desc->output;
    effect.clear_output = desc->clear_output;
    std::memcpy(effect.clear_color, desc->clear_color, sizeof effect.clear_color);
    effect.wants_history = desc->wants_history;
    effect.enabled = desc->enabled;
    effect.blend = desc->blend;
    const Handle handle = effect.handle;
    log::info("fx: {} registered pass {}", effect.owner.empty() ? "core" : effect.owner, effect.name);
    s.effects[handle] = std::move(effect);
    return handle;
}

Handle api_create_draw_list(const BridgerFxDrawListDesc* desc) {
    if (desc == nullptr) {
        return 0;
    }
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    Effect effect;
    effect.handle = s.next_handle++;
    effect.name = text_or(desc->name, "draw list");
    effect.owner = owner_of(desc->owner, _ReturnAddress());
    effect.is_pass = false;
    effect.space = desc->space;
    effect.stage = desc->stage;
    effect.priority = desc->priority;
    effect.shader = desc->shader;
    effect.output = desc->output;
    effect.blend = desc->blend;
    effect.depth = desc->depth;
    effect.cull = desc->cull;
    effect.topology = desc->topology;
    effect.fill = desc->fill;
    effect.enabled = desc->enabled;
    const Handle handle = effect.handle;
    log::info("fx: {} registered {} draw list {}", effect.owner.empty() ? "core" : effect.owner,
              effect.space == BRIDGER_FX_SPACE_WORLD ? "world" : "screen", effect.name);
    s.effects[handle] = std::move(effect);
    return handle;
}

void api_destroy_effect(Handle handle) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    s.effects.erase(handle);
}

void api_set_enabled(Handle handle, bool enabled) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    if (Effect* effect = lookup(s.effects, handle); effect != nullptr) {
        effect->enabled = enabled;
    }
}

bool api_enabled(Handle handle) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const Effect* effect = lookup(s.effects, handle);
    return effect != nullptr && effect->enabled;
}

void api_set_order(Handle handle, BridgerFxStage stage, int priority) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    if (Effect* effect = lookup(s.effects, handle); effect != nullptr) {
        effect->stage = stage;
        effect->priority = priority;
    }
}

void api_set_shader(Handle handle, Handle shader) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    if (Effect* effect = lookup(s.effects, handle); effect != nullptr) {
        effect->shader = shader;
    }
}

void api_set_output(Handle handle, Handle target) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    if (Effect* effect = lookup(s.effects, handle); effect != nullptr) {
        effect->output = target;
    }
}

void api_set_constants(Handle handle, const void* data, std::size_t size) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    Effect* effect = lookup(s.effects, handle);
    if (effect == nullptr) {
        return;
    }
    if (data == nullptr || size == 0) {
        effect->constants.clear();
        return;
    }
    size = std::min(size, kMaxConstants);
    const std::size_t padded = (size + 15) & ~std::size_t{15};
    effect->constants.assign(padded, 0);
    std::memcpy(effect->constants.data(), data, size);
}

void api_set_texture(Handle handle, std::uint32_t slot, Handle resource) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    Effect* effect = lookup(s.effects, handle);
    if (effect == nullptr || slot >= kUserSlots) {
        return;
    }
    effect->textures[slot] = resource;
}

void api_push(Handle handle, const BridgerFxVertex* vertices, std::uint32_t vertex_count,
              const std::uint32_t* indices, std::uint32_t index_count) {
    if (vertices == nullptr || vertex_count == 0) {
        return;
    }
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    Effect* effect = lookup(s.effects, handle);
    if (effect == nullptr || effect->is_pass) {
        return;
    }
    const auto base = static_cast<std::uint32_t>(effect->vertices.size());
    effect->vertices.insert(effect->vertices.end(), vertices, vertices + vertex_count);
    if (indices != nullptr && index_count > 0) {
        effect->indices.reserve(effect->indices.size() + index_count);
        for (std::uint32_t i = 0; i < index_count; ++i) {
            effect->indices.push_back(base + (indices[i] < vertex_count ? indices[i] : 0));
        }
    } else {
        effect->indices.reserve(effect->indices.size() + vertex_count);
        for (std::uint32_t i = 0; i < vertex_count; ++i) {
            effect->indices.push_back(base + i);
        }
    }
}

void api_draw_mesh(Handle handle, Handle mesh, const float* model, const float* tint) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    Effect* effect = lookup(s.effects, handle);
    if (effect == nullptr || effect->is_pass || mesh == 0) {
        return;
    }
    MeshDraw draw;
    draw.mesh = mesh;
    if (model != nullptr) {
        std::memcpy(draw.model, model, sizeof draw.model);
    } else {
        for (int i = 0; i < 16; ++i) {
            draw.model[i] = (i % 5) == 0 ? 1.0f : 0.0f;
        }
    }
    if (tint != nullptr) {
        std::memcpy(draw.tint, tint, sizeof draw.tint);
    } else {
        draw.tint[0] = draw.tint[1] = draw.tint[2] = draw.tint[3] = 1.0f;
    }
    effect->mesh_draws.push_back(draw);
}

void api_frame(BridgerFxFrame* out) {
    if (out == nullptr) {
        return;
    }
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    *out = s.frame;
}

bool api_project(const double* world, float* screen, float* depth) {
    if (world == nullptr) {
        return false;
    }
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const auto& frame = s.frame;
    if (!frame.camera.valid || frame.width == 0 || frame.height == 0) {
        return false;
    }
    math::Mat4 view_proj;
    std::memcpy(view_proj.m, frame.view_projection, sizeof view_proj.m);
    const math::Vec4 relative{static_cast<float>(world[0] - frame.camera.position[0]),
                              static_cast<float>(world[1] - frame.camera.position[1]),
                              static_cast<float>(world[2] - frame.camera.position[2]), 0.0f};
    math::Mat4 view;
    std::memcpy(view.m, frame.view, sizeof view.m);
    math::Mat4 proj;
    std::memcpy(proj.m, frame.projection, sizeof proj.m);
    const math::Vec4 in_view = math::transform(view, relative);
    const math::Vec4 clip = math::transform(proj, {in_view.x, in_view.y, in_view.z, 1.0f});
    if (clip.w <= 1e-6f) {
        return false;
    }
    if (screen != nullptr) {
        screen[0] = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(frame.width);
        screen[1] = (0.5f - clip.y / clip.w * 0.5f) * static_cast<float>(frame.height);
    }
    if (depth != nullptr) {
        *depth = clip.z / clip.w;
    }
    return true;
}

void api_set_camera_override(const BridgerFxCamera* camera) {
    auto& s = state();
    std::scoped_lock lock(s.camera_mutex);
    if (camera == nullptr) {
        s.has_override = false;
        return;
    }
    s.override_camera = *camera;
    s.override_camera.valid = camera->valid;
    s.has_override = true;
}

std::size_t api_mod_directory(const char* mod_id, char* buffer, std::size_t capacity) {
    std::string id = mod_id != nullptr ? mod_id : "";
    if (id.empty()) {
        id = loader::Registry::instance().active_mod();
    }
    for (const auto& mod : loader::Registry::instance().mods()) {
        if (mod.id == id) {
            return copy_out(mod.directory.string(), buffer, capacity);
        }
    }
    return copy_out(std::string(), buffer, capacity);
}

bool api_upscale_info(std::uint32_t* width, std::uint32_t* height, float* jitter) {
    const auto s = ngx::snapshot();
    struct Copy { const ngx::Snapshot* s; std::uint32_t* w; std::uint32_t* h; float* j; } copy{&s, width, height, jitter};
    const auto fault = guarded_call([](void* raw) {
        auto& c = *static_cast<Copy*>(raw);
        if (c.w) *c.w = c.s->render_width;
        if (c.h) *c.h = c.s->render_height;
        if (c.j) { c.j[0] = c.s->jitter_x; c.j[1] = c.s->jitter_y; }
    }, &copy);
    return !fault && s.seen && s.frames_since < 2 && s.color;
}

void api_revert_owned() {
    const auto owner = owner_of(nullptr, _ReturnAddress());
    if (!owner.empty()) destroy_owned(owner);
}

BridgerFxHandle api_pipeline_hook(std::uint64_t hash, BridgerFxDrawHook before, BridgerFxDrawHook after, void* user) {
    return pipeline::api_hook(hash, before, after, user, _ReturnAddress());
}

BridgerFxHandle api_pipeline_replace(const BridgerFxReplaceDesc* desc) {
    return pipeline::api_replace(desc, _ReturnAddress());
}

BridgerFxHandle api_material_hook(const BridgerFxMaterialHookDesc* desc) {
    return pipeline::api_material_hook(desc, _ReturnAddress());
}

const BridgerFx g_table{
    api_create_shader,
    api_destroy_shader,
    api_reload_shader,
    api_shader_ready,
    api_shader_log,
    api_create_texture,
    api_update_texture,
    api_destroy_texture,
    api_create_render_target,
    api_destroy_texture,
    api_create_mesh,
    api_destroy_mesh,
    api_create_pass,
    api_create_draw_list,
    api_destroy_effect,
    api_set_enabled,
    api_enabled,
    api_set_order,
    api_set_shader,
    api_set_output,
    api_set_constants,
    api_set_texture,
    api_push,
    api_draw_mesh,
    api_frame,
    api_project,
    api_set_camera_override,
    api_mod_directory,
    api_upscale_info,
    api_revert_owned,
    pipeline::api_count,
    pipeline::api_at,
    pipeline::api_find,
    pipeline::api_dump,
    api_pipeline_hook,
    api_pipeline_replace,
    api_material_hook,
    pipeline::api_reload,
    pipeline::api_problem,
    pipeline::api_revert,
    pipeline::api_material_constants,
    pipeline::api_material_texture,
};

}

const BridgerFx* api() {
    return &g_table;
}

}
