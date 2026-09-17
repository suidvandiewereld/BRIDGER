#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace bridger::settings {

void configure(const std::filesystem::path& root);

void load(std::string_view mod);

[[nodiscard]] bool has(std::string_view mod, std::string_view key);

[[nodiscard]] bool get_bool(std::string_view mod, std::string_view key, bool fallback);
[[nodiscard]] double get_number(std::string_view mod, std::string_view key, double fallback);
[[nodiscard]] std::string get_text(std::string_view mod, std::string_view key,
                                   std::string_view fallback);

void set_bool(std::string_view mod, std::string_view key, bool value);
void set_number(std::string_view mod, std::string_view key, double value);
void set_text(std::string_view mod, std::string_view key, std::string_view value);

void erase(std::string_view mod, std::string_view key);

[[nodiscard]] std::size_t count(std::string_view mod);
[[nodiscard]] std::filesystem::path path_for(std::string_view mod);

void flush(float delta_seconds);
void flush_now();

}
