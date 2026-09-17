#include "core/settings.h"

#include <fstream>
#include <map>
#include <mutex>

#include <nlohmann/json.hpp>

#include "core/log.h"

namespace bridger::settings {
namespace {

using json = nlohmann::json;

constexpr float kFlushInterval = 1.0f;

struct Store {
    json values = json::object();
    bool dirty = false;
    bool loaded = false;
};

std::mutex g_mutex;
std::filesystem::path g_root;
std::map<std::string, Store, std::less<>> g_stores;
float g_since_flush = 0.0f;

Store& store(std::string_view mod) {
    const auto found = g_stores.find(mod);
    if (found != g_stores.end()) {
        return found->second;
    }
    return g_stores.emplace(std::string(mod), Store{}).first->second;
}

std::filesystem::path file_for(std::string_view mod) {
    return g_root / "config" / (std::string(mod) + ".json");
}

void write(const std::string& mod, Store& current) {
    if (g_root.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(g_root / "config", error);
    const auto path = file_for(mod);
    std::ofstream stream(path, std::ios::trunc);
    if (!stream.is_open()) {
        log::warn("settings: could not write {}", path.string());
        current.dirty = false;
        return;
    }
    stream << current.values.dump(2) << '\n';
    current.dirty = false;
}

}

void configure(const std::filesystem::path& root) {
    std::scoped_lock lock(g_mutex);
    g_root = root;
}

void load(std::string_view mod) {
    std::scoped_lock lock(g_mutex);
    Store& current = store(mod);
    if (current.loaded) {
        return;
    }
    current.loaded = true;
    const auto path = file_for(mod);
    std::ifstream stream(path);
    if (!stream.is_open()) {
        return;
    }
    try {
        json document = json::parse(stream, nullptr, true, true);
        if (document.is_object()) {
            current.values = std::move(document);
        }
    } catch (const json::exception& error) {
        log::warn("settings: {} is not valid json: {}", path.string(), error.what());
    }
}

bool has(std::string_view mod, std::string_view key) {
    std::scoped_lock lock(g_mutex);
    const auto found = g_stores.find(mod);
    return found != g_stores.end() && found->second.values.contains(key);
}

bool get_bool(std::string_view mod, std::string_view key, bool fallback) {
    std::scoped_lock lock(g_mutex);
    const auto found = g_stores.find(mod);
    if (found == g_stores.end()) {
        return fallback;
    }
    const auto value = found->second.values.find(key);
    if (value == found->second.values.end() || !value->is_boolean()) {
        return fallback;
    }
    return value->get<bool>();
}

double get_number(std::string_view mod, std::string_view key, double fallback) {
    std::scoped_lock lock(g_mutex);
    const auto found = g_stores.find(mod);
    if (found == g_stores.end()) {
        return fallback;
    }
    const auto value = found->second.values.find(key);
    if (value == found->second.values.end() || !value->is_number()) {
        return fallback;
    }
    return value->get<double>();
}

std::string get_text(std::string_view mod, std::string_view key, std::string_view fallback) {
    std::scoped_lock lock(g_mutex);
    const auto found = g_stores.find(mod);
    if (found == g_stores.end()) {
        return std::string(fallback);
    }
    const auto value = found->second.values.find(key);
    if (value == found->second.values.end() || !value->is_string()) {
        return std::string(fallback);
    }
    return value->get<std::string>();
}

void set_bool(std::string_view mod, std::string_view key, bool value) {
    std::scoped_lock lock(g_mutex);
    Store& current = store(mod);
    json& slot = current.values[std::string(key)];
    if (slot.is_boolean() && slot.get<bool>() == value) {
        return;
    }
    slot = value;
    current.dirty = true;
}

void set_number(std::string_view mod, std::string_view key, double value) {
    std::scoped_lock lock(g_mutex);
    Store& current = store(mod);
    json& slot = current.values[std::string(key)];
    if (slot.is_number() && slot.get<double>() == value) {
        return;
    }
    slot = value;
    current.dirty = true;
}

void set_text(std::string_view mod, std::string_view key, std::string_view value) {
    std::scoped_lock lock(g_mutex);
    Store& current = store(mod);
    json& slot = current.values[std::string(key)];
    if (slot.is_string() && slot.get<std::string>() == value) {
        return;
    }
    slot = std::string(value);
    current.dirty = true;
}

void erase(std::string_view mod, std::string_view key) {
    std::scoped_lock lock(g_mutex);
    const auto found = g_stores.find(mod);
    if (found == g_stores.end()) {
        return;
    }
    if (found->second.values.erase(std::string(key)) > 0) {
        found->second.dirty = true;
    }
}

std::size_t count(std::string_view mod) {
    std::scoped_lock lock(g_mutex);
    const auto found = g_stores.find(mod);
    return found == g_stores.end() ? 0 : found->second.values.size();
}

std::filesystem::path path_for(std::string_view mod) {
    std::scoped_lock lock(g_mutex);
    return file_for(mod);
}

void flush(float delta_seconds) {
    std::scoped_lock lock(g_mutex);
    g_since_flush += delta_seconds;
    if (g_since_flush < kFlushInterval) {
        return;
    }
    g_since_flush = 0.0f;
    for (auto& [mod, current] : g_stores) {
        if (current.dirty) {
            write(mod, current);
        }
    }
}

void flush_now() {
    std::scoped_lock lock(g_mutex);
    g_since_flush = 0.0f;
    for (auto& [mod, current] : g_stores) {
        if (current.dirty) {
            write(mod, current);
        }
    }
}

}
