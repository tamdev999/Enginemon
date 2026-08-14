#pragma once
// engine/mod/mod_manager.hpp
// Mod loading, dependency resolution, and startup composition
// Mods modify mutable definition/behavior at startup, then freeze

#include "engine/core/types.hpp"
#include "engine/core/game_definition.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <variant>

namespace enginemon {

// Forward declarations
class LuaRuntime;
class GameDefinition;

// Mod metadata (from mod.json)
struct ModInfo {
    std::string id;                 // Unique identifier (e.g. "my-cool-mod")
    std::string name;               // Display name
    std::string version;            // Semantic version
    std::string author;
    std::string description;
    
    // Dependencies
    struct Dependency {
        std::string mod_id;
        std::string min_version;    // Empty = any
        std::string max_version;    // Empty = any
        bool optional = false;
    };
    std::vector<Dependency> dependencies;
    
    // Incompatibilities
    std::vector<std::string> conflicts;
    
    // Load order hints
    std::vector<std::string> load_before;   // Load this mod before these
    std::vector<std::string> load_after;    // Load this mod after these
    
    // Entry points
    std::string main_script;        // Main Lua file
    std::string setup_script;       // Optional setup/config
    
    // Content paths
    std::filesystem::path root_path;
    std::filesystem::path scripts_path;
    std::filesystem::path assets_path;
    std::filesystem::path data_path;
};

// Mod state
enum class ModState {
    Discovered,     // Found on disk
    Enabled,        // User enabled
    Disabled,       // User disabled
    Loading,        // Currently loading
    Loaded,         // Successfully loaded
    Error           // Failed to load
};

// Loaded mod
struct LoadedMod {
    ModInfo info;
    ModState state = ModState::Discovered;
    std::string error_message;
    
    // Runtime state
    int lua_ref = LUA_NOREF;        // Reference to mod table in Lua
};

// Mod composition operations
// These represent what mods can do during startup composition

// Add a new entry to a registry
template<typename Id, typename Data>
struct ModOp_Add {
    Id id;
    Data data;
};

// Patch an existing entry
template<typename Id, typename Data>
struct ModOp_Patch {
    Id id;
    std::function<void(Data&)> patcher;
};

// Replace an entry entirely
template<typename Id, typename Data>
struct ModOp_Replace {
    Id id;
    Data data;
};

// Remove an entry
template<typename Id>
struct ModOp_Remove {
    Id id;
};

// Mod manager
class ModManager {
public:
    ModManager();
    ~ModManager();
    
    // Discovery
    void scan_directory(const std::filesystem::path& mods_dir);
    void add_mod_path(const std::filesystem::path& mod_path);
    
    // Enable/disable
    void enable_mod(const std::string& mod_id);
    void disable_mod(const std::string& mod_id);
    bool is_enabled(const std::string& mod_id) const;
    
    // Load order
    std::vector<std::string> compute_load_order() const;
    bool validate_dependencies() const;
    std::vector<std::string> get_missing_dependencies() const;
    std::vector<std::string> get_conflicts() const;
    
    // Loading
    // Called after base game loaded, before registries frozen
    bool load_all(GameDefinition& game, LuaRuntime& lua);
    bool load_mod(const std::string& mod_id, GameDefinition& game, LuaRuntime& lua);
    
    // Query
    std::vector<const LoadedMod*> all_mods() const;
    const LoadedMod* get_mod(const std::string& mod_id) const;
    std::vector<const LoadedMod*> enabled_mods() const;
    std::vector<const LoadedMod*> loaded_mods() const;
    
    // Settings
    void set_mod_setting(const std::string& mod_id, const std::string& key, 
                         const std::variant<bool, int, float, std::string>& value);
    std::optional<std::variant<bool, int, float, std::string>> 
        get_mod_setting(const std::string& mod_id, const std::string& key) const;
    
    // Save/load mod configuration
    void save_config(const std::filesystem::path& path) const;
    void load_config(const std::filesystem::path& path);
    
    // Mod compatibility with saves
    struct SaveCompatibility {
        bool compatible = true;
        std::vector<std::string> missing_mods;      // Mods in save but not loaded
        std::vector<std::string> version_mismatches;// Version differs from save
        std::vector<std::string> new_mods;          // Loaded but not in save
    };
    SaveCompatibility check_save_compatibility(const std::filesystem::path& save_path) const;

private:
    std::unordered_map<std::string, LoadedMod> mods_;
    std::unordered_set<std::string> enabled_ids_;
    
    // Settings storage
    std::unordered_map<std::string, 
        std::unordered_map<std::string, std::variant<bool, int, float, std::string>>> 
        mod_settings_;
    
    // Load helpers
    bool load_mod_info(const std::filesystem::path& mod_path, ModInfo& info);
    bool execute_mod_setup(LoadedMod& mod, LuaRuntime& lua);
    bool execute_mod_main(LoadedMod& mod, GameDefinition& game, LuaRuntime& lua);
    
    // Dependency resolution (topological sort)
    std::vector<std::string> topological_sort(
        const std::vector<std::string>& mod_ids) const;
    
    bool has_cycle(const std::string& mod_id, 
                   std::unordered_set<std::string>& visiting,
                   std::unordered_set<std::string>& visited) const;
};

// ============================================================================
// Mod Lua API
// Exposed to mods during composition phase
// ============================================================================

// Example mod script:
//
// local mod = {}
//
// -- Called during composition phase (can modify registries)
// function mod.setup(ctx)
//     -- Add a new Pokemon
//     ctx.registry.species:add({
//         id = 252,  -- After Celebi
//         name = "FAKEMON",
//         base_stats = { hp = 50, attack = 60, ... },
//         ...
//     })
//     
//     -- Patch an existing Pokemon
//     ctx.registry.species:patch(25, function(pikachu)
//         pikachu.base_stats.speed = 100
//     end)
//     
//     -- Add a new move
//     ctx.registry.moves:add({ ... })
//     
//     -- Add map script
//     ctx.scripts:add_map_script("MyNewArea", [[
//         function on_enter(ctx)
//             ctx.ui:text("Welcome to the new area!")
//         end
//     ]])
// end
//
// -- Called after composition, during normal gameplay
// -- Can hook into events
// function mod.init(ctx)
//     ctx.events:on("battle_start", function(battle)
//         print("Battle started!")
//     end)
// end
//
// return mod

} // namespace enginemon
