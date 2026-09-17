#pragma once

#include <cstddef>
#include <cstdint>

#define BRIDGER_API_VERSION 11u

extern "C" {

enum BridgerLogLevel {
    BRIDGER_LOG_TRACE = 0,
    BRIDGER_LOG_INFO = 1,
    BRIDGER_LOG_WARN = 2,
    BRIDGER_LOG_ERROR = 3,
};

enum BridgerModifier {
    BRIDGER_MOD_NONE = 0,
    BRIDGER_MOD_SHIFT = 1,
    BRIDGER_MOD_CONTROL = 2,
    BRIDGER_MOD_ALT = 4,
};

struct BridgerModInfo {
    std::uint32_t api_version;
    const char* id;
    const char* name;
    const char* version;
    const char* author;
    const char* description;
};

enum BridgerColor {
    BRIDGER_COLOR_TEXT = 0,
    BRIDGER_COLOR_DIM = 1,
    BRIDGER_COLOR_FAINT = 2,
    BRIDGER_COLOR_ACCENT = 3,
    BRIDGER_COLOR_GOOD = 4,
    BRIDGER_COLOR_WARN = 5,
    BRIDGER_COLOR_BAD = 6,
};

using BridgerPanelDraw = void (*)(void* user);
using BridgerTick = void (*)(void* user, float delta_seconds);
using BridgerGuarded = void (*)(void* user);
using BridgerCallback = void (*)(void* user);

struct BridgerUi {
    void (*text)(const char* value);
    void (*text_colored)(std::uint32_t color, const char* value);
    void (*label_value)(const char* label, const char* value);
    bool (*button)(const char* label, float width);
    bool (*checkbox)(const char* label, bool value);
    bool (*selectable)(const char* label, bool selected, float width);
    void (*separator)();
    void (*spacing)(float amount);
    void (*same_line)(float offset);
    void (*progress)(float fraction, float width, std::uint32_t color);
    void (*begin_scroll)(const char* id, float height);
    void (*end_scroll)();
    std::uint32_t (*accent_color)();
    std::uint32_t (*dim_color)();

    bool (*slider_float)(const char* label, float* value, float min, float max, float width);
    bool (*slider_int)(const char* label, int* value, int min, int max, float width);
    bool (*input_text)(const char* id, char* buffer, std::size_t capacity, const char* placeholder,
                       float width);
    void (*header)(const char* label);
    void (*indent)(float amount);
    void (*unindent)(float amount);
    float (*available_width)();

    void (*begin_settings)(float label_width);
    void (*end_settings)();
    bool (*setting_bool)(const char* label, bool* value, const char* help);
    bool (*setting_float)(const char* label, float* value, float min, float max,
                          const char* suffix, const char* help);
    bool (*setting_int)(const char* label, int* value, int min, int max, const char* suffix,
                        const char* help);
    bool (*setting_combo)(const char* label, int* index, const char* const* items, int count,
                          const char* help);
    bool (*setting_key)(const char* label, std::uint32_t* key, const char* help);
    bool (*setting_text)(const char* label, char* buffer, std::size_t capacity,
                         const char* placeholder, const char* help);

    void (*readout)(const char* label, const char* value);
    void (*readout_colored)(const char* label, std::uint32_t color, const char* value);

    bool (*revert_marker)(bool modified);

    bool (*begin_group)(const char* label, bool default_open);
    void (*end_group)();

    void (*note)(const char* value);
    void (*badge)(const char* label, std::uint32_t color);
    void (*keycap)(const char* label);
    void (*tooltip)(const char* value);
    void (*text_wrapped)(std::uint32_t color, const char* value);
    bool (*ghost_button)(const char* label, float width);
    std::uint32_t (*color)(BridgerColor role);
    void (*newline)();

    void (*push_id)(const char* value);
    void (*pop_id)();
};

using BridgerFxHandle = std::uint32_t;

enum BridgerFxShaderKind {
    BRIDGER_FX_SHADER_FULLSCREEN = 0,
    BRIDGER_FX_SHADER_WORLD = 1,
    BRIDGER_FX_SHADER_SCREEN = 2,
};

enum BridgerFxFormat {
    BRIDGER_FX_FORMAT_BACKBUFFER = 0,
    BRIDGER_FX_FORMAT_RGBA8 = 1,
    BRIDGER_FX_FORMAT_RGBA8_SRGB = 2,
    BRIDGER_FX_FORMAT_BGRA8 = 3,
    BRIDGER_FX_FORMAT_R8 = 4,
    BRIDGER_FX_FORMAT_RG8 = 5,
    BRIDGER_FX_FORMAT_R16F = 6,
    BRIDGER_FX_FORMAT_RG16F = 7,
    BRIDGER_FX_FORMAT_RGBA16F = 8,
    BRIDGER_FX_FORMAT_R32F = 9,
    BRIDGER_FX_FORMAT_RG32F = 10,
    BRIDGER_FX_FORMAT_RGBA32F = 11,
    BRIDGER_FX_FORMAT_R11G11B10F = 12,
    BRIDGER_FX_FORMAT_RGB10A2 = 13,
    BRIDGER_FX_FORMAT_BC1 = 14,
    BRIDGER_FX_FORMAT_BC2 = 15,
    BRIDGER_FX_FORMAT_BC3 = 16,
    BRIDGER_FX_FORMAT_BC4 = 17,
    BRIDGER_FX_FORMAT_BC5 = 18,
    BRIDGER_FX_FORMAT_BC6H = 19,
    BRIDGER_FX_FORMAT_BC7 = 20,
};

enum BridgerFxBlend {
    BRIDGER_FX_BLEND_OPAQUE = 0,
    BRIDGER_FX_BLEND_ALPHA = 1,
    BRIDGER_FX_BLEND_PREMULTIPLIED = 2,
    BRIDGER_FX_BLEND_ADDITIVE = 3,
    BRIDGER_FX_BLEND_MULTIPLY = 4,
};

enum BridgerFxDepth {
    BRIDGER_FX_DEPTH_NONE = 0,
    BRIDGER_FX_DEPTH_TEST = 1,
    BRIDGER_FX_DEPTH_WRITE = 2,
    BRIDGER_FX_DEPTH_TEST_WRITE = 3,
};

enum BridgerFxCull {
    BRIDGER_FX_CULL_NONE = 0,
    BRIDGER_FX_CULL_BACK = 1,
    BRIDGER_FX_CULL_FRONT = 2,
};

enum BridgerFxTopology {
    BRIDGER_FX_TOPOLOGY_TRIANGLES = 0,
    BRIDGER_FX_TOPOLOGY_LINES = 1,
    BRIDGER_FX_TOPOLOGY_POINTS = 2,
};

enum BridgerFxFill {
    BRIDGER_FX_FILL_SOLID = 0,
    BRIDGER_FX_FILL_WIREFRAME = 1,
};

enum BridgerFxSpace {
    BRIDGER_FX_SPACE_WORLD = 0,
    BRIDGER_FX_SPACE_SCREEN = 1,
};

enum BridgerFxStage {
    BRIDGER_FX_STAGE_SCENE = 0,
    BRIDGER_FX_STAGE_POST = 1,
    BRIDGER_FX_STAGE_OVERLAY = 2,
    BRIDGER_FX_STAGE_PRE_UPSCALE = 3,
};

struct BridgerFxShaderDesc {
    const char* name;
    const char* owner;
    const char* source;
    const char* path;
    const char* vs_entry;
    const char* ps_entry;
    const char* const* defines;
    std::uint32_t define_count;
    BridgerFxShaderKind kind;
};

struct BridgerFxTextureDesc {
    const char* name;
    std::uint32_t width;
    std::uint32_t height;
    BridgerFxFormat format;
    const void* pixels;
    std::uint32_t row_pitch;
    const char* path;
};

struct BridgerFxRenderTargetDesc {
    const char* name;
    std::uint32_t width;
    std::uint32_t height;
    float scale;
    BridgerFxFormat format;
    bool depth;
};

struct BridgerFxPassDesc {
    const char* name;
    const char* owner;
    BridgerFxHandle shader;
    BridgerFxStage stage;
    int priority;
    BridgerFxHandle output;
    bool clear_output;
    float clear_color[4];
    bool wants_history;
    bool enabled;
    BridgerFxBlend blend;
};

struct BridgerFxDrawListDesc {
    const char* name;
    const char* owner;
    BridgerFxSpace space;
    BridgerFxHandle shader;
    BridgerFxStage stage;
    int priority;
    BridgerFxHandle output;
    BridgerFxBlend blend;
    BridgerFxDepth depth;
    BridgerFxCull cull;
    BridgerFxTopology topology;
    BridgerFxFill fill;
    bool enabled;
};

struct BridgerFxVertex {
    float position[3];
    float normal[3];
    float uv[2];
    std::uint32_t color;
};

struct BridgerFxCamera {
    double position[3];
    float right[3];
    float forward[3];
    float up[3];
    float fov_degrees;
    float near_plane;
    float far_plane;
    bool valid;
};

struct BridgerFxFrame {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t frame;
    float time;
    float delta;
    BridgerFxCamera camera;
    float view[16];
    float projection[16];
    float view_projection[16];
    bool depth_available;
    bool device_ready;
};

enum BridgerFxShaderStage {
    BRIDGER_FX_STAGE_VS = 0,
    BRIDGER_FX_STAGE_PS = 1,
    BRIDGER_FX_STAGE_DS = 2,
    BRIDGER_FX_STAGE_HS = 3,
    BRIDGER_FX_STAGE_GS = 4,
    BRIDGER_FX_STAGE_CS = 5,
};

enum BridgerFxDrawKind {
    BRIDGER_FX_DRAW_INSTANCED = 0,
    BRIDGER_FX_DRAW_INDEXED = 1,
    BRIDGER_FX_DRAW_DISPATCH = 2,
    BRIDGER_FX_DRAW_INDIRECT = 3,
};

struct BridgerFxPipelineInfo {
    std::uint64_t hash;
    std::uint64_t stage_hash[6];
    std::uint32_t compute;
    std::uint32_t draws_last_frame;
    std::uint32_t draws_total;
    std::uint32_t first_use;
    std::uint32_t render_targets;
    std::uint32_t rtv_format[8];
    std::uint32_t dsv_format;
    std::uint32_t topology;
    std::uint32_t replaced;
    std::uint32_t hooked;
    std::uint32_t replaceable;
};

struct BridgerFxDraw {
    std::uint64_t pipeline;
    void* command_list;
    std::uint32_t kind;
    std::uint32_t count;
    std::uint32_t instances;
    std::uint32_t groups[3];
};
using BridgerFxDrawHook = bool (*)(const BridgerFxDraw* draw, void* user);

struct BridgerFxReplaceDesc {
    std::uint64_t pipeline;
    BridgerFxShaderStage stage;
    const char* path;
    const char* entry;
    const char* const* defines;
    std::uint32_t define_count;
};

struct BridgerFxMaterialHookDesc {
    const char* path;
    const char* entry;
    const char* const* defines;
    std::uint32_t define_count;
    std::uint32_t min_render_targets;
    std::uint32_t max_render_targets;
    std::uint64_t only_pipeline;
};

struct BridgerFx {
    BridgerFxHandle (*create_shader)(const BridgerFxShaderDesc* desc);
    void (*destroy_shader)(BridgerFxHandle shader);
    bool (*reload_shader)(BridgerFxHandle shader);
    bool (*shader_ready)(BridgerFxHandle shader);
    std::size_t (*shader_log)(BridgerFxHandle shader, char* buffer, std::size_t capacity);

    BridgerFxHandle (*create_texture)(const BridgerFxTextureDesc* desc);
    bool (*update_texture)(BridgerFxHandle texture, const void* pixels, std::uint32_t row_pitch);
    void (*destroy_texture)(BridgerFxHandle texture);
    BridgerFxHandle (*create_render_target)(const BridgerFxRenderTargetDesc* desc);
    void (*destroy_render_target)(BridgerFxHandle target);
    BridgerFxHandle (*create_mesh)(const BridgerFxVertex* vertices, std::uint32_t vertex_count,
                                   const std::uint32_t* indices, std::uint32_t index_count);
    void (*destroy_mesh)(BridgerFxHandle mesh);

    BridgerFxHandle (*create_pass)(const BridgerFxPassDesc* desc);
    BridgerFxHandle (*create_draw_list)(const BridgerFxDrawListDesc* desc);
    void (*destroy_effect)(BridgerFxHandle effect);
    void (*set_enabled)(BridgerFxHandle effect, bool enabled);
    bool (*enabled)(BridgerFxHandle effect);
    void (*set_order)(BridgerFxHandle effect, BridgerFxStage stage, int priority);
    void (*set_shader)(BridgerFxHandle effect, BridgerFxHandle shader);
    void (*set_output)(BridgerFxHandle effect, BridgerFxHandle target);
    void (*set_constants)(BridgerFxHandle effect, const void* data, std::size_t size);
    void (*set_texture)(BridgerFxHandle effect, std::uint32_t slot, BridgerFxHandle resource);

    void (*push)(BridgerFxHandle list, const BridgerFxVertex* vertices, std::uint32_t vertex_count,
                 const std::uint32_t* indices, std::uint32_t index_count);
    void (*draw_mesh)(BridgerFxHandle list, BridgerFxHandle mesh, const float* model,
                      const float* tint);

    void (*frame)(BridgerFxFrame* out);
    bool (*project)(const double* world, float* screen, float* depth);
    void (*set_camera_override)(const BridgerFxCamera* camera);
    std::size_t (*mod_directory)(const char* mod_id, char* buffer, std::size_t capacity);
    bool (*upscale_info)(std::uint32_t* width, std::uint32_t* height, float* jitter_xy);
    void (*revert_owned)();

    std::uint32_t (*pipeline_count)();
    bool (*pipeline_at)(std::uint32_t index, BridgerFxPipelineInfo* out);
    bool (*pipeline_find)(std::uint64_t hash, BridgerFxPipelineInfo* out);
    std::size_t (*pipeline_dump)(std::uint64_t hash, char* buffer, std::size_t capacity);
    BridgerFxHandle (*pipeline_hook)(std::uint64_t hash, BridgerFxDrawHook before,
                                     BridgerFxDrawHook after, void* user);
    BridgerFxHandle (*pipeline_replace)(const BridgerFxReplaceDesc* desc);
    BridgerFxHandle (*material_hook)(const BridgerFxMaterialHookDesc* desc);
    bool (*pipeline_reload)(BridgerFxHandle handle);
    std::size_t (*pipeline_problem)(BridgerFxHandle handle, char* buffer, std::size_t capacity);
    void (*pipeline_revert)(BridgerFxHandle handle);

    bool (*material_constants)(BridgerFxHandle hook, const void* data, std::size_t size);
    bool (*material_texture)(BridgerFxHandle hook, std::uint32_t slot, BridgerFxHandle texture);
};

using BridgerContentHandle = std::uint32_t;

struct BridgerField {
    const char* name;
    const char* type;
    std::uint32_t offset;
    std::uint32_t size;
    const void* getter;
    const void* setter;
};

enum BridgerContentOrder {
    BRIDGER_CONTENT_REPLACE = 0,
    BRIDGER_CONTENT_BEFORE = 1,
    BRIDGER_CONTENT_AFTER = 2,
};

using BridgerMessageHandler = void (*)(void* self, void* message, void* user);

enum BridgerContentKind {
    BRIDGER_CONTENT_OBJECT = 0,
    BRIDGER_CONTENT_PATCH = 1,
    BRIDGER_CONTENT_INJECTION = 2,
    BRIDGER_CONTENT_HANDLER = 3,
    BRIDGER_CONTENT_METHOD = 4,
    BRIDGER_CONTENT_FUNCTION = 5,
};

struct BridgerContentEntry {
    BridgerContentHandle handle;
    BridgerContentKind kind;
    const char* owner;
    const char* detail;
    std::uint64_t address;
    bool intact;
};

struct BridgerContent {
    int (*field_offset)(const char* type_name, const char* field_name);
    bool (*field)(const char* type_name, const char* field_name, BridgerField* out);
    std::uint32_t (*field_count)(const char* type_name);
    bool (*field_at)(const char* type_name, std::uint32_t index, BridgerField* out);
    bool (*object_field)(const void* object, const char* field_name, BridgerField* out);
    std::uint32_t (*type_alignment)(const char* type_name);

    void* (*create)(const char* type_name);
    void* (*clone)(const void* source);
    bool (*destroy)(void* object);
    bool (*owns)(const void* object);

    BridgerContentHandle (*patch)(void* address, const void* bytes, std::size_t size);

    BridgerContentHandle (*inject)(std::int32_t* count, void*** data, void* const* items,
                                   std::uint32_t item_count, bool persistent);

    BridgerContentHandle (*override_handler)(const char* class_name, const char* message_name,
                                             BridgerMessageHandler fn, void* user,
                                             BridgerContentOrder order);
    void* (*original_handler)(BridgerContentHandle handle);

    void* (*handler_for)(const void* receiver, const char* message_name);
    bool (*deliver)(void* receiver, void* message);

    BridgerContentHandle (*replace_function)(void* target, void* detour, void** original);
    BridgerContentHandle (*force_result)(void* target, std::uint64_t result);

    void** (*vtable)(const char* class_name);
    void** (*vtable_of)(const void* object);
    BridgerContentHandle (*override_method)(void** vtable, std::uint32_t slot, void* fn,
                                            void** original);
    BridgerContentHandle (*override_instance)(void* object, std::uint32_t slot, void* fn,
                                              void** original);

    bool (*revert)(BridgerContentHandle handle);
    bool (*intact)(BridgerContentHandle handle);
    bool (*reapply)(BridgerContentHandle handle);
    void (*revert_all)(const char* mod_id);
    std::uint32_t (*count)(const char* mod_id);
    bool (*entry_at)(std::uint32_t index, BridgerContentEntry* out);
};

struct BridgerApi {
    std::uint32_t version;
    std::uintptr_t image_base;

    void (*log)(BridgerLogLevel level, const char* message);
    void* (*resolve)(std::uintptr_t rva);
    const void* (*find_type)(const char* name);
    void* (*find_symbol)(const char* group, const char* name);
    bool (*install_hook)(void* target, void* detour, void** original);
    bool (*remove_hook)(void* target);
    void (*register_panel)(const char* label, BridgerPanelDraw draw, void* user);
    void (*register_tick)(BridgerTick tick, void* user);
    std::uint32_t (*guarded)(BridgerGuarded body, void* user);
    const BridgerUi* ui;
    void (*register_game_tick)(BridgerTick tick, void* user);

    void* (*find_handler)(const char* class_name, const char* message_name);
    const void* (*rtti_of)(const void* object);
    const char* (*rtti_name)(const void* rtti);
    bool (*rtti_is_a)(const void* rtti, const void* base);
    std::uint32_t (*type_size)(const void* rtti);
    void* (*scan_pattern)(const char* pattern);
    void (*run_on_game_thread)(BridgerCallback fn, void* user);

    bool (*register_hotkey)(std::uint32_t key, std::uint32_t modifiers, BridgerCallback fn,
                            void* user);
    bool (*overlay_visible)();
    bool (*key_down)(std::uint32_t key);

    bool (*provide)(const char* name, const void* table);
    const void* (*require)(const char* name);
    bool (*mod_loaded)(const char* id);

    bool (*settings_has)(const char* mod_id, const char* key);
    bool (*settings_get_bool)(const char* mod_id, const char* key, bool fallback);
    double (*settings_get_number)(const char* mod_id, const char* key, double fallback);
    std::size_t (*settings_get_text)(const char* mod_id, const char* key, const char* fallback,
                                     char* buffer, std::size_t capacity);
    void (*settings_set_bool)(const char* mod_id, const char* key, bool value);
    void (*settings_set_number)(const char* mod_id, const char* key, double value);
    void (*settings_set_text)(const char* mod_id, const char* key, const char* value);
    void (*settings_erase)(const char* mod_id, const char* key);

    bool (*rebind_hotkey)(BridgerCallback fn, void* user, std::uint32_t key,
                          std::uint32_t modifiers);

    const BridgerFx* fx;

    const BridgerContent* content;
};

using BridgerModInfoFn = const BridgerModInfo* (*)();
using BridgerModLoadFn = bool (*)(const BridgerApi*);
using BridgerModUnloadFn = void (*)();

}

#define BRIDGER_MOD_EXPORT extern "C" __declspec(dllexport)
