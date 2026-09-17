#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "bridger/api.h"

namespace bridger::loader {

enum class State {
    Discovered,
    Disabled,
    Loaded,
    Failed,
};

struct Mod {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string entry;
    std::string error;
    std::filesystem::path directory;
    std::vector<std::string> dependencies;
    State state = State::Discovered;
    void* module = nullptr;
    BridgerModUnloadFn unload = nullptr;
    std::filesystem::path shadow;
    std::filesystem::file_time_type source_time{};
    bool script = false;
    bool loose = false;
};

struct Tick {
    std::string owner;
    BridgerTick fn = nullptr;
    void* user = nullptr;
};

struct Panel {
    std::string owner;
    std::string label;
    BridgerPanelDraw draw = nullptr;
    void* user = nullptr;
};

class Registry {
public:
    static Registry& instance();

    void configure(const std::filesystem::path& root, std::uintptr_t image_base);
    void discover();
    void load_all();
    void unload_all();

    void request_reload(const std::string& id);
    void process_pending_reloads();
    void poll_for_changes(float dt);
    void set_auto_reload(bool on) { auto_reload_ = on; }
    [[nodiscard]] bool auto_reload() const { return auto_reload_; }

    [[nodiscard]] const std::vector<Mod>& mods() const { return mods_; }
    [[nodiscard]] const std::vector<Panel>& panels() const { return panels_; }
    [[nodiscard]] const std::vector<Tick>& ticks() const { return ticks_; }
    [[nodiscard]] const std::vector<Tick>& game_ticks() const { return game_ticks_; }
    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    [[nodiscard]] std::uintptr_t image_base() const { return image_base_; }

    void set_enabled(const std::string& id, bool enabled);
    [[nodiscard]] bool enabled(const std::string& id) const;
    void save_config() const;

    void add_panel(Panel panel);
    void add_tick(Tick tick);
    void add_game_tick(Tick tick);
    void set_active_mod(const std::string& id) { active_ = id; }
    [[nodiscard]] const std::string& active_mod() const;

private:
    void load_config();
    bool read_manifest(Mod& mod) const;
    [[nodiscard]] std::vector<Mod> find_new_scripts() const;
    bool load_one(Mod& mod);
    bool unload_one(Mod& mod);
    void reload(const std::string& id);
    void remove_owned(const std::string& id, void* module);
    [[nodiscard]] std::vector<std::size_t> resolve_order() const;

    std::filesystem::path root_;
    std::uintptr_t image_base_ = 0;
    std::vector<Mod> mods_;
    std::vector<Panel> panels_;
    std::vector<Tick> ticks_;
    std::vector<Tick> game_ticks_;
    std::vector<std::string> disabled_;
    std::string active_;
    std::mutex pending_mutex_;
    std::vector<std::string> pending_;
    std::vector<Mod> pending_new_;
    bool auto_reload_ = true;
    float watch_timer_ = 0.0f;
    float discover_timer_ = 0.0f;

    static constexpr std::size_t kModCapacity = 512;
};

const BridgerApi* api();

std::string owner_of_address(const void* address);

bool ensure_game_tick();

bool on_game_thread();
bool game_thread_ticking(unsigned within_ms = 1000);

class ScopedActor {
public:
    explicit ScopedActor(std::string id);
    ~ScopedActor();
    ScopedActor(const ScopedActor&) = delete;
    ScopedActor& operator=(const ScopedActor&) = delete;

private:
    std::string previous_;
    bool had_previous_ = false;
};

class DispatchGuard {
public:
    DispatchGuard();
    ~DispatchGuard();
    DispatchGuard(const DispatchGuard&) = delete;
    DispatchGuard& operator=(const DispatchGuard&) = delete;
};

}
