#pragma once
// crystal/world/connection_resolver.hpp
// Resolves Crystal map connections into continuous world coordinates
// Many connected Crystal maps → single continuous exterior World

#include "engine/core/types.hpp"
#include "engine/core/game_definition.hpp"
#include "crystal/rom/loader.hpp"
#include "crystal/rom/symbol_map.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>

namespace crystal {

using namespace enginemon;

// A resolved map with world position
struct ResolvedMap {
    MapId id;
    int32_t world_x;        // Top-left corner in world coordinates
    int32_t world_y;
    uint8_t width;          // In tiles
    uint8_t height;
    bool is_exterior;       // Part of continuous world
};

// Resolves map connections into world coordinates
class ConnectionResolver {
public:
    ConnectionResolver(const RomData& rom, const SymbolMap& symbols);
    
    // Resolve all map connections
    // Returns resolved maps with world coordinates
    std::vector<ResolvedMap> resolve();
    
    // Get resolved position for a map
    std::optional<std::pair<int32_t, int32_t>> get_world_position(MapId id) const;
    
    // Check if map is part of continuous exterior world
    bool is_exterior(MapId id) const;
    
    // Statistics
    struct Stats {
        size_t total_maps = 0;
        size_t exterior_maps = 0;
        size_t interior_maps = 0;
        size_t connection_count = 0;
        int32_t world_min_x = 0;
        int32_t world_min_y = 0;
        int32_t world_max_x = 0;
        int32_t world_max_y = 0;
    };
    Stats get_stats() const { return stats_; }

private:
    const RomData& rom_;
    const SymbolMap& symbols_;
    
    // Extracted connections
    struct Connection {
        MapId from_map;
        MapId to_map;
        Direction direction;
        int16_t offset;     // Tile offset along connection edge
    };
    std::vector<Connection> connections_;
    
    // Extracted map sizes
    std::unordered_map<MapId, std::pair<uint8_t, uint8_t>> map_sizes_;
    
    // Resolution state
    std::unordered_map<MapId, std::pair<int32_t, int32_t>> resolved_positions_;
    std::unordered_set<MapId> exterior_maps_;
    Stats stats_;
    
    // Extraction
    void extract_connections();
    void extract_map_sizes();
    
    // Resolution algorithm
    void resolve_connected_component(MapId start);
    void propagate_position(MapId from, MapId to, const Connection& conn);
    
    // Interior detection
    bool detect_interior(MapId id) const;
    
    // Normalization (shift so min coords are 0,0)
    void normalize_positions();
};

// Map group/area categories from Crystal
// Used for determining interior vs exterior
namespace MapGroup {
    // Johto exterior
    constexpr uint8_t NEW_BARK_TOWN = 0x01;
    constexpr uint8_t CHERRYGROVE_CITY = 0x02;
    constexpr uint8_t VIOLET_CITY = 0x03;
    constexpr uint8_t AZALEA_TOWN = 0x04;
    constexpr uint8_t GOLDENROD_CITY = 0x06;
    constexpr uint8_t ECRUTEAK_CITY = 0x08;
    constexpr uint8_t OLIVINE_CITY = 0x09;
    constexpr uint8_t CIANWOOD_CITY = 0x0A;
    constexpr uint8_t MAHOGANY_TOWN = 0x0B;
    constexpr uint8_t BLACKTHORN_CITY = 0x0C;
    constexpr uint8_t LAKE_OF_RAGE = 0x0D;
    
    // Kanto exterior  
    constexpr uint8_t PALLET_TOWN = 0x17;
    constexpr uint8_t VIRIDIAN_CITY = 0x18;
    constexpr uint8_t PEWTER_CITY = 0x19;
    constexpr uint8_t CERULEAN_CITY = 0x1A;
    constexpr uint8_t VERMILION_CITY = 0x1C;
    constexpr uint8_t LAVENDER_TOWN = 0x1B;
    constexpr uint8_t CELADON_CITY = 0x1D;
    constexpr uint8_t SAFFRON_CITY = 0x1F;
    constexpr uint8_t FUCHSIA_CITY = 0x1E;
    constexpr uint8_t CINNABAR_ISLAND = 0x20;
    constexpr uint8_t INDIGO_PLATEAU = 0x21;
    
    // Routes are exterior
    constexpr uint8_t ROUTE_START = 0x22;
    constexpr uint8_t ROUTE_END = 0x45;
    
    // Special areas
    constexpr uint8_t SILVER_CAVE = 0x46;
}

// Helper to determine if a map group is exterior
bool is_exterior_group(uint8_t group);

// Helper to get human-readable map name
std::string get_map_name(MapId id, const SymbolMap& symbols);

} // namespace crystal
