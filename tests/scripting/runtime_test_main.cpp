// runtime_test_main.cpp — global definitions, run_test, forward decls, main()
#include "engine/world/collision.hpp"
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include <iostream>
#include <string>
#include "scripting/runtime_test_shared.hpp"

// ============================================================
// GLOBAL DEFINITIONS (declared extern in runtime_test_shared.hpp)
// ============================================================
int  g_tests_passed      = 0;
int  g_tests_failed      = 0;
bool g_current_test_failed = false;

const crystal::RomData*           g_rom     = nullptr;
const crystal::ExtractionProfile* g_profile = nullptr;
std::string g_generated_lua;

void run_test(const char* name, void (*test)()) {
    std::cout << "Running " << name << "... ";
    g_current_test_failed = false;
    try {
        test();
        if (g_current_test_failed) {
            std::cout << "FAIL\n"; g_tests_failed++;
        } else {
            std::cout << "PASS\n"; g_tests_passed++;
        }
    } catch (const std::exception& e) {
        std::cout << "EXCEPTION: " << e.what() << "\n"; g_tests_failed++;
    }
}

// Forward declarations — test functions defined in other TUs
void test_active_coroutine_resume_reyield_sets_script_resumed();
void test_active_coroutine_timed_resume_sets_script_resumed();
void test_async_movement_batched_table();
void test_async_movement_completion_callback();
void test_async_movement_fast_forward();
void test_async_movement_final_position();
void test_async_movement_lua_yields();
void test_async_movement_manager_basic();
void test_async_movement_not_instant();
void test_async_movement_position_not_jumped();
void test_async_movement_progresses_over_ticks();
void test_async_movement_resumes_after_complete();
void test_async_movement_with_turns();
void test_bank_utils_bank_to_flat_ptr_at_4000();
void test_bank_utils_bank_to_flat_ptr_at_7fff();
void test_bank_utils_bank_to_flat_ptr_in_rom0();
void test_bank_utils_flat_to_bank_nonzero();
void test_bank_utils_flat_to_bank_one();
void test_bank_utils_flat_to_bank_zero();
void test_bank_utils_getstring_lowering_matches_canonical_helper();
void test_bank_utils_local_ptr_to_flat_getstring_nonzero_bank();
void test_bank_utils_local_ptr_to_flat_sdefer_nonzero_bank();
void test_bank_utils_round_trip();
void test_bank_utils_sdefer_lowering_matches_canonical_helper();
void test_batch1_audio_ops_distinct();
void test_batch1_no_crystal_ids_in_ops();
void test_batch1_screen_fade_variants();
void test_batch1_sprite_ops_distinct();
void test_batch1_sync_palettes_variants();
void test_batch10_check_pokerus_invalidates_context();
void test_batch10_checkpoke_invalidates_context();
void test_batch10_checksave_invalidates_context();
void test_batch10_checkwarp_preserves_context();
void test_batch10_getcoins_uses_coins_source();
void test_batch10_getmoney_uses_typed_money_account();
void test_batch10_getnum_uses_scriptvar_source();
void test_batch10_giveegg_invalidates_context();
void test_batch10_givepoke_invalidates_context();
void test_batch10_number_sources_are_distinct();
void test_batch10_pocketisfull_does_not_invalidate_context();
void test_batch10_pocketisfull_emits_notify_not_absorbed();
void test_batch10_show_text_empty_sequence_fails_legality();
void test_batch10_show_text_nonempty_sequence_passes_legality();
void test_batch10_startbattle_invalidates_context();
void test_batch2_59_not_60_60_not_61();
void test_batch2_no_sem_special_for_59_60();
void test_batch2_special_59_waits_sfx();
void test_batch2_special_60_plays_map_music();
void test_batch3_heal_party_egg_pp_unchanged();
void test_batch3_heal_party_egg_skip();
void test_batch3_heal_party_no_script_result();
void test_batch3_heal_party_pp_formula();
void test_batch3_heal_party_pp_restoration();
void test_batch3_no_sem_special_for_27();
void test_batch3_special_27_heals_party();
void test_batch3_special_27_production_lowering();
void test_batch4_balance_overlay_no_script_result();
void test_batch4_balance_overlay_semantic_distinctions();
void test_batch4_no_sem_special_for_79_80_81();
void test_batch4_special_79_production_lowering();
void test_batch4_special_80_production_lowering();
void test_batch4_special_81_production_lowering();
void test_batch5_check_pokerus_script_result();
void test_batch5_gameboy_check_absorption_proof();
void test_batch5_no_sem_special_for_78_102();
void test_batch5_special_102_production_lowering();
void test_batch5_special_144_remains_sem_special();
void test_batch5_special_78_production_lowering();
void test_batch6_absorption_accounting_invariant();
void test_batch6_special_157_no_sem_special();
void test_batch6_special_157_production_absorption();
void test_batch6_unhandled_special_produces_sem_special();
void test_batch7_special_160_branch_equivalence();
void test_batch7_special_160_no_sem_special();
void test_batch7_special_160_not_zero_instructions();
void test_batch7_special_160_overwrites_stale_script_var();
void test_batch7_special_160_production_lowering();
void test_batch8_dst_operations_no_script_result();
void test_batch8_special_163_emits_sem_yesno();
void test_batch8_special_163_no_sem_special();
void test_batch8_special_163_yesorno_equivalence();
void test_batch8_special_166_167_differ_by_enabled();
void test_batch8_special_166_167_no_sem_special();
void test_batch8_special_166_emits_dst_true();
void test_batch8_special_167_emits_dst_false();
void test_batch9_setval_establishes_context();
void test_batch9_special_152_all_source_valid_selectors_accepted();
void test_batch9_special_152_invalid_encoding_rejected();
void test_batch9_special_152_palette_normalization();
void test_batch9_special_40_no_context_fallback();
void test_batch9_special_40_with_context();
void test_batch9_species_domain_from_profile_not_hardcoded();
void test_behavior_known_unimplemented_errors_explicitly();
void test_behavior_sdefer_routes_to_scheduler();
void test_behavior_unknown_unregistered_errors_explicitly();
void test_behavior_writes_script_var_errors_before_branch();
void test_bg_event_conditional_script_decode();
void test_bg_event_directional_types_preserved();
void test_bg_event_hidden_item_collected_blocked();
void test_bg_event_hidden_item_semantic_decode();
void test_bg_event_hidden_item_uncollected_triggers();
void test_bg_event_ifnotset_set_blocked();
void test_bg_event_ifnotset_unset_triggers();
void test_bg_event_ifset_ifnotset_condition_flag_integration();
void test_bg_event_ifset_set_triggers();
void test_bg_event_ifset_unset_blocked();
void test_bg_event_type_package_roundtrip_all_types();
void test_charmap_contractions();
void test_charmap_pokemon_text();
void test_collision_blocked_bounds();
void test_collision_blocked_entity();
void test_collision_blocked_wall();
void test_collision_class_semantic_queries();
void test_collision_classifier_adversarial_misclassified_ids();
void test_collision_classifier_source_proven_constants();
void test_collision_dimension_uses_collision_not_tile_width();
void test_collision_entity_target_blocks();
void test_collision_ledge_detection();
void test_collision_passable_floor();
void test_collision_semantic_boundary_adversarial();
void test_collision_side_wall_blocks();
void test_compat_exact_hash_gives_exacthash_match();
void test_compat_incompatible_profile_fails_explicitly();
void test_compat_modified_hash_layout_valid_not_rejected();
void test_connection_landing_math();
void test_connection_newbark_to_route29();
void test_connection_semantic_azalea_west_to_route34();
void test_connection_semantic_cherrygrove_north_to_route30();
void test_connection_semantic_route26_west_to_route27();
void test_connection_semantic_route27_east_to_route26();
void test_connection_strip_after_strip_rejected();
void test_connection_strip_before_strip_rejected();
void test_connection_strip_first_valid_coordinate();
void test_connection_strip_last_valid_coordinate();
void test_coord_event_field_decode();
void test_coord_event_scripts_in_corpus();
void test_corpus_battletowertext_distinct_from_normal_trainer_text();
void test_corpus_battletowertext_no_sem_special();
void test_corpus_battletowertext_produces_trainer_text();
void test_corpus_callasm_0x9f5cb_produces_read_state_var();
void test_corpus_callasm_nearby_addresses_rejected();
void test_corpus_readmem_0xcf64_produces_read_state_var();
void test_corpus_readmem_nearby_addresses_rejected();
void test_create_pokemon_missing_species_throws();
void test_create_pokemon_registered_species_succeeds();
void test_cry_and_pokepic_same_source_semantics();
void test_cry_literal_species();
void test_cry_script_var_distinct_from_literal();
void test_cry_zero_dynamic_species();
void test_daycare_species_252_save_load_accepted();
void test_decode_newbarktownsign();
void test_decoder_unique_command_identity_cfg_integrity();
void test_decoder_unique_command_identity_loop();
void test_decoder_unique_command_identity_semantic_ir();
void test_directional_ledge_semantic_preservation();
void test_duplicate_physical_binding_release();
void test_elms_lab_has_exit_warp();
void test_emitter_callstd_no_crash();
void test_emitter_jumpstd_no_crash();
void test_emitter_special_no_crash();
void test_emitter_warp_to_spawn_has_binding();
void test_event_script_id_canonical_format_bg();
void test_event_script_id_canonical_format_coord();
void test_event_script_id_canonical_format_npc();
void test_event_script_id_missing_fails_explicitly();
void test_event_script_id_no_local_positional_survives();
void test_f1_turnobject_direction_preserved_right();
void test_f1_turnobject_direction_preserved_up();
void test_f2_movement_order_right_down_down();
void test_f2_movement_order_right_right_up();
void test_f2_text_ram_op_preserved_not_dropped();
void test_f3_givepoke_held_item_preserved();
void test_f3_no_second_player_authority();
void test_f3_player_authority_step_syncs_gamestate();
void test_f3_player_authority_warp_uses_latest_position();
void test_f4_failed_prepare_warp_leaves_everything_unchanged();
void test_f4_failed_prepare_connection_leaves_everything_unchanged();
void test_f4_prepare_warp_does_not_mutate_player();
void test_f4_scripted_warp_coordinates_preserved();
void test_f4_transition_failure_leaves_old_world_coherent();
void test_f4_transition_staged_world_state_separate();
void test_f5_save_v2_npc_section_mandatory_truncations();
void test_f6_canonical_rng_not_reset_on_map_transition();
void test_f6_simultaneous_wakeup_deterministic_order();
void test_f7_save_invalid_direction_rejected();
void test_f7_text_sequence_ordered_consumption();
void test_f8_money_account_player_vs_mom();
void test_fidelity11_askforphonenumber_distinct_from_addphonenumber();
void test_fidelity11_catchtutorial_distinct_from_startbattle();
void test_fidelity11_catchtutorial_preserves_type_byte();
void test_fidelity11_deactivatefacing_distinct_from_pause();
void test_fidelity11_endall_distinct_from_end();
void test_fidelity11_getname_type1_pokemon();
void test_fidelity11_getname_type2_move();
void test_fidelity11_getname_type3_dummy_not_item();
void test_fidelity11_getname_type4_item_not_trainer();
void test_fidelity11_getname_type5_partyot();
void test_fidelity11_getname_type7_trainer();
void test_fidelity11_invalid_domain_gates_linker();
void test_fidelity11_jumptext_distinct_from_writetext();
void test_fidelity11_jumptextfaceplayer_preserves_text();
void test_fidelity11_loadmenu_preserves_header_pointer();
void test_fidelity11_promptbutton_distinct_from_waitbutton();
void test_fidelity11_verbosegiveitemvar_variable_semantics();
void test_fidelity11_verticalmenu_distinct_from_2dmenu();
void test_fidelity11_writetext_distinct_pointers_distinct_sequences();
void test_field_context_activate_consumes_actor();
void test_field_context_clear_context_clears_all();
void test_field_context_encounter_failure_clears_encounter();
void test_field_context_encounter_success_establishes_encounter();
void test_field_context_full_rock_smash_encounter_flow();
void test_field_context_full_strength_flow();
void test_field_context_load_encounter_consumes_encounter();
void test_field_context_new_runtime_starts_clean();
void test_field_context_no_encounter_no_stale_state();
void test_field_context_prepare_nickname_preserves_actor();
void test_field_context_read_species_preserves_encounter();
void test_field_context_rock_smash_available_establishes_actor();
void test_field_context_rock_smash_unavailable_clears_actor();
void test_field_context_runtime_isolation_actor();
void test_field_context_runtime_isolation_encounter();
void test_field_context_strength_active_persists_across_scripts();
void test_field_context_strength_already_active_clears_actor();
void test_field_context_strength_available_establishes_actor();
void test_field_context_strength_unavailable_clears_actor();
void test_field_context_user_declines_flow();
void test_flag_api_stub_isolation();
void test_gamestate_deserialize_malformed_rejects();
void test_gamestate_flags_persist();
void test_gamestate_rng_persist();
void test_gamestate_serialize_insertion_order_determinism();
void test_gamestate_serialize_roundtrip();
void test_gamestate_variables_persist();
void test_gamestate_warp_memory_persist();
void test_headless_loop_facing_update();
void test_headless_loop_input_locked_during_movement();
void test_headless_loop_movement_blocked();
void test_headless_loop_movement_ticks();
void test_headless_loop_spawn_player();
void test_headless_newbark_determinism();
void test_headless_newbark_script_execution();
void test_headless_newbark_sign_interaction();
void test_headless_newbark_teacher_interaction();
void test_headless_newbark_walk_one_tile();
void test_icon_format_128_bytes_total();
void test_icon_format_16x16_geometry();
void test_icon_format_bigmon_packaged_via_closure();
void test_icon_format_pikachu_pixel_hash();
void test_input_edge_held_across_multiple_ticks();
void test_input_edge_multiple_render_frames_preserves_press();
void test_input_edge_new_press_after_release();
void test_input_edge_one_press_four_ticks_consumed_once();
void test_input_edge_one_press_one_tick_consumed_once();
void test_input_edge_press_release_before_tick();
void test_input_edge_release_consumed_once();
void test_input_edge_zero_tick_frame_preserves_press();
void test_input_edge_zero_tick_frame_preserves_release();
void test_input_snapshot_direction_helper();
void test_input_system_arrow_bindings();
void test_input_system_default_bindings();
void test_input_system_gamepad_bindings();
void test_input_system_get_action_interact();
void test_input_system_get_action_movement();
void test_input_system_key_events();
void test_input_system_latch();
void test_input_system_rebind();
void test_interaction_bg_event_facing_requirement();
void test_interaction_bg_event_found();
void test_interaction_bounds_check();
void test_interaction_counter_extends_reach();
void test_interaction_counter_tile_detection();
void test_interaction_directional_bg_wrong_facing();
void test_interaction_facing_calculation();
void test_interaction_moving_npc_not_interactable();
void test_interaction_object_found();
void test_interaction_object_priority_over_bg();
void test_load_map_owns_copy_prevents_dangling();
void test_lua_coroutine_cleanup_via_resume();
void test_lua_coroutine_cleanup_via_resume_with_result();
void test_lua_flags_vars_persist_through_gamestate_save_load();
void test_lua_flags_without_gamestate_uses_stubs_only();
void test_lua_goto_works();
void test_lua_load_script_directory_deterministic_order();
void test_lua_runtime_creates();
void test_lua_runtime_ctx_exists();
void test_lua_runtime_executes_script_yields_on_wait_button();
void test_lua_runtime_executes_simple();
void test_lua_runtime_full_pipeline();
void test_lua_runtime_loads_generated_script();
void test_lua_runtime_multiple_scripts();
void test_lua_runtime_wait_frames();
void test_movement_combined_steps_and_turns();
void test_movement_face_changes_facing();
void test_movement_lua_emit_steps();
void test_movement_lua_emit_turn();
void test_movement_no_terminator_throws();
void test_movement_parse_commands();
void test_movement_parse_with_turn();
void test_movement_player_movement();
void test_movement_valid_terminates_correctly();
void test_movement_world_state_changes();
void test_multipage_rival_script_three_segments();
void test_multipage_text_stream_encoding();
void test_multipage_text_with_cont_preserves_scroll_line();
void test_multipage_text_with_para_advances_all_pages();
void test_native_text_from_runtime_decimal_preserves_operands();
void test_native_text_from_runtime_no_silent_blank_fallthrough();
void test_native_text_from_runtime_ram_preserves_operands();
void test_native_text_from_runtime_unsupported_throws();
void test_newbark_has_connections();
void test_newbark_has_warps();
void test_newbark_npc_behaviors_extracted();
void test_newbarktown_bg_event_positions();
void test_newbarktown_collision_movement_blocked();
void test_newbarktown_door_tiles();
void test_newbarktown_entity_collision();
void test_newbarktown_interaction_determinism();
void test_newbarktown_known_blocked_tiles();
void test_newbarktown_known_walkable_tiles();
void test_newbarktown_no_rom_addresses_in_scripts();
void test_newbarktown_object_positions();
void test_newbarktown_object_priority_integration();
void test_newbarktown_package_roundtrip_interaction();
void test_newbarktown_real_collision_map();
void test_newbarktown_sign_correct_facing();
void test_newbarktown_sign_wrong_facing();
void test_newbarktown_teacher_decode();
void test_newbarktown_teacher_default_branch();
void test_newbarktown_teacher_first_branch();
void test_newbarktown_teacher_interaction();
void test_newbarktown_teacher_lua_emit();
void test_newbarktown_teacher_second_branch();
void test_newbarktown_teacher_third_branch();
void test_newbarktown_water_tiles();
void test_no_second_authoritative_rng_stream();
void test_npc_can_traverse_side_wall_from_allowed_direction();
void test_npc_cannot_cross_side_wall_from_forbidden_direction();
void test_npc_collision_with_player();
void test_npc_destination_occupancy_blocks_conflicting_movement();
void test_npc_frozen_blocks_movement();
void test_npc_idle_timer_countdown();
void test_npc_movement_behavior_conversion();
void test_npc_movement_facing_conversion();
void test_npc_respects_radius_bounds();
void test_npc_rng_determinism_via_gamestate();
void test_npc_rng_save_restore_determinism();
// Scripted movement P0
void test_scripted_movement_e2e_npc_steps_left_position_commits();
void test_scripted_movement_e2e_turn_changes_facing_not_position();
void test_scripted_movement_e2e_coroutine_resumes_only_after_completion();
void test_scripted_movement_e2e_nonzero_npc_not_player();
void test_scripted_movement_e2e_two_actors_no_alias();
void test_scripted_movement_malformed_payload_fails_explicitly();
void test_scripted_movement_destructor_clears_stale_manager_pointer();
void test_scripted_movement_rebind_clears_old_wires_new();
void test_scripted_movement_async_auto_enabled_by_set_lua_runtime();
void test_scripted_movement_e2e_command_order_preserved();
void test_npc_spin_changes_facing();
void test_npc_standing_never_moves();
void test_npc_walk_changes_position();
void test_npc_walk_up_down_direction();
void test_object_event_palette_type_decode();
void test_package_context_isolation();
void test_pcg_bounded_one_always_zero();
void test_pcg_bounded_representative_value();
void test_pcg_bounded_zero_throws_no_draw();
void test_pcg_dv_draw_count_two_semantic();
void test_pcg_known_sequence_seed_deadbeef();
void test_pcg_map_transition_does_not_perturb_canonical();
void test_pcg_next_u64_hi_lo_ordering();
void test_pcg_next_u8_draw_count();
void test_pcg_npc_movement_uses_canonical_rng();
void test_pcg_npc_save_load_canonical_continuation();
void test_pcg_presentation_rng_does_not_perturb_canonical();
void test_pcg_save_load_exact_continuation();
void test_pcg_seed_zero_known_state();
void test_pcg_v4_migration_deterministic();
void test_pcg_v5_save_roundtrip();
void test_pcstorage_deposit_moves_pokemon_exactly_once();
void test_player_destination_reserved_against_npc();
void test_pokemon_move_slots_initialized_to_defaults();
void test_pokepic_literal_species();
void test_pokepic_zero_dynamic_species();
void test_presentation_hook_isolation();
void test_random_chance_correct_probability_contract();
void test_random_chance_invalid_percent_throws();
void test_renderer_staged_prepare_isolates_cross_operation_failure();
void test_renderer_staged_prepare_worldstate_unchanged_on_load_failure();
void test_reset_after_script_completed();
void test_reset_cancels_active_coroutine_no_timed_resume();
void test_reset_when_no_script_active();
void test_reset_when_script_yielded();
void test_save_mutate_load_identical();
void test_scheduler_2_second_hitch_retains_debt();
void test_scheduler_500ms_hitch_retains_debt();
void test_scheduler_interpolation_alpha_clamped();
void test_scheduler_repeated_updates_catch_up();
void test_scheduler_total_ticks_equals_elapsed_time();
void test_script_errors_after_resume_sets_error();
void test_script_errors_immediately_returns_false();
void test_script_finishes_normally_sets_complete();
void test_script_resumed_no_resume_attempt();
void test_script_resumed_yielded_to_completed();
void test_script_resumed_yielded_to_yielded();
void test_script_runtime_error_during_start_returns_false();
void test_script_state_checkcellnum_invalidates();
void test_script_state_checkphonecall_invalidates();
void test_script_state_checktime_invalidates();
void test_script_state_checkver_establishes_context();
void test_script_state_cry_and_pokepic_same_source_semantics();
void test_script_state_cry_nonzero_is_literal();
void test_script_state_cry_zero_is_script_var_not_literal();
void test_script_state_delcmdqueue_invalidates();
void test_script_state_pokepic_nonzero_is_literal();
void test_script_state_pokepic_zero_is_script_var_not_literal();
void test_script_state_setval_preserved_across_noop_command();
void test_script_state_setval_yesorno_invalidates_context();
void test_script_state_verbosegiveitemvar_invalidates();
void test_script_var_propagates_across_non_writer();
void test_script_yielded_locks_input();
void test_sem_game_specific_event_behavior_name_is_source_proven_not_raw_id();
void test_sem_game_specific_event_no_write_preserves_context();
void test_sem_game_specific_event_writes_var_flag_blocks_constant_propagation();
void test_sem_special_clean_ir_still_passes_legality();
void test_sem_special_rejected_by_stage5_legality_gate();
void test_sem_special_still_rejected_after_registry_cleanup();
void test_semantic_fix_encountermusic_distinct_from_playmapmusic();
void test_semantic_fix_getmoney_preserves_account();
void test_semantic_fix_getstring_preserves_text_pointer();
void test_semantic_fix_gettrainername_preserves_both_operands();
void test_semantic_fix_newloadmap_preserves_method();
void test_semantic_fix_reanchormap_distinct_from_refreshmap();
void test_semantic_fix_refreshmap_distinct_from_reanchormap();
void test_semantic_fix_sdefer_bank_resolution();
void test_semantic_fix_text_identity_distinguishes_controls();
void test_semantic_fix_text_identity_distinguishes_ram_addresses();
void test_special_tileset_fixed_palette_extracted();
void test_species_finder_bulbasaur_record_correct();
void test_species_finder_last_record_is_mew();
void test_species_finder_non251_profile_same_path();
void test_species_finder_stock_count_is_251();
void test_species_icon_map_covers_full_domain();
void test_species_linker_refs_are_exact_resolved();
void test_species_linker_unknown_species_invalid_domain();
void test_sprite_id_mapping_authoritative();
void test_stale_script_var_giveitem_invalidates();
void test_stale_script_var_yesorno_invalidates_before_map_radio();
void test_text_arg_slot_numbering_table_driven();
void test_text_arg_slot_wplayername_hard_fails();
void test_text_battle_nickname_is_ram_source_not_arg();
void test_text_enemy_nickname_is_ram_source_not_arg();
void test_text_literal_at_returns_to_outer_stream();
void test_text_literal_tx_opcode_overlap_0x14();
void test_text_prepared_string2_is_ram_source_not_arg();
void test_text_presentation_ops_dropped_not_failed();
void test_text_ram_source_domain_distinct_from_arg_domain();
void test_text_resource_can_begin_with_dynamic_command();
void test_text_string_buffer_id0_maps_to_arg_slot0();
void test_text_string_buffer_id4_maps_to_arg_slot4();
void test_text_string_buffer_id6_maps_to_arg_slot6();
void test_text_string_buffer_invalid_id255_hard_fails();
void test_text_string_buffer_invalid_id7_hard_fails();
void test_text_tx_bcd_hard_fails();
void test_text_tx_day_produces_day_op();
void test_text_tx_decimal_unknown_address_hard_fails();
void test_text_tx_decimal_wscriptvar_produces_script_var_decimal();
void test_text_tx_far_inlines_referenced_text();
void test_text_tx_far_without_registry_hard_fails();
void test_text_tx_pause_frame_count_preserved_explicitly();
void test_text_tx_pause_produces_pause_with_30_frames();
void test_text_tx_prompt_button_produces_inline_prompt_button_not_dropped();
void test_text_tx_prompt_button_standalone_produces_single_element();
void test_text_tx_ram_unknown_address_hard_fails();
void test_text_tx_ram_wstringbuffer3_maps_to_arg_slot0();
void test_text_tx_ram_wstringbuffer4_maps_to_arg_slot1();
void test_text_tx_ram_wstringbuffer5_maps_to_arg_slot2();
void test_text_tx_raw_unknown_opcode_hard_fails();
void test_text_tx_sound_fanfare_produces_typed_sound_kind();
void test_text_tx_sound_item_produces_typed_sound_kind();
void test_textraw_empty_raw_handled();
void test_textraw_identity_distinguishes_contents();
void test_textraw_identity_identical_contents_match();
void test_tileset_id_bounds_0_1_36_37();
void test_timing_144hz_rendering_produces_same_ticks();
void test_timing_60hz_rendering_produces_consistent_ticks();
void test_timing_equivalent_elapsed_same_tick_count();
void test_timing_irregular_frames_same_result();
void test_timing_scheduler_advance();
void test_timing_scheduler_basic();
void test_tx_box_height_width_semantics();
void test_tx_far_dedup_different_bank_gets_different_id();
void test_tx_far_identity_distinguishes_address();
void test_tx_far_identity_distinguishes_bank();
void test_typechart_dual_type_immunity_remains_zero();
void test_typechart_dual_type_out_of_range_throws();
void test_typechart_explicit_values_survive_lookup();
void test_typechart_immunity_is_zero_not_unset();
void test_typechart_max_valid_index_accepted();
void test_typechart_out_of_range_get_throws();
void test_typechart_out_of_range_set_throws();
void test_unrelated_coroutine_resume_does_not_set_script_resumed();
void test_vm_call_returns_to_continuation();
void test_vm_callee_end_does_not_exit_top_level();
void test_vm_deferred_failure_propagates_error();

// Capability closure E2E tests
void test_capability_show_hide_npc_updates_npc_state();
void test_capability_face_actor_updates_npc_facing();
void test_capability_face_actor_player_updates_player_facing();
void test_capability_face_player_updates_npc_facing_toward_player();
void test_capability_teleport_npc_updates_npc_position();
void test_capability_set_last_talked_records_id();
void test_capability_set_scene_persists_in_game_state();
void test_capability_state_var_persists_in_game_state();
void test_vm_endall_inside_nested_call_terminates();
void test_vm_nested_calls_unwind_correctly();
void test_vm_result_nonzero_integer_true();
void test_vm_result_one_takes_true_branch();
void test_vm_result_zero_takes_false_branch();
void test_vm_sdefer_cleared_on_loop_destroy();
void test_vm_sdefer_cleared_on_rebind();
void test_vm_setvar_from_result_zero_stores_zero();
void test_wait_frames_before_expiry_no_resume();
void test_movement_callback_wires_to_live_object();

// RTC tests
void test_rtc_hour_minute_from_offset();
void test_rtc_fake_clock_advance();
void test_rtc_set_clock_recomputes_offset();
void test_rtc_period_boundaries();
void test_rtc_weekday_derivation();
void test_rtc_midnight_daily_rollover();
void test_rtc_negative_offset();
void test_rtc_ticks_do_not_advance_rtc();
void test_rtc_save_load_preserves_offset();
void test_rtc_set_daylight_saving_adjusts_offset();
void test_rtc_dst_enable_twice_idempotent();
void test_rtc_dst_disable_twice_idempotent();
void test_rtc_dst_enable_set_clock_disable_net_reversal();
void test_door_auto_step_routes_through_movement_manager();
void test_crystal_npc_visibility_flag_set_means_hidden();
void test_show_hide_npc_persists_to_gamestate_flags();
void test_hide_npc_no_flag_is_transient();
void test_scripted_coordinate_warp_uses_explicit_coords();
void test_warp_to_spawn_uses_gamestate_backup_warp();
void test_warp_to_spawn_no_fn_errors_explicitly();
void test_production_bootstrap_script_mutates_gamestate();
void test_no_gamestate_flags_fallback_to_stubs();
void test_lua_syntax_error_throws_explicitly();
void test_flag_identity_hex_canonical();
void test_flag_gamestate_set_clear_check_roundtrip();
void test_flag_map_condition_matches_script_set();
void test_wait_frames_expiry_reyield_sets_resumed();
void test_wait_frames_expiry_sets_resumed();
void test_wait_seconds_not_immediate_resume();
void test_wait_seconds_precision_0_05s_is_3_ticks();
void test_wait_seconds_precision_0_1s_is_6_ticks();
void test_wait_seconds_precision_1_0s_is_60_ticks();
void test_wait_seconds_resume_sets_flag();
void test_wait_seconds_resumes_after_duration();
void test_wait_seconds_zero_duration();
void test_warp_elms_lab_to_newbark_last_map();
void test_warp_invalid_index_out_of_range_fails();
void test_warp_invalid_index_zero_fails();
void test_warp_newbark_to_elms_lab();
void test_warp_target_map_no_warps_fails();
void test_warp_valid_index_succeeds();
void test_world_api_stub_isolation();
void test_world_manager_get_warp_at();
void test_world_manager_load_map();
void test_writecmdqueue_same_ptr_different_banks_distinct();
void test_yielded_script_remains_nonterminal();

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <rom_path>\n";
        std::cerr << "\nRuns Lua runtime tests with Crystal ROM scripts.\n";
        return 1;
    }
    
    // Load ROM
    std::cout << "Loading ROM: " << argv[1] << "\n";
    auto rom = RomData::load(argv[1]);
    if (!rom) {
        std::cerr << "Failed to load ROM\n";
        return 1;
    }
    
    // Get profile
    auto& registry = ProfileRegistry::instance();
    auto profile = registry.get_profile_by_hash(rom->hash());
    if (!profile) {
        std::cerr << "ROM not supported (SHA-1: " << rom->hash() << ")\n";
        return 1;
    }
    
    g_rom = rom.get();
    g_profile = profile;
    
    std::cout << "\n=== Running Lua Runtime Tests ===\n\n";
    
    // Basic runtime tests
    RUN_TEST(lua_runtime_creates);
    RUN_TEST(lua_runtime_executes_simple);
    RUN_TEST(lua_runtime_ctx_exists);
    
    // Script generation tests
    RUN_TEST(decode_newbarktownsign);
    RUN_TEST(lua_runtime_loads_generated_script);
    
    // Execution tests
    RUN_TEST(lua_runtime_executes_script_yields_on_wait_button);
    RUN_TEST(lua_runtime_full_pipeline);
    RUN_TEST(lua_runtime_multiple_scripts);
    RUN_TEST(lua_runtime_wait_frames);
    RUN_TEST(lua_goto_works);
    
    // NewBarkTownTeacherScript tests - complex script with conditionals + flags
    RUN_TEST(newbarktown_teacher_decode);
    RUN_TEST(newbarktown_teacher_lua_emit);
    RUN_TEST(newbarktown_teacher_default_branch);
    RUN_TEST(newbarktown_teacher_first_branch);
    RUN_TEST(newbarktown_teacher_second_branch);
    RUN_TEST(newbarktown_teacher_third_branch);
    
    // Charmap tests - verify text decoding against pokecrystal
    RUN_TEST(charmap_pokemon_text);
    RUN_TEST(charmap_contractions);
    
    // Movement tests - end-to-end movement verification
    RUN_TEST(movement_parse_commands);
    RUN_TEST(movement_parse_with_turn);
    RUN_TEST(movement_lua_emit_steps);
    RUN_TEST(movement_lua_emit_turn);
    RUN_TEST(f2_movement_order_right_down_down);
    RUN_TEST(f2_movement_order_right_right_up);
    RUN_TEST(movement_world_state_changes);
    RUN_TEST(movement_face_changes_facing);
    RUN_TEST(movement_combined_steps_and_turns);
    RUN_TEST(movement_player_movement);
    
    // Async movement tests - simulation-driven asynchronous movement
    RUN_TEST(async_movement_manager_basic);
    RUN_TEST(async_movement_not_instant);
    RUN_TEST(async_movement_progresses_over_ticks);
    RUN_TEST(async_movement_final_position);
    RUN_TEST(async_movement_with_turns);
    RUN_TEST(async_movement_fast_forward);
    RUN_TEST(async_movement_completion_callback);
    RUN_TEST(async_movement_batched_table);
    RUN_TEST(async_movement_lua_yields);
    RUN_TEST(async_movement_position_not_jumped);
    RUN_TEST(async_movement_resumes_after_complete);
    
    // Collision tests - native overworld collision system
    RUN_TEST(collision_class_semantic_queries);
    RUN_TEST(collision_passable_floor);
    RUN_TEST(collision_blocked_wall);
    RUN_TEST(collision_blocked_bounds);
    RUN_TEST(collision_blocked_entity);
    RUN_TEST(collision_entity_target_blocks);
    RUN_TEST(collision_side_wall_blocks);
    RUN_TEST(collision_ledge_detection);
    RUN_TEST(collision_semantic_boundary_adversarial);
    
    // Interaction tests - A-button interaction system
    RUN_TEST(interaction_facing_calculation);
    RUN_TEST(interaction_counter_tile_detection);
    RUN_TEST(interaction_bg_event_facing_requirement);
    RUN_TEST(interaction_object_found);
    RUN_TEST(interaction_bg_event_found);
    RUN_TEST(interaction_object_priority_over_bg);
    RUN_TEST(interaction_moving_npc_not_interactable);
    RUN_TEST(interaction_directional_bg_wrong_facing);
    RUN_TEST(interaction_counter_extends_reach);
    RUN_TEST(interaction_bounds_check);
    
    // New Bark Town integration tests - real ROM data
    RUN_TEST(newbarktown_real_collision_map);
    RUN_TEST(newbarktown_known_walkable_tiles);
    RUN_TEST(newbarktown_known_blocked_tiles);
    RUN_TEST(newbarktown_water_tiles);
    RUN_TEST(newbarktown_door_tiles);
    RUN_TEST(newbarktown_collision_movement_blocked);
    RUN_TEST(newbarktown_entity_collision);
    RUN_TEST(newbarktown_bg_event_positions);
    RUN_TEST(newbarktown_object_positions);
    
    // New Bark Town interaction integration tests
    RUN_TEST(newbarktown_sign_wrong_facing);
    RUN_TEST(newbarktown_sign_correct_facing);
    RUN_TEST(newbarktown_teacher_interaction);
    RUN_TEST(newbarktown_object_priority_integration);
    RUN_TEST(newbarktown_package_roundtrip_interaction);
    RUN_TEST(bg_event_type_package_roundtrip_all_types);
    RUN_TEST(bg_event_ifset_ifnotset_condition_flag_integration);
    RUN_TEST(newbarktown_no_rom_addresses_in_scripts);
    RUN_TEST(newbarktown_interaction_determinism);
    
    // Headless game loop tests - end-to-end playable simulation
    RUN_TEST(headless_loop_spawn_player);
    RUN_TEST(headless_loop_facing_update);
    RUN_TEST(headless_loop_movement_blocked);
    RUN_TEST(headless_loop_movement_ticks);
    RUN_TEST(headless_loop_input_locked_during_movement);
    RUN_TEST(headless_newbark_walk_one_tile);
    RUN_TEST(headless_newbark_sign_interaction);
    RUN_TEST(headless_newbark_teacher_interaction);
    RUN_TEST(headless_newbark_determinism);
    RUN_TEST(headless_newbark_script_execution);
    
    // World continuity tests - warps and connections
    RUN_TEST(newbark_has_warps);
    RUN_TEST(newbark_has_connections);
    RUN_TEST(elms_lab_has_exit_warp);
    RUN_TEST(world_manager_load_map);
    RUN_TEST(world_manager_get_warp_at);
    RUN_TEST(warp_newbark_to_elms_lab);
    RUN_TEST(warp_elms_lab_to_newbark_last_map);
    
    // Targeted runtime correctness fix regression tests
    RUN_TEST(collision_dimension_uses_collision_not_tile_width);
    RUN_TEST(warp_invalid_index_zero_fails);
    RUN_TEST(warp_invalid_index_out_of_range_fails);
    RUN_TEST(warp_target_map_no_warps_fails);
    RUN_TEST(warp_valid_index_succeeds);
    RUN_TEST(load_map_owns_copy_prevents_dangling);
    
    // Pre-RNG runtime correctness pass regression tests
    RUN_TEST(typechart_immunity_is_zero_not_unset);
    RUN_TEST(typechart_dual_type_immunity_remains_zero);
    RUN_TEST(typechart_explicit_values_survive_lookup);
    // Fix 2: create_pokemon hard-fail
    RUN_TEST(create_pokemon_missing_species_throws);
    RUN_TEST(create_pokemon_registered_species_succeeds);
    // Fix 3: TypeChart domain enforcement
    RUN_TEST(typechart_out_of_range_get_throws);
    RUN_TEST(typechart_out_of_range_set_throws);
    RUN_TEST(typechart_max_valid_index_accepted);
    RUN_TEST(typechart_dual_type_out_of_range_throws);
    // Fix 4: Lua load order determinism
    RUN_TEST(lua_load_script_directory_deterministic_order);
    RUN_TEST(pcstorage_deposit_moves_pokemon_exactly_once);
    RUN_TEST(pokemon_move_slots_initialized_to_defaults);
    RUN_TEST(connection_strip_first_valid_coordinate);
    RUN_TEST(connection_strip_last_valid_coordinate);
    RUN_TEST(connection_strip_before_strip_rejected);
    RUN_TEST(connection_strip_after_strip_rejected);
    RUN_TEST(player_destination_reserved_against_npc);
    RUN_TEST(npc_cannot_cross_side_wall_from_forbidden_direction);
    RUN_TEST(npc_can_traverse_side_wall_from_allowed_direction);
    
    RUN_TEST(connection_newbark_to_route29);
    RUN_TEST(connection_landing_math);
    RUN_TEST(connection_semantic_cherrygrove_north_to_route30);
    RUN_TEST(connection_semantic_azalea_west_to_route34);
    RUN_TEST(connection_semantic_route26_west_to_route27);
    RUN_TEST(connection_semantic_route27_east_to_route26);
    
    // Save/load tests - GameState serialization
    RUN_TEST(gamestate_serialize_roundtrip);
    RUN_TEST(gamestate_flags_persist);
    RUN_TEST(gamestate_variables_persist);
    RUN_TEST(lua_flags_vars_persist_through_gamestate_save_load);
    RUN_TEST(lua_flags_without_gamestate_uses_stubs_only);
    RUN_TEST(gamestate_warp_memory_persist);
    RUN_TEST(gamestate_rng_persist);
    RUN_TEST(save_mutate_load_identical);
    RUN_TEST(gamestate_serialize_insertion_order_determinism);
    RUN_TEST(gamestate_deserialize_malformed_rejects);

    // F3: Player authority
    RUN_TEST(f3_player_authority_step_syncs_gamestate);
    RUN_TEST(f3_player_authority_warp_uses_latest_position);

    // F4: Transactional transition
    RUN_TEST(f4_transition_failure_leaves_old_world_coherent);
    RUN_TEST(f4_transition_staged_world_state_separate);
    RUN_TEST(renderer_staged_prepare_worldstate_unchanged_on_load_failure);
    RUN_TEST(renderer_staged_prepare_isolates_cross_operation_failure);

    // F3/F4 adversarial (from new pass)
    RUN_TEST(f3_no_second_player_authority);
    RUN_TEST(f4_prepare_warp_does_not_mutate_player);
    RUN_TEST(f4_failed_prepare_warp_leaves_everything_unchanged);
    RUN_TEST(f4_failed_prepare_connection_leaves_everything_unchanged);

    // F5: Save v2 NPC section mandatory
    RUN_TEST(f5_save_v2_npc_section_mandatory_truncations);

    // F6: Deterministic simultaneous scheduling
    RUN_TEST(f6_simultaneous_wakeup_deterministic_order);

    // F7: Ordered text sequence consumption
    RUN_TEST(f7_text_sequence_ordered_consumption);
    RUN_TEST(scheduler_interpolation_alpha_clamped);
    
    // Multi-page text state machine tests
    RUN_TEST(multipage_text_stream_encoding);
    RUN_TEST(multipage_text_with_para_advances_all_pages);
    RUN_TEST(multipage_text_with_cont_preserves_scroll_line);
    RUN_TEST(multipage_rival_script_three_segments);
    
    // Input system tests - SDL3 abstraction
    RUN_TEST(input_system_default_bindings);
    RUN_TEST(input_system_arrow_bindings);
    RUN_TEST(input_system_gamepad_bindings);
    RUN_TEST(input_system_key_events);
    RUN_TEST(input_system_get_action_movement);
    RUN_TEST(input_system_get_action_interact);
    RUN_TEST(input_system_rebind);
    RUN_TEST(input_system_latch);
    RUN_TEST(input_snapshot_direction_helper);
    
    // NPC movement tests
    RUN_TEST(npc_movement_behavior_conversion);
    RUN_TEST(npc_movement_facing_conversion);
    RUN_TEST(npc_idle_timer_countdown);
    RUN_TEST(npc_frozen_blocks_movement);
    RUN_TEST(npc_standing_never_moves);
    RUN_TEST(npc_spin_changes_facing);
    RUN_TEST(npc_walk_changes_position);
    RUN_TEST(npc_respects_radius_bounds);
    RUN_TEST(npc_collision_with_player);
    RUN_TEST(npc_walk_up_down_direction);
    RUN_TEST(newbark_npc_behaviors_extracted);
    
    // RNG ownership tests (Audit 7)
    RUN_TEST(npc_rng_determinism_via_gamestate);
    RUN_TEST(npc_rng_save_restore_determinism);

    // Scripted movement P0 — production correct pipeline
    RUN_TEST(scripted_movement_e2e_npc_steps_left_position_commits);
    RUN_TEST(scripted_movement_e2e_turn_changes_facing_not_position);
    RUN_TEST(scripted_movement_e2e_coroutine_resumes_only_after_completion);
    RUN_TEST(scripted_movement_e2e_nonzero_npc_not_player);
    RUN_TEST(scripted_movement_e2e_two_actors_no_alias);
    RUN_TEST(scripted_movement_malformed_payload_fails_explicitly);
    RUN_TEST(scripted_movement_destructor_clears_stale_manager_pointer);
    RUN_TEST(scripted_movement_rebind_clears_old_wires_new);
    RUN_TEST(scripted_movement_async_auto_enabled_by_set_lua_runtime);
    RUN_TEST(scripted_movement_e2e_command_order_preserved);
    
    // Field-move context lifecycle tests
    RUN_TEST(field_context_strength_available_establishes_actor);
    RUN_TEST(field_context_strength_unavailable_clears_actor);
    RUN_TEST(field_context_strength_already_active_clears_actor);
    RUN_TEST(field_context_activate_consumes_actor);
    RUN_TEST(field_context_rock_smash_available_establishes_actor);
    RUN_TEST(field_context_rock_smash_unavailable_clears_actor);
    RUN_TEST(field_context_encounter_success_establishes_encounter);
    RUN_TEST(field_context_encounter_failure_clears_encounter);
    RUN_TEST(field_context_load_encounter_consumes_encounter);
    RUN_TEST(field_context_read_species_preserves_encounter);
    RUN_TEST(field_context_prepare_nickname_preserves_actor);
    RUN_TEST(field_context_clear_context_clears_all);
    RUN_TEST(field_context_user_declines_flow);
    RUN_TEST(field_context_no_encounter_no_stale_state);
    RUN_TEST(field_context_full_strength_flow);
    RUN_TEST(field_context_full_rock_smash_encounter_flow);
    
    // Field-move context isolation tests (per-runtime ownership)
    RUN_TEST(field_context_new_runtime_starts_clean);
    RUN_TEST(field_context_runtime_isolation_actor);
    RUN_TEST(field_context_runtime_isolation_encounter);
    RUN_TEST(world_api_stub_isolation);  // Proves world_api stub state is per-runtime
    RUN_TEST(flag_api_stub_isolation);   // Proves flag_api stub state is per-runtime
    RUN_TEST(package_context_isolation);  // Proves PackageContext is per-instance, not global
    RUN_TEST(presentation_hook_isolation);  // Proves PresentationHooks are per-instance, not global
    RUN_TEST(field_context_strength_active_persists_across_scripts);
    
    // Batch 1 Special semantic op tests
    RUN_TEST(batch1_screen_fade_variants);
    RUN_TEST(batch1_sync_palettes_variants);
    RUN_TEST(batch1_sprite_ops_distinct);
    RUN_TEST(batch1_audio_ops_distinct);
    RUN_TEST(batch1_no_crystal_ids_in_ops);
    
    // Batch 2 Special semantic op tests - Audio operations (IDs 59, 60)
    RUN_TEST(batch2_special_59_waits_sfx);
    RUN_TEST(batch2_special_60_plays_map_music);
    RUN_TEST(batch2_no_sem_special_for_59_60);
    RUN_TEST(batch2_59_not_60_60_not_61);
    
    // Batch 3 Special semantic op tests - HealParty (ID 27)
    RUN_TEST(batch3_special_27_heals_party);
    RUN_TEST(batch3_no_sem_special_for_27);
    RUN_TEST(batch3_heal_party_pp_formula);
    RUN_TEST(batch3_heal_party_egg_skip);
    RUN_TEST(batch3_heal_party_no_script_result);
    RUN_TEST(batch3_special_27_production_lowering);
    RUN_TEST(batch3_heal_party_pp_restoration);
    RUN_TEST(batch3_heal_party_egg_pp_unchanged);
    
    // Batch 4 Special semantic op tests - Balance Overlays (IDs 79, 80, 81)
    RUN_TEST(batch4_special_79_production_lowering);
    RUN_TEST(batch4_special_80_production_lowering);
    RUN_TEST(batch4_special_81_production_lowering);
    RUN_TEST(batch4_balance_overlay_semantic_distinctions);
    RUN_TEST(batch4_no_sem_special_for_79_80_81);
    RUN_TEST(batch4_balance_overlay_no_script_result);
    
    // Batch 5 Special semantic op tests - CheckPokerus (ID 78), GameboyCheck (ID 102)
    RUN_TEST(batch5_special_78_production_lowering);
    RUN_TEST(batch5_special_102_production_lowering);
    RUN_TEST(batch5_special_144_remains_sem_special);
    RUN_TEST(batch5_no_sem_special_for_78_102);
    RUN_TEST(batch5_gameboy_check_absorption_proof);
    RUN_TEST(batch5_check_pokerus_script_result);
    
    // Batch 6 Special semantic op tests - StubbedTrainerRankings_Healings (ID 157)
    RUN_TEST(batch6_special_157_production_absorption);
    RUN_TEST(batch6_special_157_no_sem_special);
    RUN_TEST(batch6_unhandled_special_produces_sem_special);
    RUN_TEST(batch6_absorption_accounting_invariant);
    
    // Batch 7 Special semantic op tests - CheckMobileAdapterStatusSpecial (ID 160)
    RUN_TEST(batch7_special_160_production_lowering);
    RUN_TEST(batch7_special_160_no_sem_special);
    RUN_TEST(batch7_special_160_not_zero_instructions);
    RUN_TEST(batch7_special_160_overwrites_stale_script_var);
    RUN_TEST(batch7_special_160_branch_equivalence);
    
    // Corpus closure: Battle Tower deferred script tests
    RUN_TEST(corpus_battletowertext_produces_trainer_text);
    RUN_TEST(corpus_battletowertext_no_sem_special);
    RUN_TEST(corpus_battletowertext_distinct_from_normal_trainer_text);
    RUN_TEST(corpus_readmem_0xcf64_produces_read_state_var);
    RUN_TEST(corpus_readmem_nearby_addresses_rejected);
    RUN_TEST(corpus_callasm_0x9f5cb_produces_read_state_var);
    RUN_TEST(corpus_callasm_nearby_addresses_rejected);
    
    // Batch 8 Special semantic op tests - YesNo (163), DST (166, 167)
    RUN_TEST(batch8_special_163_emits_sem_yesno);
    RUN_TEST(batch8_special_163_no_sem_special);
    RUN_TEST(batch8_special_163_yesorno_equivalence);
    RUN_TEST(batch8_special_166_emits_dst_true);
    RUN_TEST(batch8_special_167_emits_dst_false);
    RUN_TEST(batch8_special_166_167_differ_by_enabled);
    RUN_TEST(batch8_special_166_167_no_sem_special);
    RUN_TEST(batch8_dst_operations_no_script_result);
    
    // Batch 9 Special semantic op tests - Block-Local ScriptVar Context (40, 57, 66, 67, 95, 152)
    RUN_TEST(batch9_setval_establishes_context);
    RUN_TEST(batch9_special_40_with_context);
    RUN_TEST(batch9_special_40_no_context_fallback);
    RUN_TEST(batch9_special_152_palette_normalization);
    RUN_TEST(batch9_special_152_invalid_encoding_rejected);
    RUN_TEST(batch9_special_152_all_source_valid_selectors_accepted);
    RUN_TEST(batch9_species_domain_from_profile_not_hardcoded);
    
    // Decoder unique command identity tests (loop/back-edge handling)
    RUN_TEST(decoder_unique_command_identity_loop);
    RUN_TEST(decoder_unique_command_identity_cfg_integrity);
    RUN_TEST(decoder_unique_command_identity_semantic_ir);
    
    // Simulation timing tests (Audit 8 - render/sim decoupling)
    RUN_TEST(timing_scheduler_basic);
    RUN_TEST(timing_scheduler_advance);
    RUN_TEST(timing_60hz_rendering_produces_consistent_ticks);
    RUN_TEST(timing_144hz_rendering_produces_same_ticks);
    RUN_TEST(timing_irregular_frames_same_result);
    RUN_TEST(timing_equivalent_elapsed_same_tick_count);
    
    // Input edge consumption adversarial tests (Audit 8)
    RUN_TEST(input_edge_one_press_one_tick_consumed_once);
    RUN_TEST(input_edge_one_press_four_ticks_consumed_once);
    RUN_TEST(input_edge_held_across_multiple_ticks);
    RUN_TEST(input_edge_release_consumed_once);
    RUN_TEST(input_edge_zero_tick_frame_preserves_press);
    RUN_TEST(input_edge_zero_tick_frame_preserves_release);
    RUN_TEST(input_edge_press_release_before_tick);
    RUN_TEST(input_edge_new_press_after_release);
    RUN_TEST(input_edge_multiple_render_frames_preserves_press);
    
    // Scheduler debt retention adversarial tests (Audit 8)
    RUN_TEST(scheduler_500ms_hitch_retains_debt);
    RUN_TEST(scheduler_2_second_hitch_retains_debt);
    RUN_TEST(scheduler_repeated_updates_catch_up);
    RUN_TEST(scheduler_total_ticks_equals_elapsed_time);
    
    // Coroutine lifecycle adversarial tests (Audit 7 fix verification)
    RUN_TEST(lua_coroutine_cleanup_via_resume);
    RUN_TEST(lua_coroutine_cleanup_via_resume_with_result);
    
    // NPC destination occupancy adversarial test (Audit 7 fix verification)
    RUN_TEST(npc_destination_occupancy_blocks_conflicting_movement);
    
    // Map event decode regression tests (Pre-RNG Semantic Fix Pass)
    RUN_TEST(coord_event_field_decode);
    RUN_TEST(bg_event_directional_types_preserved);
    RUN_TEST(bg_event_hidden_item_semantic_decode);
    RUN_TEST(bg_event_conditional_script_decode);
    RUN_TEST(object_event_palette_type_decode);
    RUN_TEST(script_resumed_no_resume_attempt);
    RUN_TEST(script_resumed_yielded_to_completed);
    RUN_TEST(script_resumed_yielded_to_yielded);
    
    // Timed yield tests - WaitFrames / WaitSeconds script_resumed tracking
    RUN_TEST(wait_frames_before_expiry_no_resume);
    // Move-safety regression: callback fires against live object, not stale moved-from address
    RUN_TEST(movement_callback_wires_to_live_object);
    // RTC
    RUN_TEST(rtc_hour_minute_from_offset);
    RUN_TEST(rtc_fake_clock_advance);
    RUN_TEST(rtc_set_clock_recomputes_offset);
    RUN_TEST(rtc_period_boundaries);
    RUN_TEST(rtc_weekday_derivation);
    RUN_TEST(rtc_midnight_daily_rollover);
    RUN_TEST(rtc_negative_offset);
    RUN_TEST(rtc_ticks_do_not_advance_rtc);
    RUN_TEST(rtc_save_load_preserves_offset);
    RUN_TEST(rtc_set_daylight_saving_adjusts_offset);
    RUN_TEST(rtc_dst_enable_twice_idempotent);
    RUN_TEST(rtc_dst_disable_twice_idempotent);
    RUN_TEST(rtc_dst_enable_set_clock_disable_net_reversal);
    RUN_TEST(scripted_coordinate_warp_uses_explicit_coords);
    RUN_TEST(warp_to_spawn_uses_gamestate_backup_warp);
    RUN_TEST(warp_to_spawn_no_fn_errors_explicitly);
    RUN_TEST(scripted_coordinate_warp_uses_explicit_coords);
    RUN_TEST(warp_to_spawn_uses_gamestate_backup_warp);
    RUN_TEST(warp_to_spawn_no_fn_errors_explicitly);
    RUN_TEST(door_auto_step_routes_through_movement_manager);
    RUN_TEST(crystal_npc_visibility_flag_set_means_hidden);
    RUN_TEST(show_hide_npc_persists_to_gamestate_flags);
    RUN_TEST(hide_npc_no_flag_is_transient);
    RUN_TEST(production_bootstrap_script_mutates_gamestate);
    RUN_TEST(no_gamestate_flags_fallback_to_stubs);
    RUN_TEST(lua_syntax_error_throws_explicitly);
    RUN_TEST(flag_identity_hex_canonical);
    RUN_TEST(flag_gamestate_set_clear_check_roundtrip);
    RUN_TEST(flag_map_condition_matches_script_set);
    RUN_TEST(wait_frames_expiry_sets_resumed);
    RUN_TEST(wait_frames_expiry_reyield_sets_resumed);
    RUN_TEST(wait_seconds_not_immediate_resume);
    RUN_TEST(wait_seconds_resumes_after_duration);
    RUN_TEST(wait_seconds_resume_sets_flag);
    RUN_TEST(wait_seconds_zero_duration);
    
    // WaitSeconds precision tests - integer tick conversion
    RUN_TEST(wait_seconds_precision_0_05s_is_3_ticks);
    RUN_TEST(wait_seconds_precision_0_1s_is_6_ticks);
    RUN_TEST(wait_seconds_precision_1_0s_is_60_ticks);
    
    // Coroutine identity tests - correct resume attribution
    RUN_TEST(unrelated_coroutine_resume_does_not_set_script_resumed);
    RUN_TEST(active_coroutine_timed_resume_sets_script_resumed);
    RUN_TEST(active_coroutine_resume_reyield_sets_script_resumed);
    
    // Script lifecycle tests - completion vs error distinction, reset cleanup
    RUN_TEST(script_finishes_normally_sets_complete);
    RUN_TEST(script_errors_after_resume_sets_error);
    RUN_TEST(script_errors_immediately_returns_false);
    RUN_TEST(script_runtime_error_during_start_returns_false);
    RUN_TEST(yielded_script_remains_nonterminal);
    RUN_TEST(reset_cancels_active_coroutine_no_timed_resume);
    RUN_TEST(reset_when_no_script_active);
    RUN_TEST(reset_after_script_completed);
    RUN_TEST(reset_when_script_yielded);
    
    RUN_TEST(coord_event_scripts_in_corpus);
    
    // Collision classifier adversarial tests (Pre-RNG cleanup)
    RUN_TEST(collision_classifier_adversarial_misclassified_ids);
    RUN_TEST(collision_classifier_source_proven_constants);
    
    // Pre-RNG correctness regression tests
    RUN_TEST(script_yielded_locks_input);
    RUN_TEST(tileset_id_bounds_0_1_36_37);
    RUN_TEST(duplicate_physical_binding_release);
    RUN_TEST(special_tileset_fixed_palette_extracted);
    
    // BG condition flag evaluation adversarial tests
    RUN_TEST(bg_event_ifset_unset_blocked);
    RUN_TEST(bg_event_ifset_set_triggers);
    RUN_TEST(bg_event_ifnotset_unset_triggers);
    RUN_TEST(bg_event_ifnotset_set_blocked);
    RUN_TEST(bg_event_hidden_item_uncollected_triggers);
    RUN_TEST(bg_event_hidden_item_collected_blocked);
    
    // Final pre-RNG correctness tests (semantic gap closure)
    RUN_TEST(sprite_id_mapping_authoritative);
    RUN_TEST(directional_ledge_semantic_preservation);
    
    // Script state and dynamic resource semantics tests (August 2026 — Findings 1-5)
    RUN_TEST(stale_script_var_yesorno_invalidates_before_map_radio);
    RUN_TEST(script_var_propagates_across_non_writer);
    RUN_TEST(stale_script_var_giveitem_invalidates);
    RUN_TEST(cry_literal_species);
    RUN_TEST(cry_zero_dynamic_species);
    RUN_TEST(cry_script_var_distinct_from_literal);
    RUN_TEST(pokepic_literal_species);
    RUN_TEST(pokepic_zero_dynamic_species);
    RUN_TEST(cry_and_pokepic_same_source_semantics);
    RUN_TEST(writecmdqueue_same_ptr_different_banks_distinct);
    RUN_TEST(movement_valid_terminates_correctly);
    RUN_TEST(movement_no_terminator_throws);

    // Script state and dynamic resource semantics tests (August 2026 — 5 Findings)
    RUN_TEST(script_state_setval_yesorno_invalidates_context);
    RUN_TEST(script_state_setval_preserved_across_noop_command);
    RUN_TEST(script_state_cry_zero_is_script_var_not_literal);
    RUN_TEST(script_state_cry_nonzero_is_literal);
    RUN_TEST(script_state_pokepic_zero_is_script_var_not_literal);
    RUN_TEST(script_state_pokepic_nonzero_is_literal);
    RUN_TEST(script_state_cry_and_pokepic_same_source_semantics);
    RUN_TEST(script_state_verbosegiveitemvar_invalidates);
    RUN_TEST(script_state_checkcellnum_invalidates);
    RUN_TEST(script_state_delcmdqueue_invalidates);
    RUN_TEST(script_state_checkphonecall_invalidates);
    RUN_TEST(script_state_checktime_invalidates);
    RUN_TEST(script_state_checkver_establishes_context);

    // Crystal text frontend fidelity tests (August 2026 — Findings 1-4)
    RUN_TEST(text_literal_tx_opcode_overlap_0x14);
    RUN_TEST(text_literal_at_returns_to_outer_stream);
    RUN_TEST(text_resource_can_begin_with_dynamic_command);
    RUN_TEST(tx_box_height_width_semantics);
    RUN_TEST(tx_far_identity_distinguishes_bank);
    RUN_TEST(tx_far_identity_distinguishes_address);
    RUN_TEST(tx_far_dedup_different_bank_gets_different_id);
    RUN_TEST(textraw_identity_distinguishes_contents);
    RUN_TEST(textraw_identity_identical_contents_match);
    RUN_TEST(textraw_empty_raw_handled);

    // Canonical bank address helper tests (August 2026)
    RUN_TEST(bank_utils_flat_to_bank_zero);
    RUN_TEST(bank_utils_flat_to_bank_one);
    RUN_TEST(bank_utils_flat_to_bank_nonzero);
    RUN_TEST(bank_utils_bank_to_flat_ptr_at_4000);
    RUN_TEST(bank_utils_bank_to_flat_ptr_at_7fff);
    RUN_TEST(bank_utils_bank_to_flat_ptr_in_rom0);
    RUN_TEST(bank_utils_round_trip);
    RUN_TEST(bank_utils_local_ptr_to_flat_sdefer_nonzero_bank);
    RUN_TEST(bank_utils_local_ptr_to_flat_getstring_nonzero_bank);
    RUN_TEST(bank_utils_sdefer_lowering_matches_canonical_helper);
    RUN_TEST(bank_utils_getstring_lowering_matches_canonical_helper);

    // Semantic correctness fix tests (August 2026)
    RUN_TEST(semantic_fix_gettrainername_preserves_both_operands);
    RUN_TEST(semantic_fix_getstring_preserves_text_pointer);
    RUN_TEST(semantic_fix_getmoney_preserves_account);
    RUN_TEST(semantic_fix_encountermusic_distinct_from_playmapmusic);
    RUN_TEST(semantic_fix_newloadmap_preserves_method);
    RUN_TEST(semantic_fix_reanchormap_distinct_from_refreshmap);
    RUN_TEST(semantic_fix_refreshmap_distinct_from_reanchormap);
    RUN_TEST(semantic_fix_sdefer_bank_resolution);
    RUN_TEST(semantic_fix_text_identity_distinguishes_controls);
    RUN_TEST(semantic_fix_text_identity_distinguishes_ram_addresses);
    RUN_TEST(text_string_buffer_id4_maps_to_arg_slot4);
    RUN_TEST(text_string_buffer_id0_maps_to_arg_slot0);
    RUN_TEST(text_string_buffer_id6_maps_to_arg_slot6);
    RUN_TEST(text_string_buffer_invalid_id7_hard_fails);
    RUN_TEST(text_string_buffer_invalid_id255_hard_fails);
    RUN_TEST(text_tx_ram_wstringbuffer3_maps_to_arg_slot0);
    RUN_TEST(text_tx_ram_wstringbuffer4_maps_to_arg_slot1);
    RUN_TEST(text_tx_ram_wstringbuffer5_maps_to_arg_slot2);
    RUN_TEST(text_tx_ram_unknown_address_hard_fails);
    RUN_TEST(text_tx_bcd_hard_fails);
    RUN_TEST(text_tx_decimal_wscriptvar_produces_script_var_decimal);
    RUN_TEST(text_tx_decimal_unknown_address_hard_fails);
    RUN_TEST(text_tx_far_inlines_referenced_text);
    RUN_TEST(text_tx_far_without_registry_hard_fails);
    RUN_TEST(text_tx_day_produces_day_op);
    RUN_TEST(text_tx_sound_item_produces_typed_sound_kind);
    RUN_TEST(text_tx_sound_fanfare_produces_typed_sound_kind);
    RUN_TEST(text_presentation_ops_dropped_not_failed);
    RUN_TEST(text_tx_raw_unknown_opcode_hard_fails);
    RUN_TEST(text_arg_slot_numbering_table_driven);
    RUN_TEST(text_ram_source_domain_distinct_from_arg_domain);
    RUN_TEST(text_arg_slot_wplayername_hard_fails);
    RUN_TEST(text_tx_prompt_button_produces_inline_prompt_button_not_dropped);
    RUN_TEST(text_tx_prompt_button_standalone_produces_single_element);
    RUN_TEST(text_tx_pause_produces_pause_with_30_frames);
    RUN_TEST(text_tx_pause_frame_count_preserved_explicitly);
    RUN_TEST(text_enemy_nickname_is_ram_source_not_arg);
    RUN_TEST(text_battle_nickname_is_ram_source_not_arg);
    RUN_TEST(text_prepared_string2_is_ram_source_not_arg);
    RUN_TEST(sem_game_specific_event_writes_var_flag_blocks_constant_propagation);
    RUN_TEST(sem_game_specific_event_no_write_preserves_context);
    RUN_TEST(sem_game_specific_event_behavior_name_is_source_proven_not_raw_id);

    // 11-Finding semantic fidelity pass tests (August 2026)
    RUN_TEST(fidelity11_writetext_distinct_pointers_distinct_sequences);
    RUN_TEST(fidelity11_jumptext_distinct_from_writetext);
    RUN_TEST(fidelity11_jumptextfaceplayer_preserves_text);
    RUN_TEST(fidelity11_endall_distinct_from_end);
    RUN_TEST(fidelity11_catchtutorial_distinct_from_startbattle);
    RUN_TEST(fidelity11_catchtutorial_preserves_type_byte);
    RUN_TEST(fidelity11_loadmenu_preserves_header_pointer);
    RUN_TEST(fidelity11_verticalmenu_distinct_from_2dmenu);
    RUN_TEST(fidelity11_deactivatefacing_distinct_from_pause);
    RUN_TEST(fidelity11_verbosegiveitemvar_variable_semantics);
    RUN_TEST(fidelity11_askforphonenumber_distinct_from_addphonenumber);
    RUN_TEST(fidelity11_promptbutton_distinct_from_waitbutton);
    RUN_TEST(fidelity11_getname_type1_pokemon);
    RUN_TEST(fidelity11_getname_type2_move);
    RUN_TEST(fidelity11_getname_type3_dummy_not_item);
    RUN_TEST(fidelity11_getname_type4_item_not_trainer);
    RUN_TEST(fidelity11_getname_type5_partyot);
    RUN_TEST(fidelity11_getname_type7_trainer);
    RUN_TEST(fidelity11_invalid_domain_gates_linker);

    // Pre-Oracle semantic cleanup tests (August 2026 — 6 Fixes)
    RUN_TEST(batch10_checksave_invalidates_context);
    RUN_TEST(batch10_startbattle_invalidates_context);
    RUN_TEST(batch10_checkpoke_invalidates_context);
    RUN_TEST(batch10_givepoke_invalidates_context);
    RUN_TEST(batch10_giveegg_invalidates_context);
    RUN_TEST(batch10_check_pokerus_invalidates_context);
    RUN_TEST(batch10_checkwarp_preserves_context);
    RUN_TEST(batch10_pocketisfull_emits_notify_not_absorbed);
    RUN_TEST(batch10_pocketisfull_does_not_invalidate_context);
    RUN_TEST(batch10_show_text_empty_sequence_fails_legality);
    RUN_TEST(batch10_show_text_nonempty_sequence_passes_legality);
    RUN_TEST(batch10_getcoins_uses_coins_source);
    RUN_TEST(batch10_getnum_uses_scriptvar_source);
    RUN_TEST(batch10_getmoney_uses_typed_money_account);
    RUN_TEST(batch10_number_sources_are_distinct);
    RUN_TEST(sem_special_rejected_by_stage5_legality_gate);
    RUN_TEST(sem_special_clean_ir_still_passes_legality);

    // F1-F8 production fix tests
    RUN_TEST(emitter_jumpstd_no_crash);
    RUN_TEST(emitter_callstd_no_crash);
    RUN_TEST(emitter_special_no_crash);
    RUN_TEST(emitter_warp_to_spawn_has_binding);
    RUN_TEST(f1_turnobject_direction_preserved_right);
    RUN_TEST(f1_turnobject_direction_preserved_up);
    RUN_TEST(f4_scripted_warp_coordinates_preserved);
    RUN_TEST(f3_givepoke_held_item_preserved);
    RUN_TEST(f8_money_account_player_vs_mom);
    RUN_TEST(f2_text_ram_op_preserved_not_dropped);
    RUN_TEST(native_text_from_runtime_ram_preserves_operands);
    RUN_TEST(native_text_from_runtime_decimal_preserves_operands);
    RUN_TEST(native_text_from_runtime_unsupported_throws);
    RUN_TEST(native_text_from_runtime_no_silent_blank_fallthrough);
    RUN_TEST(f6_canonical_rng_not_reset_on_map_transition);
    RUN_TEST(f7_save_invalid_direction_rejected);

    // Icon format source-fidelity tests
    RUN_TEST(icon_format_16x16_geometry);
    RUN_TEST(icon_format_128_bytes_total);
    RUN_TEST(icon_format_pikachu_pixel_hash);
    RUN_TEST(icon_format_bigmon_packaged_via_closure);

    // GameplayRng (PCG-XSH-RR) adversarial deterministic tests
    RUN_TEST(pcg_known_sequence_seed_deadbeef);
    RUN_TEST(pcg_next_u8_draw_count);
    RUN_TEST(pcg_next_u64_hi_lo_ordering);
    RUN_TEST(pcg_seed_zero_known_state);
    RUN_TEST(pcg_bounded_representative_value);
    RUN_TEST(pcg_bounded_one_always_zero);
    RUN_TEST(pcg_bounded_zero_throws_no_draw);
    RUN_TEST(pcg_save_load_exact_continuation);
    RUN_TEST(pcg_map_transition_does_not_perturb_canonical);
    RUN_TEST(pcg_presentation_rng_does_not_perturb_canonical);

    // NPC canonical RNG + random_chance adversarial tests
    RUN_TEST(pcg_npc_movement_uses_canonical_rng);
    RUN_TEST(pcg_npc_save_load_canonical_continuation);
    RUN_TEST(random_chance_correct_probability_contract);
    RUN_TEST(random_chance_invalid_percent_throws);
    RUN_TEST(no_second_authoritative_rng_stream);
    RUN_TEST(pcg_dv_draw_count_two_semantic);
    RUN_TEST(pcg_v4_migration_deterministic);
    RUN_TEST(pcg_v5_save_roundtrip);
    // GameSpecificEvent capability boundary adversarial tests
    RUN_TEST(behavior_sdefer_routes_to_scheduler);
    RUN_TEST(behavior_known_unimplemented_errors_explicitly);
    RUN_TEST(behavior_unknown_unregistered_errors_explicitly);
    RUN_TEST(behavior_writes_script_var_errors_before_branch);
    RUN_TEST(sem_special_still_rejected_after_registry_cleanup);
    // Max-compat + Species Finder adversarial tests
    RUN_TEST(compat_exact_hash_gives_exacthash_match);
    RUN_TEST(compat_modified_hash_layout_valid_not_rejected);
    RUN_TEST(compat_incompatible_profile_fails_explicitly);
    RUN_TEST(species_finder_stock_count_is_251);
    RUN_TEST(species_finder_bulbasaur_record_correct);
    RUN_TEST(species_finder_last_record_is_mew);
    RUN_TEST(species_finder_non251_profile_same_path);
    RUN_TEST(species_linker_refs_are_exact_resolved);
    RUN_TEST(species_linker_unknown_species_invalid_domain);
    RUN_TEST(daycare_species_252_save_load_accepted);
    RUN_TEST(species_icon_map_covers_full_domain);
    // Map event ↔ script ID namespace adversarial tests
    RUN_TEST(event_script_id_canonical_format_npc);
    RUN_TEST(event_script_id_canonical_format_bg);
    RUN_TEST(event_script_id_canonical_format_coord);
    RUN_TEST(event_script_id_missing_fails_explicitly);
    RUN_TEST(event_script_id_no_local_positional_survives);
    // Script VM P0 adversarial tests
    RUN_TEST(vm_result_zero_takes_false_branch);
    RUN_TEST(vm_result_one_takes_true_branch);
    RUN_TEST(vm_result_nonzero_integer_true);
    RUN_TEST(vm_setvar_from_result_zero_stores_zero);
    RUN_TEST(vm_call_returns_to_continuation);
    RUN_TEST(vm_nested_calls_unwind_correctly);
    RUN_TEST(vm_callee_end_does_not_exit_top_level);
    RUN_TEST(vm_endall_inside_nested_call_terminates);
    RUN_TEST(vm_sdefer_cleared_on_rebind);
    RUN_TEST(vm_sdefer_cleared_on_loop_destroy);
    RUN_TEST(vm_deferred_failure_propagates_error);

    // Capability closure E2E tests
    RUN_TEST(capability_show_hide_npc_updates_npc_state);
    RUN_TEST(capability_face_actor_updates_npc_facing);
    RUN_TEST(capability_face_actor_player_updates_player_facing);
    RUN_TEST(capability_face_player_updates_npc_facing_toward_player);
    RUN_TEST(capability_teleport_npc_updates_npc_position);
    RUN_TEST(capability_set_last_talked_records_id);
    RUN_TEST(capability_set_scene_persists_in_game_state);
    RUN_TEST(capability_state_var_persists_in_game_state);

    // Summary
    std::cout << "\n=== Results ===\n";
    std::cout << std::dec << "Passed: " << g_tests_passed << "\n";
    std::cout << "Failed: " << g_tests_failed << "\n";
    
    return g_tests_failed > 0 ? 1 : 0;
}
