// engine/scripting/lua_runtime.cpp
// Lua orchestration layer implementation
// Same system for generated Crystal scripts and user mod scripts

#include "engine/scripting/lua_runtime.hpp"
#include "engine/scripting/api_bindings.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>  // For std::ceil in WaitSeconds tick conversion
#include <vector>
#include <algorithm>

namespace enginemon {

LuaRuntime::LuaRuntime() {
    // Create main Lua state
    L_ = luaL_newstate();
    if (!L_) {
        throw std::runtime_error("Failed to create Lua state");
    }
    
    // Open standard libraries (safe subset)
    luaL_requiref(L_, "_G", luaopen_base, 1);
    luaL_requiref(L_, "coroutine", luaopen_coroutine, 1);
    luaL_requiref(L_, "table", luaopen_table, 1);
    luaL_requiref(L_, "string", luaopen_string, 1);
    luaL_requiref(L_, "math", luaopen_math, 1);
    lua_pop(L_, 5);
    
    // Set up error handler in registry
    lua_pushcfunction(L_, lua_error_handler);
    lua_setfield(L_, LUA_REGISTRYINDEX, "enginemon_error_handler");
    
    // Create ctx table even without GameContext for basic functionality
    bind_api();
}

LuaRuntime::~LuaRuntime() {
    if (L_) {
        lua_close(L_);
        L_ = nullptr;
    }
}

void LuaRuntime::initialize(GameContext& ctx) {
    ctx_ = &ctx;
    bind_api();
}

int LuaRuntime::lua_error_handler(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    
    // Get debug.traceback for stack trace
    luaL_traceback(L, L, msg, 1);
    return 1;
}

void LuaRuntime::bind_api() {
    // Create the ctx table that scripts use
    lua_newtable(L_);
    
    // Bind each subsystem API
    bind_ui_api();
    bind_world_api();
    bind_battle_api();
    bind_party_api();
    bind_inventory_api();
    bind_audio_api();
    bind_flag_api();
    bind_text_buf_api();
    bind_time_api();
    bind_util_api();
    bind_field_api();
    bind_game_api();
    
    // Store ctx as global
    lua_setglobal(L_, "ctx");
    
    // Also make ctx available in registry for C++ API functions
    lua_getglobal(L_, "ctx");
    lua_setfield(L_, LUA_REGISTRYINDEX, "enginemon_ctx");
}

void LuaRuntime::bind_ui_api() {
    lua_newtable(L_);  // ctx.ui table
    
    // Store pointer to runtime for callbacks
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, -2, "_runtime");
    
    // Register methods
    lua_pushcfunction(L_, ui_api::open_text);
    lua_setfield(L_, -2, "open_text");
    
    lua_pushcfunction(L_, ui_api::close_text);
    lua_setfield(L_, -2, "close_text");
    
    lua_pushcfunction(L_, ui_api::text);
    lua_setfield(L_, -2, "text");
    
    // Semantic text sequence API - preserves LINE/CONT/PARA distinctions
    lua_pushcfunction(L_, ui_api::text_sequence);
    lua_setfield(L_, -2, "text_sequence");
    
    lua_pushcfunction(L_, ui_api::yes_no);
    lua_setfield(L_, -2, "yes_no");
    
    lua_pushcfunction(L_, ui_api::choice);
    lua_setfield(L_, -2, "choice");
    
    lua_pushcfunction(L_, ui_api::fade_out);
    lua_setfield(L_, -2, "fade_out");
    
    lua_pushcfunction(L_, ui_api::fade_in);
    lua_setfield(L_, -2, "fade_in");
    
    lua_pushcfunction(L_, ui_api::show_map_name);
    lua_setfield(L_, -2, "show_map_name");

    // In-stream input gate (TX_PROMPT_BUTTON â€” non-terminating, text continues after)
    lua_pushcfunction(L_, ui_api::inline_prompt_button);
    lua_setfield(L_, -2, "inline_prompt_button");

    // Timed pause in text stream (TX_PAUSE â€” frames delay, button-skippable)
    lua_pushcfunction(L_, ui_api::pause_text);
    lua_setfield(L_, -2, "pause_text");

    lua_setfield(L_, -2, "ui");  // Set as ctx.ui
}

void LuaRuntime::bind_world_api() {
    lua_newtable(L_);  // ctx.world table
    
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, -2, "_runtime");
    
    lua_pushcfunction(L_, world_api::move_actor_dispatch);
    lua_setfield(L_, -2, "move_actor");
    
    lua_pushcfunction(L_, world_api::face_actor);
    lua_setfield(L_, -2, "face_actor");
    
    lua_pushcfunction(L_, world_api::get_player_pos);
    lua_setfield(L_, -2, "get_player_pos");
    
    lua_pushcfunction(L_, world_api::get_actor_pos);
    lua_setfield(L_, -2, "get_actor_pos");
    
    lua_pushcfunction(L_, world_api::get_actor_facing);
    lua_setfield(L_, -2, "get_actor_facing");
    
    lua_pushcfunction(L_, world_api::teleport_player);
    lua_setfield(L_, -2, "teleport_player");
    
    lua_pushcfunction(L_, world_api::warp);
    lua_setfield(L_, -2, "warp");
    
    lua_pushcfunction(L_, world_api::warp_to_spawn);
    lua_setfield(L_, -2, "warp_to_spawn");
    
    lua_pushcfunction(L_, world_api::show_npc);
    lua_setfield(L_, -2, "show_npc");
    
    lua_pushcfunction(L_, world_api::hide_npc);
    lua_setfield(L_, -2, "hide_npc");
    
    lua_pushcfunction(L_, world_api::npc_visible);
    lua_setfield(L_, -2, "npc_visible");
    
    lua_pushcfunction(L_, world_api::current_map);
    lua_setfield(L_, -2, "current_map");
    
    lua_pushcfunction(L_, world_api::map_name);
    lua_setfield(L_, -2, "map_name");
    
    // face_player is called from scripts as ctx.world:face_player()
    lua_pushcfunction(L_, world_api::face_player);
    lua_setfield(L_, -2, "face_player");
    
    lua_pushcfunction(L_, world_api::apply_movement);
    lua_setfield(L_, -2, "apply_movement");

    lua_pushcfunction(L_, world_api::set_variable_sprite);
    lua_setfield(L_, -2, "set_variable_sprite");

    lua_pushcfunction(L_, world_api::set_daycare_species);
    lua_setfield(L_, -2, "set_daycare_species");
    
    lua_setfield(L_, -2, "world");  // Set as ctx.world
}

void LuaRuntime::bind_battle_api() {
    lua_newtable(L_);
    
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, -2, "_runtime");
    
    lua_pushcfunction(L_, battle_api::start_wild);
    lua_setfield(L_, -2, "start_wild");
    
    lua_pushcfunction(L_, battle_api::start_trainer);
    lua_setfield(L_, -2, "start_trainer");
    
    // Result constants
    lua_pushinteger(L_, 0);
    lua_setfield(L_, -2, "RESULT_WIN");
    lua_pushinteger(L_, 1);
    lua_setfield(L_, -2, "RESULT_LOSE");
    lua_pushinteger(L_, 2);
    lua_setfield(L_, -2, "RESULT_RUN");
    lua_pushinteger(L_, 3);
    lua_setfield(L_, -2, "RESULT_CAUGHT");
    
    lua_setfield(L_, -2, "battle");
}

void LuaRuntime::bind_party_api() {
    lua_newtable(L_);
    
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, -2, "_runtime");
    
    lua_pushcfunction(L_, party_api::count);
    lua_setfield(L_, -2, "count");
    
    lua_pushcfunction(L_, party_api::get);
    lua_setfield(L_, -2, "get");
    
    lua_pushcfunction(L_, party_api::has_species);
    lua_setfield(L_, -2, "has_species");
    
    lua_pushcfunction(L_, party_api::heal_all);
    lua_setfield(L_, -2, "heal_all");
    
    lua_pushcfunction(L_, party_api::add_pokemon);
    lua_setfield(L_, -2, "add_pokemon");
    
    lua_pushcfunction(L_, party_api::has_move);
    lua_setfield(L_, -2, "has_move");
    
    lua_pushcfunction(L_, party_api::can_use_hm);
    lua_setfield(L_, -2, "can_use_hm");
    
    lua_setfield(L_, -2, "party");
}

void LuaRuntime::bind_inventory_api() {
    lua_newtable(L_);
    
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, -2, "_runtime");
    
    lua_pushcfunction(L_, inventory_api::give);
    lua_setfield(L_, -2, "give");
    
    lua_pushcfunction(L_, inventory_api::take);
    lua_setfield(L_, -2, "take");
    
    lua_pushcfunction(L_, inventory_api::has);
    lua_setfield(L_, -2, "has");
    
    lua_pushcfunction(L_, inventory_api::count);
    lua_setfield(L_, -2, "count");
    
    lua_pushcfunction(L_, inventory_api::give_money);
    lua_setfield(L_, -2, "give_money");
    
    lua_pushcfunction(L_, inventory_api::take_money);
    lua_setfield(L_, -2, "take_money");
    
    lua_pushcfunction(L_, inventory_api::has_money);
    lua_setfield(L_, -2, "has_money");
    
    lua_pushcfunction(L_, inventory_api::money);
    lua_setfield(L_, -2, "money");

    lua_pushcfunction(L_, inventory_api::prepare_money_text);
    lua_setfield(L_, -2, "prepare_money_text");
    
    lua_setfield(L_, -2, "inventory");
}

void LuaRuntime::bind_audio_api() {
    lua_newtable(L_);
    
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, -2, "_runtime");
    
    lua_pushcfunction(L_, audio_api::play_music);
    lua_setfield(L_, -2, "play_music");
    
    lua_pushcfunction(L_, audio_api::stop_music);
    lua_setfield(L_, -2, "stop_music");
    
    lua_pushcfunction(L_, audio_api::play_sfx);
    lua_setfield(L_, -2, "play_sfx");
    
    lua_pushcfunction(L_, audio_api::play_cry);
    lua_setfield(L_, -2, "play_cry");
    
    lua_pushcfunction(L_, audio_api::wait_sfx);
    lua_setfield(L_, -2, "wait_sfx");
    
    lua_setfield(L_, -2, "audio");
}

void LuaRuntime::bind_flag_api() {
    lua_newtable(L_);
    
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, -2, "_runtime");
    
    lua_pushcfunction(L_, flag_api::set);
    lua_setfield(L_, -2, "set");
    
    lua_pushcfunction(L_, flag_api::clear);
    lua_setfield(L_, -2, "clear");
    
    lua_pushcfunction(L_, flag_api::check);
    lua_setfield(L_, -2, "check");
    
    lua_pushcfunction(L_, flag_api::set_var);
    lua_setfield(L_, -2, "set_var");
    
    lua_pushcfunction(L_, flag_api::get_var);
    lua_setfield(L_, -2, "get_var");
    
    lua_pushcfunction(L_, flag_api::add_var);
    lua_setfield(L_, -2, "add_var");
    
    lua_setfield(L_, -2, "flags");
}

void LuaRuntime::bind_text_buf_api() {
    lua_newtable(L_);

    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, -2, "_runtime");

    lua_pushcfunction(L_, text_buf_api::set);
    lua_setfield(L_, -2, "set");

    lua_pushcfunction(L_, text_buf_api::get);
    lua_setfield(L_, -2, "get");

    lua_setfield(L_, -2, "text_buf");
}

void LuaRuntime::bind_time_api() {
    lua_newtable(L_);
    
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, -2, "_runtime");
    
    lua_pushcfunction(L_, time_api::hour);
    lua_setfield(L_, -2, "hour");
    
    lua_pushcfunction(L_, time_api::minute);
    lua_setfield(L_, -2, "minute");
    
    lua_pushcfunction(L_, time_api::day_of_week);
    lua_setfield(L_, -2, "day_of_week");
    
    lua_pushcfunction(L_, time_api::time_of_day);
    lua_setfield(L_, -2, "time_of_day");
    
    lua_pushcfunction(L_, time_api::is_morning);
    lua_setfield(L_, -2, "is_morning");
    
    lua_pushcfunction(L_, time_api::is_day);
    lua_setfield(L_, -2, "is_day");
    
    lua_pushcfunction(L_, time_api::is_night);
    lua_setfield(L_, -2, "is_night");
    
    lua_setfield(L_, -2, "time");
}

void LuaRuntime::bind_util_api() {
    lua_newtable(L_);
    
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, -2, "_runtime");
    
    lua_pushcfunction(L_, util_api::wait_frames);
    lua_setfield(L_, -2, "wait_frames");
    
    lua_pushcfunction(L_, util_api::wait_seconds);
    lua_setfield(L_, -2, "wait_seconds");
    
    lua_pushcfunction(L_, util_api::random);
    lua_setfield(L_, -2, "random");
    
    lua_pushcfunction(L_, util_api::random_chance);
    lua_setfield(L_, -2, "random_chance");
    
    lua_setfield(L_, -2, "util");
}

void LuaRuntime::bind_field_api() {
    lua_newtable(L_);  // ctx.field table
    
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, -2, "_runtime");
    
    // Field move capability checks
    lua_pushcfunction(L_, field_api::check_strength);
    lua_setfield(L_, -2, "check_strength");
    
    lua_pushcfunction(L_, field_api::activate_strength);
    lua_setfield(L_, -2, "activate_strength");
    
    lua_pushcfunction(L_, field_api::check_rock_smash);
    lua_setfield(L_, -2, "check_rock_smash");
    
    // Field actor context operations
    lua_pushcfunction(L_, field_api::prepare_nickname);
    lua_setfield(L_, -2, "prepare_nickname");
    
    lua_pushcfunction(L_, field_api::play_actor_cry);
    lua_setfield(L_, -2, "play_actor_cry");
    
    // Field encounter operations
    lua_pushcfunction(L_, field_api::try_rock_encounter);
    lua_setfield(L_, -2, "try_rock_encounter");
    
    lua_pushcfunction(L_, field_api::read_encounter_species);
    lua_setfield(L_, -2, "read_encounter_species");
    
    lua_pushcfunction(L_, field_api::load_pending_encounter);
    lua_setfield(L_, -2, "load_pending_encounter");
    
    // Context management
    lua_pushcfunction(L_, field_api::clear_context);
    lua_setfield(L_, -2, "clear_context");
    
    // Result constants for capability checks
    lua_pushinteger(L_, 0);
    lua_setfield(L_, -2, "STRENGTH_AVAILABLE");
    lua_pushinteger(L_, 1);
    lua_setfield(L_, -2, "STRENGTH_UNAVAILABLE");
    lua_pushinteger(L_, 2);
    lua_setfield(L_, -2, "STRENGTH_ALREADY_ACTIVE");
    
    lua_pushinteger(L_, 0);
    lua_setfield(L_, -2, "ROCK_SMASH_AVAILABLE");
    lua_pushinteger(L_, 1);
    lua_setfield(L_, -2, "ROCK_SMASH_UNAVAILABLE");
    
    lua_setfield(L_, -2, "field");  // Set as ctx.field
}

void LuaRuntime::bind_game_api() {
    lua_newtable(L_);  // ctx.game table

    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, -2, "_runtime");

    // Game behavior dispatch (Sem_GameSpecificEvent)
    lua_pushcfunction(L_, game_api::behavior);
    lua_setfield(L_, -2, "behavior");

    // Scene management
    lua_pushcfunction(L_, game_api::set_scene);
    lua_setfield(L_, -2, "set_scene");

    lua_pushcfunction(L_, game_api::check_scene);
    lua_setfield(L_, -2, "check_scene");

    lua_pushcfunction(L_, game_api::set_map_scene);
    lua_setfield(L_, -2, "set_map_scene");

    lua_pushcfunction(L_, game_api::check_map_scene);
    lua_setfield(L_, -2, "check_map_scene");

    // State queries
    lua_pushcfunction(L_, game_api::check_link_mode);
    lua_setfield(L_, -2, "check_link_mode");

    lua_pushcfunction(L_, game_api::check_save);
    lua_setfield(L_, -2, "check_save");

    // End-game sequences
    lua_pushcfunction(L_, game_api::hall_of_fame);
    lua_setfield(L_, -2, "hall_of_fame");

    lua_pushcfunction(L_, game_api::credits);
    lua_setfield(L_, -2, "credits");

    // Party/Dex
    lua_pushcfunction(L_, game_api::register_dex_entry);
    lua_setfield(L_, -2, "register_dex_entry");

    lua_pushcfunction(L_, game_api::find_party_mon);
    lua_setfield(L_, -2, "find_party_mon");

    lua_pushcfunction(L_, game_api::check_pokerus);
    lua_setfield(L_, -2, "check_pokerus");

    // Standard scripts
    lua_pushcfunction(L_, game_api::call_std);
    lua_setfield(L_, -2, "call_std");

    lua_pushcfunction(L_, game_api::jump_std);
    lua_setfield(L_, -2, "jump_std");

    // Wild encounters
    lua_pushcfunction(L_, game_api::wild_on);
    lua_setfield(L_, -2, "wild_on");

    lua_pushcfunction(L_, game_api::wild_off);
    lua_setfield(L_, -2, "wild_off");

    // Misc map ops
    lua_pushcfunction(L_, game_api::reload_map);
    lua_setfield(L_, -2, "reload_map");

    lua_pushcfunction(L_, game_api::refresh_map);
    lua_setfield(L_, -2, "refresh_map");

    lua_pushcfunction(L_, game_api::reanchor_map);
    lua_setfield(L_, -2, "reanchor_map");

    lua_pushcfunction(L_, game_api::new_load_map);
    lua_setfield(L_, -2, "new_load_map");

    lua_pushcfunction(L_, game_api::change_block);
    lua_setfield(L_, -2, "change_block");

    lua_pushcfunction(L_, game_api::set_blackout_point);
    lua_setfield(L_, -2, "set_blackout_point");

    lua_pushcfunction(L_, game_api::catch_tutorial);
    lua_setfield(L_, -2, "catch_tutorial");

    lua_pushcfunction(L_, game_api::deactivate_facing);
    lua_setfield(L_, -2, "deactivate_facing");

    lua_pushcfunction(L_, game_api::sync_palettes);
    lua_setfield(L_, -2, "sync_palettes");

    lua_pushcfunction(L_, game_api::set_player_palette);
    lua_setfield(L_, -2, "set_player_palette");

    lua_pushcfunction(L_, game_api::describe_decoration);
    lua_setfield(L_, -2, "describe_decoration");

    lua_pushcfunction(L_, game_api::set_daylight_saving);
    lua_setfield(L_, -2, "set_daylight_saving");

    lua_pushcfunction(L_, game_api::give_poke_mail);
    lua_setfield(L_, -2, "give_poke_mail");

    lua_pushcfunction(L_, game_api::check_poke_mail);
    lua_setfield(L_, -2, "check_poke_mail");

    lua_pushcfunction(L_, game_api::check_warp);
    lua_setfield(L_, -2, "check_warp");

    lua_pushcfunction(L_, game_api::pocket_full_notify);
    lua_setfield(L_, -2, "pocket_full_notify");

    lua_pushcfunction(L_, game_api::show_balance_overlay);
    lua_setfield(L_, -2, "show_balance_overlay");

    lua_pushcfunction(L_, game_api::play_radio);
    lua_setfield(L_, -2, "play_radio");

    lua_pushcfunction(L_, game_api::write_cmd_queue);
    lua_setfield(L_, -2, "write_cmd_queue");

    lua_pushcfunction(L_, game_api::delete_cmd_queue);
    lua_setfield(L_, -2, "delete_cmd_queue");

    lua_pushcfunction(L_, game_api::modify_warp);
    lua_setfield(L_, -2, "modify_warp");

    lua_pushcfunction(L_, game_api::read_state_var);
    lua_setfield(L_, -2, "read_state_var");

    lua_pushcfunction(L_, game_api::write_state_var);
    lua_setfield(L_, -2, "write_state_var");

    lua_pushcfunction(L_, game_api::set_state_var);
    lua_setfield(L_, -2, "set_state_var");

    lua_setfield(L_, -2, "game");  // Set as ctx.game
}

void LuaRuntime::load_script_file(const std::filesystem::path& path) {
    if (luaL_dofile(L_, path.string().c_str()) != LUA_OK) {
        std::string error = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        throw std::runtime_error("Lua load error: " + error);
    }
}

void LuaRuntime::load_script_directory(const std::filesystem::path& dir) {
    // Collect all .lua paths first, then sort lexicographically before executing.
    // std::filesystem::directory_iterator order is filesystem-implementation-defined
    // and non-reproducible across OS versions and filesystem types.
    // Sorting by path string guarantees identical load order regardless of
    // directory entry creation order.
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".lua") {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& p : paths) {
        load_script_file(p);
    }
}

void LuaRuntime::execute_string(const std::string& code, const std::string& name) {
    // Load the code
    if (luaL_loadbuffer(L_, code.c_str(), code.size(), name.c_str()) != LUA_OK) {
        std::string error = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        throw std::runtime_error("Lua syntax error: " + error);
    }
    
    // Record stack top before call
    int top_before = lua_gettop(L_) - 1;  // -1 for the function we just loaded
    
    // Execute it
    if (lua_pcall(L_, 0, LUA_MULTRET, 0) != LUA_OK) {
        std::string error = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        throw std::runtime_error("Lua runtime error: " + error);
    }
    
    // Pop any return values to keep stack clean
    int nresults = lua_gettop(L_) - top_before;
    if (nresults > 0) {
        lua_pop(L_, nresults);
    }
}

uint32_t LuaRuntime::start_script(const std::string& script_name) {
    // Get the script table
    lua_getglobal(L_, script_name.c_str());
    if (!lua_istable(L_, -1)) {
        lua_pop(L_, 1);
        throw std::runtime_error("Script not found: " + script_name);
    }
    
    // Get script.main function
    lua_getfield(L_, -1, "main");
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 2);
        throw std::runtime_error("Script has no main function: " + script_name);
    }
    
    // Create a new thread (coroutine)
    lua_State* thread = lua_newthread(L_);  // Pushes thread onto L_
    
    // Store thread in registry using luaL_ref for proper GC protection
    // (avoids collision with Lua's reserved integer keys)
    lua_pushvalue(L_, -1);  // Copy thread for ref
    int ref = luaL_ref(L_, LUA_REGISTRYINDEX);  // registry[ref] = thread, pops copy
    
    // Move the main function from L_ to thread
    // Stack on L_: [script_table, main_func, thread]
    lua_pushvalue(L_, -2);  // Copy main function
    lua_xmove(L_, thread, 1);  // Move copy to thread
    
    // Pop thread, main function, and script table from L_
    lua_pop(L_, 3);
    
    // Create coroutine entry
    uint32_t id = next_coroutine_id_++;
    ScriptCoroutine co;
    co.thread = thread;
    co.state = ScriptState::Ready;
    co.source_file = script_name;
    co.registry_ref = ref;  // Store ref for cleanup
    coroutines_[id] = co;
    
    // Start execution (first resume - pass ctx as argument)
    resume_first(id);
    
    return id;
}

uint32_t LuaRuntime::start_script(ScriptId id) {
    // TODO: Look up script by ID in game data
    return start_script("script_" + std::to_string(id));
}

uint32_t LuaRuntime::start_map_script(MapId map, const std::string& event_name) {
    std::string script_name = "map_" + std::to_string(map) + "_" + event_name;
    return start_script(script_name);
}

void LuaRuntime::resume_first(uint32_t coroutine_id) {
    // First resume: push ctx as the argument to main(ctx)
    auto it = coroutines_.find(coroutine_id);
    if (it == coroutines_.end()) return;
    
    ScriptCoroutine& co = it->second;
    co.state = ScriptState::Running;
    
    // Push ctx as argument to script.main(ctx)
    lua_getglobal(co.thread, "ctx");
    
    int nres = 0;
    int status = lua_resume(co.thread, L_, 1, &nres);
    
    if (status == LUA_OK) {
        co.state = ScriptState::Finished;
        if (nres > 0) lua_pop(co.thread, nres);
        cleanup_coroutine(coroutine_id);
    }
    else if (status == LUA_YIELD) {
        co.state = ScriptState::Yielded;
        
        // Parse yield reason
        if (nres > 0 && lua_isstring(co.thread, -nres)) {
            std::string yield_type = lua_tostring(co.thread, -nres);
            
            if (yield_type == "wait_frames") {
                co.yield_reason = YieldReason::WaitFrames;
                if (nres > 1 && lua_isinteger(co.thread, -nres + 1)) {
                    // WaitFrames: direct integer tick count
                    co.wait_ticks = static_cast<int>(lua_tointeger(co.thread, -nres + 1));
                }
            }
            else if (yield_type == "wait_seconds") {
                co.yield_reason = YieldReason::WaitSeconds;
                if (nres > 1 && lua_isnumber(co.thread, -nres + 1)) {
                    // WaitSeconds: convert to integer ticks using ceil()
                    // ceil() ensures coroutine never resumes earlier than requested
                    // 0.05s â†’ ceil(0.05 * 60) = ceil(3.0) = 3 ticks
                    // 0.1s  â†’ ceil(0.1 * 60)  = ceil(6.0) = 6 ticks
                    // 1.0s  â†’ ceil(1.0 * 60)  = ceil(60.0) = 60 ticks
                    float seconds = static_cast<float>(lua_tonumber(co.thread, -nres + 1));
                    co.wait_ticks = static_cast<int>(std::ceil(seconds * SIM_TICKS_PER_SECOND));
                }
            }
            else if (yield_type == "wait_button") {
                co.yield_reason = YieldReason::Dialog;
            }
            else if (yield_type == "dialog") {
                co.yield_reason = YieldReason::Dialog;
            }
            else if (yield_type == "choice") {
                co.yield_reason = YieldReason::Choice;
            }
            else if (yield_type == "movement") {
                co.yield_reason = YieldReason::Movement;
                // Second yield value is the actor_id (uint32_t) being moved.
                // Stored in yield_data so HeadlessGameLoop::update_script() can
                // call is_actor_moving(actor_id) for the correct actor.
                if (nres > 1 && lua_isinteger(co.thread, -nres + 1)) {
                    co.yield_data = static_cast<uint32_t>(lua_tointeger(co.thread, -nres + 1));
                }
            }
            else if (yield_type == "fade") {
                co.yield_reason = YieldReason::Fade;
            }
            else if (yield_type == "battle") {
                co.yield_reason = YieldReason::Battle;
            }
            else if (yield_type == "warp") {
                co.yield_reason = YieldReason::Warp;
            }
            else {
                co.yield_reason = YieldReason::Custom;
            }
        }
        if (nres > 0) lua_pop(co.thread, nres);
    }
    else {
        co.state = ScriptState::Error;
        
        std::string error = lua_tostring(co.thread, -1);
        std::string traceback;
        luaL_traceback(co.thread, co.thread, error.c_str(), 1);
        traceback = lua_tostring(co.thread, -1);
        lua_pop(co.thread, 2);
        
        if (error_handler_) {
            error_handler_(error, traceback);
        } else {
            std::cerr << "Script error: " << error << "\n" << traceback << "\n";
        }
        cleanup_coroutine(coroutine_id);
    }
}

void LuaRuntime::cleanup_coroutine(uint32_t coroutine_id) {
    auto it = coroutines_.find(coroutine_id);
    if (it == coroutines_.end()) return;
    
    ScriptCoroutine& co = it->second;
    
    // Release Lua registry reference (protects coroutine from GC)
    // The NOREF check ensures we only unref once
    if (co.registry_ref != LUA_NOREF) {
        luaL_unref(L_, LUA_REGISTRYINDEX, co.registry_ref);
        co.registry_ref = LUA_NOREF;
    }
    co.thread = nullptr;
    
    // Record final state before erasing
    // This allows get_state() to return the correct final state
    ScriptState final_state = co.state;
    completed_states_[coroutine_id] = final_state;
    
    // Now safe to erase - Lua resources are released, final state is preserved
    coroutines_.erase(it);
}

void LuaRuntime::resume(uint32_t coroutine_id) {
    auto it = coroutines_.find(coroutine_id);
    if (it == coroutines_.end()) return;
    
    ScriptCoroutine& co = it->second;
    if (co.state == ScriptState::Finished || co.state == ScriptState::Error) {
        return;
    }
    
    co.state = ScriptState::Running;
    
    // Subsequent resume: do NOT push ctx - yield returns no values
    int nres = 0;
    int status = lua_resume(co.thread, L_, 0, &nres);
    
    if (status == LUA_OK) {
        // Script finished
        co.state = ScriptState::Finished;
        if (nres > 0) lua_pop(co.thread, nres);
        cleanup_coroutine(coroutine_id);
    }
    else if (status == LUA_YIELD) {
        // Script yielded
        co.state = ScriptState::Yielded;
        
        // Check what it yielded with
        if (nres > 0 && lua_isstring(co.thread, -nres)) {
            std::string yield_type = lua_tostring(co.thread, -nres);
            
            if (yield_type == "wait_frames") {
                co.yield_reason = YieldReason::WaitFrames;
                if (nres > 1 && lua_isinteger(co.thread, -nres + 1)) {
                    // WaitFrames: direct integer tick count
                    co.wait_ticks = static_cast<int>(lua_tointeger(co.thread, -nres + 1));
                }
            }
            else if (yield_type == "wait_seconds") {
                co.yield_reason = YieldReason::WaitSeconds;
                if (nres > 1 && lua_isnumber(co.thread, -nres + 1)) {
                    // WaitSeconds: convert to integer ticks using ceil()
                    float seconds = static_cast<float>(lua_tonumber(co.thread, -nres + 1));
                    co.wait_ticks = static_cast<int>(std::ceil(seconds * SIM_TICKS_PER_SECOND));
                }
            }
            else if (yield_type == "wait_button") {
                co.yield_reason = YieldReason::Dialog;
            }
            else if (yield_type == "dialog") {
                co.yield_reason = YieldReason::Dialog;
            }
            else if (yield_type == "choice") {
                co.yield_reason = YieldReason::Choice;
            }
            else if (yield_type == "movement") {
                co.yield_reason = YieldReason::Movement;
                // Second yield value is the actor_id (uint32_t) being moved.
                // Stored in yield_data so HeadlessGameLoop::update_script() can
                // call is_actor_moving(actor_id) for the correct actor.
                if (nres > 1 && lua_isinteger(co.thread, -nres + 1)) {
                    co.yield_data = static_cast<uint32_t>(lua_tointeger(co.thread, -nres + 1));
                }
            }
            else if (yield_type == "fade") {
                co.yield_reason = YieldReason::Fade;
            }
            else if (yield_type == "battle") {
                co.yield_reason = YieldReason::Battle;
            }
            else if (yield_type == "warp") {
                co.yield_reason = YieldReason::Warp;
            }
            else {
                co.yield_reason = YieldReason::Custom;
            }
        }
        
        lua_pop(co.thread, nres);
    }
    else {
        // Error
        co.state = ScriptState::Error;
        
        std::string error = lua_tostring(co.thread, -1);
        std::string traceback;
        
        // Get traceback
        luaL_traceback(co.thread, co.thread, error.c_str(), 1);
        traceback = lua_tostring(co.thread, -1);
        
        lua_pop(co.thread, 2);  // Pop error and traceback
        
        if (error_handler_) {
            error_handler_(error, traceback);
        } else {
            std::cerr << "Script error: " << error << "\n" << traceback << "\n";
        }
        cleanup_coroutine(coroutine_id);
    }
}

void LuaRuntime::resume_with_result(uint32_t coroutine_id, int result) {
    auto it = coroutines_.find(coroutine_id);
    if (it == coroutines_.end()) return;
    
    ScriptCoroutine& co = it->second;
    if (co.state != ScriptState::Yielded) return;
    
    // Push result before resuming
    lua_pushinteger(co.thread, result);
    
    co.state = ScriptState::Running;
    
    int nres = 0;
    int status = lua_resume(co.thread, L_, 1, &nres);
    
    // Handle result - MUST use same finalization as resume()
    if (status == LUA_OK) {
        // Script finished - clean up registry ref and active entry
        co.state = ScriptState::Finished;
        if (nres > 0) lua_pop(co.thread, nres);
        cleanup_coroutine(coroutine_id);  // FIX: Was missing!
    }
    else if (status == LUA_YIELD) {
        co.state = ScriptState::Yielded;
        
        // Parse yield reason (same as resume())
        if (nres > 0 && lua_isstring(co.thread, -nres)) {
            std::string yield_type = lua_tostring(co.thread, -nres);
            
            if (yield_type == "wait_frames") {
                co.yield_reason = YieldReason::WaitFrames;
                if (nres > 1 && lua_isinteger(co.thread, -nres + 1)) {
                    // WaitFrames: direct integer tick count
                    co.wait_ticks = static_cast<int>(lua_tointeger(co.thread, -nres + 1));
                }
            }
            else if (yield_type == "wait_seconds") {
                co.yield_reason = YieldReason::WaitSeconds;
                if (nres > 1 && lua_isnumber(co.thread, -nres + 1)) {
                    // WaitSeconds: convert to integer ticks using ceil()
                    float seconds = static_cast<float>(lua_tonumber(co.thread, -nres + 1));
                    co.wait_ticks = static_cast<int>(std::ceil(seconds * SIM_TICKS_PER_SECOND));
                }
            }
            else if (yield_type == "wait_button" || yield_type == "dialog") {
                co.yield_reason = YieldReason::Dialog;
            }
            else if (yield_type == "choice") {
                co.yield_reason = YieldReason::Choice;
            }
            else if (yield_type == "movement") {
                co.yield_reason = YieldReason::Movement;
                // Second yield value is the actor_id (uint32_t) being moved.
                // Stored in yield_data so HeadlessGameLoop::update_script() can
                // call is_actor_moving(actor_id) for the correct actor.
                if (nres > 1 && lua_isinteger(co.thread, -nres + 1)) {
                    co.yield_data = static_cast<uint32_t>(lua_tointeger(co.thread, -nres + 1));
                }
            }
            else if (yield_type == "fade") {
                co.yield_reason = YieldReason::Fade;
            }
            else if (yield_type == "battle") {
                co.yield_reason = YieldReason::Battle;
            }
            else if (yield_type == "warp") {
                co.yield_reason = YieldReason::Warp;
            }
            else {
                co.yield_reason = YieldReason::Custom;
            }
        }
        if (nres > 0) lua_pop(co.thread, nres);
    }
    else {
        // Error - clean up registry ref and active entry
        co.state = ScriptState::Error;
        std::string error = lua_tostring(co.thread, -1);
        std::string traceback;
        luaL_traceback(co.thread, co.thread, error.c_str(), 1);
        traceback = lua_tostring(co.thread, -1);
        lua_pop(co.thread, 2);
        
        if (error_handler_) {
            error_handler_(error, traceback);
        } else {
            std::cerr << "Script error: " << error << "\n" << traceback << "\n";
        }
        cleanup_coroutine(coroutine_id);  // FIX: Was missing!
    }
}

void LuaRuntime::resume_with_error(uint32_t coroutine_id, const std::string& error) {
    auto it = coroutines_.find(coroutine_id);
    if (it == coroutines_.end()) return;
    
    it->second.state = ScriptState::Error;
    
    if (error_handler_) {
        error_handler_(error, "");
    }
    
    // FIX: Clean up registry ref and active entry on error path
    cleanup_coroutine(coroutine_id);
}

std::vector<uint32_t> LuaRuntime::update(float delta_time) {
    // Clear previous update's resumed IDs
    resumed_ids_this_update_.clear();
    
    std::vector<uint32_t> to_resume;
    
    for (auto& [id, co] : coroutines_) {
        if (co.state != ScriptState::Yielded) continue;
        
        switch (co.yield_reason) {
            case YieldReason::WaitFrames:
            case YieldReason::WaitSeconds:
                // Both use unified integer tick counter
                // Decrement and check <= 0 for resume
                co.wait_ticks--;
                if (co.wait_ticks <= 0) {
                    to_resume.push_back(id);
                }
                break;
                
            default:
                // Other yields require explicit resume from engine
                break;
        }
    }
    
    // F6: Deterministic simultaneous wakeup ordering.
    // coroutines_ is an unordered_map â€” iteration order is hash-dependent and
    // non-reproducible across runs/platforms.  Sort to_resume by coroutine ID
    // (ascending) before executing any resume.  IDs are monotonically allocated
    // (next_coroutine_id_++ in start_script()) and are never reused during a
    // session, so ascending ID == ascending creation order.  This gives
    // deterministic behaviour when two coroutines expire on the same tick.
    std::sort(to_resume.begin(), to_resume.end());
    
    for (uint32_t id : to_resume) {
        // Record the identity of this resumed coroutine
        resumed_ids_this_update_.push_back(id);
        resume(id);
    }
    
    return resumed_ids_this_update_;
}

ScriptState LuaRuntime::get_state(uint32_t coroutine_id) const {
    // Check active coroutines first
    auto it = coroutines_.find(coroutine_id);
    if (it != coroutines_.end()) {
        return it->second.state;
    }
    
    // Check completed states (coroutine was cleaned up but state preserved)
    auto cit = completed_states_.find(coroutine_id);
    if (cit != completed_states_.end()) {
        return cit->second;
    }
    
    // Unknown coroutine ID - never existed or was never tracked
    return ScriptState::Error;
}

YieldReason LuaRuntime::get_yield_reason(uint32_t coroutine_id) const {
    auto it = coroutines_.find(coroutine_id);
    if (it == coroutines_.end()) return YieldReason::None;
    return it->second.yield_reason;
}

std::optional<std::any> LuaRuntime::get_yield_data(uint32_t coroutine_id) const {
    auto it = coroutines_.find(coroutine_id);
    if (it == coroutines_.end()) return std::nullopt;
    if (!it->second.yield_data.has_value()) return std::nullopt;
    return it->second.yield_data;
}

bool LuaRuntime::has_active_scripts() const {
    for (const auto& [id, co] : coroutines_) {
        if (co.state == ScriptState::Running || co.state == ScriptState::Yielded) {
            return true;
        }
    }
    return false;
}

void LuaRuntime::cancel(uint32_t coroutine_id) {
    auto it = coroutines_.find(coroutine_id);
    if (it != coroutines_.end()) {
        it->second.state = ScriptState::Finished;
        // cleanup_coroutine will erase from map (Audit 7)
        cleanup_coroutine(coroutine_id);
    }
}

void LuaRuntime::cancel_all() {
    for (auto& [id, co] : coroutines_) {
        co.state = ScriptState::Finished;
        if (co.registry_ref != LUA_NOREF) {
            luaL_unref(L_, LUA_REGISTRYINDEX, co.registry_ref);
            co.registry_ref = LUA_NOREF;
        }
        co.thread = nullptr;
        // Record final state before clearing
        completed_states_[id] = ScriptState::Finished;
    }
    coroutines_.clear();
}

void LuaRuntime::set_error_handler(ErrorHandler handler) {
    error_handler_ = std::move(handler);
}

// Yield helper implementations
void script_wait_frames(lua_State* L, int frames) {
    lua_pushstring(L, "wait_frames");
    lua_pushinteger(L, frames);
    lua_yield(L, 2);
}

void script_wait_seconds(lua_State* L, float seconds) {
    lua_pushstring(L, "wait_seconds");
    lua_pushnumber(L, seconds);
    lua_yield(L, 2);
}

void script_wait_dialog(lua_State* L) {
    lua_pushstring(L, "dialog");
    lua_yield(L, 1);
}

void script_wait_choice(lua_State* L, const std::vector<std::string>& options) {
    lua_pushstring(L, "choice");
    lua_yield(L, 1);
}

void script_wait_movement(lua_State* L) {
    lua_pushstring(L, "movement");
    lua_yield(L, 1);
}

void script_wait_fade(lua_State* L) {
    lua_pushstring(L, "fade");
    lua_yield(L, 1);
}

void script_wait_battle(lua_State* L) {
    lua_pushstring(L, "battle");
    lua_yield(L, 1);
}

} // namespace enginemon
