#pragma once
// crystal/rom/symbol_map.hpp
// Loads RGBDS symbol files from pokecrystal builds
// Provides named access to ROM locations instead of raw offsets

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace crystal {

// Symbol types
enum class SymbolType {
    Label,      // Code/data label
    Constant,   // EQU constant
    Section,    // Section name
    Unknown
};

// Single symbol entry
struct Symbol {
    std::string name;
    SymbolType type;
    uint8_t bank;           // ROM bank (0-127 for 2MB)
    uint16_t address;       // Address within bank
    uint32_t flat_address;  // Flat ROM offset
    std::string file;       // Source file (if available)
    int line;               // Source line (if available)
};

// Loaded symbol map
class SymbolMap {
public:
    // Load from RGBDS .sym file
    static std::unique_ptr<SymbolMap> load(const std::filesystem::path& path);
    
    // Load from multiple files (merge)
    static std::unique_ptr<SymbolMap> load_multiple(
        const std::vector<std::filesystem::path>& paths);
    
    // Lookup by name
    const Symbol* find(const std::string& name) const;
    
    // Lookup by address (find symbol at or before address)
    const Symbol* find_at(uint32_t flat_address) const;
    
    // Get all symbols matching prefix (e.g. "Route1_" for all Route1 symbols)
    std::vector<const Symbol*> find_prefix(const std::string& prefix) const;
    
    // Get all symbols in a bank
    std::vector<const Symbol*> find_in_bank(uint8_t bank) const;
    
    // Check if symbol exists
    bool has(const std::string& name) const;
    
    // Get address by name (convenience)
    std::optional<uint32_t> address(const std::string& name) const;
    
    // Get name by address (for disassembly/debugging)
    std::optional<std::string> name_at(uint32_t flat_address) const;
    
    // All symbols
    const std::vector<Symbol>& all() const { return symbols_; }
    
    // Statistics
    size_t count() const { return symbols_.size(); }

private:
    std::vector<Symbol> symbols_;
    std::unordered_map<std::string, size_t> by_name_;
    std::unordered_map<uint32_t, size_t> by_address_;
    
    void build_indexes();
    static Symbol parse_line(const std::string& line);
};

// Well-known Crystal symbols (for quick access)
// These are the canonical pokecrystal names
namespace symbols {
    // Pokemon data
    constexpr const char* BASE_DATA = "BaseData";
    constexpr const char* POKEMON_NAMES = "PokemonNames";
    constexpr const char* EGG_MOVES = "EggMoves";
    constexpr const char* EVOS_ATTACKS = "EvosAttacksPointers";
    
    // Move data
    constexpr const char* MOVES = "Moves";
    constexpr const char* MOVE_NAMES = "MoveNames";
    
    // Item data
    constexpr const char* ITEM_ATTRIBUTES = "ItemAttributes";
    constexpr const char* ITEM_NAMES = "ItemNames";
    
    // Type data
    constexpr const char* TYPE_MATCHUPS = "TypeMatchups";
    constexpr const char* TYPE_NAMES = "TypeNames";
    
    // Map data
    constexpr const char* MAP_GROUP_POINTERS = "MapGroupPointers";
    constexpr const char* MAP_ATTRIBUTES = "MapAttributes";
    
    // Trainer data
    constexpr const char* TRAINER_GROUPS = "TrainerGroups";
    constexpr const char* TRAINER_NAMES = "TrainerNames";
    
    // Encounter data
    constexpr const char* JOHTO_GRASS = "JohtoGrassWildMons";
    constexpr const char* KANTO_GRASS = "KantoGrassWildMons";
    constexpr const char* JOHTO_WATER = "JohtoWaterWildMons";
    constexpr const char* SWARM_GRASS = "SwarmGrassWildMons";
    
    // Graphics
    constexpr const char* POKEMON_PICS = "PokemonPicPointers";
    constexpr const char* TRAINER_PICS = "TrainerPicPointers";
    constexpr const char* TILESETS = "Tilesets";
    
    // Audio
    constexpr const char* MUSIC = "Music";
    constexpr const char* SFX = "SFX";
    constexpr const char* CRIES = "CryHeaders";
    
    // Scripts
    constexpr const char* SCRIPT_COMMANDS = "ScriptCommandTable";
    constexpr const char* SPECIAL_POINTERS = "SpecialPointers";
}

} // namespace crystal
