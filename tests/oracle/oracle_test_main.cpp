#include "oracle_shared.hpp"

// =============================================================================
// Forward declarations for all test functions (defined in other TUs).
// Required because RUN_TEST takes a function pointer.
// =============================================================================
// Phase 1 / Package seam / F-series tests (oracle_test_p1_p4.cpp)
void test_fixture_operand_order_gettrainername();
void test_fixture_flag_namespace_event_vs_engine();
void test_fixture_text_tx_ram_mixed();
void test_fixture_text_tx_decimal();
void test_fixture_movement_step_dig();
void test_fixture_movement_skyfall_top();
void test_fixture_sdefer_bank_resolution();
void test_fixture_connection_offset_direction();
void test_negative_truncated_operand_fails_explicitly();
void test_negative_invalid_movement_opcode_throws();
void test_package_seam_bg_event_ifset_condition_flag();
void test_package_seam_sprite_id_boundary();
void test_package_seam_map_connection_direction_offset();
void test_f3_oversized_map_id_throws_before_write();
void test_f3_normal_id_writes_correctly();
void test_f4_block_count_overflow_returns_nullopt();
void test_f4_invalid_connection_direction_returns_nullopt();
void test_f4_valid_map_deserializes_correctly();
void test_pkg_reader_malformed_warp_string_fails_closed();
void test_pkg_reader_malformed_bgevent_type_fails_closed();
void test_pkg_reader_malformed_object_string_fails_closed();
void test_collision_chunk_removed_tileset_carries_collision();
void test_f1_tileset_truncated_tile_data_returns_nullopt();
void test_f1_tileset_truncated_block_data_returns_nullopt();
void test_f1_tileset_truncated_palette_section_returns_nullopt();
void test_f1_tileset_valid_minimal_roundtrips();
void test_f2_duplicate_map_id_throws();
void test_f2_duplicate_sprite_id_throws();
void test_f2_external_package_duplicate_id_rejected();
void test_f3_valid_cached_package_accepted();
void test_f3_damaged_cached_package_rejected_as_miss();
void test_f4_package_header_layout_runtime_verify();
// Phase 2
void test_p2_event_zero_and_one_byte_ops();
void test_p2_event_word_operand_ops();
void test_p2_event_multi_byte_ops();
void test_p2_event_pointer_and_branch_ops();
void test_p2_movement_directional_family();
void test_p2_movement_parameterized_family();
void test_p2_movement_non_directional_misc();
void test_p2_text_tx_box_and_bcd();
void test_p2_text_tx_stringbuffer_and_far();
void test_p2_text_literal_overlap_opcodes();
void test_p2_negative_truncated_script_operand_produces_wrong_value();
void test_p2_negative_truncated_tx_operand_produces_wrong_value();
// Phase 3
void test_p3_s1_end_vs_endall_distinct_types();
void test_p3_s2_waitbutton_vs_promptbutton_distinct();
void test_p3_s3_askforphone_vs_addphone_distinct();
void test_p3_s4_newloadmap_method_preserved();
void test_p3_s5_catchtutorial_distinct_from_startbattle();
void test_p3_s6_deactivatefacing_distinct_from_pause();
void test_p3_s7_verbosegiveitemvar_semantics();
void test_p3_s8_s9_speciesource_literal_vs_scriptvar();
void test_p3_s10_menu_variants_distinct();
void test_p3_p1_bgevent_type_roundtrip();
void test_p3_p2_connection_signed_strip_offset_roundtrip();
void test_p3_p3_object_event_fields_roundtrip();
void test_p3_l1_flag_namespace_linker_distinct();
void test_p3_l4_scriptvar_species_no_linker_reference();
void test_p3_ser1_signed_offset_boundary_values();
void test_p3_ser_conn_three_fields_independent_roundtrip();
void test_p3_ser2_sprite_id_string_boundary();
// Phase 4
void test_p4_1_setflag_engine_flypoint_new_bark();
void test_p4_2_givepoke_cyndaquil_level5_berry();
void test_p4_3_warp_elmslab_x6_y3();
void test_p4_4_promptbutton_distinct_pause_preserved();
void test_p4_5_sdefer_deferred_body_is_separate_root();
void test_p4_6_variablesprite_copycat_lass();
void test_p4_7_icon_format_source_fidelity();
void test_p4_8_fullcompiler_package_seam();
void test_p4_rgbds_bank_correct();
void test_p4_rgbds_bank_wrong_decimal_gives_wrong_bytes();
void test_p4_symbol_map_parses_bank_as_hex();
// Phase 5 (oracle_test_p5_p55.cpp)
void test_p5_vm_numeric_result_zero_false_branch();
void test_p5_vm_numeric_result_nonzero_true_branch();
void test_p5_vm_result_zero_stored_back_remains_zero();
void test_p5_vm_scall_returns_to_caller_continuation();
void test_p5_vm_nested_scall_unwinds_all_frames();
void test_p5_vm_endall_terminates_all_frames();
void test_p5_npc_interaction_starts_packaged_script();
void test_p5_bg_interaction_executes_packaged_script();
void test_p5_coord_event_canonical_ids_in_package();
void test_p5_deferred_script_executes_after_trigger();
void test_p5_deferred_missing_fails_explicitly();
void test_p5_state_flag_set_check_clear();
void test_p5_state_variable_set_check();
void test_p5_state_scene_set_check();
void test_p5_state_money_mutate_check();
void test_p5_state_random_branch_canonical_rng();
void test_p5_negative_missing_script_explicit_failure();
void test_p5_negative_unimplemented_behavior_explicit_failure();
// Phase 5.5
void test_p55_flag_setflag_endcallback_nbt_flypoint();
void test_p55_branch_checkevent_teacher_flag_set();
void test_p55_branch_checkevent_teacher_no_flags();
void test_p55_scall_real_aide_walk_potion();
void test_p55_nested_scall_aide_give_balls();
void test_p55_hardened_elm_directions_behavioral();
void test_p55_hardened_rng_branch_deterministic();
void test_p55_hardened_endall_compiler_path_structural();
void test_p55_endall_emits_core_vm_not_behavior_table();
void test_p55_checkscene_elmslab_callback();
void test_p55_random_branch_olivine_youngster();
void test_p55_package_reference_canonical_id_elmslab_bg();
void test_p55_item_verbosegiveitem_aide_potion();
void test_p55_save_load_flag_continuity();
void test_p55_negative_pokepic_capability_deferred();
void test_p55_endall_no_behavior_table_in_corpus();
void test_p55_closure_scall_abc();
void test_p55_closure_farscall_fullpipe();
void test_p55_closure_endall_behavioral();

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <rom_path>\n";
        std::cerr << "Note: ROM path is required for context (not used by most oracle tests)\n";
        return 1;
    }

    std::cout << "\n=== Crystal Frontend Oracle — Phase 1 ===\n\n";

    // Load ROM + profile for tests that need live extraction
    auto rom_owned = crystal::RomData::load(argv[1]);
    if (rom_owned) {
        auto& registry = crystal::ProfileRegistry::instance();
        auto profile = registry.get_profile_by_hash(rom_owned->hash());
        if (profile) {
            g_rom     = rom_owned.get();
            g_profile = profile;
        } else {
            std::cerr << "Warning: ROM hash not in ProfileRegistry — live extraction tests will be skipped\n";
        }
    } else {
        std::cerr << "Warning: Could not load ROM — live extraction tests will be skipped\n";
    }

    // =========================================================================
    // Phase 5: compile the oracle package ONCE, shared by all P5 tests.
    // Compile is expensive (~100ms), so we do it once at startup rather than
    // once per test.  If compile fails, Phase 5 tests are skipped gracefully.
    // =========================================================================
    if (g_rom && g_profile) {
        std::cout << "[Phase 5] Compiling oracle package (this may take a moment)...\n";
        std::cout.flush();
        auto pkg_path = std::filesystem::temp_directory_path() / "oracle_p5_fullpipe.emon";
        std::filesystem::remove(pkg_path);
        crystal::FullGameCompiler oracle_compiler(*g_rom, *g_profile);
        crystal::FullCompilerConfig oracle_cfg;
        oracle_cfg.use_package_cache = false;
        oracle_cfg.worker_count = 1;
        bool oracle_ok = oracle_compiler.compile(pkg_path, oracle_cfg);
        if (oracle_ok && std::filesystem::exists(pkg_path)) {
            g_oracle_package_path = pkg_path;
            g_oracle_reader = enginemon::PackageReader::open(pkg_path);
            if (g_oracle_reader) {
                std::cout << "[Phase 5] Oracle package ready: " << pkg_path << "\n\n";
            } else {
                std::cerr << "[Phase 5] Warning: compiled but PackageReader::open failed\n";
            }
        } else {
            std::cerr << "[Phase 5] Warning: oracle compile failed — Phase 5 tests will be skipped\n";
        }
    }

    // Binary-layout + lowering fixtures (decode from hand-authored bytes)
    RUN_TEST(fixture_operand_order_gettrainername);
    RUN_TEST(fixture_flag_namespace_event_vs_engine);
    RUN_TEST(fixture_text_tx_ram_mixed);
    RUN_TEST(fixture_text_tx_decimal);
    RUN_TEST(fixture_movement_step_dig);
    RUN_TEST(fixture_movement_skyfall_top);
    RUN_TEST(fixture_sdefer_bank_resolution);

    // Phase 1.5: connection binary-layout fixture
    RUN_TEST(fixture_connection_offset_direction);

    // Negative fixtures — must fail explicitly
    RUN_TEST(negative_truncated_operand_fails_explicitly);
    RUN_TEST(negative_invalid_movement_opcode_throws);

    // Package seam fixtures (Oracle Phase 1)
    RUN_TEST(package_seam_bg_event_ifset_condition_flag);
    RUN_TEST(package_seam_sprite_id_boundary);
    RUN_TEST(package_seam_map_connection_direction_offset);

    // F3: Package length narrowing — oversized IDs must throw before write
    RUN_TEST(f3_oversized_map_id_throws_before_write);
    RUN_TEST(f3_normal_id_writes_correctly);

    // F4: Fail-soft deserialization — malformed payloads return nullopt
    RUN_TEST(f4_block_count_overflow_returns_nullopt);
    RUN_TEST(f4_invalid_connection_direction_returns_nullopt);
    RUN_TEST(f4_valid_map_deserializes_correctly);
    RUN_TEST(pkg_reader_malformed_warp_string_fails_closed);
    RUN_TEST(pkg_reader_malformed_bgevent_type_fails_closed);
    RUN_TEST(pkg_reader_malformed_object_string_fails_closed);
    RUN_TEST(collision_chunk_removed_tileset_carries_collision);

    // F1–F4 Runtime package/cache integrity hardening
    RUN_TEST(f1_tileset_truncated_tile_data_returns_nullopt);
    RUN_TEST(f1_tileset_truncated_block_data_returns_nullopt);
    RUN_TEST(f1_tileset_truncated_palette_section_returns_nullopt);
    RUN_TEST(f1_tileset_valid_minimal_roundtrips);
    RUN_TEST(f2_duplicate_map_id_throws);
    RUN_TEST(f2_duplicate_sprite_id_throws);
    RUN_TEST(f2_external_package_duplicate_id_rejected);
    RUN_TEST(f3_valid_cached_package_accepted);
    RUN_TEST(f3_damaged_cached_package_rejected_as_miss);
    RUN_TEST(f4_package_header_layout_runtime_verify);

    // =========================================================================
    // Oracle Phase 2 — Structural Breadth
    // =========================================================================

    // Event opcode structural coverage
    RUN_TEST(p2_event_zero_and_one_byte_ops);
    RUN_TEST(p2_event_word_operand_ops);
    RUN_TEST(p2_event_multi_byte_ops);
    RUN_TEST(p2_event_pointer_and_branch_ops);

    // Movement structural coverage
    RUN_TEST(p2_movement_directional_family);
    RUN_TEST(p2_movement_parameterized_family);
    RUN_TEST(p2_movement_non_directional_misc);

    // Text structural coverage
    RUN_TEST(p2_text_tx_box_and_bcd);
    RUN_TEST(p2_text_tx_stringbuffer_and_far);
    RUN_TEST(p2_text_literal_overlap_opcodes);

    // Negative structural coverage
    RUN_TEST(p2_negative_truncated_script_operand_produces_wrong_value);
    RUN_TEST(p2_negative_truncated_tx_operand_produces_wrong_value);

    // =========================================================================
    // Oracle Phase 3 — Semantic + Package Seam Breadth
    // =========================================================================

    // Semantic distinction fixtures
    RUN_TEST(p3_s1_end_vs_endall_distinct_types);
    RUN_TEST(p3_s2_waitbutton_vs_promptbutton_distinct);
    RUN_TEST(p3_s3_askforphone_vs_addphone_distinct);
    RUN_TEST(p3_s4_newloadmap_method_preserved);
    RUN_TEST(p3_s5_catchtutorial_distinct_from_startbattle);
    RUN_TEST(p3_s6_deactivatefacing_distinct_from_pause);
    RUN_TEST(p3_s7_verbosegiveitemvar_semantics);
    RUN_TEST(p3_s8_s9_speciesource_literal_vs_scriptvar);
    RUN_TEST(p3_s10_menu_variants_distinct);

    // Package seam breadth
    RUN_TEST(p3_p1_bgevent_type_roundtrip);
    RUN_TEST(p3_p2_connection_signed_strip_offset_roundtrip);
    RUN_TEST(p3_p3_object_event_fields_roundtrip);

    // Linker / reference domain
    RUN_TEST(p3_l1_flag_namespace_linker_distinct);
    RUN_TEST(p3_l4_scriptvar_species_no_linker_reference);

    // Serialization boundaries
    RUN_TEST(p3_ser1_signed_offset_boundary_values);
    RUN_TEST(p3_ser_conn_three_fields_independent_roundtrip);
    RUN_TEST(p3_ser2_sprite_id_string_boundary);

    // =========================================================================
    // Oracle Phase 4 — Vertical Slices: source→decode→lower→observable effect
    // =========================================================================
    // All expected values are hand-authored from pokecrystal source and
    // RGBDS-verified byte encodings.  NEVER derived from Enginemon output.
    //
    // Source provenance per test:
    //   P4-1: setflag ENGINE_FLYPOINT_NEW_BARK   macros/scripts/events.asm $36 dw
    //         constants/engine_flags.asm          ENGINE_FLYPOINT_NEW_BARK=65
    //         maps/NewBarkTown.asm                NewBarkTownFlypointCallback
    //   P4-2: givepoke CYNDAQUIL 5 BERRY         macros/scripts/events.asm $2d
    //         constants/pokemon_constants.asm     CYNDAQUIL=$9B
    //         constants/item_constants.asm        BERRY=$AD
    //         maps/ElmsLab.asm                    CyndaquilPokeBallScript
    //   P4-3: warp ELMS_LAB 6 3                  macros/scripts/events.asm $3c
    //         constants/map_constants.asm         ELMS_LAB group=24 map=5
    //         maps/NewBarkTown.asm                warp_event 6,3,ELMS_LAB,1
    //   P4-4: promptbutton + pause 30             macros/scripts/events.asm $55 $8B
    //         maps/ElmsLab.asm                    ElmsLabWalkUpToElmScript/HealingMachine
    //   P4-5: sdefer ElmsLabWalkUpToElmScript     macros/scripts/events.asm $8D dw
    //         maps/ElmsLab.asm                    ElmsLabMeetElmScene
    //   P4-6: variablesprite SPRITE_COPYCAT LASS  macros/scripts/events.asm $6D
    //         constants/sprite_constants.asm      SPRITE_VARS=$F0 SPRITE_COPYCAT=$FB
    //         maps/CopycatsHouse2F.asm            Copycat script
    //   P4-7: PokémonIcon 128-byte payload        pokecrystal GetIcon lb bc BANK(Icons) 8
    //         data/sprite_anims/oam.asm            OAMData_RedWalk 4 OBJ 2×2 → 16×16
    //   P4-8: FullGameCompiler package seam       real ROM → compile → PackageReader
    //         ScriptId exists, Lua non-empty, package assets present
    // =========================================================================
    RUN_TEST(p4_1_setflag_engine_flypoint_new_bark);
    RUN_TEST(p4_2_givepoke_cyndaquil_level5_berry);
    RUN_TEST(p4_3_warp_elmslab_x6_y3);
    RUN_TEST(p4_4_promptbutton_distinct_pause_preserved);
    RUN_TEST(p4_5_sdefer_deferred_body_is_separate_root);
    RUN_TEST(p4_6_variablesprite_copycat_lass);
    RUN_TEST(p4_7_icon_format_source_fidelity);
    RUN_TEST(p4_8_fullcompiler_package_seam);

    // RGBDS bank notation adversarial tests
    RUN_TEST(p4_rgbds_bank_correct);
    RUN_TEST(p4_rgbds_bank_wrong_decimal_gives_wrong_bytes);
    RUN_TEST(p4_symbol_map_parses_bank_as_hex);

    // =========================================================================
    // Oracle Phase 5 — Full-Pipe End-to-End Execution Oracle
    // =========================================================================
    // Production path: ROM → FullGameCompiler → EMON package → PackageReader
    //   → LuaRuntime → HeadlessGameLoop → GameState observation.
    //
    // NO hand-constructed SemanticIR. NO fake package reader. NO mocked Lua.
    // All tests traverse the real Stage 7 emitter and runtime bindings.
    //
    // The package is compiled once at test startup (g_oracle_package_path) and
    // shared across all Phase 5 tests via g_oracle_reader.
    //
    // Source provenance for expected values:
    //   P5-VM-*:   VM laws independently specified (not derived from Enginemon output)
    //   P5-NPC:    NewBarkTownTeacherScript — bank 0x6A, addr 0x406F
    //              pokecrystal/maps/NewBarkTown.asm
    //   P5-BG:     NewBarkTownSign — bank 0x6A, addr 0x40C8
    //              pokecrystal/maps/NewBarkTown.asm
    //   P5-COORD:  NewBarkTown coord events — 2 total per golden_test.cpp
    //              pokecrystal/maps/NewBarkTown.asm (coord_event entries)
    //   P5-DEFER:  sdefer semantics from pokecrystal Script_sdefer (opcode 0x86)
    //   P5-STATE-*: flag/var/scene/money/item/rng from Crystal Gen 2 mechanics
    // =========================================================================
    if (g_oracle_reader) {
        RUN_TEST(p5_vm_numeric_result_zero_false_branch);
        RUN_TEST(p5_vm_numeric_result_nonzero_true_branch);
        RUN_TEST(p5_vm_result_zero_stored_back_remains_zero);
        RUN_TEST(p5_vm_scall_returns_to_caller_continuation);
        RUN_TEST(p5_vm_nested_scall_unwinds_all_frames);
        RUN_TEST(p5_vm_endall_terminates_all_frames);
        RUN_TEST(p5_npc_interaction_starts_packaged_script);
        RUN_TEST(p5_bg_interaction_executes_packaged_script);
        RUN_TEST(p5_coord_event_canonical_ids_in_package);
        RUN_TEST(p5_deferred_script_executes_after_trigger);
        RUN_TEST(p5_deferred_missing_fails_explicitly);
        RUN_TEST(p5_state_flag_set_check_clear);
        RUN_TEST(p5_state_variable_set_check);
        RUN_TEST(p5_state_scene_set_check);
        RUN_TEST(p5_state_money_mutate_check);
        RUN_TEST(p5_state_random_branch_canonical_rng);
        RUN_TEST(p5_negative_missing_script_explicit_failure);
        RUN_TEST(p5_negative_unimplemented_behavior_explicit_failure);
    } else {
        std::cout << "[Phase 5] SKIP: oracle package not available (compile failed or ROM not loaded)\n";
    }

    // =========================================================================
    // Oracle Phase 5.5 — Full-Pipe Compiler-to-Runtime Semantic Verticals
    // =========================================================================
    if (g_oracle_reader) {
        RUN_TEST(p55_flag_setflag_endcallback_nbt_flypoint);
        RUN_TEST(p55_branch_checkevent_teacher_flag_set);
        RUN_TEST(p55_branch_checkevent_teacher_no_flags);
        RUN_TEST(p55_scall_real_aide_walk_potion);
        RUN_TEST(p55_nested_scall_aide_give_balls);
        RUN_TEST(p55_hardened_elm_directions_behavioral);
        RUN_TEST(p55_hardened_rng_branch_deterministic);
        RUN_TEST(p55_hardened_endall_compiler_path_structural);
        RUN_TEST(p55_endall_emits_core_vm_not_behavior_table);
        RUN_TEST(p55_checkscene_elmslab_callback);
        RUN_TEST(p55_random_branch_olivine_youngster);
        RUN_TEST(p55_package_reference_canonical_id_elmslab_bg);
        RUN_TEST(p55_item_verbosegiveitem_aide_potion);
        RUN_TEST(p55_save_load_flag_continuity);
        RUN_TEST(p55_negative_pokepic_capability_deferred);
        RUN_TEST(p55_endall_no_behavior_table_in_corpus);
        RUN_TEST(p55_closure_scall_abc);
    } else {
        std::cout << "[Phase 5.5] SKIP: oracle package not available\n";
    }
    // Phase 5.5 closure tests that use ROM+profile but not oracle package
    RUN_TEST(p55_closure_farscall_fullpipe);
    RUN_TEST(p55_closure_endall_behavioral);

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << g_tests_passed << "\n";
    std::cout << "Failed: " << g_tests_failed << "\n";

    // Clean up oracle package
    g_oracle_reader.reset();
    if (!g_oracle_package_path.empty() && std::filesystem::exists(g_oracle_package_path)) {
        std::filesystem::remove(g_oracle_package_path);
    }

    return (g_tests_failed == 0) ? 0 : 1;
}
