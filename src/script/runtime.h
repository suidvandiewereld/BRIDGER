#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "loader/registry.h"

namespace bridger::script {

void start(const std::filesystem::path& root);
void prepare_exit();
void shutdown();

bool load(const loader::Mod& mod, std::string& error);
void stop(const std::string& id);
void close(const std::string& id);

std::filesystem::file_time_type sources_stamp(const loader::Mod& mod);
bool check_syntax(const loader::Mod& mod, std::string& error);

struct Line {
    int kind = 0;
    std::string source;
    std::string text;
};

struct Evaluation {
    bool ok = false;
    std::string result;
    std::string output;
};

Evaluation evaluate(const std::string& target, const std::string& code, unsigned timeout_ms = 5000);
void submit(const std::string& target, const std::string& code);

std::vector<std::string> complete(const std::string& target, const std::string& prefix);

std::vector<Line> console_lines(std::size_t max = 2000);
std::uint64_t console_since(std::uint64_t since, std::vector<Line>& out);
void console_clear();
std::vector<std::string> targets();

struct Status {
    std::string id;
    bool alive = false;
    std::string last_error;
    std::uint64_t errors = 0;
    std::uint64_t callbacks = 0;
    std::size_t hooks = 0;
    std::size_t memory_kb = 0;
};
std::vector<Status> statuses();

void draw_panel();

}
