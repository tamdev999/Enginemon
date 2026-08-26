#pragma once
// engine/scripting/api_bindings.hpp
// C++ semantic APIs exposed to Lua
// These are what generated Crystal scripts and mod scripts call
// Example: ctx.world:move_actor(...) -> World::move_actor()

#include "engine/core/types.hpp"
#include "engine/scripting/lua_runtime.hpp"  // For LuaRuntime, StubServices, StubActorState, etc.
#include <lua.hpp>
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace enginemon {

class GameContext;

// Registration helper
void register_all_apis(lua_State* L, GameContext& ctx);

// ============================================================================
// World API - ctx.world
// ============================================================================
// Forward declaration
class MovementManager;

namespace world_api {
    // Actor state type alias (from StubServices)
    using ActorState = StubActorState;
    
    // Movement
    // ctx.world:move_actor(actor_id, direction, steps) - legacy
    // ctx.world:move_actor(actor_id, {down=N, up=N, ...}) - semantic
    // Yields until movement complete (async)
    int move_actor(lua_State* L);
    int move_actor_dispatch(lua_State* L);  // Dispatcher that detects arg type
    
    // ctx.world:face_actor(actor_id, direction)
    int face_actor(lua_State* L);
    
    // ctx.world:face_player()
    // Makes the NPC face the player
    int face_player(lua_State* L);
    
    // ctx.world:get_player_pos() -> x, y, map_id
    int get_player_pos(lua_State* L);
    
    // ctx.world:get_actor_pos(actor_id) -> x, y
    int get_actor_pos(lua_State* L);
    
    // ctx.world:get_actor_facing(actor_id) -> string
    int get_actor_facing(lua_State* L);
    
    // ctx.world:teleport_player(map_id, x, y)
    int teleport_player(lua_State* L);
    
    // ctx.world:warp(map_id, warp_id)
    // Yields during transition
    int warp(lua_State* L);

    // ctx.world:warp_to_spawn()
    // Warp the player to their last-set backup/spawn warp position.
    // Semantic equivalent of Sem_WarpToBackup.
    int warp_to_spawn(lua_State* L);
    
    // NPC visibility
    // ctx.world:show_npc(npc_id)
    int show_npc(lua_State* L);
    
    // ctx.world:hide_npc(npc_id)
    int hide_npc(lua_State* L);
    
    // ctx.world:npc_visible(npc_id) -> bool
    int npc_visible(lua_State* L);
    
    // Map queries
    // ctx.world:current_map() -> map_id
    int current_map(lua_State* L);
    
    // ctx.world:map_name(map_id) -> string
    int map_name(lua_State* L);
    
    // ctx.world:apply_movement(object_id, movement_data)
    // Applies a movement sequence to an object, yields until complete
    int apply_movement(lua_State* L);
    
    // ctx.world:set_variable_sprite(slot_name, sprite_ref)
    // Assigns a stable SpriteId to a named variable slot.
    // slot_name: semantic slot (e.g., "copycat", "fuchsia_gym_1")
    // sprite_ref: stable typed SpriteId string (e.g., "fixed:lass", "fixed:janine")
    // Stores directly in GameState::variable_sprites[slot_name] = sprite_ref.
    // No Crystal numeric index mapping in the engine layer.
    // Source: Crystal variablesprite opcode 0x6D + wVariableSprites semantic.
    int set_variable_sprite(lua_State* L);

    // ctx.world:set_daycare_species(slot, species_id)
    // Sets the species occupying a Day Care slot (1 or 2).
    // species_id: 1-251 (valid Crystal species), or 0 to clear the slot.
    // Stores in GameState::daycare_slot[slot-1].
    // Source: Crystal wBreedMon1Species / wBreedMon2Species semantics.
    int set_daycare_species(lua_State* L);
    
    // Test helpers - operate on specific runtime's stub state
    void reset_world_state(LuaRuntime* runtime);
    void set_actor_pos(LuaRuntime* runtime, int id, int x, int y);
    void set_actor_facing(LuaRuntime* runtime, int id, const std::string& facing);
    ActorState get_actor_state(LuaRuntime* runtime, int id);
    const std::vector<std::pair<std::string, std::string>>& get_movement_calls(LuaRuntime* runtime);
    
    // Movement manager access - per-runtime
    MovementManager& get_movement_manager(LuaRuntime* runtime);
    
    // Enable/disable async movement mode - per-runtime
    void set_async_movement(LuaRuntime* runtime, bool enabled);
    bool is_async_movement_enabled(LuaRuntime* runtime);
}

// ============================================================================
// Battle API - ctx.battle
// ============================================================================
namespace battle_api {
    // ctx.battle:start_wild(species_id, level)
    // Yields until battle complete, returns result
    int start_wild(lua_State* L);
    
    // ctx.battle:start_trainer(trainer_id)
    // Yields until battle complete, returns result
    int start_trainer(lua_State* L);
    
    // Battle result constants exposed as ctx.battle.RESULT_*
}

// ============================================================================
// Party API - ctx.party
// ============================================================================
namespace party_api {
    // ctx.party:count() -> number
    int count(lua_State* L);
    
    // ctx.party:get(slot) -> pokemon table or nil
    int get(lua_State* L);
    
    // ctx.party:has_species(species_id) -> bool
    int has_species(lua_State* L);
    
    // ctx.party:heal_all()
    int heal_all(lua_State* L);
    
    // ctx.party:add_pokemon(species_id, level) -> bool
    int add_pokemon(lua_State* L);
    
    // ctx.party:has_move(move_id) -> bool
    // Checks if any party member knows move
    int has_move(lua_State* L);
    
    // ctx.party:can_use_hm(hm_name) -> bool
    // Checks badge + party move (e.g. "cut", "surf")
    int can_use_hm(lua_State* L);
}

// ============================================================================
// Inventory API - ctx.inventory
// ============================================================================
namespace inventory_api {
    // ctx.inventory:give(item_id, count)
    int give(lua_State* L);
    
    // ctx.inventory:take(item_id, count) -> bool
    int take(lua_State* L);
    
    // ctx.inventory:has(item_id, count?) -> bool
    int has(lua_State* L);
    
    // ctx.inventory:count(item_id) -> number
    int count(lua_State* L);
    
    // ctx.inventory:give_money(amount)
    int give_money(lua_State* L);
    
    // ctx.inventory:take_money(amount) -> bool
    int take_money(lua_State* L);
    
    // ctx.inventory:has_money(amount) -> bool
    int has_money(lua_State* L);
    
    // ctx.inventory:money() -> number
    int money(lua_State* L);

    // ctx.inventory:prepare_money_text(account, buffer_slot)
    // Copies the current money balance for account into strbuf<N>_money in GameState.
    // account: 0=player, 1=mom. buffer_slot: 0-2.
    // Source: Crystal getmoney opcode → text display path.
    int prepare_money_text(lua_State* L);
}

// ============================================================================
// Runtime Text Sequence - Semantic representation for rendering
// Shared between frontend and runtime to preserve LINE/CONT/PARA distinctions
// ============================================================================

// Semantic text operations (mirrors crystal::TextOp but runtime-owned)
enum class RuntimeTextOp : uint8_t {
    Text,       // Printable text run
    Line,       // Move to line 2, no wait
    Next,       // Clear box, continue (no wait)
    Para,       // Wait → clear → continue
    Cont,       // Wait → scroll → continue  
    Scroll,     // Scroll without wait
    Done,       // End text processing
    Prompt,     // Show cursor, wait, end
    Ram,        // TX_RAM: display RAM contents {addr}
    Bcd,        // TX_BCD: display BCD number {addr, flags}
    Decimal,    // TX_DECIMAL: display decimal {addr, param}
    Buffer,     // TX_STRINGBUFFER: display string buffer {id}
    Far,        // TX_FAR: far text pointer {addr, bank}
    Move,       // TX_MOVE: move cursor {pos}
    Box,        // TX_BOX: draw text box {addr, w, h}
    Day,        // TX_DAY: day of week
    Low,        // TX_LOW: move to bottom
    WaitButton, // TX_PROMPTBUTTON in text
    TxScroll,   // TX_SCROLL
    Sound,      // Sound effect in text
    Raw,        // Raw bytes
    Asm,        // Inline ASM
    Unsupported,// Op received but not yet implemented by runtime
};

// Single element in a runtime text sequence
struct RuntimeTextElement {
    RuntimeTextOp op;
    std::string text;      // For RuntimeTextOp::Text
    std::string op_name;   // For RuntimeTextOp::Unsupported: the original op string
    uint32_t addr = 0;     // For Ram/Bcd/Decimal/Far/Move/Box: address/pointer
    uint8_t param = 0;     // For Bcd: flags; Decimal: bytes|digits; Buffer: id; Box: w; Far: bank
    uint8_t param2 = 0;    // For Box: h
    
    static RuntimeTextElement make_text(const std::string& s) { return {RuntimeTextOp::Text, s}; }
    static RuntimeTextElement make_line() { return {RuntimeTextOp::Line}; }
    static RuntimeTextElement make_next() { return {RuntimeTextOp::Next}; }
    static RuntimeTextElement make_para() { return {RuntimeTextOp::Para}; }
    static RuntimeTextElement make_cont() { return {RuntimeTextOp::Cont}; }
    static RuntimeTextElement make_scroll() { return {RuntimeTextOp::Scroll}; }
    static RuntimeTextElement make_done() { return {RuntimeTextOp::Done}; }
    static RuntimeTextElement make_prompt() { return {RuntimeTextOp::Prompt}; }
    static RuntimeTextElement make_unsupported(const std::string& name) {
        RuntimeTextElement e;
        e.op = RuntimeTextOp::Unsupported;
        e.op_name = name;
        return e;
    }
};

// Complete runtime text sequence
struct RuntimeTextSequence {
    std::vector<RuntimeTextElement> elements;
    
    bool empty() const { return elements.empty(); }
    
    // Debug string for logging
    std::string debug_string() const;
};

// ============================================================================
// UI/Dialog API - ctx.ui
// ============================================================================
namespace ui_api {
    // Callback types for visible rendering
    using TextCallback = std::function<void(const std::string&)>;
    using VoidCallback = std::function<void()>;
    using TextSequenceCallback = std::function<void(const RuntimeTextSequence&)>;
    
    // ctx.ui:open_text()
    // Opens a text box UI
    int open_text(lua_State* L);
    
    // ctx.ui:close_text()
    // Closes the text box UI
    int close_text(lua_State* L);
    
    // ctx.ui:text(message) - legacy string-based API
    // Shows text box, yields until dismissed
    int text(lua_State* L);
    
    // ctx.ui:text_sequence(seq_table) - semantic API
    // Receives semantic text sequence with preserved LINE/CONT/PARA distinctions
    // seq_table format: { {op="text", text="..."}, {op="line"}, {op="para"}, ... }
    int text_sequence(lua_State* L);
    
    // ctx.ui:text_scroll(message)
    // Scrolling text, yields until complete
    int text_scroll(lua_State* L);
    
    // ctx.ui:choice(options_table) -> selected_index
    // Shows choice menu, yields until selected
    int choice(lua_State* L);
    
    // ctx.ui:yes_no() -> bool
    // Shorthand for yes/no choice
    int yes_no(lua_State* L);
    
    // ctx.ui:show_map_name()
    int show_map_name(lua_State* L);

    // ctx.ui:inline_prompt_button()
    // Non-terminating in-stream input gate (TX_PROMPT_BUTTON semantics).
    // Shows blinking cursor, waits for A/B, then text continues.
    // DISTINCT from prompt() which terminates the text stream.
    // Yields "wait_button" — caller must resume to continue.
    int inline_prompt_button(lua_State* L);

    // ctx.ui:pause_text(frames)
    // Timed pause between text elements (TX_PAUSE semantics).
    // Waits `frames` real-time frames, skippable if button held.
    // frames = 30 for all vanilla Crystal uses (~0.5s at 60fps).
    int pause_text(lua_State* L);
    
    // ctx.ui:fade_out(speed?)
    // Yields until fade complete
    int fade_out(lua_State* L);
    
    // ctx.ui:fade_in(speed?)
    // Yields until fade complete
    int fade_in(lua_State* L);
}

// ============================================================================
// Audio API - ctx.audio
// ============================================================================
namespace audio_api {
    // ctx.audio:play_music(music_id)
    int play_music(lua_State* L);
    
    // ctx.audio:stop_music()
    int stop_music(lua_State* L);
    
    // ctx.audio:play_sfx(sfx_id)
    int play_sfx(lua_State* L);
    
    // ctx.audio:play_cry(species_id)
    int play_cry(lua_State* L);
    
    // ctx.audio:wait_sfx()
    // Yields until current SFX completes
    int wait_sfx(lua_State* L);
}

// ============================================================================
// Flags/Variables API - ctx.flags
// ============================================================================
namespace flag_api {
    // ctx.flags:set(flag_id)
    int set(lua_State* L);
    
    // ctx.flags:clear(flag_id)
    int clear(lua_State* L);
    
    // ctx.flags:check(flag_id) -> bool
    int check(lua_State* L);
    
    // ctx.flags:set_var(var_id, value)
    int set_var(lua_State* L);
    
    // ctx.flags:get_var(var_id) -> number
    int get_var(lua_State* L);
    
    // ctx.flags:add_var(var_id, delta)
    int add_var(lua_State* L);
    
    // Test helpers - operate on specific runtime's stub state
    void reset_test_state(LuaRuntime* runtime);
    void set_test_flag(LuaRuntime* runtime, int flag_id, bool value);
    bool get_test_flag(LuaRuntime* runtime, int flag_id);
    const std::vector<std::pair<std::string, int>>& get_flag_calls(LuaRuntime* runtime);
}

// ============================================================================
// Text Buffer API - ctx.text_buf
// Transient per-script text argument slots, keyed by "strbuf<N>_<type>".
// Backed by StubServices::text_buffers (not persisted to GameState).
// ============================================================================
namespace text_buf_api {
    // ctx.text_buf:set(key, value) -- write a text-buffer slot
    int set(lua_State* L);
    // ctx.text_buf:get(key) -> int -- read a text-buffer slot (0 if absent)
    int get(lua_State* L);
}

// ============================================================================
// Time API - ctx.time
// ============================================================================
namespace time_api {
    // ctx.time:hour() -> 0-23
    int hour(lua_State* L);
    
    // ctx.time:minute() -> 0-59
    int minute(lua_State* L);
    
    // ctx.time:day_of_week() -> 0-6 (Sunday=0)
    int day_of_week(lua_State* L);
    
    // ctx.time:time_of_day() -> "morning", "day", "night"
    int time_of_day(lua_State* L);
    
    // ctx.time:is_morning() -> bool
    int is_morning(lua_State* L);
    
    // ctx.time:is_day() -> bool
    int is_day(lua_State* L);
    
    // ctx.time:is_night() -> bool
    int is_night(lua_State* L);
}

// ============================================================================
// Utility API - ctx.util
// ============================================================================
namespace util_api {
    // ctx.util:wait(frames)
    // Yields for N frames
    int wait_frames(lua_State* L);
    
    // ctx.util:wait_seconds(seconds)
    // Yields for N seconds
    int wait_seconds(lua_State* L);
    
    // ctx.util:random(min, max) -> number
    int random(lua_State* L);
    
    // ctx.util:random_chance(percent) -> bool
    int random_chance(lua_State* L);
}

// ============================================================================
// Field Move API - ctx.field
// ============================================================================
// Implements ScriptExecutionContext lifecycle for field moves
// Context is owned by LuaRuntime instance, NOT process-global state

namespace field_api {
    // Capability check results - re-exported from StubServices
    using StrengthResult = StubStrengthResult;
    using RockSmashResult = StubRockSmashResult;
    
    // Test configuration - operate on specific runtime's stub state
    void set_strength_check_result(LuaRuntime* runtime, StrengthResult result, uint8_t slot = 0, SpeciesId species = 0);
    void set_rock_smash_check_result(LuaRuntime* runtime, RockSmashResult result, uint8_t slot = 0, SpeciesId species = 0);
    void set_encounter_result(LuaRuntime* runtime, bool has_encounter, SpeciesId species = 0, uint8_t level = 0);
    
    // Query current context state - operate on specific runtime's context
    bool has_selected_actor(LuaRuntime* runtime);
    bool has_pending_encounter(LuaRuntime* runtime);
    SpeciesId get_selected_actor_species(LuaRuntime* runtime);
    SpeciesId get_pending_encounter_species(LuaRuntime* runtime);
    uint8_t get_pending_encounter_level(LuaRuntime* runtime);
    bool is_strength_active(LuaRuntime* runtime);
    
    // Lua API functions
    int check_strength(lua_State* L);
    int activate_strength(lua_State* L);
    int check_rock_smash(lua_State* L);
    int prepare_nickname(lua_State* L);
    int try_rock_encounter(lua_State* L);
    int read_encounter_species(lua_State* L);
    int load_pending_encounter(lua_State* L);
    int play_actor_cry(lua_State* L);
    int clear_context(lua_State* L);
}

// ============================================================================
// Game API - ctx.game
// Miscellaneous game operations: behaviors, scenes, state, std scripts, etc.
// ============================================================================
namespace game_api {
    int behavior(lua_State* L);           // Sem_GameSpecificEvent dispatch
    int set_scene(lua_State* L);          // Sem_SetScene
    int check_scene(lua_State* L);        // Sem_CheckScene → sets result
    int set_map_scene(lua_State* L);      // Sem_SetMapScene(map, scene)
    int check_map_scene(lua_State* L);    // Sem_CheckMapScene(map) → sets result
    int check_link_mode(lua_State* L);    // Sem_CheckLinkMode → sets result
    int check_save(lua_State* L);         // Sem_CheckSave → sets result
    int hall_of_fame(lua_State* L);       // Sem_HallOfFame
    int credits(lua_State* L);            // Sem_Credits
    int register_dex_entry(lua_State* L); // Sem_RegisterNewDexEntry(species)
    int find_party_mon(lua_State* L);     // Sem_FindPartyMon(species, require_ot) → result
    int check_pokerus(lua_State* L);      // Sem_CheckPartyPokerus → result
    int call_std(lua_State* L);           // Sem_CallStd(std_id, name)
    int jump_std(lua_State* L);           // Sem_JumpStd(std_id, name)
    int wild_on(lua_State* L);            // Sem_WildOn
    int wild_off(lua_State* L);           // Sem_WildOff
    int reload_map(lua_State* L);         // Sem_ReloadMap
    int refresh_map(lua_State* L);        // Sem_RefreshMap
    int reanchor_map(lua_State* L);       // Sem_ReanchorMap
    int new_load_map(lua_State* L);       // Sem_NewLoadMap(method_byte)
    int change_block(lua_State* L);       // Sem_ChangeBlock(x, y, block)
    int set_blackout_point(lua_State* L); // Sem_SetBlackoutPoint(map)
    int catch_tutorial(lua_State* L);     // Sem_CatchTutorial(type)
    int deactivate_facing(lua_State* L);  // Sem_DeactivateFacing(frames)
    int sync_palettes(lua_State* L);      // Sem_SyncPalettes
    int set_player_palette(lua_State* L); // Sem_SetPlayerPalette(selector)
    int describe_decoration(lua_State* L);// Sem_DescribeDecoration(id)
    int set_daylight_saving(lua_State* L);// Sem_SetDaylightSaving(enabled)
    int give_poke_mail(lua_State* L);     // Sem_GivePokeMail(mail_id)
    int check_poke_mail(lua_State* L);    // Sem_CheckPokeMail(mail_id) → result
    int check_warp(lua_State* L);         // Sem_CheckWarp
    int pocket_full_notify(lua_State* L); // Sem_PocketFullNotify
    int show_balance_overlay(lua_State* L);// Sem_ShowBalanceOverlay(content)
    int play_radio(lua_State* L);         // Sem_PlayRadio(channel)
    int write_cmd_queue(lua_State* L);    // Sem_WriteCmdQueue(addr)
    int delete_cmd_queue(lua_State* L);   // Sem_DeleteCmdQueue(type)
    int modify_warp(lua_State* L);        // Sem_ModifyWarp(warp_id, map)
    int read_state_var(lua_State* L);     // Sem_ReadStateVar(id) → result
    int write_state_var(lua_State* L);    // Sem_WriteStateVar(id)
    int set_state_var(lua_State* L);      // Sem_SetStateVar(id, value)
}

} // namespace enginemon
