#pragma once
// engine/battle/trainer_ai.hpp
// Trainer AI system - registry/behavior driven
//
// EXTENSIBILITY:
// Trainer → AI BehaviorId → implementation
// Implementations can be:
//   - Vanilla native C++ AI (default Crystal behavior)
//   - Lua mod AI (scripted behavior)
//   - Custom native AI (registered by mods via C++ plugin)
//
// Mods can:
//   - Replace vanilla AI entirely
//   - Assign different AI to specific trainers/classes
//   - Register new AI implementations

#include "engine/core/types.hpp"
#include "engine/battle/battle.hpp"
#include "engine/battle/battle_rules.hpp"
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>

namespace enginemon {

// Forward declarations (battle.hpp already included above for BattleAction/Battle)
class LuaRuntime;

// AI behavior identifier — used for legacy/test tier-based construction.
using AIBehaviorId = uint16_t;

// Well-known vanilla AI behaviors (Crystal tier constants, test use only)
namespace VanillaAI {
    constexpr AIBehaviorId NONE = 0;
    constexpr AIBehaviorId BASIC = 1;
    constexpr AIBehaviorId SMART = 2;
    constexpr AIBehaviorId AGGRESSIVE = 3;
    constexpr AIBehaviorId DEFENSIVE = 4;
    constexpr AIBehaviorId STATUS_FOCUS = 5;
    constexpr AIBehaviorId GYM_LEADER = 6;
    constexpr AIBehaviorId ELITE_FOUR = 7;
    constexpr AIBehaviorId CHAMPION = 8;
}

// Typed Crystal AI bitmask — unambiguously represents TRNATTR_AI_MOVE_WEIGHTS.
// Production path uses this type; tests may use AIBehaviorId (legacy tiers) directly.
// Crystal bit definitions (trainer_data_constants.asm):
//   bit 0 = AI_BASIC   bit 1 = AI_SETUP    bit 2 = AI_TYPES
//   bit 3 = AI_OFFENSIVE   bit 4 = AI_SMART
struct CrystalAIFlags {
    uint16_t flags = CrystalAIFlags::BASIC;

    static constexpr uint16_t BASIC      = 1 << 0;
    static constexpr uint16_t SETUP      = 1 << 1;
    static constexpr uint16_t TYPES      = 1 << 2;
    static constexpr uint16_t OFFENSIVE  = 1 << 3;
    static constexpr uint16_t SMART      = 1 << 4;

    bool has(uint16_t bit) const { return (flags & bit) != 0; }

    static CrystalAIFlags from_rom(uint16_t raw) {
        CrystalAIFlags f;
        f.flags = (raw != 0) ? raw : CrystalAIFlags::BASIC;
        return f;
    }
};

// AI decision context - what the AI knows when deciding
struct AIContext {
    const Battle& battle;
    const BattlePokemon& self;
    const BattlePokemon& opponent;
    
    // Available actions
    bool can_fight;
    bool can_switch;
    bool can_use_item;
    bool can_run;               // Always false for trainer battles
    
    // Party state
    size_t usable_party_count;
    std::vector<size_t> available_switches;
    
    // Items available (trainer battles)
    std::vector<ItemId> available_items;
};

// AI decision result
struct AIDecision {
    BattleAction action;
    
    // Optional debug info
    std::string reasoning;      // Why this decision was made
    float confidence;           // 0-1, how confident in this choice
};

// Base AI implementation interface
class ITrainerAI {
public:
    virtual ~ITrainerAI() = default;
    
    // Make a battle decision
    virtual AIDecision decide(const AIContext& ctx) = 0;
    
    // Optional: called at battle start
    virtual void on_battle_start(const Battle& battle) {}
    
    // Optional: called when opponent switches
    virtual void on_opponent_switch(const BattlePokemon& new_pokemon) {}
    
    // Optional: called after each turn
    virtual void on_turn_end(const Battle& battle) {}
    
    // Metadata
    virtual AIBehaviorId behavior_id() const = 0;
    virtual std::string name() const = 0;
};

// Vanilla Crystal AI implementation
class VanillaCrystalAI : public ITrainerAI {
public:
    // Legacy constructor: takes a tier constant (for tests that construct directly).
    explicit VanillaCrystalAI(AIBehaviorId behavior);

    // Production constructor: takes a typed CrystalAIFlags bitmask from the ROM.
    // Uses exact bitmask-driven pass dispatch — no tier inference, no range-sniffing.
    explicit VanillaCrystalAI(CrystalAIFlags flags);

    // Base overload (uses internal static fallback tables — for tests without a package).
    AIDecision decide(const AIContext& ctx) override;

    // ROM-derived overload: uses BattleRules extracted from the package.
    // Production code should use this overload.
    AIDecision decide(const AIContext& ctx, const BattleRules& rules);

    AIBehaviorId behavior_id() const override { return behavior_; }
    std::string name() const override;

private:
    AIBehaviorId behavior_;     // Legacy tier constant (0 when crystal_flags_ is authoritative)
    CrystalAIFlags crystal_flags_; // Exact ROM bitmask (populated by CrystalAIFlags constructor)
    bool use_crystal_flags_ = false; // true when constructed from CrystalAIFlags
    
    // Crystal AI subroutines (ported from pokecrystal)
    int score_move(const AIContext& ctx, size_t move_slot);
    bool should_switch(const AIContext& ctx);
    bool should_use_item(const AIContext& ctx);
    size_t choose_switch_target(const AIContext& ctx);
};

// Lua-scripted AI implementation
class LuaTrainerAI : public ITrainerAI {
public:
    LuaTrainerAI(AIBehaviorId id, const std::string& name, 
                 LuaRuntime& lua, const std::string& script_function);
    
    AIDecision decide(const AIContext& ctx) override;
    void on_battle_start(const Battle& battle) override;
    void on_opponent_switch(const BattlePokemon& new_pokemon) override;
    void on_turn_end(const Battle& battle) override;
    
    AIBehaviorId behavior_id() const override { return id_; }
    std::string name() const override { return name_; }

private:
    AIBehaviorId id_;
    std::string name_;
    LuaRuntime* lua_;
    std::string script_function_;
};

// AI Factory - creates AI instances from behavior IDs
using AIFactory = std::function<std::unique_ptr<ITrainerAI>(AIBehaviorId)>;

// AI Registry - manages available AI implementations
class AIRegistry {
public:
    AIRegistry();
    
    // Register AI implementations
    void register_native(AIBehaviorId id, AIFactory factory);
    void register_lua(AIBehaviorId id, const std::string& name,
                     const std::string& script_function);
    
    // Create AI instance
    std::unique_ptr<ITrainerAI> create(AIBehaviorId id);
    
    // Override AI for specific trainer/class
    void set_trainer_override(TrainerId trainer, AIBehaviorId ai);
    void set_class_override(uint8_t trainer_class, AIBehaviorId ai);
    
    // Get AI for trainer (checks overrides, then default)
    AIBehaviorId get_ai_for_trainer(TrainerId trainer, uint8_t trainer_class);
    
    // List registered AIs
    std::vector<std::pair<AIBehaviorId, std::string>> list_registered() const;
    
    // Freeze (after mod loading)
    void freeze() { frozen_ = true; }

private:
    std::unordered_map<AIBehaviorId, AIFactory> factories_;
    std::unordered_map<AIBehaviorId, std::string> names_;
    std::unordered_map<TrainerId, AIBehaviorId> trainer_overrides_;
    std::unordered_map<uint8_t, AIBehaviorId> class_overrides_;
    
    LuaRuntime* lua_ = nullptr;
    AIBehaviorId next_custom_id_ = 1000;  // Custom IDs start at 1000
    bool frozen_ = false;
};

} // namespace enginemon
