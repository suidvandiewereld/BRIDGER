#include "overlay/inspect.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "debugger/model.h"
#include "decima/dumper.h"
#include "loader/registry.h"
#include "ui/font.h"
#include "ui/ui.h"

namespace bridger::overlay::inspect {
namespace {

struct Field {
    std::uint32_t offset = 0;
    std::string name;
    std::string type;
    std::string value;
    std::uintptr_t follow = 0;
    bool property = false;
};

std::string g_input;
std::string g_status = "Enter an address, or open one from a mod panel.";
std::uintptr_t g_address = 0;
std::string g_type_name;
std::uint32_t g_type_size = 0;
std::vector<Field> g_fields;
std::vector<std::uintptr_t> g_history;
std::string g_filter;

std::atomic<std::uintptr_t> g_requested{0};
std::atomic<bool> g_request_pending{false};

using debugger::model::describe;
using debugger::model::parse_address;
using debugger::model::type_name_at;

void add_member(const decima::TypeMember& member, std::uintptr_t base_offset);

void collect(const std::string& type_name, std::uintptr_t base_offset, int depth) {
    if (depth > 8) {
        return;
    }
    decima::TypeDetail detail;
    if (!describe(type_name, detail)) {
        return;
    }
    for (const auto& base : detail.bases) {
        collect(base.name, base_offset + base.offset, depth + 1);
    }
    for (const auto& member : detail.members) {
        add_member(member, base_offset);
    }
}

void add_member(const decima::TypeMember& member, std::uintptr_t base_offset) {
    Field field;
    field.offset = static_cast<std::uint32_t>(base_offset) + member.offset;
    field.name = member.name;
    field.type = member.type;
    if (member.getter != 0) {
        field.property = true;
        field.value = std::format("via getter {:#x}", member.getter);
        g_fields.push_back(std::move(field));
        return;
    }
    field.value = debugger::model::format_value(member.type, g_address + field.offset, field.follow);
    g_fields.push_back(std::move(field));
}

void load(std::uintptr_t address) {
    g_address = address;
    g_fields.clear();
    g_type_name.clear();
    g_type_size = 0;

    if (address == 0) {
        g_status = "Enter an address, or open one from a mod panel.";
        return;
    }
    const auto* api = loader::api();
    const auto* rtti = api != nullptr ? api->rtti_of(reinterpret_cast<const void*>(address))
                                      : nullptr;
    g_type_name = type_name_at(address);
    if (g_type_name.empty() || rtti == nullptr) {
        g_status = std::format("{:#x} is not a recognisable engine object "
                               "(no RTTI in vtable slot 0)", address);
        return;
    }
    decima::TypeDetail root;
    if (decima::describe_type(reinterpret_cast<std::uintptr_t>(rtti), root)) {
        g_type_size = root.size;
        for (const auto& base : root.bases) {
            collect(base.name, base.offset, 1);
        }
        for (const auto& member : root.members) {
            add_member(member, 0);
        }
    }
    std::stable_sort(g_fields.begin(), g_fields.end(),
                     [](const Field& a, const Field& b) { return a.offset < b.offset; });
    g_status = g_fields.empty() ? "Type has no reflected fields." : std::string{};
}

bool matches(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

}

void focus(std::uintptr_t address) {
    g_requested.store(address, std::memory_order_relaxed);
    g_request_pending.store(true, std::memory_order_release);
}

bool pending() {
    return g_request_pending.load(std::memory_order_acquire);
}

void draw() {
    const auto& t = ui::theme();

    if (g_request_pending.exchange(false, std::memory_order_acq_rel)) {
        const auto address = g_requested.load(std::memory_order_relaxed);
        if (g_address != 0 && g_address != address) {
            g_history.push_back(g_address);
        }
        g_input = std::format("{:x}", address);
        load(address);
    }

    if (ui::input_text("inspect_address", g_input, 320.0f, "object address, e.g. 1f7c04a1200")) {
        std::uintptr_t parsed = 0;
        if (parse_address(g_input, parsed)) {
            g_history.clear();
            load(parsed);
        } else {
            g_status = "Not a hexadecimal address.";
        }
    }
    ui::same_line(0.0f);
    if (!g_history.empty() && ui::button("Back")) {
        const auto previous = g_history.back();
        g_history.pop_back();
        g_input = std::format("{:x}", previous);
        load(previous);
    }

    if (!g_type_name.empty()) {
        ui::text_colored(t.text, g_type_name);
        const ui::font::ScopedFace micro_face(ui::font::Face::Micro);
        ui::text_colored(t.text_faint,
                         std::format("{:#x}   {} bytes   {} field(s){}", g_address, g_type_size,
                                     g_fields.size(),
                                     g_history.empty()
                                         ? std::string{}
                                         : std::format("   {} back", g_history.size())));
    }
    if (!g_status.empty()) {
        ui::text_colored(t.text_dim, g_status);
    }
    if (g_fields.empty()) {
        return;
    }

    ui::input_text("inspect_filter", g_filter, 320.0f, "filter fields");
    if (ui::button("Refresh")) {
        load(g_address);
    }
    ui::same_line(0.0f);
    ui::text_colored(t.text_faint, "values are read when the view loads, not every frame");

    std::vector<std::uintptr_t> follow;
    ui::begin_scroll("inspect_fields", ui::remaining_height() - 8.0f);
    {
        const ui::font::ScopedFace micro_face(ui::font::Face::Micro);
        for (const auto& field : g_fields) {
            if (!matches(field.name, g_filter) && !matches(field.type, g_filter)) {
                continue;
            }
            ui::begin_row(16.0f);
            ui::row_cell(56.0f, t.text_faint,
                         field.property ? "  --" : std::format("+{:03x}", field.offset));
            ui::row_cell(210.0f, t.text, field.name);
            ui::row_cell(190.0f, t.text_faint, field.type);
            ui::end_row();
            ui::indent(56.0f);
            if (field.follow != 0) {
                const ui::IdScope row_id(field.offset);
                if (ui::selectable(field.value, false, 420.0f)) {
                    follow.push_back(field.follow);
                }
            } else {
                ui::text_colored(t.text_dim, field.value);
            }
            ui::unindent(56.0f);
        }
    }
    ui::end_scroll();

    if (!follow.empty()) {
        g_history.push_back(g_address);
        g_input = std::format("{:x}", follow.front());
        load(follow.front());
    }
}

}
