#pragma once
// engine/world/world.hpp
// World representation and movement
// Connected exterior maps form a continuous coordinate space
// Interiors remain separate spaces

#include "engine/core/types.hpp"
#include "engine/core/game_definition.hpp"
#include <memory>
#include <vector>
#include <optional>
#include <functional>
#include <unordered_map>

namespace enginemon {

// Forward declarations
class Actor;
class Collision;

// Tile info at a position
struct TileInfo {
    uint8_t tile_id;
    uint8_t collision;
    bool walkable;
    bool surfable;
    bool jumpable;
    bool warp;
    int8_t elevation;       // For ledges
};

// Movement result
enum class MoveResult {
    Success,
    Blocked,            // Collision
    WarpTrigger,        // Hit a warp point
    WildEncounter,      // Triggered wild encounter
    Ledge,              // Jumped ledge
    MapBoundary,        // Hit map edge (connected maps handle this)
    Event               // Triggered tile event
};

// Actor types
enum class ActorType {
    Player,
    NPC,
    Object              // Signs, boulders, etc.
};

// Movement types for NPCs
enum class MovementType : uint8_t {
    Standing,           // Never moves
    LookAround,         // Randomly changes facing
    WalkUpDown,
    WalkLeftRight,
    WalkAny,            // Random walk in radius
    FollowPath,         // Scripted path
    Spinning,           // Spinner
    Following           // Following player
};

// Single actor in the world
class Actor {
public:
    uint16_t id;
    ActorType type;
    
    // Position (continuous world coordinates for exteriors)
    int32_t world_x;
    int32_t world_y;
    MapId current_map;      // Which map tile we're "in" for events
    
    // Local position within current map tile
    uint8_t local_x() const;
    uint8_t local_y() const;
    
    // Movement state
    Direction facing = Direction::Down;
    bool is_moving = false;
    float move_progress = 0.0f;     // 0-1 during movement
    int32_t move_target_x = 0;
    int32_t move_target_y = 0;
    
    // NPC-specific
    MovementType movement_type = MovementType::Standing;
    uint8_t movement_radius = 0;
    int32_t home_x = 0;             // Starting position
    int32_t home_y = 0;
    
    // Visibility
    bool visible = true;
    FlagId visibility_flag = 0;
    bool flag_inverted = false;
    
    // Sprite
    SpriteId sprite;
    uint8_t animation_frame = 0;
    
    // Script
    ScriptId interact_script = 0;
};

// World state
class World {
public:
    World();
    ~World();
    
    // Initialize from game definition
    void initialize(const GameDefinition& game);
    
    // Player management
    Actor& player();
    const Actor& player() const;
    void spawn_player(MapId map, uint8_t x, uint8_t y, Direction facing = Direction::Down);
    
    // NPC management
    Actor* get_actor(uint16_t id);
    const Actor* get_actor(uint16_t id) const;
    std::vector<Actor*> get_actors_on_map(MapId map);
    std::vector<Actor*> get_actors_in_view(int32_t cx, int32_t cy, int32_t radius);
    
    // Movement (semantic API for scripts)
    // Returns immediately, movement happens over frames
    void move_actor(uint16_t actor_id, Direction dir, uint8_t steps = 1);
    void face_actor(uint16_t actor_id, Direction dir);
    bool is_actor_moving(uint16_t actor_id) const;
    
    // Player movement (input-driven)
    MoveResult try_move_player(Direction dir);
    void update_movement(float delta_time);
    
    // Teleport (instant, no animation)
    void teleport(uint16_t actor_id, MapId map, int32_t x, int32_t y);
    void teleport_player(MapId map, uint8_t x, uint8_t y);
    
    // Warps
    void trigger_warp(const WarpData& warp);
    const WarpData* get_warp_at(MapId map, uint8_t x, uint8_t y) const;
    
    // Tile queries
    TileInfo get_tile(MapId map, uint8_t x, uint8_t y) const;
    TileInfo get_tile_world(int32_t world_x, int32_t world_y) const;
    bool can_walk(MapId map, uint8_t x, uint8_t y, Direction from) const;
    
    // Map queries
    const MapData* get_map(MapId id) const;
    MapId map_at_world_coords(int32_t x, int32_t y) const;
    MapId current_map() const { return current_map_; }
    
    // Visibility
    void show_actor(uint16_t actor_id);
    void hide_actor(uint16_t actor_id);
    void refresh_actor_visibility();  // Check flags
    
    // Wild encounters
    bool should_trigger_wild_encounter(MapId map, uint8_t x, uint8_t y) const;
    std::optional<std::pair<SpeciesId, uint8_t>> roll_wild_encounter(
        MapId map, TimeOfDay time) const;
    
    // Continuous world
    bool is_exterior(MapId map) const;
    void world_to_map_coords(int32_t wx, int32_t wy, MapId& out_map, 
                             uint8_t& out_x, uint8_t& out_y) const;
    void map_to_world_coords(MapId map, uint8_t x, uint8_t y,
                             int32_t& out_wx, int32_t& out_wy) const;
    
    // Callbacks
    using WarpCallback = std::function<void(const WarpData&)>;
    using EncounterCallback = std::function<void(SpeciesId, uint8_t)>;
    using EventCallback = std::function<void(ScriptId)>;
    
    void set_warp_callback(WarpCallback cb) { warp_callback_ = std::move(cb); }
    void set_encounter_callback(EncounterCallback cb) { encounter_callback_ = std::move(cb); }
    void set_tile_event_callback(EventCallback cb) { tile_event_callback_ = std::move(cb); }

private:
    // Game data reference
    const GameDefinition* game_ = nullptr;
    
    // Current state
    MapId current_map_ = MAP_NONE;
    
    // Actors
    Actor player_;
    std::vector<Actor> npcs_;
    std::unordered_map<uint16_t, size_t> actor_index_;  // id -> npcs_ index
    uint16_t next_actor_id_ = 1;
    
    // Movement queue (for scripted multi-step movement)
    struct MovementCommand {
        uint16_t actor_id;
        Direction direction;
        uint8_t steps_remaining;
    };
    std::vector<MovementCommand> movement_queue_;
    
    // Collision helper
    std::unique_ptr<Collision> collision_;
    
    // Callbacks
    WarpCallback warp_callback_;
    EncounterCallback encounter_callback_;
    EventCallback tile_event_callback_;
    
    // Movement helpers
    void execute_step(Actor& actor);
    void handle_map_transition(Actor& actor);
    bool check_collision(int32_t x, int32_t y, Direction dir) const;
};

} // namespace enginemon
