#pragma once
// engine/scripting/semantic_ir.hpp
// Generic Semantic Script IR for Enginemon
//
// This IR is the output of the semantic legalization stage.
// It contains NO:
// - Crystal opcode IDs
// - ROM addresses
// - Banked pointers
// - GB RAM addresses
// - ASM routine addresses
//
// It uses only semantic IDs (FlagId, ItemId, MapId, etc.) and
// operations that map directly to runtime/Lua emission.

#include "engine/core/types.hpp"
#include <variant>
#include <vector>
#include <string>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace enginemon {

// =============================================================================
// SEMANTIC TEXT
// =============================================================================

// Text argument types for semantic substitutions (replaces GB RAM/string-buffer)
enum class TextArgType : uint8_t {
    ItemName,       // Item name from ItemId
    PokemonName,    // Pokemon species name from SpeciesId  
    TrainerName,    // Trainer name from TrainerId or class+id
    PlayerName,     // Current player name
    RivalName,      // Rival name
    Number,         // Numeric value (money, coins, etc.)
    String,         // Generic string
};

// A single text argument with its semantic value
struct SemanticTextArg {
    TextArgType type;
    uint16_t id = 0;            // ItemId, SpeciesId, etc. for name lookups
    uint32_t value = 0;         // For Number type
    std::string str_value;      // For String type (renamed to avoid conflict with factory)
    
    static SemanticTextArg item_name(ItemId item) { 
        SemanticTextArg arg; arg.type = TextArgType::ItemName; arg.id = item; return arg; 
    }
    static SemanticTextArg pokemon_name(SpeciesId species) { 
        SemanticTextArg arg; arg.type = TextArgType::PokemonName; arg.id = species; return arg; 
    }
    static SemanticTextArg trainer_name(uint16_t trainer) { 
        SemanticTextArg arg; arg.type = TextArgType::TrainerName; arg.id = trainer; return arg; 
    }
    static SemanticTextArg number(uint32_t n) { 
        SemanticTextArg arg; arg.type = TextArgType::Number; arg.value = n; return arg; 
    }
    static SemanticTextArg from_string(const std::string& s) { 
        SemanticTextArg arg; arg.type = TextArgType::String; arg.str_value = s; return arg; 
    }
};

// Text control operations (semantic text flow, not Crystal-specific)
enum class SemanticTextOp : uint8_t {
    Text,       // Printable text run
    Arg,        // Text argument placeholder (references SemanticTextArg by index)
    Line,       // Move to line 2, no wait
    Next,       // Clear box, continue (no wait)
    Para,       // Wait → clear → continue
    Cont,       // Wait → scroll → continue
    Scroll,     // Scroll without wait
    Done,       // End text processing
    Prompt,     // Show cursor, wait, end
};

struct SemanticTextElement {
    SemanticTextOp op;
    std::string text;       // For SemanticTextOp::Text
    uint8_t arg_index = 0;  // For SemanticTextOp::Arg (index into args vector)
    
    static SemanticTextElement make_text(const std::string& s) { return {SemanticTextOp::Text, s, 0}; }
    static SemanticTextElement make_arg(uint8_t idx) { return {SemanticTextOp::Arg, "", idx}; }
    static SemanticTextElement make_line() { return {SemanticTextOp::Line, "", 0}; }
    static SemanticTextElement make_next() { return {SemanticTextOp::Next, "", 0}; }
    static SemanticTextElement make_para() { return {SemanticTextOp::Para, "", 0}; }
    static SemanticTextElement make_cont() { return {SemanticTextOp::Cont, "", 0}; }
    static SemanticTextElement make_scroll() { return {SemanticTextOp::Scroll, "", 0}; }
    static SemanticTextElement make_done() { return {SemanticTextOp::Done, "", 0}; }
    static SemanticTextElement make_prompt() { return {SemanticTextOp::Prompt, "", 0}; }
};

struct SemanticTextSequence {
    std::vector<SemanticTextElement> elements;
    std::vector<SemanticTextArg> args;  // Typed arguments for substitutions
    
    bool empty() const { return elements.empty(); }
    std::string debug_string() const;
};

// =============================================================================
// SEMANTIC LABELS (block-local only)
// =============================================================================

// Labels are block-local indices, not ROM addresses
using SemanticLabelId = uint32_t;
constexpr SemanticLabelId INVALID_LABEL = UINT32_MAX;

struct SemanticLabelRef {
    SemanticLabelId id = INVALID_LABEL;
    std::string name;   // For debugging/emission
    
    bool is_valid() const { return id != INVALID_LABEL; }
};

// =============================================================================
// SEMANTIC OPERATIONS
// =============================================================================

// --- Control Flow ---
struct Sem_End {};                      // Script termination
struct Sem_Return {};                   // Return from subroutine
struct Sem_Jump { SemanticLabelRef target; };
struct Sem_JumpIf {
    SemanticLabelRef target;
    std::string condition;              // "true", "false", "== N", etc.
};
struct Sem_Call { SemanticLabelRef target; };

// --- Flags and Variables ---
struct Sem_SetFlag { FlagId flag; };
struct Sem_ClearFlag { FlagId flag; };
struct Sem_CheckFlag { FlagId flag; };  // Sets result
// SetVar uses VarValueSource (typed variant), NOT -1 sentinel for script result copy.
// Use VarValueSource::literal(v) for immediate values, VarValueSource::script_result() for copying wScriptVar.
struct Sem_SetVar { VarId var; VarValueSource source; };
struct Sem_AddVar { VarId var; int16_t delta; };
struct Sem_CheckVar { VarId var; std::string op; int16_t value; };  // Sets result
struct Sem_Random { uint8_t range; };   // Sets result to random(0..range-1)

// --- Semantic State Variables (mini-games, puzzles, persistent event state) ---
// These are gameplay state variables identified by semantic ID, NOT RAM addresses
// readmem → Sem_ReadStateVar (loads value into wScriptVar for conditionals)
// writemem → Sem_WriteStateVar (writes wScriptVar to state)  
// loadmem → Sem_SetStateVar (writes immediate value to state)
struct Sem_ReadStateVar { StateVarId state_var; };   // Read state into wScriptVar
struct Sem_WriteStateVar { StateVarId state_var; };  // Write wScriptVar to state
struct Sem_SetStateVar { StateVarId state_var; uint8_t value; };  // Set immediate value

// --- Link Mode Capability Query (read-only) ---
// This is a capability/status query, NOT a mutable variable
// Used to check if link cable is connected and which generation
struct Sem_CheckLinkMode {};  // Sets wScriptVar to 0 if Gen1 or not connected, non-zero if Gen2

// =============================================================================
// SCRIPT EXECUTION CONTEXT - TYPED SEMANTIC STATE
// =============================================================================
// These structures represent typed semantic context that persists across script
// operations and block boundaries. They replace Crystal's GB RAM state (wCurPartyMon,
// wTempWildMonSpecies, etc.) with explicit typed structures.
//
// Lifecycle is managed by semantic operations:
// - Capability checks ESTABLISH context (presence = std::optional has value)
// - Operations CONSUME context (read then clear, or read and preserve)
// - Script termination CLEARS all context
// - Absence = std::nullopt (no hidden boolean needed)

// Party slot identifier (0-5 for 6-Pokemon party)
using PartySlotId = uint8_t;
constexpr PartySlotId PARTY_SLOT_NONE = 0xFF;

// =============================================================================
// FIELD MOVE SEMANTIC OPERATIONS
// =============================================================================
// These operations support HM field moves (Strength, Rock Smash) with typed context.
// The selected actor context carries semantic information (species, move type) across
// block boundaries without exposing wCurPartyMon or other GB RAM addresses.
//
// Reference: pokecrystal/engine/events/overworld.asm (TryStrengthOW, SetStrengthFlag, HasRockSmash)
//            pokecrystal/engine/events/treemons.asm (RockMonEncounter)

// Field move types for capability checks
enum class FieldMoveType : uint8_t {
    Strength,       // Pushing boulders
    RockSmash,      // Breaking rocks (may trigger encounter)
    // Note: Cut, Surf, Fly, Flash, Waterfall, Whirlpool not in corpus - don't add speculatively
};

// Strength capability check result (semantic, not Crystal 0/1/2)
enum class StrengthCapabilityResult : uint8_t {
    Available,      // Party has move AND badge, not yet active
    Unavailable,    // No move OR no badge
    AlreadyActive,  // Strength already activated this session
};

// Rock Smash capability check result (semantic)
enum class RockSmashCapabilityResult : uint8_t {
    Available,      // Party has Rock Smash
    Unavailable,    // No Rock Smash in party
};

// =============================================================================
// SelectedFieldActor - Context for field move actor selection
// =============================================================================
// Lifecycle:
//   ESTABLISHED by: Sem_CheckStrengthCapability (on Available)
//                   Sem_CheckRockSmashCapability (on Available)
//   CONSUMED by:    Sem_PrepareFieldMoveNickname (reads, preserves)
//                   Sem_ActivateStrength (reads, clears)
//                   Sem_PlayFieldActorCry (reads, preserves)
//   CLEARED by:     Script termination
//                   Explicit clear after field-move completion
//
// Absence (std::nullopt) means no field actor is selected.
// No separate boolean needed - use optional presence.

struct SelectedFieldActor {
    PartySlotId slot;           // Which party member has the move (0-5)
    FieldMoveType move;         // Which field move was checked
    SpeciesId species;          // Species for cry/nickname lookup
    
    SelectedFieldActor(PartySlotId s, FieldMoveType m, SpeciesId sp)
        : slot(s), move(m), species(sp) {}
};

// =============================================================================
// PendingFieldEncounter - Context for field-triggered wild encounter
// =============================================================================
// Lifecycle:
//   ESTABLISHED by: Sem_TryRockSmashEncounter (on encounter success, 40% chance)
//                   (Future: Sem_TryHeadbuttEncounter, Sem_TrySweetScentEncounter)
//   CONSUMED by:    Sem_LoadPendingEncounter (reads species/level, clears)
//                   Sem_ReadEncounterSpecies (reads species only, preserves)
//   CLEARED by:     Script termination
//                   Sem_LoadPendingEncounter consumption
//
// Absence (std::nullopt) means no pending encounter exists.
// This is distinct from "encounter check failed" - failure means context was
// never established (optional remains nullopt), not cleared after establishment.

struct PendingFieldEncounter {
    SpeciesId species;          // Species to battle
    uint8_t level;              // Level of wild Pokemon
    
    PendingFieldEncounter(SpeciesId sp, uint8_t lv)
        : species(sp), level(lv) {}
};

// =============================================================================
// ScriptExecutionContext - Full typed context for script execution
// =============================================================================
// This structure holds ALL typed semantic context needed during script execution.
// It replaces Crystal's scattered GB RAM state with explicit, typed fields.
//
// Rules:
// - NO arbitrary key/value scratch storage
// - NO generic temporary integer slots
// - NO raw RAM-like state
// - Every field has explicit semantic meaning
// - Absence is represented by std::nullopt, not sentinel values

struct ScriptExecutionContext {
    // Field move actor selection (set by capability checks)
    std::optional<SelectedFieldActor> selected_field_actor;
    
    // Pending field encounter (set by encounter attempts)
    std::optional<PendingFieldEncounter> pending_field_encounter;
    
    // Script variable (wScriptVar equivalent - result of checks/operations)
    // This IS needed as it's the primary communication channel for conditionals
    int16_t script_var = 0;
    
    // Strength active flag - set when Strength is activated for current map session
    // This is session-level state (survives script termination until map change)
    bool strength_active = false;
    
    // Clear all context (called on script termination)
    // Note: strength_active is session-level and NOT cleared here
    void clear() {
        selected_field_actor = std::nullopt;
        pending_field_encounter = std::nullopt;
        script_var = 0;
    }
    
    // Check if any field-move context exists
    bool has_field_context() const {
        return selected_field_actor.has_value() || pending_field_encounter.has_value();
    }
};

// =============================================================================
// FIELD MOVE SEMANTIC OPERATIONS
// =============================================================================

// Check if Strength can be used overworld
// Sets script_var to StrengthCapabilityResult equivalent (0=Available, 1=Unavailable, 2=AlreadyActive)
// On Available: establishes ctx.selected_field_actor with party slot and species
// Reference: pokecrystal TryStrengthOW
struct Sem_CheckStrengthCapability {};

// Activate Strength field effect
// Requires: ctx.selected_field_actor (established by Sem_CheckStrengthCapability)
// Consumes: ctx.selected_field_actor (clears after activation)
// Effect: Sets strength active flag for current map session
// Reference: pokecrystal SetStrengthFlag
struct Sem_ActivateStrength {};

// Check if Rock Smash can be used
// Sets script_var to RockSmashCapabilityResult equivalent (0=Available, 1=Unavailable)
// On Available: establishes ctx.selected_field_actor with party slot and species
// Reference: pokecrystal HasRockSmash
struct Sem_CheckRockSmashCapability {};

// Prepare field move actor's nickname for text display
// Requires: ctx.selected_field_actor (reads slot, looks up nickname)
// Preserves: ctx.selected_field_actor (does not clear)
// Reference: pokecrystal GetPartyNickname (uses wCurPartyMon set by capability check)
struct Sem_PrepareFieldMoveNickname {
    uint8_t buffer_slot;    // Which text buffer to populate
};

// Attempt rock smash encounter (40% chance based on encounter table)
// On success: establishes ctx.pending_field_encounter with species and level
// On failure: ctx.pending_field_encounter remains nullopt
// Sets script_var to encountered species (0 if no encounter)
// Reference: pokecrystal RockMonEncounter
struct Sem_TryRockSmashEncounter {};

// Read encounter species from pending encounter context
// Requires: ctx.pending_field_encounter (reads species)
// Preserves: ctx.pending_field_encounter (does not clear - consumed by LoadPendingEncounter)
// Sets script_var to species ID (0 if no pending encounter)
// Reference: script pattern "readmem wTempWildMonSpecies"
struct Sem_ReadEncounterSpecies {};

// Play cry using species from selected actor context
// Requires: ctx.selected_field_actor (reads species for cry lookup)
// Preserves: ctx.selected_field_actor (does not clear)
// Reference: used after Strength activation to play Pokemon cry
struct Sem_PlayFieldActorCry {};

// =============================================================================
// PENDING ENCOUNTER CONSUMPTION
// =============================================================================

// Load pending encounter for battle setup
// Requires: ctx.pending_field_encounter (must be present)
// Consumes: ctx.pending_field_encounter (clears after loading)
// Effect: Sets up wild battle with species/level from pending encounter
// 
// This is the ONLY way to consume PendingFieldEncounter for battle.
// Distinct from Sem_LoadWildMon which takes explicit species/level parameters.
//
// Reference: Crystal randomwildmon opcode (0x5B)
// Usage pattern:
//   Sem_TryRockSmashEncounter  → may establish pending_field_encounter
//   Sem_ReadEncounterSpecies   → reads species for branch decision
//   [iffalse .done]            → skip battle if no encounter (species == 0)
//   Sem_LoadPendingEncounter   → consume pending encounter for battle
//   Sem_StartBattle
struct Sem_LoadPendingEncounter {};

// --- UI/Text ---
struct Sem_OpenText {};
struct Sem_CloseText {};
struct Sem_ShowText { SemanticTextSequence sequence; };
struct Sem_ShowTextAndEnd { SemanticTextSequence sequence; };       // jumptext equivalent
struct Sem_FacePlayerAndShowText { SemanticTextSequence sequence; }; // jumptextfaceplayer
struct Sem_WaitButton {};
struct Sem_YesNo {};                    // Sets result to true/false
struct Sem_Choice { std::vector<std::string> options; };
// Text argument preparation (semantic replacement for getXXXname commands)
// Consolidated into a single type to reduce variant size
struct Sem_PrepareTextArg {
    TextArgType arg_type;
    uint8_t buffer_slot;        // Which text argument slot to populate
    uint16_t id = 0;            // ItemId, SpeciesId, TrainerId, etc. depending on arg_type
    VarId source_var = 0;       // For Number type (reads from variable)
    std::string str_value;      // For String type
    
    static Sem_PrepareTextArg item_name(ItemId item, uint8_t slot) {
        Sem_PrepareTextArg p; p.arg_type = TextArgType::ItemName; p.id = item; p.buffer_slot = slot; return p;
    }
    static Sem_PrepareTextArg pokemon_name(SpeciesId species, uint8_t slot) {
        Sem_PrepareTextArg p; p.arg_type = TextArgType::PokemonName; p.id = species; p.buffer_slot = slot; return p;
    }
    static Sem_PrepareTextArg trainer_name(uint16_t trainer, uint8_t slot) {
        Sem_PrepareTextArg p; p.arg_type = TextArgType::TrainerName; p.id = trainer; p.buffer_slot = slot; return p;
    }
    static Sem_PrepareTextArg number(VarId var, uint8_t slot) {
        Sem_PrepareTextArg p; p.arg_type = TextArgType::Number; p.source_var = var; p.buffer_slot = slot; return p;
    }
    static Sem_PrepareTextArg string(const std::string& s, uint8_t slot) {
        Sem_PrepareTextArg p; p.arg_type = TextArgType::String; p.str_value = s; p.buffer_slot = slot; return p;
    }
};

// --- Inventory ---
struct Sem_GiveItem { ItemId item; uint8_t quantity; };
struct Sem_TakeItem { ItemId item; uint8_t quantity; };
struct Sem_CheckItem { ItemId item; };   // Sets result
struct Sem_GiveItemVerbose { ItemId item; uint8_t quantity; };  // With notification
struct Sem_GiveMoney { uint32_t amount; uint8_t account; };
struct Sem_TakeMoney { uint32_t amount; uint8_t account; };
struct Sem_CheckMoney { uint32_t amount; uint8_t account; };
struct Sem_GiveCoins { uint16_t coins; };
struct Sem_TakeCoins { uint16_t coins; };
struct Sem_CheckCoins { uint16_t coins; };

// --- Party/Pokemon ---
struct Sem_GivePokemon {
    SpeciesId species;
    uint8_t level;
    ItemId held_item;
    bool has_nickname;              // If true, nickname/ot_name are valid
    std::string nickname;
    std::string ot_name;
};
struct Sem_GiveEgg { SpeciesId species; uint8_t level; };

// =============================================================================
// Sem_HealParty - Heal all eligible party members
// =============================================================================
// Source-proven contract from pokecrystal/engine/pokemon/health.asm (HealParty):
//
// Behavior:
//   - Iterates through all party slots
//   - SKIPS eggs (cp EGG / jr z, .next)
//   - For each non-egg member:
//     1. Restore HP to max (revives fainted Pokemon)
//     2. Clear all status conditions (poison, burn, sleep, freeze, paralyze)
//     3. Restore all move PP to maximum (preserving PP Up investment)
//
// PP restoration (from Gen2Recomped):
//   max_pp = base_pp + (base_pp / 5) * pp_ups
//   Each PP Up adds 20% of base PP to the maximum.
//   PP Up investment (0-3 per move) is PRESERVED, not modified.
//
// What is NOT modified:
//   - DVs, Stat Exp, Level, Experience
//   - Friendship/Happiness
//   - Held items
//   - Pokérus status
//   - Met info (location, level, time)
//   - Egg status/cycles (eggs are skipped entirely)
//
// Script result:
//   - Does NOT modify wScriptVar
//   - Produces no script result value
//
// This is a generic engine concept usable across Gen 1/2/3 frontends.
// =============================================================================
struct Sem_HealParty {};

struct Sem_CheckPokemon { SpeciesId species; };  // Sets result

// --- Movement/Object ---
// Semantic movement sequence - preserves all gameplay-visible movement operations
// Order, direction, speed, waits, jumps, turns are all preserved
// Target uses MovementTarget (typed variant), NOT raw object_id with 0xFF sentinel
struct Sem_ApplyMovement {
    MovementTarget target;                      // Who receives the movement (object, player, or last_talked)
    std::vector<MovementCommand> commands;      // Full ordered movement sequence
};
struct Sem_FacePlayer { uint8_t object_id; };
struct Sem_FaceObject { uint8_t object1; uint8_t object2; };
struct Sem_TurnObject { uint8_t object_id; Direction facing; };
struct Sem_ShowObject { uint8_t object_id; };
struct Sem_HideObject { uint8_t object_id; };
struct Sem_MoveObject { uint8_t object_id; uint8_t x; uint8_t y; };
struct Sem_SetLastTalked { uint8_t object_id; };
struct Sem_VariableSprite { uint8_t slot; uint8_t sprite; };
struct Sem_Follow { uint8_t object1; uint8_t object2; };
struct Sem_StopFollow {};
struct Sem_Emote { uint8_t emote_id; uint8_t object_id; uint8_t duration; };

// --- Map/Warp/Scene ---
struct Sem_Warp { MapId map; uint8_t x; uint8_t y; };
struct Sem_WarpFacing { Direction facing; MapId map; uint8_t x; uint8_t y; };
// Warp to backup location (Crystal BADWARP - when script warp has map=0)
// This triggers MAPSETUP_BADWARP which restores player to wBackupMap location
// Distinct from Sem_Warp because there's no MapId to validate
struct Sem_WarpToBackup { uint8_t x; uint8_t y; };
struct Sem_WarpToBackupFacing { Direction facing; uint8_t x; uint8_t y; };
struct Sem_SetScene { uint8_t scene; };
struct Sem_CheckScene {};                // Sets result to current map scene
struct Sem_SetMapScene { MapId map; uint8_t scene; };
struct Sem_CheckMapScene { MapId map; };  // Sets result
struct Sem_ModifyWarp { uint8_t warp_id; MapId target_map; };
struct Sem_SetBlackoutPoint { MapId map; };
struct Sem_ReloadMap {};
struct Sem_RefreshMap {};
struct Sem_ChangeBlock { uint8_t x; uint8_t y; uint8_t block; };

// --- Battle ---
struct Sem_LoadWildMon { SpeciesId species; uint8_t level; };
struct Sem_LoadTrainer { uint8_t trainer_group; uint8_t trainer_id; };
struct Sem_StartBattle {};
struct Sem_ReloadMapAfterBattle {};
// Win/Loss text uses semantic TextId references, NOT ROM address strings.
// TextId values are resolved by the frontend during compilation.
// Absence (Crystal's "0" operand) is modeled as std::nullopt, NOT a magic sentinel.
struct Sem_SetWinLossText { 
    std::optional<TextId> win_text;   // std::nullopt = no win text
    std::optional<TextId> loss_text;  // std::nullopt = no loss text (common)
};
struct Sem_TrainerText { uint8_t text_id; };
struct Sem_TrainerFlagAction { uint8_t action; };
struct Sem_CheckJustBattled {};  // Sets result
struct Sem_EndIfJustBattled {};  // Conditional end

// --- Audio ---
struct Sem_PlayMusic { MusicId music; };
struct Sem_PlaySound { SfxId sound; };
struct Sem_PlayCry { SpeciesId species; };
struct Sem_PlaySlowCry { SpeciesId species; };  // Slowed-down cry (lower pitch, longer duration)
struct Sem_WaitSound {};
struct Sem_FadeOutMusic { MusicId music; uint8_t fade_time; };
struct Sem_FadeToSilence {};  // Fade current music to silence with fixed timing
struct Sem_PlayMapMusic {};
struct Sem_RestartMapMusic {};  // Restart stored map music without querying map
struct Sem_WarpSound {};
struct Sem_SpecialSound {};
struct Sem_SetMusicRestartFlag { bool prevent_restart; };  // Controls music restart on map load

// --- Time/Wait ---
struct Sem_Wait { uint8_t duration; };
struct Sem_Pause { uint8_t length; };
struct Sem_CheckTime { uint8_t time_flags; };  // Sets result

// --- Phone ---
struct Sem_AddPhoneNumber { uint8_t person; };
struct Sem_DeletePhoneNumber { uint8_t person; };
struct Sem_CheckPhoneNumber { uint8_t person; };
struct Sem_CheckPhoneCall {};  // Checks if a phone call is pending (sets wScriptVar)
struct Sem_SpecialPhoneCall { uint16_t call_id; };  // Trigger special phone call event

// --- Decoration ---
struct Sem_DescribeDecoration { uint8_t decoration_id; };  // Show decoration description text

// --- Pokemon Mail ---
// Mail operations use semantic PokeMailId, NOT ROM pointers.
// PokeMailId is assigned by the frontend during compilation from mail data extraction.
struct Sem_GivePokeMail { PokeMailId mail_id; };    // Attach mail to Pokemon  
struct Sem_CheckPokeMail { PokeMailId mail_id; };   // Check if Pokemon has mail

// --- Visual Effects ---
struct Sem_Earthquake { uint8_t param; };
struct Sem_FadeIn {};
struct Sem_FadeOut {};

// --- Screen Fade (parameterized) ---
// Generic screen fade operation replacing the 4 Crystal-specific fades
// Blocks script execution until fade animation completes (4 steps × 2 frames = 8 frames)
//
// All four fades share: direction (In/Out), color (White/Black), blocking (8 frames)
// FadeOutToWhite additionally pre-fills the screen with white before fading.
// This is observably distinct (brief flash) and preserved via the prefill flag.
enum class FadeDirection : uint8_t { In, Out };
enum class FadeColor : uint8_t { White, Black };
struct Sem_ScreenFade {
    FadeDirection direction;
    FadeColor color;
    bool prefill;  // True for FadeOutToWhite only (calls FillWhiteBGColor before fade)
};

// --- Renderer Synchronization ---
// These operations ensure visual state matches game state after changes.
// They may include a blocking wait for hardware synchronization.

// Palette/background synchronization with blocking wait
// Used when scripts need to ensure palette state is consistent before continuing
// Runtime: May emit a frame wait to ensure visual consistency
// Source: ReloadSpritesNoPalettes (51), ClearBGPalettes (52)
struct Sem_SyncPalettes {
    uint8_t wait_frames;  // Blocking wait: 1 for ReloadSpritesNoPalettes, 4 for ClearBGPalettes
};

// Request player sprite refresh after state change
// Called after flag changes that affect player visual appearance (e.g., ENGINE_KRIS_IN_CABLE_CLUB)
// Runtime: Triggers player sprite recomputation based on current flags/state
// Source: UpdatePlayerSprite (56)
struct Sem_RefreshPlayerSprite {};

// Request sprite refresh after variablesprite changes
// Called after Sem_VariableSprite to ensure the new sprite graphics are loaded
// In Enginemon, Sem_VariableSprite should immediately reflect the change, so this
// becomes a sync point to ensure consistency.
// Source: LoadUsedSpritesGFX (94) - also runs MAPCALLBACK_SPRITES
struct Sem_SyncSprites {};

// Full sprite state rebuild
// Clears and rebuilds the entire sprite list, then loads all sprite graphics
// Used in scenarios where multiple sprites may have changed or after major state transitions
// Source: RefreshSprites (158)
struct Sem_RebuildSprites {};

// --- Misc ---
struct Sem_WildOn {};
struct Sem_WildOff {};
struct Sem_Special { uint16_t special_id; std::string name; };
struct Sem_Pokepic { SpeciesId species; };
struct Sem_ClosePokepic {};
struct Sem_Pokemart { uint8_t dialog_id; uint16_t mart_id; };
struct Sem_Elevator { ElevatorId elevator_id; };  // Semantic ID, NOT ROM pointer
struct Sem_Trade { uint8_t trade_id; };
struct Sem_FruitTree { uint8_t tree_id; };
struct Sem_HallOfFame {};
struct Sem_Credits {};
struct Sem_CheckSave {};   // Sets result
struct Sem_CheckWarp {};   // Sets result based on warp validity (used after Battle Tower)
struct Sem_SetPlayerPalette { uint8_t palette_id; };  // Change player sprite palette

// --- Standard Scripts (known semantic behaviors) ---
// StdScriptId is a semantic identifier for standard scripts, NOT Crystal's raw table index.
// The semantic linker verifies these references resolve to compiled StdScript bodies.
struct Sem_CallStd { StdScriptId std_id; std::string name; };
struct Sem_JumpStd { StdScriptId std_id; std::string name; };

// --- Unlowered (explicit failure case) ---
// Commands that cannot be semantically lowered remain explicit
struct Sem_Unlowered {
    uint8_t opcode;                 // Original Crystal opcode
    std::vector<uint8_t> raw_bytes; // Original bytes for debugging
    std::string reason;             // Why it couldn't be lowered
    std::string provenance;         // Source file/line
};

// =============================================================================
// COMPILER DIAGNOSTIC (NOT packageable - frontend only)
// =============================================================================

// Diagnostic for commands that failed to lower
// This is SEPARATE from SemanticOp and cannot enter the package
struct UnloweredDiagnostic {
    uint8_t opcode;                 // Original Crystal opcode
    std::vector<uint8_t> raw_bytes; // Original bytes for debugging
    std::string reason;             // Why it couldn't be lowered
    std::string provenance;         // Source file/line (ROM address)
    size_t block_index;             // Which block this was in
    size_t instruction_index;       // Position in block
};

// =============================================================================
// SEMANTIC OPERATION VARIANT (packageable - NO Crystal concepts)
// =============================================================================

using SemanticOp = std::variant<
    // Control flow
    Sem_End, Sem_Return, Sem_Jump, Sem_JumpIf, Sem_Call,
    
    // Flags/Variables
    Sem_SetFlag, Sem_ClearFlag, Sem_CheckFlag,
    Sem_SetVar, Sem_AddVar, Sem_CheckVar, Sem_Random,
    
    // UI/Text
    Sem_OpenText, Sem_CloseText, Sem_ShowText, Sem_ShowTextAndEnd,
    Sem_FacePlayerAndShowText, Sem_WaitButton, Sem_YesNo, Sem_Choice,
    Sem_PrepareTextArg,  // Consolidated text argument preparation
    
    // Inventory
    Sem_GiveItem, Sem_TakeItem, Sem_CheckItem, Sem_GiveItemVerbose,
    Sem_GiveMoney, Sem_TakeMoney, Sem_CheckMoney,
    Sem_GiveCoins, Sem_TakeCoins, Sem_CheckCoins,
    
    // Party/Pokemon
    Sem_GivePokemon, Sem_GiveEgg, Sem_HealParty, Sem_CheckPokemon,
    
    // Movement/Object
    Sem_ApplyMovement, Sem_FacePlayer, Sem_FaceObject, Sem_TurnObject,
    Sem_ShowObject, Sem_HideObject, Sem_MoveObject, Sem_SetLastTalked,
    Sem_VariableSprite, Sem_Follow, Sem_StopFollow, Sem_Emote,
    
    // Map/Warp/Scene
    Sem_Warp, Sem_WarpFacing, Sem_WarpToBackup, Sem_WarpToBackupFacing,
    Sem_SetScene, Sem_CheckScene,
    Sem_SetMapScene, Sem_CheckMapScene, Sem_ModifyWarp, Sem_SetBlackoutPoint,
    Sem_ReloadMap, Sem_RefreshMap, Sem_ChangeBlock,
    
    // Battle
    Sem_LoadWildMon, Sem_LoadTrainer, Sem_StartBattle, Sem_ReloadMapAfterBattle,
    Sem_SetWinLossText, Sem_TrainerText, Sem_TrainerFlagAction,
    Sem_CheckJustBattled, Sem_EndIfJustBattled,
    
    // Audio
    Sem_PlayMusic, Sem_PlaySound, Sem_PlayCry, Sem_PlaySlowCry, Sem_WaitSound,
    Sem_FadeOutMusic, Sem_FadeToSilence, Sem_PlayMapMusic, Sem_RestartMapMusic,
    Sem_WarpSound, Sem_SpecialSound, Sem_SetMusicRestartFlag,
    
    // Time/Wait
    Sem_Wait, Sem_Pause, Sem_CheckTime,
    
    // Phone
    Sem_AddPhoneNumber, Sem_DeletePhoneNumber, Sem_CheckPhoneNumber, Sem_CheckPhoneCall,
    Sem_SpecialPhoneCall,  // Special phone call events
    
    // Decoration
    Sem_DescribeDecoration,
    
    // Pokemon Mail
    Sem_GivePokeMail, Sem_CheckPokeMail,
    
    // Visual Effects
    Sem_Earthquake, Sem_FadeIn, Sem_FadeOut, Sem_ScreenFade,
    
    // Renderer Synchronization
    Sem_SyncPalettes, Sem_RefreshPlayerSprite, Sem_SyncSprites, Sem_RebuildSprites,
    
    // Misc
    Sem_WildOn, Sem_WildOff, Sem_Special, Sem_Pokepic, Sem_ClosePokepic,
    Sem_Pokemart, Sem_Elevator, Sem_Trade, Sem_FruitTree,
    Sem_HallOfFame, Sem_Credits, Sem_CheckSave, Sem_CheckWarp,
    Sem_SetPlayerPalette,
    
    // Standard scripts
    Sem_CallStd, Sem_JumpStd,
    
    // Semantic state variables (mini-games, puzzles)
    Sem_ReadStateVar, Sem_WriteStateVar, Sem_SetStateVar,
    
    // Link mode capability query
    Sem_CheckLinkMode,
    
    // Field move operations (Strength, Rock Smash)
    Sem_CheckStrengthCapability, Sem_ActivateStrength,
    Sem_CheckRockSmashCapability, Sem_PrepareFieldMoveNickname,
    Sem_TryRockSmashEncounter, Sem_ReadEncounterSpecies,
    Sem_PlayFieldActorCry,
    
    // Pending encounter consumption (distinct from LoadWildMon)
    Sem_LoadPendingEncounter
    
    // NOTE: Sem_Unlowered is REMOVED from this variant
    // Failed lowering produces UnloweredDiagnostic in LoweringResult, not a packageable op
>;

// =============================================================================
// SEMANTIC INSTRUCTION
// =============================================================================

struct SemanticInstruction {
    SemanticOp op;
    
    // Optional provenance (for debugging, not for runtime)
    std::string source_label;
    uint32_t source_index = 0;      // Index in source block's commands
    
    // NOTE: is_unlowered() removed - SemanticOp cannot contain unlowered commands
    // Failed lowering produces UnloweredDiagnostic separately
};

// =============================================================================
// SEMANTIC BASIC BLOCK
// =============================================================================

struct SemanticBasicBlock {
    // Block identity
    SemanticLabelId id = INVALID_LABEL;
    std::string label;              // For emission
    bool is_entry = false;
    
    // Instructions (all are valid semantic ops - no unlowered)
    std::vector<SemanticInstruction> instructions;
    
    // Terminator (last instruction should be a terminator)
    // Terminators: Sem_End, Sem_Return, Sem_Jump, Sem_JumpIf (conditional fallthrough)
    
    // Statistics
    size_t instruction_count() const { return instructions.size(); }
};

// =============================================================================
// SEMANTIC SCRIPT IR
// =============================================================================

struct SemanticScriptIR {
    // Script identity (semantic, not ROM-based)
    std::string script_id;          // e.g., "new_bark_town::bg_event_0"
    std::string script_name;        // Human-readable name
    
    // Blocks
    std::vector<SemanticBasicBlock> blocks;
    
    // Label resolution
    std::unordered_map<SemanticLabelId, size_t> label_to_block;
    
    // Statistics
    size_t total_instructions() const;
    bool is_fully_lowered() const;
    
    // Provenance (for debugging)
    uint32_t source_rom_address = 0;
};

// =============================================================================
// LOWERING RESULT
// =============================================================================

struct LoweringResult {
    SemanticScriptIR ir;            // Only contains valid semantic ops
    bool success = false;
    
    // Compiler-side diagnostics (NOT in ir - cannot be packaged)
    std::vector<UnloweredDiagnostic> unlowered;
    
    // Statistics
    size_t commands_consumed = 0;   // Source commands processed
    size_t commands_lowered = 0;    // Semantic instructions produced
    size_t commands_unlowered = 0;  // Commands that failed to lower (in unlowered vector)
    size_t commands_absorbed = 0;   // Consumed but no instruction (e.g., string format ops)
    
    // Diagnostics
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    
    // Per-opcode breakdown
    std::unordered_map<uint8_t, size_t> lowered_by_opcode;
    std::unordered_map<uint8_t, size_t> unlowered_by_opcode;
    std::unordered_map<uint8_t, size_t> absorbed_by_opcode;  // Track which opcodes are absorbed
};

// =============================================================================
// CORPUS LOWERING STATISTICS
// =============================================================================

struct Stage4CorpusStats {
    // Script counts
    size_t total_scripts = 0;
    size_t fully_lowered_scripts = 0;
    size_t partially_lowered_scripts = 0;
    
    // Command counts (invariant: total_commands = commands_lowered + commands_unlowered + commands_absorbed)
    size_t total_commands = 0;      // Source commands consumed
    size_t commands_lowered = 0;    // Semantic instructions produced
    size_t commands_unlowered = 0;  // Sem_Unlowered instructions
    size_t commands_absorbed = 0;   // Consumed but no instruction (e.g., string format)
    
    // Opcode coverage
    size_t unique_opcodes_encountered = 0;
    size_t opcodes_with_lowering = 0;
    size_t opcodes_without_lowering = 0;
    
    // Per-opcode breakdown
    std::unordered_map<uint8_t, size_t> lowered_by_opcode;
    std::unordered_map<uint8_t, size_t> unlowered_by_opcode;
    std::unordered_map<uint8_t, size_t> absorbed_by_opcode;  // Track which opcodes are absorbed
    std::unordered_map<uint8_t, std::string> opcode_names;
    
    // Native/RAM references
    size_t native_refs_lowered = 0;
    size_t native_refs_unlowered = 0;
    size_t ram_refs_lowered = 0;
    size_t ram_refs_unlowered = 0;
    
    // Helpers
    double lowering_percentage() const {
        if (total_commands == 0) return 100.0;
        return 100.0 * commands_lowered / total_commands;
    }
    
    double script_coverage() const {
        if (total_scripts == 0) return 100.0;
        return 100.0 * fully_lowered_scripts / total_scripts;
    }
    
    void accumulate(const LoweringResult& result);
};

} // namespace enginemon
