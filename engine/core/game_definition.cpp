// engine/core/game_definition.cpp
// GameDefinition implementation

#include "engine/core/game_definition.hpp"
#include <fstream>
#include <stdexcept>

namespace enginemon {

const MapData* WorldData::get_map(MapId id) const {
    for (const auto& map : maps) {
        if (map.id == id) return &map;
    }
    return nullptr;
}

std::vector<const MapConnection*> WorldData::get_connections_from(MapId id) const {
    std::vector<const MapConnection*> result;
    for (const auto& conn : connections) {
        if (conn.from_map == id) {
            result.push_back(&conn);
        }
    }
    return result;
}

std::vector<const WarpData*> WorldData::get_warps_on(MapId id) const {
    std::vector<const WarpData*> result;
    for (const auto& warp : warps) {
        if (warp.map == id) {
            result.push_back(&warp);
        }
    }
    return result;
}

std::vector<const NpcData*> WorldData::get_npcs_on(MapId id) const {
    std::vector<const NpcData*> result;
    for (const auto& npc : npcs) {
        if (npc.map == id) {
            result.push_back(&npc);
        }
    }
    return result;
}

std::unique_ptr<GameDefinition> GameDefinition::load(const std::filesystem::path& path) {
    auto def = std::make_unique<GameDefinition>();
    // TODO: Implement loading from compiled game directory
    // This will load the binary data files produced by the Crystal frontend
    return def;
}

void GameDefinition::save(const std::filesystem::path& path) const {
    // TODO: Implement saving to compiled game directory
    // Create directory structure and write all data files
}

bool GameDefinition::validate() const {
    // TODO: Validate all cross-references
    // - All species referenced exist
    // - All moves referenced exist
    // - All maps referenced exist
    // - Scripts reference valid entities
    return true;
}

} // namespace enginemon
