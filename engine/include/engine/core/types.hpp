#pragma once
// engine/core/types.hpp
// Native type definitions - no Game Boy concepts
//
// EXTENSIBILITY:
// Vanilla Crystal implements Gen 2 mechanics only (no abilities, etc).
// However, this is NOT a hard limitation of the engine architecture.
//
// Future mods/frontends can extend mechanics by:
// - Adding new entries to enums (Status, Weather, etc.) via registry
// - Registering new effect handlers for moves/items/abilities
// - Adding new data fields through the composition system
//
// The engine uses registries and behavior IDs rather than hard-coded
// switch statements, allowing new mechanics to be plugged in.

#include <cstdint>
#include <string>
#include <string_view>
#include <array>
#include <vector>
#include <optional>
#include <span>
// SemanticEffectDescription lives in engine/battle/semantic_effect.hpp.
// Included here so MoveData can embed it.  semantic_effect.hpp has no
// back-include of types.hpp (uses only <cstdint>) so there is no cycle.
#include "engine/battle/semantic_effect.hpp"
#include "engine/battle/semantic_program.hpp"

namespace enginemon {

// Stable identifiers for registry lookups
// These are content-addressed or sequential IDs, never ROM offsets
using SpeciesId = uint16_t;
using MoveId = uint16_t;
using ItemId = uint16_t;
using TypeId = uint8_t;
using TrainerId = uint16_t;
using MapId = uint16_t;
using TilesetId = uint16_t;
using SpriteId = uint16_t;
using MusicId = uint16_t;
using SfxId = uint16_t;
using ScriptId = uint32_t;
using VarId = uint16_t;
using TextId = uint32_t;
using MovementId = uint32_t;
using StateVarId = uint16_t;  // Semantic mini-game/puzzle state variable ID
using PokeMailId = uint16_t;  // Semantic Pokemon mail ID (NOT ROM pointer)
using StdScriptId = uint16_t; // Semantic standard script ID

// Flag namespaces - Crystal has TWO distinct flag arrays that must NOT be conflated
// EventFlags: 800 flags (0-799) for story/progress events
// EngineFlags: 190 flags (0-189) for gameplay/engine state
// These are SEPARATE storage arrays; EventFlag{5} != EngineFlag{5}
enum class FlagNamespace : uint8_t {
    Event = 0,    // wEventFlags (800 bits)
    Engine = 1,   // wEngineFlags (190 bits)
};

// Typed flag reference that preserves namespace distinction
struct FlagRef {
    FlagNamespace ns;
    uint16_t value;

    // Comparison operators - different namespaces are NEVER equal
    bool operator==(const FlagRef& other) const {
        return ns == other.ns && value == other.value;
    }
    bool operator!=(const FlagRef& other) const { return !(*this == other); }
    bool operator<(const FlagRef& other) const {
        if (ns != other.ns) return ns < other.ns;
        return value < other.value;
    }

    // Factory methods for clarity
    static FlagRef event_flag(uint16_t v) { return {FlagNamespace::Event, v}; }
    static FlagRef engine_flag(uint16_t v) { return {FlagNamespace::Engine, v}; }

    // Human-readable debug string
    std::string to_string() const {
        return (ns == FlagNamespace::Event ? "EventFlag{" : "EngineFlag{") +
               std::to_string(value) + "}";
    }
};

// DEPRECATED: Legacy FlagId for backward compatibility during migration
// New code should use FlagRef directly
using FlagId = uint16_t;

// Semantic AI pass set — which behavior passes the AI will run.
// The engine layer works only with these named boolean fields.
// The Crystal frontend decodes ROM bitmasks (TRNATTR_AI_MOVE_WEIGHTS) into AIPassSet
// at extraction/load time; no Crystal bit-position knowledge belongs in the engine.
struct AIPassSet {
    bool run_basic     = true;   // AI_BASIC — redundancy/status-block filter (always active)
    bool run_setup     = false;  // AI_SETUP — stat-boosting move encouragement on turn 1
    bool run_types     = false;  // AI_TYPES — type matchup awareness
    bool run_offensive = false;  // AI_OFFENSIVE — discourages non-damaging moves
    bool run_smart     = false;  // AI_SMART — context-specific per-effect adjustments

    static AIPassSet all()        { return {true, true, true, true, true}; }
    static AIPassSet basic_only() { return {true, false, false, false, false}; }
};

// Null/invalid markers
inline constexpr SpeciesId SPECIES_NONE = 0;
inline constexpr MoveId MOVE_NONE = 0;
inline constexpr ItemId ITEM_NONE = 0;
inline constexpr TypeId TYPE_NONE = 0xFF;
inline constexpr MapId MAP_NONE = 0xFFFF;
inline constexpr TextId TEXT_NONE = 0xFFFFFFFF;
inline constexpr MovementId MOVEMENT_NONE = 0xFFFFFFFF;
inline constexpr StateVarId STATEVAR_NONE = 0xFFFF;
inline constexpr PokeMailId POKEMAIL_NONE = 0xFFFF;
inline constexpr StdScriptId STDSCRIPT_NONE = 0xFFFF;

// Well-known semantic state variable IDs
// These are semantic gameplay states, NOT RAM addresses
// These represent mini-game/puzzle states that are common enough to
// name in the engine layer. Game-specific mini-game state vars
// (e.g. Battle Tower streak) live in the frontend that defines them.
enum class WellKnownStateVar : uint16_t {
    FarfetchdPosition = 1,           // Ilex Forest mini-game position (1-10)
    MooMooBerries = 2,               // MooMoo Farm berry feeding count
    UndergroundSwitchPositions = 3,  // Goldenrod Underground switch puzzle state
    // NOTE: 4 and 5 are Crystal-specific (BattleTower).
    // They are defined as CrystalStateVar::BattleTowerBeatenTrainers /
    // CrystalStateVar::BattleTowerLevelGroup in
    // frontends/crystal/include/crystal/script/crystal_state_vars.hpp
    // and must not be referenced by generic engine code.
};

// Link mode capability query results (read-only)
enum class LinkModeCapability : uint8_t {
    NotConnected = 0,
    ConnectedGen1 = 0,      // False in script means connected to Gen 1
    ConnectedGen2 = 1,      // Non-zero means connected to Gen 2
};

// Pokemon stats
struct BaseStats {
    uint8_t hp;
    uint8_t attack;
    uint8_t defense;
    uint8_t speed;
    uint8_t special_attack;
    uint8_t special_defense;
};

// Pokemon species definition
struct SpeciesData {
    SpeciesId id;
    std::string name;
    std::string category;  // e.g. "Seed Pokemon"

    BaseStats base_stats;
    TypeId type1;
    TypeId type2;  // TYPE_NONE if single type

    uint8_t catch_rate;
    uint8_t base_exp;
    uint8_t gender_ratio;  // 0-254 for female chance, 255 for genderless
    uint8_t egg_cycles;
    uint8_t base_friendship;
    uint8_t growth_rate;

    std::array<uint16_t, 2> ev_yield;  // HP/Atk, Def/Speed, SpAtk/SpDef packed

    std::vector<std::pair<uint8_t, MoveId>> learnset;  // level -> move
    std::vector<MoveId> tm_compatibility;
    std::vector<SpeciesId> egg_moves;

    SpriteId front_sprite;
    SpriteId back_sprite;
    SpriteId icon_sprite;
    uint8_t palette_id;
};

// Type definition
struct TypeData {
    TypeId id;
    std::string name;
};

// Type effectiveness chart entry
struct TypeEffectiveness {
    TypeId attacking;
    TypeId defending;
    uint8_t multiplier;  // 0=immune, 5=not very effective, 10=normal, 20=super effective
};

// Move categories
enum class MoveCategory : uint8_t {
    Physical,
    Special,
    Status
};

// Move target types
enum class MoveTarget : uint8_t {
    Selected,       // Single target
    Self,           // User
    AllAdjacent,    // All adjacent (not used in Gen 2, but extensible)
    AllFoes,        // All opponents
    Field           // Field effect
};

// ============================================================================
// Semantic move effect identifier.
// Stable EMON-assigned values — independent of any source-game's numeric ABI.
// The Crystal frontend maps raw EFFECT_* ROM bytes to these stable IDs when
// writing MoveData into the package.
// Engine code (AI, mechanics) works only against these semantic IDs.
// ============================================================================
using EffectId = uint8_t;

// Stable semantic effect ID assignments (EMON-owned, not Crystal-sourced).
// Values start at 1; 0 is reserved for Unknown/None.
// Future frontends must map their own effect IDs to these at extraction time.
namespace SemEffect {
    static constexpr EffectId Unknown      =   0;  // Unrecognised / unmapped effect
    static constexpr EffectId Sleep        =   1;  // Puts target to sleep
    static constexpr EffectId Heal         =   2;  // Restores user HP (Recover/Morning Sun/…)
    static constexpr EffectId Selfdestruct =   3;  // User faints
    static constexpr EffectId DreamEater   =   4;  // Drains sleeping target
    static constexpr EffectId HyperBeam    =   5;  // Recharge move
    static constexpr EffectId Nightmare    =   6;  // Damages sleeping target each turn
    static constexpr EffectId Toxic        =   7;  // Poisons (badly)
    static constexpr EffectId Poison       =   8;  // Poisons target (regular)
    static constexpr EffectId Paralyze     =   9;  // Paralyses target
    static constexpr EffectId BatonPass    =  10;  // Switches out passing stat stages
    static constexpr EffectId BellyDrum    =  11;  // Max attack, half HP
    static constexpr EffectId Protect      =  12;  // Blocks moves this turn
    static constexpr EffectId Endure       =  13;  // Survives with 1 HP
    static constexpr EffectId Reflect      =  14;  // Physical damage screen
    static constexpr EffectId LightScreen  =  15;  // Special damage screen
    static constexpr EffectId RainDance    =  16;  // Rain weather
    static constexpr EffectId SunnyDay     =  17;  // Sun weather
    // Stat stage modifiers — broad semantic categories used by ai_setup
    static constexpr EffectId StatUp       =  18;  // Any stat-raising move
    static constexpr EffectId StatDown     =  19;  // Any stat-lowering move (on opponent)
    // Strict pure-damage: executing as single-hit damage is HP-correct for all reachable states.
    // Mapped from Crystal NORMAL_HIT(0), ALWAYS_HIT(17), PRIORITY_HIT(103).
    // All other damaging effects — including named ones like Selfdestruct, DreamEater, HyperBeam —
    // are NOT mapped here; they remain SemEffect::Unknown until explicitly implemented.
    static constexpr EffectId PureDamage   =  20;  // Ordinary single-hit; no extra HP consequences
    static constexpr EffectId Recoil       =  21;  // User takes max(1, damage >> recoil_shift) after dealing damage
    static constexpr EffectId Drain        =  22;  // User heals max(1, damage >> drain_shift) after dealing damage
    // Used for sleep-synergy detection in AI_Smart
    // (ai_basic/ai_setup use BattleRules lists; only ai_smart uses these direct checks)
}

// Move definition
struct MoveData {
    MoveId id;
    std::string name;

    TypeId type;
    MoveCategory category;
    MoveTarget target;

    uint8_t power;          // 0 for status moves
    uint8_t accuracy;       // 0 for always-hit moves
    uint8_t pp;
    int8_t priority;        // Usually 0, positive = faster

    EffectId effect_id  = SemEffect::Unknown;  // Semantic effect identifier (EMON-stable)
    uint8_t effect_chance;  // Raw Crystal MOVE_CHANCE byte (0–255).
                            // Assembled from the source percentage by the percent RGBDS macro
                            // (e.g. "20 percent" in moves.asm assembles to floor(20*255/100)=51).
                            // Crystal BattleCommand_EffectChance fires iff BattleRandom < this byte.
                            // Runtime must compare rng_byte directly against this value; no *255/100.

    bool makes_contact;
    bool is_sound_based;

    // Animation/presentation
    uint8_t animation_id;

    // Semantic effect description — produced by Crystal frontend from decoded effect script.
    // execute_move() reads this struct for execution dispatch instead of effect_id.
    // Populated at package load time; zero-init gives "ordinary single-hit damage" defaults.
    // Defined in engine/include/engine/battle/semantic_effect.hpp.
    SemanticEffectDescription effect_desc;

    // Architecture B — ordered semantic program for stateful/multi-turn effects.
    // When has_program == true, execute_move() calls execute_program() instead of
    // the Architecture A flat-dispatch path.
    // effect_desc.is_supported remains false for B effects (Architecture A never executes them).
    bool has_program = false;
    SemanticEffectProgram effect_program;
};

// Item pocket types
enum class ItemPocket : uint8_t {
    Items,
    KeyItems,
    Balls,
    TmsHms
};

// ============================================================================
// Semantic held-item effect type — engine-semantic, never contains raw Crystal
// HELD_* numeric values or raw Crystal item/species IDs.
//
// The Crystal frontend maps raw HELD_* bytes from the ROM into this enum
// at package-build time. The runtime dispatches on HeldItemEffectType only.
// ============================================================================
enum class HeldItemEffectType : uint8_t {
    None               =  0,  // No held-item battle effect
    CritStageBoost     =  1,  // +param crit stages (Scope Lens)
    CritStageBoostSpecies = 2, // +param crit stages only for species_restriction (Stick, Lucky Punch)
    AccuracyReduction  =  3,  // Defender: accuracy -= param before stage mult (BrightPowder)
    TurnOrderBoost     =  4,  // RNG < param → holder goes first (Quick Claw)
    PostHitFlinch      =  5,  // After hit: RNG < param → target flinches (King's Rock)
    TypeDamageBoost    =  6,  // Damage ×(100+param)/100 for move type == boosted_type
    KOSurvival         =  7,  // RNG < param → survive KO at 1 HP (Focus Band, not consumed)
    EndTurnHealFraction=  8,  // End-of-turn: restore max_hp/param HP (Leftovers, param=16)
    EndTurnHealBelowHalf= 9,  // End-of-turn: if hp < max_hp/2, restore param HP; consumed (Berry)
    StatusCure         = 10,  // End-of-turn: cure Status matching param (status-specific berries)
    AnyStatusCure      = 11,  // End-of-turn: cure any major status and confusion (MiracleBerry)
    ConfusionCure      = 12,  // End-of-turn: cure confusion; consumed (Bitter Berry)
    EndTurnRestorePP   = 13,  // End-of-turn: restore 5 PP to first depleted move; consumed (Mysteryberry)
    SpeciesDefenseBoost= 14,  // Damage-calc: ×1.5 defense for species_restriction (Metal Powder/Ditto)
    BerserkActivation  = 15,  // Battle-start: consumed → +2 Atk, set Confusion (Berserk Gene)
    GuaranteedEscape   = 16,  // Always succeed wild escape attempt (Smoke Ball)
    AmuletCoin         = 17,  // Double battle prize money and Pay Day payout (Amulet Coin)
};

// Item definition
struct ItemData {
    ItemId id;
    std::string name;
    std::string description;

    ItemPocket pocket;
    uint16_t price;
    uint8_t held_param;     // Semantic parameter for held effect (e.g. HP restored, chance threshold)
    uint8_t field_effect;   // Effect when used from menu (0 = none)

    bool is_key_item;
    bool is_tm_hm;
    MoveId tm_move;         // If is_tm_hm, which move it teaches

    // ── Semantic held-item fields ────────────────────────────────────────────
    // Populated by the Crystal frontend at package-build time.
    // The runtime dispatches on held_effect_type; it never reads raw HELD_*
    // values or raw Crystal item/species IDs from this struct.
    HeldItemEffectType held_effect_type = HeldItemEffectType::None;

    // TypeDamageBoost: the move type that receives the damage bonus.
    // TYPE_NONE (0xFF) for all other effect types.
    TypeId boosted_type = TYPE_NONE;

    // CritStageBoostSpecies / SpeciesDefenseBoost: the species that must hold
    // this item for the effect to apply.  SPECIES_NONE (0) = no restriction.
    SpeciesId species_restriction = SPECIES_NONE;

    // true for single-use held items that are consumed on activation
    // (berries, status-cure items, Berserk Gene, MiracleBerry, etc.)
    bool consumable = false;
};

// Trainer class
struct TrainerClassData {
    uint8_t id;
    std::string name;
    uint8_t base_money;     // Money = base_money * highest_level
    uint8_t ai_flags;       // AI behavior flags
};

// Trainer definition
struct TrainerData {
    TrainerId id;
    std::string name;
    uint8_t trainer_class;

    struct Pokemon {
        SpeciesId species;
        uint8_t level;
        ItemId held_item;
        std::array<MoveId, 4> moves;  // MOVE_NONE if not specified
        // DVs from Crystal's TrainerClassDVs table (09:70d6), per trainer class.
        // Gen 2 DVs are 4-bit values (0–15).  Materialized by the frontend extractor;
        // the engine uses them directly rather than defaulting to iv=9.
        uint8_t dv_atk = 9;   // Attack DV  (vanilla default 9)
        uint8_t dv_def = 8;   // Defense DV (vanilla default 8)
        uint8_t dv_spd = 8;   // Speed DV   (vanilla default 8)
        uint8_t dv_spc = 8;   // Special DV (vanilla default 8)
    };
    std::vector<Pokemon> party;

    std::vector<ItemId> items;  // Items trainer can use
    ScriptId before_battle_script;
    ScriptId after_win_script;
    ScriptId after_lose_script;
};

// Weather types (Gen 2 vanilla)
// NOTE: Additional weather types can be registered by mods
enum class Weather : uint8_t {
    None,
    Rain,
    Sun,
    Sandstorm,
    // Gen 2 doesn't have Hail, but mods could add it
    // New weather types register via WeatherRegistry
};

// Status conditions (Gen 2 vanilla)
// NOTE: Additional status types can be registered by mods
enum class Status : uint8_t {
    None,
    Sleep,
    Poison,
    BadPoison,  // Toxic
    Burn,
    Freeze,
    Paralysis,
    // New status types register via StatusRegistry with handlers
};

// Volatile status (battle-only, multiple can stack).
// Widened to uint32_t for Architecture B bits (10-18); existing bits 0-9 unchanged.
enum class VolatileStatus : uint32_t {
    None         = 0,
    // -- Existing bits (0-9) -------------------------------------------
    Confusion    = 1u << 0,
    Flinch       = 1u << 1,
    Trapped      = 1u << 2,   // Mean Look, etc.
    Seeded       = 1u << 3,   // Leech Seed
    Cursed       = 1u << 4,   // Curse (Ghost)
    Nightmare    = 1u << 5,
    Infatuation  = 1u << 6,
    FocusEnergy  = 1u << 7,
    Substitute   = 1u << 8,
    Recharge     = 1u << 9,   // Must recharge next turn (Hyper Beam)
    // -- Architecture B bits (10-18) ------------------------------------
    // Set/cleared by SemanticEffectProgram ops and engine hooks only.
    Bide         = 1u << 10,  // Storing energy for Bide release
    Rampage      = 1u << 11,  // Locked into Rampage (Thrash/Petal Dance/Outrage)
    Rollout      = 1u << 12,  // Active Rollout chain (SUBSTATUS_ROLLOUT)
    Flying       = 1u << 13,  // Invulnerable in air (Fly, Sky Attack)
    Underground  = 1u << 14,  // Invulnerable underground (Dig)
    Rage         = 1u << 15,  // Rage volatile -- accumulates Attack on hit
    Curled       = 1u << 16,  // Defense Curl -- doubles next Rollout power
    Charging     = 1u << 17,  // Charge turn in progress (two-phase moves)
    Transformed  = 1u << 18,  // Transform active
    // -- New bits (19-28) added for new A/B mechanics -------------------
    Mist         = 1u << 19,  // Mist -- protects from opponent stat drops (indefinite, clears on switch)
    Identified   = 1u << 20,  // Foresight/Odor Sleuth -- removes Ghost immunities; clears on switch
    CantRun      = 1u << 21,  // Mean Look / ArenaTrap -- prevents target from fleeing or switching
    Protect      = 1u << 22,  // Protect -- blocks all damage this turn; cleared at turn start
    Endure       = 1u << 23,  // Endure -- survive any hit with 1 HP; cleared at turn start
    LockOn       = 1u << 24,  // Lock-On -- next move always hits; consumed on next hit check
    DestinyBond  = 1u << 25,  // Destiny Bond -- if user faints from direct damage, opponent faints too
    Perish       = 1u << 26,  // Perish Song countdown active
    Minimized    = 1u << 27,  // Minimize used — Stomp deals double damage
                               // Source: suiCune MinimizeDropSub sets wPlayerMinimized/wEnemyMinimized.
                               // Set only by Minimize (move 107); NOT set by Double Team.
                               // Clears on ordinary switch-out (volatile_status reset to 0).
};

// Direction for movement
enum class Direction : uint8_t {
    Down  = 0,
    Up    = 1,
    Left  = 2,
    Right = 3
};

// =============================================================================
// MOVEMENT COMMANDS (semantic representation)
// =============================================================================

// Movement command types - semantic operations independent of Crystal encoding
// These represent gameplay-visible movement behaviors
// AUTHORITATIVE SOURCE: pokecrystal/macros/scripts/movement.asm
enum class MovementType : uint8_t {
    // Directional steps (combine with Direction) - 0x00-0x37
    TurnHead,           // 0x00-0x03: Turn to face direction without moving
    TurnStep,           // 0x04-0x07: Turn step (visual turn animation)
    SlowStep,           // 0x08-0x0B: Slow walking step
    Step,               // 0x0C-0x0F: Normal walking step
    BigStep,            // 0x10-0x13: Larger step (visual)
    SlowSlideStep,      // 0x14-0x17: Slow sliding (ice)
    SlideStep,          // 0x18-0x1B: Normal sliding
    FastSlideStep,      // 0x1C-0x1F: Fast sliding
    TurnAway,           // 0x20-0x23: Turn away from direction
    TurnIn,             // 0x24-0x27: Turn toward direction
    TurnWaterfall,      // 0x28-0x2B: Waterfall turn
    SlowJumpStep,       // 0x2C-0x2F: Slow ledge jump
    JumpStep,           // 0x30-0x33: Normal ledge jump
    FastJumpStep,       // 0x34-0x37: Fast ledge jump

    // Non-directional control commands - 0x38+
    RemoveSliding,      // 0x38: Stop sliding state
    SetSliding,         // 0x39: Start sliding state
    RemoveFixedFacing,  // 0x3A: Allow facing changes
    FixFacing,          // 0x3B: Lock current facing
    ShowObject,         // 0x3C: Make object visible
    HideObject,         // 0x3D: Make object invisible
    StepSleep,          // 0x3E-0x46: Wait frames (param = frame count)
    StepEnd,            // 0x47: End movement sequence
    StepWaitEnd,        // 0x48: Wait for input then end (has length param)
    RemoveObject,       // 0x49: Remove from map
    StepLoop,           // 0x4A: Loop back to start
    StepStop,           // 0x4B: Stop and wait
    TeleportFrom,       // 0x4C: Teleport departure effect
    TeleportTo,         // 0x4D: Teleport arrival effect
    Skyfall,            // 0x4E: Fall from sky effect
    StepDig,            // 0x4F: Dig animation (has length param)
    StepBump,           // 0x50: Bump animation
    FishGotBite,        // 0x51: Fish got a bite animation
    FishCastRod,        // 0x52: Fish cast rod animation
    HideEmote,          // 0x53: Hide emote bubble
    ShowEmote,          // 0x54: Show emote bubble
    StepShake,          // 0x55: Shake animation (has displacement param)
    TreeShake,          // 0x56: Tree shake (cut)
    RockSmash,          // 0x57: Rock smash effect (has length param)
    ReturnDig,          // 0x58: Return from dig animation (has length param)
    SkyfallTop,         // 0x59: Fall from sky (top variant) - terminal
};

// Single semantic movement command
struct MovementCommand {
    MovementType type;
    Direction direction = Direction::Down;  // For directional commands
    uint8_t param = 0;                      // For sleep/dig/shake (frame count)

    // Helper to check if this moves the object
    bool is_step() const {
        switch (type) {
            case MovementType::SlowStep:
            case MovementType::Step:
            case MovementType::BigStep:
            case MovementType::SlowSlideStep:
            case MovementType::SlideStep:
            case MovementType::FastSlideStep:
            case MovementType::SlowJumpStep:
            case MovementType::JumpStep:
            case MovementType::FastJumpStep:
                return true;
            default:
                return false;
        }
    }

    // Helper to check if this is a facing change only
    bool is_turn() const {
        switch (type) {
            case MovementType::TurnHead:
            case MovementType::TurnStep:
            case MovementType::TurnAway:
            case MovementType::TurnIn:
            case MovementType::TurnWaterfall:
                return true;
            default:
                return false;
        }
    }

    // Check if directional (uses direction field)
    bool is_directional() const {
        return static_cast<uint8_t>(type) <= static_cast<uint8_t>(MovementType::FastJumpStep);
    }
};

// =============================================================================
// MOVEMENT TARGET (semantic object reference for movement commands)
// =============================================================================
// Replaces Crystal's raw object_id byte with typed semantic references.
// No sentinel values (0xFF for LAST_TALKED) - use explicit variants instead.

// Target types for movement commands (which object receives the movement)
enum class MovementTargetType : uint8_t {
    Object,     // Specific object by ID
    Player,     // The player character
    LastTalked, // Last NPC the player interacted with
};

// Semantic movement target - typed representation replacing Crystal's raw object_id
struct MovementTarget {
    MovementTargetType type;
    uint8_t object_id = 0;  // Only valid when type == Object

    // Factory methods for clear semantics
    static MovementTarget object(uint8_t id) { return {MovementTargetType::Object, id}; }
    static MovementTarget player() { return {MovementTargetType::Player, 0}; }
    static MovementTarget last_talked() { return {MovementTargetType::LastTalked, 0}; }

    // Check if this is the player (object_id 0 OR explicit Player type)
    bool is_player() const {
        return type == MovementTargetType::Player ||
               (type == MovementTargetType::Object && object_id == 0);
    }

    // Check if this targets the last talked NPC
    bool is_last_talked() const { return type == MovementTargetType::LastTalked; }
};

// =============================================================================
// SET VAR VALUE SOURCE (typed representation for variable assignment sources)
// =============================================================================
// Replaces the -1 sentinel in Sem_SetVar.value with an explicit typed source.
// No magic values - explicit variants for each source type.

enum class VarValueSourceType : uint8_t {
    Literal,      // Immediate constant value
    ScriptResult, // Copy from wScriptVar (script's last result)
};

// Semantic value source for variable assignment
struct VarValueSource {
    VarValueSourceType type;
    int16_t value = 0;  // Only valid when type == Literal

    // Factory methods for clear semantics
    static VarValueSource literal(int16_t v) { return {VarValueSourceType::Literal, v}; }
    static VarValueSource script_result() { return {VarValueSourceType::ScriptResult, 0}; }

    // Check source type
    bool is_literal() const { return type == VarValueSourceType::Literal; }
    bool is_script_result() const { return type == VarValueSourceType::ScriptResult; }
};

// Time of day
enum class TimeOfDay : uint8_t {
    Morning,  // 4:00 - 9:59
    Day,      // 10:00 - 17:59
    Night     // 18:00 - 3:59
};

// =============================================================================
// ELEVATOR DEFINITIONS (compiled semantic data)
// =============================================================================

// Semantic elevator ID (NOT a ROM pointer)
using ElevatorId = uint16_t;
inline constexpr ElevatorId ELEVATOR_NONE = 0xFFFF;

// Floor label constants (matching Crystal's FLOOR_* values for display)
enum class FloorLabel : uint8_t {
    B4F = 0,
    B3F = 1,
    B2F = 2,
    B1F = 3,
    F1 = 4,
    F2 = 5,
    F3 = 6,
    F4 = 7,
    F5 = 8,
    F6 = 9,
    F7 = 10,
    F8 = 11,
    F9 = 12,
    F10 = 13,
    F11 = 14,
    Roof = 15,
};

// Single floor destination in an elevator
struct ElevatorFloor {
    FloorLabel label;       // Display label (FLOOR_1F, etc.)
    uint8_t warp_id;        // Destination warp ID in target map
    MapId target_map;       // Semantic map ID to warp to

    bool operator==(const ElevatorFloor& other) const {
        return label == other.label && warp_id == other.warp_id && target_map == other.target_map;
    }
};

// Compiled elevator definition with ordered floor list
struct ElevatorDefinition {
    ElevatorId id;                          // Semantic ID assigned at compile time
    std::string name;                       // Debug/display name (e.g., "GoldenrodDeptStoreElevator")
    std::vector<ElevatorFloor> floors;      // Ordered floor destinations

    bool operator==(const ElevatorDefinition& other) const {
        return floors == other.floors;  // Identity is determined by floor list content
    }
};

// Day of week
enum class DayOfWeek : uint8_t {
    Sunday,
    Monday,
    Tuesday,
    Wednesday,
    Thursday,
    Friday,
    Saturday
};

} // namespace enginemon
