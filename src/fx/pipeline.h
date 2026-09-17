#pragma once

#include <Windows.h>
#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bridger/api.h"

namespace bridger::fx::pipeline {

void watch_device_creation();
void wait_for_device(unsigned timeout_ms);
bool prepare(void** device_vtable, void** command_list_vtable);
void configure(const std::filesystem::path& root);
void end_frame(unsigned frame);
void shutdown();
void destroy_owned(std::string_view owner);

void set_enabled(bool enabled);
[[nodiscard]] bool enabled();
void set_retain(bool retain);
[[nodiscard]] bool retain();
void set_bindings(bool enabled);
void set_bindings_early(bool enabled);
[[nodiscard]] bool bindings();
void set_stream_output(bool enabled);
void set_geometry_capture(bool enabled);
[[nodiscard]] bool geometry_capture();

struct Snapshot {
    unsigned pipelines = 0;
    unsigned graphics = 0;
    unsigned compute = 0;
    unsigned stream_created = 0;
    unsigned library_loaded = 0;
    unsigned blobs = 0;
    std::size_t retained_bytes = 0;
    unsigned draws_last_frame = 0;
    unsigned dispatches_last_frame = 0;
    unsigned used_last_frame = 0;
    unsigned hooked = 0;
    unsigned replaced = 0;
    unsigned material_applied = 0;
    unsigned material_failed = 0;
    unsigned material_skipped = 0;
    bool material_active = false;
    unsigned stream_twins = 0;
    unsigned stream_draws_last_frame = 0;
    unsigned stream_skipped_last_frame = 0;
    std::string material_owner;
    std::string material_problem;
    std::string problem;
    bool hooks_installed = false;
};
[[nodiscard]] Snapshot snapshot();

struct Row {
    std::uint64_t hash = 0;
    bool compute = false;
    unsigned draws = 0;
    unsigned order = 0;
    unsigned render_targets = 0;
    DXGI_FORMAT rtv0 = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    bool replaced = false;
    bool hooked = false;
    bool material = false;
    std::string problem;
};
std::vector<Row> busiest(unsigned limit);

std::string dump(std::uint64_t hash);

std::uint32_t api_count();
bool api_at(std::uint32_t index, BridgerFxPipelineInfo* out);
bool api_find(std::uint64_t hash, BridgerFxPipelineInfo* out);
std::size_t api_dump(std::uint64_t hash, char* buffer, std::size_t capacity);
BridgerFxHandle api_hook(std::uint64_t hash, BridgerFxDrawHook before, BridgerFxDrawHook after,
                         void* user, const void* caller);
BridgerFxHandle api_replace(const BridgerFxReplaceDesc* desc, const void* caller);
BridgerFxHandle api_material_hook(const BridgerFxMaterialHookDesc* desc, const void* caller);
bool api_reload(BridgerFxHandle handle);
bool api_material_constants(BridgerFxHandle handle, const void* data, std::size_t size);
bool api_material_texture(BridgerFxHandle handle, std::uint32_t slot, BridgerFxHandle texture);
std::size_t api_problem(BridgerFxHandle handle, char* buffer, std::size_t capacity);
void api_revert(BridgerFxHandle handle);

}
