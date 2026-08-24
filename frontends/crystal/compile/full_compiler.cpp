// crystal/compile/full_compiler.cpp
// Full-game Crystal ROM → EMON package compiler implementation

#include "crystal/compile/full_compiler.hpp"
#include "crystal/compile/corpus_discovery.hpp"
#include "crystal/rom/symbol_map.hpp"
#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/decoder.hpp"  // For text decoding
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/semantic_legalizer.hpp"
#include "crystal/script/behavior_table.hpp"
#include "crystal/script/legality_gate.hpp"
#include "crystal/script/native_registry.hpp"
#include "crystal/script/elevator_registry.hpp"
#include "crystal/script/trainer_registry.hpp"
#include "crystal/script/pokemail_registry.hpp"
#include "crystal/script/text_registry.hpp"
#include "crystal/script/semantic_lua_emitter.hpp"
#include "crystal/extract/sprite_ids.hpp"
#include "crystal/extract/species_extractor.hpp"
#include "engine/scripting/semantic_ir.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <format>
#include <stdexcept>
#include <queue>
#include <set>

namespace crystal {

using namespace enginemon::build;

// NOTE: MapIdRef is now defined in full_compiler.hpp

//=============================================================================
// CONFIG
//=============================================================================

std::string FullCompilerConfig::compute_options_hash() const {
    // Hash compilation options that affect output
    std::string opts;
    opts += std::to_string(static_cast<int>(tileset_time_of_day));
    opts += emit_address_comments ? "1" : "0";
    
    // Simple hash
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (char c : opts) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 0x100000001b3ULL;
    }
    
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return buf;
}

//=============================================================================
// CONSTRUCTOR/DESTRUCTOR
//=============================================================================

FullGameCompiler::FullGameCompiler(const RomData& rom, const ExtractionProfile& profile)
    : rom_(rom)
    , profile_(profile)
{
    // Create extractors
    map_extractor_ = std::make_unique<MapExtractor>(rom, profile);
    tileset_extractor_ = std::make_unique<TilesetExtractor>(rom, profile);
    sprite_extractor_ = std::make_unique<SpriteExtractor>(rom, profile);
    font_extractor_ = std::make_unique<FontExtractor>(rom, profile);
    
    // Create shared asset cache
    asset_cache_ = std::make_unique<AssetCache>();
}

FullGameCompiler::~FullGameCompiler() = default;

//=============================================================================
// MAIN COMPILE ENTRY POINT
//=============================================================================

bool FullGameCompiler::compile(const std::filesystem::path& output_path,
                                const FullCompilerConfig& config) {
    // Single-use contract: once compile() has been called (successfully or not),
    // the instance cannot be reused.  Accumulated build state is not reset
    // between calls; a second call would collide on duplicate map/tileset IDs.
    if (compile_called_) {
        throw std::logic_error(
            "FullGameCompiler::compile() called more than once on the same instance. "
            "Create a new FullGameCompiler for each compilation.");
    }
    compile_called_ = true;

    std::cout << "=== Full Game Crystal Compiler ===\n";
    std::cout << "Source ROM: " << profile_.version_string << "\n";
    std::cout << "Output: " << output_path << "\n\n";
    
    // Start total timer
    stats_.total_time.start();
    
    // Set up thread pool
    size_t worker_count = config.worker_count;
    if (worker_count == 0) {
        worker_count = default_worker_count();
    }
    stats_.worker_count = static_cast<uint32_t>(worker_count);
    thread_pool_ = std::make_unique<ThreadPool>(worker_count);
    
    std::cout << "Workers: " << worker_count << "\n\n";
    
    // Set up package cache
    if (config.use_package_cache) {
        auto cache_dir = config.cache_dir.empty() 
            ? PackageCache::default_cache_dir() 
            : config.cache_dir;
        package_cache_ = std::make_unique<PackageCache>(cache_dir);
        
        // Check for cached package
        auto build_id = make_build_identity(config);
        if (auto cached = package_cache_->find(build_id)) {
            std::cout << "Using cached package: " << cached->string() << "\n";
            
            std::error_code ec;
            std::filesystem::copy_file(*cached, output_path,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) {
                stats_.total_time.stop();
                std::cout << "\nPackage copied from cache in " 
                          << std::fixed << std::setprecision(1)
                          << stats_.total_time.elapsed_ms() << " ms\n";
                return true;
            }
            std::cout << "Cache copy failed, recompiling...\n\n";
        }
    }
    
    //=========================================================================
    // PHASE 1: Discovery (serial)
    //=========================================================================
    
    std::cout << "Phase 1: Content Discovery...\n";
    stats_.discovery_time.start();
    
    if (!discover_content()) {
        std::cerr << "Content discovery failed\n";
        return false;
    }
    
    stats_.discovery_time.stop();
    std::cout << "  Maps: " << content_.maps.size() << "\n";
    std::cout << "  Tilesets: " << content_.tilesets.size() << "\n";
    std::cout << "  Sprites: " << content_.sprites.size() << "\n";
    std::cout << "  Discovery time: " << std::fixed << std::setprecision(1)
              << stats_.discovery_time.elapsed_ms() << " ms\n\n";
    
    //=========================================================================
    // PHASE 2: Typed Script Pipeline (serial)
    //=========================================================================
    
    std::cout << "Phase 2: Typed Script Pipeline...\n";
    enginemon::build::PhaseTimer script_pipeline_time;
    script_pipeline_time.start();
    
    // Initialize pipeline components
    if (!init_typed_pipeline()) {
        std::cerr << "Failed to initialize typed script pipeline\n";
        return false;
    }
    
    // Pre-populate behavior_names from canonical BEHAVIOR_TABLE so the
    // Stage 5 legality gate can validate Sem_GameSpecificEvent names
    // during process_map_root_scripts() / process_std_scripts().
    compiled_game_data_.behavior_names.clear();
    for (std::size_t i = 0; i < BEHAVIOR_TABLE_SIZE; ++i) {
        compiled_game_data_.behavior_names.insert(BEHAVIOR_TABLE[i].behavior_name);
    }
    
    // Collect all script addresses
    std::set<uint32_t> map_root_addresses;
    std::map<uint32_t, enginemon::MapId> address_to_map;
    collect_script_addresses(map_root_addresses, address_to_map);
    
    std::cout << "  Map-root script addresses: " << map_root_addresses.size() << "\n";
    std::cout << "  StdScript table: " << std_scripts_->size() << " entries\n";
    
    // Process map-root scripts through typed pipeline
    if (!process_map_root_scripts(map_root_addresses, address_to_map)) {
        std::cerr << "FATAL: Map-root script processing failed\n";
        return false;
    }
    
    // Process StdScript bodies
    if (!process_std_scripts()) {
        std::cerr << "FATAL: StdScript processing failed\n";
        return false;
    }
    
    // Finalize registries (elevator, etc.) after all scripts processed
    finalize_registries();
    
    // Build production game data from actual discovered content
    build_production_game_data();
    
    // Link all scripts through SemanticLinker
    if (!link_scripts(address_to_map)) {
        std::cerr << "FATAL: Script linking failed\n";
        return false;
    }
    
    script_pipeline_time.stop();
    std::cout << "  Pipeline time: " << script_pipeline_time.elapsed_ms() << " ms\n\n";
    
    //=========================================================================
    // PHASE 3: Parallel Asset Compilation
    //=========================================================================
    
    std::cout << "Phase 3: Parallel Asset Compilation...\n";
    std::cout << "  Submitting " << content_.maps.size() << " jobs...\n";
    std::cout.flush();
    stats_.compilation_time.start();
    
    submit_compilation_jobs(config);
    
    std::cout << "  Jobs submitted, waiting...\n";
    std::cout.flush();
    
    // Wait for all jobs to complete
    thread_pool_->wait_all();
    
    std::cout << "  All jobs complete.\n";
    std::cout.flush();
    
    stats_.compilation_time.stop();
    stats_.completed_jobs = thread_pool_->stats().jobs_completed.load();
    
    // Update cache stats
    stats_.cache_hits = asset_cache_->stats().cache_hits.load();
    stats_.cache_misses = asset_cache_->stats().cache_misses.load();
    
    std::cout << "  Compilation time: " << stats_.compilation_time.elapsed_ms() << " ms\n";
    std::cout << "  Jobs completed: " << stats_.completed_jobs.load() << "\n";
    std::cout << "  Cache hits: " << stats_.cache_hits.load() << "\n";
    std::cout << "  Cache misses: " << stats_.cache_misses.load() << "\n\n";
    
    //=========================================================================
    // PHASE 4: Linking (serial)
    //=========================================================================
    
    std::cout << "Phase 4: Asset Linking and Validation...\n";
    stats_.linker_time.start();
    
    // Check for compilation errors
    if (!linker_input_.errors.empty()) {
        std::cerr << "\nCompilation errors:\n";
        for (const auto& err : linker_input_.errors) {
            std::cerr << "  - " << err << "\n";
        }
        return false;
    }
    
    // Create package writer
    PackageWriter writer;
    writer.set_source_rom(profile_.sha1, profile_.version_string);
    
    // Link all results
    if (!link_results(writer)) {
        std::cerr << "Linking failed\n";
        return false;
    }
    
    // Validate cross-references
    validation_ = validate_references();
    if (!validation_.success) {
        std::cerr << "\nValidation errors:\n";
        for (const auto& err : validation_.errors) {
            std::cerr << "  - " << err.message << "\n";
        }
        return false;
    }
    
    stats_.linker_time.stop();
    std::cout << "  Linker time: " << stats_.linker_time.elapsed_ms() << " ms\n\n";
    
    //=========================================================================
    // PHASE 5: Serialization (serial)
    //=========================================================================
    
    std::cout << "Phase 5: Serialization...\n";
    stats_.serialization_time.start();
    
    if (!write_package(output_path, writer)) {
        std::cerr << "Package serialization failed\n";
        return false;
    }
    
    stats_.serialization_time.stop();
    std::cout << "  Serialization time: " << stats_.serialization_time.elapsed_ms() << " ms\n";
    
    stats_.total_time.stop();
    
    // Store in cache if enabled
    if (package_cache_) {
        auto build_id = make_build_identity(config);
        if (package_cache_->store(build_id, output_path)) {
            std::cout << "  Package cached for future builds\n";
        }
    }
    
    // Print summary
    stats_.print_summary();
    
    // Print linked corpus summary
    std::cout << "\n=== Linked Corpus Summary ===\n";
    std::cout << "Map-root bodies:     " << linked_corpus_.map_root_bodies << "\n";
    std::cout << "StdScript bodies:    " << linked_corpus_.std_script_bodies << "\n";
    std::cout << "Total unique bodies: " << linked_corpus_.total_bodies << "\n";
    std::cout << "ExactResolved:       " << linked_corpus_.stats.total_exact_resolved() << "\n";
    std::cout << "OwnershipValidated:  " << linked_corpus_.stats.total_ownership_validated() << "\n";
    std::cout << "RangeOnly:           " << linked_corpus_.stats.total_range_only() << "\n";
    
    return true;
}

//=============================================================================
// PHASE 1: Discovery
//=============================================================================

bool FullGameCompiler::discover_content() {
    try {
        discover_all_maps();
    } catch (const std::runtime_error& e) {
        std::cerr << "FATAL: Map discovery failed: " << e.what() << "\n";
        return false;
    }
    discover_tilesets();
    discover_sprites();
    return true;
}

void FullGameCompiler::discover_all_maps() {
    // Use the shared production discovery implementation.
    // discover_reachable_maps() throws std::runtime_error if a reachable map
    // cannot be extracted — propagate that as a hard discovery failure.
    auto discovered_refs = discover_reachable_maps(rom_, profile_, *map_extractor_);
    
    // Convert MapIdRef results to DiscoveredMap entries.
    // Every ref returned was successfully extracted inside discover_reachable_maps();
    // a second extraction failure here is a hard error (e.g., ROM corruption between calls).
    for (const auto& ref : discovered_refs) {
        auto result = map_extractor_->extract_map(ref.group, ref.map);
        if (!result.success) {
            throw std::runtime_error(
                std::format("discover_all_maps: re-extraction of reachable map ({},{}) failed: {}",
                            ref.group, ref.map, result.error));
        }
        
        const auto& map = result.map;
        if (map.map_id.empty()) continue;
        
        if (content_.map_index.contains(map.map_id)) {
            // Two distinct (group,index) pairs resolved to the same semantic MapId.
            // This is an identity collision — silently dropping the second entry
            // would omit a real map from the compiled package.
            const auto& first_dm = content_.maps[content_.map_index.at(map.map_id)];
            throw std::runtime_error(
                std::format("discover_all_maps: MapId identity collision — '{}' produced by "
                            "({},{}) and ({},{}).  Both map entries must produce distinct IDs.",
                            map.map_id, first_dm.group, first_dm.index, ref.group, ref.map));
        }
        
        DiscoveredMap dm;
        dm.map_id = map.map_id;
        dm.group = ref.group;
        dm.index = ref.map;
        content_.map_index[dm.map_id] = content_.maps.size();
        content_.maps.push_back(dm);
    }
    
    // Debug: Print discovered maps
    std::cout << "    Discovered maps by (group,index):\n";
    for (const auto& dm : content_.maps) {
        std::cout << "      (" << (int)dm.group << "," << (int)dm.index << ") " << dm.map_id << "\n";
    }
}

void FullGameCompiler::discover_tilesets() {
    // Discover tilesets referenced by maps.
    // Each map in content_.maps already passed extraction in discover_all_maps();
    // a failure here is a structural regression and must be a hard error.
    
    std::unordered_set<std::string> seen;
    
    // First pass: collect unique tilesets from all maps
    for (const auto& dm : content_.maps) {
        auto result = map_extractor_->extract_map(dm.group, dm.index);
        if (!result.success) {
            throw std::runtime_error(
                std::format("discover_tilesets: re-extraction of known map '{}' ({},{}) failed: {}",
                            dm.map_id, dm.group, dm.index,
                            result.error.empty() ? "(no detail)" : result.error));
        }
        if (!result.map.tileset_id.empty()) {
            if (!seen.contains(result.map.tileset_id)) {
                seen.insert(result.map.tileset_id);
                
                DiscoveredTileset dt;
                dt.tileset_id = result.map.tileset_id;
                dt.tileset_index = 0;  // Will be resolved during extraction
                
                content_.tileset_index[dt.tileset_id] = content_.tilesets.size();
                content_.tilesets.push_back(dt);
            }
        }
    }
}

void FullGameCompiler::discover_sprites() {
    // Player sprites (always included)
    DiscoveredSprite player;
    player.sprite_id = "chris";
    player.sprite_index = 0;
    player.is_player = true;
    content_.sprite_index["chris"] = content_.sprites.size();
    content_.sprites.push_back(player);
    
    // Discover NPC sprites from all map objects.
    // Maps in content_.maps already passed extraction — failure is structural.
    std::unordered_set<std::string> seen;   // tracks raw tagged sprite_ids (dedup per obj)
    std::unordered_set<std::string> seen_pkg;  // tracks resolved bare names (dedup per asset)
    seen.insert("chris");
    seen_pkg.insert("chris");
    
    for (const auto& dm : content_.maps) {
        auto result = map_extractor_->extract_map(dm.group, dm.index);
        if (!result.success) {
            throw std::runtime_error(
                std::format("discover_sprites: re-extraction of known map '{}' ({},{}) failed: {}",
                            dm.map_id, dm.group, dm.index,
                            result.error.empty() ? "(no detail)" : result.error));
        }
        for (const auto& obj : result.map.objects) {
            if (!obj.sprite_id.empty() && !seen.contains(obj.sprite_id)) {
                seen.insert(obj.sprite_id);

                // Resolve the tagged sprite_id to a package/extractor key.
                std::string pkg_name;
                if (sprite_id_is_fixed(obj.sprite_id)) {
                    pkg_name = sprite_id_fixed_name(obj.sprite_id);
                } else if (sprite_id_is_pokemon_icon(obj.sprite_id)) {
                    // Pokemon icon: package key is "pokemon_icon:<icon_type_name>"
                    // The sprite_id IS the package key already.
                    pkg_name = obj.sprite_id;  // e.g., "pokemon_icon:pikachu"
                } else {
                    // daycare / variable — runtime state required, no static asset.
                    continue;
                }

                if (pkg_name.empty()) continue;
                if (seen_pkg.contains(pkg_name)) continue;
                seen_pkg.insert(pkg_name);

                DiscoveredSprite ds;
                ds.sprite_id = pkg_name;  // store bare name as the extraction key
                ds.sprite_index = 0;
                ds.is_player = false;

                content_.sprite_index[pkg_name] = content_.sprites.size();
                content_.sprites.push_back(ds);
            }
        }
    }

    // ASSET CLOSURE: ensure every icon type reachable via Day Care species resolution
    // is packaged, even when it never appears in any static map object event.
    // Without this, ICON_BIGMON (Charizard/Dragonite/Kingdra) and any other icon
    // absent from the 0x80-0xA2 overworld range would cause silent runtime failures.
    {
        auto icon_entries = sprite_extractor_->build_species_icon_map();
        size_t closure_added = 0;
        for (const auto& [species, icon_id] : icon_entries) {
            if (seen_pkg.contains(icon_id)) continue;
            seen_pkg.insert(icon_id);

            DiscoveredSprite ds;
            ds.sprite_id    = icon_id;
            ds.sprite_index = 0;
            ds.is_player    = false;

            content_.sprite_index[icon_id] = content_.sprites.size();
            content_.sprites.push_back(ds);
            ++closure_added;
        }
        std::cout << "  Icon types added for Day Care closure: " << closure_added << "\n";
    }
}

//=============================================================================
// PHASE 2: Parallel Compilation
//=============================================================================

void FullGameCompiler::submit_compilation_jobs(const FullCompilerConfig& config) {
    // Submit map compilation jobs
    for (const auto& dm : content_.maps) {
        ++stats_.total_jobs;
        
        thread_pool_->submit([this, dm, &config]() {
            auto result = compile_map_job(dm, config);
            
            if (result.success) {
                ++stats_.maps_compiled;
                linker_input_.add_map(std::move(result));
            } else {
                ++stats_.failed_jobs;
                linker_input_.add_error(result.error);
            }
        });
    }
}

CompiledMapResult FullGameCompiler::compile_map_job(const DiscoveredMap& map_info,
                                                      const FullCompilerConfig& config) {
    CompiledMapResult result;
    result.map_id = map_info.map_id;
    result.success = false;
    
    // Extract map using group/index (avoids ID lookup overhead)
    auto map_result = map_extractor_->extract_map(map_info.group, map_info.index);
    if (!map_result.success) {
        result.error = "Failed to extract map " + map_info.map_id + ": " + map_result.error;
        return result;
    }
    
    result.map = std::move(map_result.map);
    
    // NOTE: Script processing is now done in the serial typed pipeline (Phase 2)
    // We no longer compile scripts here - this job only extracts map data
    
    result.success = true;
    return result;
}

//=============================================================================
// PHASE 2: TYPED SCRIPT PIPELINE
//=============================================================================

bool FullGameCompiler::init_typed_pipeline() {
    // Create pipeline components
    symbols_ = std::make_unique<SymbolMap>();
    typed_decoder_ = std::make_unique<TypedScriptDecoder>(rom_, *symbols_);
    script_decoder_ = std::make_unique<ScriptDecoder>(rom_, *symbols_);  // For text decoding
    
    // Load StdScripts table
    std_scripts_ = std::make_unique<StdScriptsTable>();
    if (!std_scripts_->load(rom_, profile_.offsets.std_scripts, 
                            profile_.offsets.std_scripts_count)) {
        std::cerr << "Failed to load StdScripts table\n";
        return false;
    }
    
    // Initialize registries
    native_registry_ = std::make_unique<NativeCallRegistry>();
    native_registry_->initialize();
    
    ram_registry_ = std::make_unique<RamAddressRegistry>();
    ram_registry_->initialize();
    
    elevator_registry_ = std::make_unique<ElevatorRegistry>(rom_);
    
    // Initialize PokeMail registry for semantic mail extraction
    pokemail_registry_ = std::make_unique<PokeMailRegistry>(rom_);
    
    // Initialize Text registry for semantic text extraction (win/loss text, etc.)
    // Uses script decoder's decode_text_sequence for production text extraction
    text_registry_ = std::make_unique<TextRegistry>(
        [this](uint32_t addr) { return script_decoder_->decode_text_sequence(addr); });
    
    // Initialize trainer registry from ROM
    trainer_registry_ = std::make_unique<TrainerRegistry>(rom_, 
        profile_.offsets.trainer_groups, static_cast<uint8_t>(profile_.counts.num_trainer_classes));
    std::cout << "  TrainerRegistry: " << trainer_registry_->total_count() 
              << " trainers in " << trainer_registry_->group_count_total() << " groups\n";
    
    // Setup CFG builder
    cfg_builder_ = std::make_unique<CFGBuilder>();
    cfg_builder_->set_std_scripts(std_scripts_.get());
    cfg_builder_->set_native_registry(native_registry_.get());
    
    // Setup semantic legalizer
    legalizer_ = std::make_unique<SemanticLegalizer>();
    legalizer_->set_native_registry(native_registry_.get());
    legalizer_->set_ram_registry(ram_registry_.get());
    legalizer_->set_elevator_registry(elevator_registry_.get());
    legalizer_->set_pokemail_registry(pokemail_registry_.get());
    legalizer_->set_text_registry(text_registry_.get());
    
    // Setup legality gate
    legality_gate_ = std::make_unique<LegalityGate>();
    
    // Setup semantic linker
    semantic_linker_ = std::make_unique<SemanticLinker>();
    
    return true;
}

void FullGameCompiler::collect_script_addresses(
    std::set<uint32_t>& map_root_addresses,
    std::map<uint32_t, enginemon::MapId>& address_to_map) {
    
    // USE UNIFIED CORPUS DISCOVERY (with fixed-point deferred discovery)
    // This is the AUTHORITATIVE implementation - do not duplicate logic here.
    corpus_discovery_ = discover_corpus(rom_, profile_, *map_extractor_, 
                                         *typed_decoder_, *std_scripts_);
    
    // Extract addresses and map associations from discovery result
    for (const auto& [addr, info] : corpus_discovery_.map_roots) {
        map_root_addresses.insert(addr);
        if (info.owning_map != enginemon::MAP_NONE) {
            address_to_map[addr] = info.owning_map;
        }
    }
    
    // Print discovery statistics
    const auto& s = corpus_discovery_.stats;
    std::cout << "  === Corpus Discovery (Fixed-Point) ===\n";
    std::cout << "  Initial roots:\n";
    std::cout << "    Object scripts:    " << s.object_roots << "\n";
    std::cout << "    BG event scripts:  " << s.bg_event_roots << "\n";
    std::cout << "    Coord event scripts: " << s.coord_event_roots << "\n";
    std::cout << "    Scene scripts:     " << s.scene_roots << "\n";
    std::cout << "    Callback scripts:  " << s.callback_roots << "\n";
    std::cout << "  Deferred discovery:\n";
    std::cout << "    Targets encountered: " << s.deferred_targets_encountered << "\n";
    std::cout << "    Already known:       " << s.deferred_already_known << "\n";
    std::cout << "    New deferred roots:  " << s.deferred_new_roots << "\n";
    std::cout << "    Fixed-point iters:   " << s.deferred_iterations << "\n";
    std::cout << "  Final counts:\n";
    std::cout << "    Map-root bodies:     " << s.total_map_roots() << "\n";
    std::cout << "    StdScript bodies:    " << s.std_script_roots << "\n";
    std::cout << "    Total unique bodies: " << s.total_unique_bodies() << "\n";
}

std::optional<enginemon::SemanticScriptIR> FullGameCompiler::process_script_typed(
    uint32_t rom_address,
    const std::string& script_id) {
    
    try {
        // Stage 1: Decode
        CrystalScriptIR ir = typed_decoder_->decode_script(rom_address);
        if (ir.commands.empty()) {
            std::cerr << "FATAL: Script " << script_id << " decoded to empty command list\n";
            return std::nullopt;
        }
        
        // Stage 1.5: Round-trip validation (structural integrity check)
        // This proves that decode + encode produces identical bytes.
        // NOTE: This is NOT a semantic oracle - symmetric bugs won't be caught here.
        std::vector<std::string> round_trip_errors;
        bool round_trip_ok = typed_decoder_->validate_script_round_trip(ir, &round_trip_errors);
        size_t round_trip_failure_count = round_trip_errors.size();
        
        // Stage 2: CFG
        CrystalCFG cfg = cfg_builder_->build(ir);
        if (!cfg.validation.valid) {
            std::cerr << "FATAL: Script " << script_id << " CFG validation failed\n";
            for (const auto& err : cfg.validation.errors) {
                std::cerr << "  - " << err << "\n";
            }
            return std::nullopt;
        }
        
        // Stage 4: Lower to semantic IR
        enginemon::LoweringResult lowering = legalizer_->lower(ir, cfg);
        if (lowering.commands_unlowered > 0) {
            std::cerr << "FATAL: Script " << script_id << " has " << lowering.commands_unlowered 
                      << " unlowered commands\n";
            for (const auto& diag : lowering.unlowered) {
                std::cerr << "  - opcode 0x" << std::hex << (int)diag.opcode << std::dec 
                          << " at " << diag.provenance << ": " << diag.reason << "\n";
            }
            return std::nullopt;
        }
        
        // Stage 5: Legality
        LegalityInput input;
        input.ir = &ir;
        input.decode_complete = !ir.commands.empty();  // Actual validation result
        input.round_trip_failures = static_cast<uint32_t>(round_trip_failure_count);  // Actual count
        input.unknown_opcodes = 0;
        for (const auto& cmd : ir.commands) {
            if (std::holds_alternative<Cmd_Unknown>(cmd.data)) {
                input.unknown_opcodes++;
            }
        }
        input.cfg = &cfg;
        input.native_registry = native_registry_.get();
        input.ram_registry = ram_registry_.get();
        input.lowering = &lowering;
        input.game_data = &compiled_game_data_;
        
        LegalityResult legality = legality_gate_->validate(input);
        if (!legality.is_legal) {
            std::cerr << "FATAL: Script " << script_id << " failed legality gate\n";
            for (const auto& diag : legality.diagnostics()) {
                std::cerr << "  - " << diag.reason << "\n";
            }
            return std::nullopt;
        }
        
        // Store script_id and source address
        lowering.ir.script_id = script_id;
        lowering.ir.source_rom_address = rom_address;
        
        return lowering.ir;
        
    } catch (const std::exception& e) {
        std::cerr << "FATAL: Script " << script_id << " processing exception: " << e.what() << "\n";
        return std::nullopt;
    }
}

bool FullGameCompiler::process_map_root_scripts(
    const std::set<uint32_t>& addresses,
    const std::map<uint32_t, enginemon::MapId>& address_to_map) {
    
    std::cout << "  Processing " << addresses.size() << " map-root scripts...\n";
    
    map_root_irs_.clear();
    map_root_irs_.reserve(addresses.size());
    
    for (uint32_t addr : addresses) {
        // Generate script_id based on map
        std::string script_id;
        auto map_it = address_to_map.find(addr);
        if (map_it != address_to_map.end()) {
            script_id = "map_" + std::to_string(map_it->second >> 8) + "_" + 
                        std::to_string(map_it->second & 0xFF) + "_0x" + 
                        std::to_string(addr);
        } else {
            script_id = "script_0x" + std::to_string(addr);
        }
        
        auto ir = process_script_typed(addr, script_id);
        if (!ir) {
            return false;  // Hard failure
        }
        
        map_root_irs_.push_back(std::move(*ir));
        ++stats_.scripts_compiled;
    }
    
    std::cout << "  Processed: " << map_root_irs_.size() << " / " << addresses.size() << "\n";
    return true;
}

bool FullGameCompiler::process_std_scripts() {
    std::cout << "  Processing " << std_scripts_->size() << " StdScript bodies...\n";
    
    // Collect unique StdScript addresses (avoiding overlap with map roots)
    std::set<uint32_t> std_script_addresses;
    std::map<uint32_t, uint16_t> std_addr_to_id;
    
    std::set<uint32_t> map_root_addr_set;
    for (const auto& ir : map_root_irs_) {
        map_root_addr_set.insert(ir.source_rom_address);
    }
    
    for (size_t i = 0; i < std_scripts_->size(); ++i) {
        const auto* entry = std_scripts_->get(static_cast<uint16_t>(i));
        if (entry && entry->flat_address != 0) {
            if (!std_script_addresses.contains(entry->flat_address) &&
                !map_root_addr_set.contains(entry->flat_address)) {
                std_script_addresses.insert(entry->flat_address);
                std_addr_to_id[entry->flat_address] = entry->std_id;
            }
        }
    }
    
    std_script_irs_.clear();
    std_script_irs_.reserve(std_script_addresses.size());
    
    for (uint32_t addr : std_script_addresses) {
        std::string script_id;
        uint16_t std_id = 0;
        auto std_it = std_addr_to_id.find(addr);
        if (std_it != std_addr_to_id.end()) {
            std_id = std_it->second;
            script_id = "std_" + std::to_string(std_id);
        } else {
            script_id = "std_0x" + std::to_string(addr);
        }
        
        auto ir = process_script_typed(addr, script_id);
        if (!ir) {
            return false;  // Hard failure
        }
        
        std_script_irs_.push_back(std::move(*ir));
        ++stats_.scripts_compiled;
        
        // Track successfully compiled StdScript ID for linker validation
        // Only add if we have a valid std_id (not just an address-based ID)
        if (std_it != std_addr_to_id.end()) {
            compiled_game_data_.compiled_std_scripts.insert(std_id);
        }
    }
    
    std::cout << "  Processed: " << std_script_irs_.size() << " / " << std_script_addresses.size() << "\n";
    std::cout << "  Compiled StdScript bodies: " << compiled_game_data_.compiled_std_scripts.size() << "\n";
    return true;
}

void FullGameCompiler::finalize_registries() {
    // Populate compiled_game_data_ elevators from elevator_registry_
    for (const auto& def : elevator_registry_->all_definitions()) {
        compiled_game_data_.elevators.insert(def.id);
    }
    std::cout << "  Compiled elevators: " << compiled_game_data_.elevators.size() << "\n";

    // Populate behavior_names from the canonical BEHAVIOR_TABLE.
    // Stage 5 legality gate validates every Sem_GameSpecificEvent::behavior_name
    // is in this set; unknown names hard-fail compilation.
    compiled_game_data_.behavior_names.clear();
    for (std::size_t i = 0; i < BEHAVIOR_TABLE_SIZE; ++i) {
        compiled_game_data_.behavior_names.insert(BEHAVIOR_TABLE[i].behavior_name);
    }
    std::cout << "  Compiled behaviors: " << compiled_game_data_.behavior_names.size() << "\n";
}

void FullGameCompiler::build_production_game_data() {
    const auto& c = profile_.counts;
    
    //=========================================================================
    // EXACT_RESOLVED RESOURCES - Validated against actual compiled artifacts
    // These require actual definitions extracted from ROM or produced by the compiler.
    //=========================================================================
    
    // === Maps (from actual discovered content - PRODUCTION REGISTRY) ===
    // Authority: discover_reachable_maps() fixed-point closure
    // NOT a range check - actual discovered maps only
    for (const auto& dm : content_.maps) {
        enginemon::MapId map_id = (static_cast<uint16_t>(dm.group) << 8) | dm.index;
        compiled_game_data_.maps.insert(map_id);
        
        // Get object and warp counts
        auto result = map_extractor_->extract_map(dm.group, dm.index);
        if (result.success) {
            compiled_game_data_.map_object_counts[map_id] = static_cast<uint8_t>(result.map.objects.size());
            compiled_game_data_.map_warp_counts[map_id] = static_cast<uint8_t>(result.map.warps.size());
        }
    }
    
    // === Trainers (from TrainerRegistry - AUTHORITATIVE PRODUCTION REGISTRY) ===
    // Authority: ROM TrainerGroups table extraction
    // NOT a range check - only (group, id) pairs actually in ROM pass validation
    if (trainer_registry_) {
        for (const auto& pair : trainer_registry_->all_pairs()) {
            compiled_game_data_.trainers.insert(pair);
        }
    }
    
    // === StdScripts (compiled bodies only - PRODUCTION REGISTRY) ===
    // Authority: Scripts that passed Stages 1-5 (compiled_std_scripts set)
    // ROM table is loaded for address lookup, but ExactResolved requires compiled body
    compiled_game_data_.load_std_scripts(*std_scripts_);
    // NOTE: compiled_std_scripts is populated in process_std_scripts()
    
    //=========================================================================
    // PENDING_DEFINITION RESOURCES - Authoritative closed-domain membership
    //
    // These use Crystal profile table counts as domain authority.
    // The domains are proven CONTIGUOUS (no holes) for supported ROM profiles:
    //   - Crystal defines all enum values 0..(count-1) or 1..count
    //   - No reserved/invalid values within the range
    //   - Verified against pokecrystal/constants/*.asm source
    //
    // This is NOT "extracted definition membership" - actual resource extractors
    // will be built later. Until then, count-derived membership is authoritative
    // because the Crystal domain is closed and contiguous.
    //
    // Status: PendingDefinition (domain valid, artifact producer not yet built)
    //=========================================================================
    
    // === Species [1, num_pokemon] — EXTRACTED from ROM BaseData table ===
    // Authority: ROM BaseData records at profile.offsets.base_data.
    // Each record is profile.format.pokemon.base_data_size bytes.
    // Count is profile.counts.num_pokemon (profile metadata — not ROM-derived).
    //
    // Species are ExactResolved (not PendingDefinition) because actual
    // definitions exist in species_defs. A species ID not in this map is
    // InvalidDomain at Stage 6.
    {
        auto species_result = extract_all_species(rom_, profile_);
        if (!species_result.success) {
            // Extraction failure is fatal — the species domain cannot be
            // established without valid BaseData records.
            throw std::runtime_error(
                "Species Finder failed: " + species_result.error);
        }
        compiled_game_data_.species_defs = std::move(species_result.species);
        std::cout << "    Species:      " << compiled_game_data_.species_defs.size()
                  << " definitions extracted from BaseData table\n";
    }
    
    // === Items [0, num_items) - Closed contiguous domain ===
    // Authority: pokecrystal/constants/item_constants.asm
    // NO_ITEM (0) is valid sentinel, items 1-255 all defined
    // No holes: Crystal uses all 256 item indices
    for (uint16_t i = 0; i < c.num_items; ++i) {
        compiled_game_data_.items.insert(static_cast<enginemon::ItemId>(i));
    }
    
    // === Specials: intentionally omitted ===
    // Sem_Special is rejected at Stage 5 and never reaches the linker.
    // The specials domain set has been removed from CompiledGameData.
    // Sem_GameSpecificEvent behavior names are validated via behavior_names set.
    
    // === Music [0, num_music) - Closed contiguous domain ===
    // Authority: pokecrystal/constants/music_constants.asm
    // MUSIC_NONE (0) is valid, all 103 music IDs defined
    for (uint16_t i = 0; i < c.num_music; ++i) {
        compiled_game_data_.music.insert(static_cast<enginemon::MusicId>(i));
    }
    
    // === SFX [0, num_sfx) - Closed contiguous domain ===
    // Authority: pokecrystal/constants/sfx_constants.asm
    // All 207 SFX indices defined
    for (uint16_t i = 0; i < c.num_sfx; ++i) {
        compiled_game_data_.sfx.insert(static_cast<enginemon::SfxId>(i));
    }
    
    // === Emotes [0, num_emotes) - Closed contiguous domain ===
    // Authority: pokecrystal/constants/sprite_anim_constants.asm
    // All 12 emote bubble indices defined
    for (uint8_t i = 0; i < c.num_emotes; ++i) {
        compiled_game_data_.emotes.insert(i);
    }
    
    // === Phone contacts [0, num_phone_contacts) - Closed contiguous domain ===
    // Authority: pokecrystal/data/phone/phone_contacts.asm
    // All 38 phone contact IDs defined
    for (uint8_t i = 0; i < c.num_phone_contacts; ++i) {
        compiled_game_data_.phone_persons.insert(i);
    }
    
    // === Trades [0, num_npc_trades) - Closed contiguous domain ===
    // Authority: pokecrystal/data/events/npc_trades.asm
    // All 7 NPC trade IDs defined
    for (uint8_t i = 0; i < c.num_npc_trades; ++i) {
        compiled_game_data_.trades.insert(i);
    }
    
    // === Fruit trees [1, num_fruit_trees] - Closed contiguous domain (1-indexed) ===
    // Authority: pokecrystal/data/items/fruit_trees.asm
    // IDs are 1-indexed (const_def 1), all 30 fruit trees defined
    for (uint8_t i = 1; i <= c.num_fruit_trees; ++i) {
        compiled_game_data_.fruit_trees.insert(i);
    }
    
    // === Marts [0, num_marts) - Closed contiguous domain ===
    // Authority: pokecrystal/data/items/marts.asm
    // All 34 mart inventory IDs defined
    for (uint16_t i = 0; i < c.num_marts; ++i) {
        compiled_game_data_.marts.insert(i);
    }
    
    // NOTE: Elevators populated in finalize_registries() from ElevatorRegistry
    // Elevators are ExactResolved because they're extracted from script commands
    
    //=========================================================================
    // EXACT_RESOLVED REGISTRIES - Content-addressed definitions from script processing
    // These are populated during script lowering through semantic registries
    //=========================================================================
    
    // === PokeMail (from PokeMail registry - content-addressed) ===
    // Authority: PokeMailRegistry (extracted during script lowering)
    // IDs assigned by content hash, not ROM index
    if (pokemail_registry_) {
        for (const auto& def : pokemail_registry_->all_definitions()) {
            compiled_game_data_.pokemail_ids.insert(def.id);
        }
    }
    
    // === Text (from Text registry - content-addressed) ===
    // Authority: TextRegistry (extracted during script lowering)
    // IDs assigned by content hash, not ROM index
    if (text_registry_) {
        for (const auto& def : text_registry_->all_definitions()) {
            compiled_game_data_.text_ids.insert(def.id);
        }
    }
    
    //=========================================================================
    // REPORT
    //=========================================================================
    
    std::cout << "  Production game data built:\n";
    std::cout << "  --- ExactResolved (actual artifacts) ---\n";
    std::cout << "    Maps:         " << compiled_game_data_.maps.size() 
              << " (from discovered)\n";
    std::cout << "    Trainers:     " << compiled_game_data_.trainers.size()
              << " (from TrainerRegistry - ROM extraction)\n";
    std::cout << "    StdScripts:   " << compiled_game_data_.compiled_std_scripts.size()
              << " (legalized bodies)\n";
    std::cout << "    PokeMail:     " << compiled_game_data_.pokemail_ids.size()
              << " (from registry - content-addressed)\n";
    std::cout << "    Text:         " << compiled_game_data_.text_ids.size()
              << " (from registry - content-addressed)\n";
    std::cout << "    Species:      " << compiled_game_data_.species_defs.size()
              << " (from BaseData extraction - ExactResolved)\n";
    std::cout << "  --- PendingDefinition (closed-domain authority, awaiting extractors) ---\n";
    std::cout << "    Items:        " << compiled_game_data_.items.size()
              << " [0-" << (c.num_items - 1) << "] closed contiguous\n";
    // Specials domain removed — Sem_Special rejected at Stage 5
    std::cout << "    Music:        " << compiled_game_data_.music.size()
              << " [0-" << (c.num_music - 1) << "] closed contiguous\n";
    std::cout << "    SFX:          " << compiled_game_data_.sfx.size()
              << " [0-" << (c.num_sfx - 1) << "] closed contiguous\n";
    std::cout << "    PhonePersons: " << compiled_game_data_.phone_persons.size()
              << " [0-" << (c.num_phone_contacts - 1) << "] closed contiguous\n";
    std::cout << "    Trades:       " << compiled_game_data_.trades.size()
              << " [0-" << (c.num_npc_trades - 1) << "] closed contiguous\n";
    std::cout << "    FruitTrees:   " << compiled_game_data_.fruit_trees.size()
              << " [1-" << c.num_fruit_trees << "] closed contiguous (1-indexed)\n";
    std::cout << "    Marts:        " << compiled_game_data_.marts.size()
              << " [0-" << (c.num_marts - 1) << "] closed contiguous\n";
    std::cout << "    Emotes:       " << compiled_game_data_.emotes.size()
              << " [0-" << (c.num_emotes - 1) << "] closed contiguous\n";
    std::cout << "  --- ExactResolved (populated after script processing) ---\n";
    std::cout << "    Elevators:    (finalized in finalize_registries())\n";
}

bool FullGameCompiler::link_scripts(const std::map<uint32_t, enginemon::MapId>& address_to_map) {
    std::cout << "  Linking " << (map_root_irs_.size() + std_script_irs_.size()) << " scripts...\n";
    
    // Set compiled game data
    semantic_linker_->set_game_data(&compiled_game_data_);
    
    // Set script context (map ownership) for each map-root script
    for (const auto& ir : map_root_irs_) {
        if (ir.source_rom_address != 0) {
            auto map_it = address_to_map.find(ir.source_rom_address);
            if (map_it != address_to_map.end()) {
                semantic_linker_->set_script_context(ir.script_id, map_it->second);
            }
        }
    }
    
    // Link the full corpus
    linked_corpus_ = semantic_linker_->link_full_corpus(map_root_irs_, std_script_irs_);
    
    // Check for failures
    if (linked_corpus_.stats.total_unresolved() > 0) {
        std::cerr << "FATAL: " << linked_corpus_.stats.total_unresolved() << " unresolved references\n";
        linked_corpus_.print_report();
        return false;
    }
    
    if (linked_corpus_.stats.total_invalid_ownership() > 0) {
        std::cerr << "FATAL: " << linked_corpus_.stats.total_invalid_ownership() << " invalid ownership references\n";
        linked_corpus_.print_report();
        return false;
    }
    
    if (linked_corpus_.stats.total_wrong_type() > 0) {
        std::cerr << "FATAL: " << linked_corpus_.stats.total_wrong_type() << " wrong-type references\n";
        linked_corpus_.print_report();
        return false;
    }
    
    if (linked_corpus_.stats.total_invalid_domain() > 0) {
        std::cerr << "FATAL: " << linked_corpus_.stats.total_invalid_domain() << " invalid-domain references\n";
        linked_corpus_.print_report();
        return false;
    }
    
    // Report success
    std::cout << "  Linked: " << linked_corpus_.total_bodies << "/" << linked_corpus_.total_bodies << "\n";
    
    return true;
}

//=============================================================================
// SHARED ASSET CACHE
//=============================================================================

AssetResult<ExtractedTileset> FullGameCompiler::get_tileset(const std::string& tileset_id) {
    if (test_tileset_override_) {
        return test_tileset_override_(tileset_id);
    }
    std::string key = make_cache_key("tileset", tileset_id);
    
    return asset_cache_->get_or_compute<ExtractedTileset>(key, [this, &tileset_id]() {
        auto result = tileset_extractor_->extract_tileset(tileset_id);
        if (result.success) {
            ++stats_.tilesets_compiled;
            return AssetResult<ExtractedTileset>{std::move(result.tileset)};
        }
        return AssetResult<ExtractedTileset>{
            AssetError{"Failed to extract tileset: " + tileset_id, tileset_id}
        };
    });
}

AssetResult<RuntimeSprite> FullGameCompiler::get_sprite(const std::string& sprite_id) {
    if (test_sprite_override_) {
        return test_sprite_override_(sprite_id);
    }
    std::string key = make_cache_key("sprite", sprite_id);
    
    return asset_cache_->get_or_compute<RuntimeSprite>(key, [this, &sprite_id]() {
        // Check if it's the player sprite
        if (sprite_id == "chris" || sprite_id == "kris") {
            bool is_female = (sprite_id == "kris");
            auto result = sprite_extractor_->extract_player_sprite(is_female);
            if (result.success) {
                ++stats_.sprites_compiled;
                return AssetResult<RuntimeSprite>{std::move(result.sprite)};
            }
            return AssetResult<RuntimeSprite>{
                AssetError{"Failed to extract player sprite: " + sprite_id, sprite_id}
            };
        }
        
        // Regular NPC sprite — or Pokémon icon sprite
        if (sprite_id_is_pokemon_icon(sprite_id)) {
            // Pokemon icon: extract from MonMenuIcons/IconPointers/Icons GFX (bank 23)
            std::string icon_name = sprite_id_pokemon_icon_name(sprite_id);
            auto result = sprite_extractor_->extract_pokemon_icon(icon_name);
            if (result.success) {
                ++stats_.sprites_compiled;
                return AssetResult<RuntimeSprite>{std::move(result.sprite)};
            }
            return AssetResult<RuntimeSprite>{
                AssetError{"Failed to extract pokemon icon: " + sprite_id, sprite_id}
            };
        }

        // Fixed overworld NPC sprite (bare name, e.g., "teacher")
        auto result = sprite_extractor_->extract_sprite(sprite_id);
        if (result.success) {
            ++stats_.sprites_compiled;
            return AssetResult<RuntimeSprite>{std::move(result.sprite)};
        }
        return AssetResult<RuntimeSprite>{
            AssetError{"Failed to extract sprite: " + sprite_id, sprite_id}
        };
    });
}

AssetResult<FontAtlas> FullGameCompiler::get_font() {
    if (test_font_override_) {
        return test_font_override_();
    }
    std::string key = make_cache_key("font", "crystal_main");
    
    return asset_cache_->get_or_compute<FontAtlas>(key, [this]() {
        auto result = font_extractor_->extract_font();
        if (result.success) {
            auto atlas = render_font_atlas(result.font, default_text_palette());
            return AssetResult<FontAtlas>{std::move(atlas)};
        }
        return AssetResult<FontAtlas>{
            AssetError{"Failed to extract font", "crystal_main"}
        };
    });
}

AssetResult<SpriteObjPalettes> FullGameCompiler::get_obj_palettes() {
    if (test_palettes_override_) {
        return test_palettes_override_();
    }
    std::string key = make_cache_key("obj_palettes", "default");
    
    return asset_cache_->get_or_compute<SpriteObjPalettes>(key, [this]() {
        auto result = sprite_extractor_->extract_obj_palettes();
        if (result.success) {
            return AssetResult<SpriteObjPalettes>{std::move(result.palettes)};
        }
        return AssetResult<SpriteObjPalettes>{
            AssetError{"Failed to extract OBJ palettes", "obj_palettes"}
        };
    });
}

//=============================================================================
// PHASE 4: Asset Linking
//=============================================================================

bool FullGameCompiler::link_results(PackageWriter& writer) {
    // Sort results by stable ID for deterministic output
    sort_by_stable_id();
    
    // Add maps
    for (auto& map_result : linker_input_.maps) {
        writer.add_map(map_result.map);
    }
    
    // Add tilesets through cache
    for (const auto& dt : content_.tilesets) {
        auto tileset_result = get_tileset(dt.tileset_id);
        if (!is_success(tileset_result)) {
            const auto& err = std::get<AssetError>(tileset_result);
            std::cerr << "FATAL: Required tileset '" << dt.tileset_id
                      << "' extraction failed: " << err.message << "\n";
            return false;
        }
        writer.add_tileset(get_asset(tileset_result), TimeOfDay::Day);
        emitted_tileset_ids_.insert(dt.tileset_id);
    }
    
    // Add sprites through cache
    for (const auto& ds : content_.sprites) {
        auto sprite_result = get_sprite(ds.sprite_id);
        if (!is_success(sprite_result)) {
            const auto& err = std::get<AssetError>(sprite_result);
            std::cerr << "FATAL: Required sprite '" << ds.sprite_id
                      << "' extraction failed: " << err.message << "\n";
            return false;
        }
        writer.add_sprite(get_asset(sprite_result));
        emitted_sprite_ids_.insert(ds.sprite_id);
    }
    
    // Add OBJ palettes
    auto palettes_result = get_obj_palettes();
    if (!is_success(palettes_result)) {
        const auto& err = std::get<AssetError>(palettes_result);
        std::cerr << "FATAL: Required OBJ palettes extraction failed: " << err.message << "\n";
        return false;
    }
    writer.add_obj_palettes(get_asset(palettes_result));
    emitted_obj_palettes_ = true;

    // Compile and emit the species→icon map from Crystal MonMenuIcons ROM table.
    // Source: MonMenuIcons at bank 23:6ac4 (251 entries, species → ICON_* type)
    // This mapping is Crystal-frontend knowledge compiled into the package once,
    // so the runtime never needs hardcoded Crystal game tables.
    {
        auto icon_map = sprite_extractor_->build_species_icon_map();
        if (icon_map.empty()) {
            std::cerr << "FATAL: Failed to read MonMenuIcons table from ROM\n";
            return false;
        }
        writer.add_species_icon_map(icon_map);
        std::cout << "  Species→icon entries: " << icon_map.size() << "\n";

        for (const auto& [species, icon_id] : icon_map) {
            emitted_icon_type_ids_.insert(icon_id);
        }
        std::cout << "  Distinct icon types in map: " << emitted_icon_type_ids_.size() << "\n";
    }
    
    // Add font
    auto font_result = get_font();
    if (!is_success(font_result)) {
        const auto& err = std::get<AssetError>(font_result);
        std::cerr << "FATAL: Required font extraction failed: " << err.message << "\n";
        return false;
    }
    writer.add_font_atlas(get_asset(font_result));
    emitted_font_ = true;
    
    // Stage 7: Emit all retained SemanticScriptIR bodies as package Script chunks.
    // Hard-fail if any legal body cannot be emitted — no partial output.
    {
        SemanticLuaEmitter emitter;
        uint32_t scripts_emitted = 0;

        auto emit_one = [&](const enginemon::SemanticScriptIR& ir) -> bool {
            std::string lua;
            try {
                lua = emitter.emit(ir);
            } catch (const std::exception& ex) {
                std::cerr << "FATAL: Stage 7 emission failed for script '"
                          << ir.script_id << "': " << ex.what() << "\n";
                return false;
            }
            // Wrap in the format HeadlessGameLoop::start_script expects:
            //   script = (function() ... end)()
            // The emitter already produces:
            //   script = {}
            //   function script.main(ctx) ... end
            //   return script
            // PackageWriter stores it verbatim; runtime loads with require-like load().
            writer.add_script(ir.script_id, lua);
            ++scripts_emitted;
            stats_.total_lua_bytes += static_cast<uint32_t>(lua.size());
            return true;
        };

        for (const auto& ir : map_root_irs_) {
            if (!emit_one(ir)) return false;
        }
        for (const auto& ir : std_script_irs_) {
            if (!emit_one(ir)) return false;
        }

        std::cout << "  Scripts emitted: " << scripts_emitted << "\n";

        // Invariant: every retained body must have been emitted.
        uint32_t expected = static_cast<uint32_t>(map_root_irs_.size() + std_script_irs_.size());
        if (scripts_emitted != expected) {
            std::cerr << "FATAL: Script emission count mismatch: emitted "
                      << scripts_emitted << " expected " << expected << "\n";
            return false;
        }
    }

    return true;
}

CompilerValidationResult FullGameCompiler::validate_references() {
    CompilerValidationResult result;
    result.success = true;
    
    // Build lookup sets from the EMITTED resource inventories (populated in
    // link_results()), not the discovery-intent sets.  A discovered asset whose
    // extraction failed will be absent from the emitted sets and correctly
    // flagged here rather than silently passing validation.
    std::unordered_set<std::string> map_ids;
    
    for (const auto& mr : linker_input_.maps) {
        map_ids.insert(mr.map_id);
    }
    
    // NOTE: Script validation is done through the SemanticLinker in Phase 2.
    // NOTE: emitted_sprite_ids_ and emitted_tileset_ids_ are populated by
    //       link_results() from successfully emitted assets only.
    
    // Package completeness invariant: every discovered resource must have been
    // emitted.  Discrepancies here indicate an extraction failure that was not
    // caught by link_results() — that should be impossible after the fix above,
    // but we check explicitly for defence in depth.
    for (const auto& dt : content_.tilesets) {
        if (!emitted_tileset_ids_.contains(dt.tileset_id)) {
            result.errors.push_back({
                CompilerValidationError::Type::MissingTileset,
                "(completeness)",
                dt.tileset_id,
                std::format("Discovered tileset '{}' was not emitted into package",
                            dt.tileset_id)
            });
            result.success = false;
        }
    }
    for (const auto& ds : content_.sprites) {
        if (!emitted_sprite_ids_.contains(ds.sprite_id)) {
            result.errors.push_back({
                CompilerValidationError::Type::MissingSprite,
                "(completeness)",
                ds.sprite_id,
                std::format("Discovered sprite '{}' was not emitted into package",
                            ds.sprite_id)
            });
            result.success = false;
        }
    }
    if (!emitted_obj_palettes_) {
        result.errors.push_back({
            CompilerValidationError::Type::Other,
            "(completeness)",
            "obj_palettes",
            "OBJ palettes were not emitted into package"
        });
        result.success = false;
    }
    if (!emitted_font_) {
        result.errors.push_back({
            CompilerValidationError::Type::MissingFont,
            "(completeness)",
            "font",
            "Font was not emitted into package"
        });
        result.success = false;
    }
    
    // Cross-reference checks against emitted inventories
    for (const auto& mr : linker_input_.maps) {
        const auto& map = mr.map;
        
        // Validate warp destinations
        for (const auto& warp : map.warps) {
            if (!warp.target_map_id.empty() && 
                warp.target_map_id != "LAST_MAP" &&
                !map_ids.contains(warp.target_map_id)) {
                result.errors.push_back({
                    CompilerValidationError::Type::MissingWarpDestination,
                    map.map_id,
                    warp.target_map_id,
                    std::format("Warp in {} references missing map: {}", 
                               map.map_id, warp.target_map_id)
                });
                result.success = false;
            }
        }
        
        // Validate connection destinations
        for (const auto& conn : map.connections) {
            if (!conn.target_map_id.empty() && !map_ids.contains(conn.target_map_id)) {
                result.errors.push_back({
                    CompilerValidationError::Type::MissingConnectionDestination,
                    map.map_id,
                    conn.target_map_id,
                    std::format("Connection in {} references missing map: {}",
                               map.map_id, conn.target_map_id)
                });
                result.success = false;
            }
        }
        
        // Validate object sprites against EMITTED inventory (hard error, not warning)
        for (const auto& obj : map.objects) {
            if (obj.sprite_id.empty()) continue;

            if (sprite_id_is_fixed(obj.sprite_id)) {
                std::string pkg_name = sprite_id_fixed_name(obj.sprite_id);
                if (!pkg_name.empty() && !emitted_sprite_ids_.contains(pkg_name)) {
                    result.errors.push_back({
                        CompilerValidationError::Type::MissingSprite,
                        map.map_id,
                        obj.sprite_id,
                        std::format("Object in {} references fixed sprite '{}' (key '{}') not in emitted package",
                                   map.map_id, obj.sprite_id, pkg_name)
                    });
                    result.success = false;
                }
            } else if (sprite_id_is_pokemon_icon(obj.sprite_id)) {
                // Pokemon icon sprites are now compiled — validate their presence.
                if (!emitted_sprite_ids_.contains(obj.sprite_id)) {
                    result.errors.push_back({
                        CompilerValidationError::Type::MissingSprite,
                        map.map_id,
                        obj.sprite_id,
                        std::format("Object in {} references pokemon_icon sprite '{}' not in emitted package",
                                   map.map_id, obj.sprite_id)
                    });
                    result.success = false;
                }
            }
            // daycare / variable — runtime state, no static package asset required.
        }
        
        // Validate tileset against EMITTED inventory
        if (!map.tileset_id.empty() && !emitted_tileset_ids_.contains(map.tileset_id)) {
            result.errors.push_back({
                CompilerValidationError::Type::MissingTileset,
                map.map_id,
                map.tileset_id,
                std::format("Map {} references tileset '{}' not in emitted package",
                           map.map_id, map.tileset_id)
            });
            result.success = false;
        }
    }

    // ASSET CLOSURE GATE: every icon type referenced by the species→icon map
    // must have a corresponding packaged sprite asset.
    // emitted_icon_type_ids_ is populated in link_results() from build_species_icon_map().
    // A missing entry means Day Care species resolution silently fails at runtime.
    for (const auto& icon_id : emitted_icon_type_ids_) {
        if (!emitted_sprite_ids_.contains(icon_id)) {
            result.errors.push_back({
                CompilerValidationError::Type::MissingSprite,
                "(species_icon_map)",
                icon_id,
                std::format("Species→icon map references '{}' but that icon asset "
                            "was not emitted into the package (Day Care closure failure)",
                            icon_id)
            });
            result.success = false;
        }
    }

    return result;
}

void FullGameCompiler::sort_by_stable_id() {
    // Sort maps by ID for deterministic output
    std::sort(linker_input_.maps.begin(), linker_input_.maps.end(),
              [](const CompiledMapResult& a, const CompiledMapResult& b) {
                  return a.map_id < b.map_id;
              });
    
    // NOTE: Scripts are now sorted via the typed pipeline, not per-map.
    // Script ordering is determined by address discovery order and stable IDs.
}

//=============================================================================
// PHASE 4: Serialization
//=============================================================================

bool FullGameCompiler::write_package(const std::filesystem::path& output_path,
                                      PackageWriter& writer) {
    if (!writer.write(output_path)) {
        return false;
    }
    
    // Get package size
    std::error_code ec;
    stats_.package_bytes = std::filesystem::file_size(output_path, ec);
    
    return true;
}

//=============================================================================
// HELPERS
//=============================================================================

std::string FullGameCompiler::make_global_script_id(const std::string& map_id,
                                                      const std::string& local_id) {
    return map_id + "::" + local_id;
}

BuildIdentity FullGameCompiler::make_build_identity(const FullCompilerConfig& config) const {
    BuildIdentity id;
    id.rom_sha1 = profile_.sha1;
    id.compiler_version = CRYSTAL_COMPILER_VERSION;
    id.format_version = EMON_FORMAT_VERSION;
    id.options_hash = config.compute_options_hash();
    return id;
}

//=============================================================================
// PUBLIC MAP DISCOVERY API
// Fixed-point reachable map discovery for use by compiler and tests
//=============================================================================

std::vector<MapIdRef> discover_reachable_maps(
    const RomData& rom,
    const ExtractionProfile& profile,
    MapExtractor& extractor) {
    
    // FIXED-POINT REACHABLE-MAP DISCOVERY
    // 
    // Algorithm:
    //   1. Seed with globally reachable MapIds (spawn points, new game start)
    //   2. Resolve each MapId via MapGroupPointers + fixed stride
    //   3. Extract map, validate structure
    //   4. Collect MapIds from warps, connections
    //   5. Follow script map references (warp commands in scripts)
    //   6. Enqueue unseen MapIds
    //   7. Repeat until no new MapIds
    //
    // This discovers exactly the playable map set without needing per-group counts.
    // Unreachable beta/debug/unused maps are intentionally excluded.
    
    std::set<MapIdRef> visited;
    std::queue<MapIdRef> frontier;
    std::vector<MapIdRef> result;
    
    auto enqueue = [&](uint8_t group, uint8_t map) {
        if (group == 0 || map == 0) return;
        MapIdRef ref{group, map};
        if (!visited.contains(ref)) {
            visited.insert(ref);
            frontier.push(ref);
        }
    };
    
    auto enqueue_map_id = [&](enginemon::MapId map_id) {
        if (map_id == enginemon::MAP_NONE) return;
        uint8_t group = (map_id >> 8) & 0xFF;
        uint8_t map = map_id & 0xFF;
        enqueue(group, map);
    };
    
    //=========================================================================
    // SEEDS
    //=========================================================================
    
    // New game start: NEW_BARK_TOWN (group 24, map 4)
    enqueue(24, 4);
    
    // Player's house 2F (actual starting location inside)
    enqueue(24, 7);
    
    // Spawn points table - covers fly destinations, respawn points
    if (profile.offsets.spawn_points != 0) {
        const uint32_t spawn_table = profile.offsets.spawn_points;
        for (uint8_t i = 0; i < 30; ++i) {
            uint32_t addr = spawn_table + (i * 4);
            if (addr + 2 > rom.size()) break;
            uint8_t grp = rom.read_byte(addr);
            uint8_t idx = rom.read_byte(addr + 1);
            if (grp == 0xFF) break;  // N_A terminator
            // Validate: Crystal has 26 groups
            if (grp > 0 && grp <= 26 && idx > 0 && idx < 100) {
                enqueue(grp, idx);
            }
        }
    }
    
    //=========================================================================
    // SETUP SCRIPT PROCESSING FOR MAP REFERENCE EXTRACTION
    //=========================================================================
    SymbolMap symbols;
    TypedScriptDecoder decoder(rom, symbols);
    
    NativeCallRegistry native_registry;
    native_registry.initialize();
    RamAddressRegistry ram_registry;
    ram_registry.initialize();
    
    StdScriptsTable std_scripts;
    std_scripts.load(rom, profile.offsets.std_scripts, profile.offsets.std_scripts_count);
    
    CFGBuilder cfg_builder;
    cfg_builder.set_std_scripts(&std_scripts);
    cfg_builder.set_native_registry(&native_registry);
    
    SemanticLegalizer legalizer;
    legalizer.set_native_registry(&native_registry);
    legalizer.set_ram_registry(&ram_registry);
    legalizer.set_num_pokemon(profile.counts.num_pokemon);  // Use profile domain
    
    // Track processed script addresses to avoid reprocessing
    std::set<uint32_t> processed_scripts;
    
    // Helper to extract map references from a semantic script IR.
    //
    // FAILURE POLICY:
    //   - Decode failure (std::runtime_error from TypedScriptDecoder):
    //     Re-throw.  A script address that cannot be decoded at all indicates
    //     a structural ROM issue — maps only reachable via that script would
    //     silently vanish from the graph.
    //   - CFG / lowering failure:
    //     Swallow.  The decode succeeded (the ROM bytes are valid), so any map
    //     ref encoded in direct warp/connection bytes is already in the graph.
    //     The script may legitimately fail to lower at compile time; that does
    //     not mean the ROM is unreadable.
    auto extract_script_map_refs = [&](uint32_t script_addr) {
        if (processed_scripts.contains(script_addr)) return;
        processed_scripts.insert(script_addr);
        
        // Stage 1: Decode — structural failure propagates.
        CrystalScriptIR ir = decoder.decode_script(script_addr);
        if (ir.commands.empty()) return;
        
        // Stage 2+: CFG and lowering — failures are non-fatal for discovery.
        try {
            CrystalCFG cfg = cfg_builder.build(ir);
            if (!cfg.validation.valid) return;
            
            enginemon::LoweringResult lowering = legalizer.lower(ir, cfg);
            
            for (const auto& block : lowering.ir.blocks) {
                for (const auto& inst : block.instructions) {
                    std::visit([&](const auto& sem_op) {
                        using T = std::decay_t<decltype(sem_op)>;
                        using namespace enginemon;
                        
                        if constexpr (std::is_same_v<T, Sem_Warp>) {
                            enqueue_map_id(sem_op.map);
                        } else if constexpr (std::is_same_v<T, Sem_WarpFacing>) {
                            enqueue_map_id(sem_op.map);
                        } else if constexpr (std::is_same_v<T, Sem_SetMapScene>) {
                            enqueue_map_id(sem_op.map);
                        } else if constexpr (std::is_same_v<T, Sem_CheckMapScene>) {
                            enqueue_map_id(sem_op.map);
                        } else if constexpr (std::is_same_v<T, Sem_ModifyWarp>) {
                            enqueue_map_id(sem_op.target_map);
                        } else if constexpr (std::is_same_v<T, Sem_SetBlackoutPoint>) {
                            enqueue_map_id(sem_op.map);
                        }
                        // Note: Sem_WarpToBackup and Sem_WarpToBackupFacing have NO map reference
                        // (they use wBackupMap which was set by a prior warpmod)
                    }, inst.op);
                }
            }
        } catch (...) {
            // CFG/lowering failure: acceptable, direct warp/connection bytes already handled.
        }
    };
    
    //=========================================================================
    // FIXED-POINT LOOP
    //=========================================================================
    const auto& o = profile.offsets;
    const auto& fmt = profile.format.map;
    
    while (!frontier.empty()) {
        MapIdRef ref = frontier.front();
        frontier.pop();
        
        // Extract and validate map.
        // A reachable map that cannot be extracted is a hard discovery failure:
        // its warps, connections, and scripts cannot be scanned, so descendants
        // of this node would silently disappear from the reachable graph.
        auto map_result = extractor.extract_map(ref.group, ref.map);
        if (!map_result.success) {
            throw std::runtime_error(
                std::format("discover_reachable_maps: extraction of reachable map ({},{}) failed: {}",
                            ref.group, ref.map,
                            map_result.error.empty() ? "(no detail)" : map_result.error));
        }
        
        const auto& map = map_result.map;
        
        // A reachable map with degenerate dimensions is structurally wrong —
        // extract_map() already validates dimensions and returns failure for
        // 0×0 or >100 maps, so reaching here means the extractor validated
        // them and the BFS accepted them.  If we somehow have invalid dimensions
        // at this point it is a structural inconsistency; record it and throw
        // rather than silently dropping the map (and its reachable neighbors).
        if (map.width == 0 || map.height == 0 ||
            map.width > 100 || map.height > 100) {
            throw std::runtime_error(
                std::format("discover_reachable_maps: reachable map ({},{}) has invalid "
                            "dimensions {}x{} — extraction inconsistency",
                            ref.group, ref.map, map.width, map.height));
        }
        
        // Record discovered map
        result.push_back(ref);
        
        //=====================================================================
        // COLLECT MAP REFERENCES FROM RAW EVENT DATA
        //=====================================================================
        
        // Get map header to find events pointer.
        // Bounds failures here mean the ROM data is structurally inconsistent
        // for a map we already successfully extracted — that is a hard error.
        uint32_t group_ptr_addr = o.map_group_pointers + ((ref.group - 1) * 2);
        if (group_ptr_addr + 2 > rom.size()) {
            throw std::runtime_error(
                std::format("discover_reachable_maps: map ({},{}) group pointer address "
                            "0x{:x} out of ROM bounds", ref.group, ref.map, group_ptr_addr));
        }
        
        uint16_t group_addr = rom.read_word(group_ptr_addr);
        uint32_t group_flat = rom.bank_to_flat(o.map_groups_bank, group_addr);
        uint32_t map_entry_addr = group_flat + ((ref.map - 1) * 9);
        
        if (map_entry_addr + 9 > rom.size()) {
            throw std::runtime_error(
                std::format("discover_reachable_maps: map ({},{}) map-entry address "
                            "0x{:x} out of ROM bounds", ref.group, ref.map, map_entry_addr));
        }
        
        auto entry = rom.read_bytes(map_entry_addr, 9);
        uint8_t attr_bank = entry[0];
        uint16_t attr_ptr = entry[3] | (entry[4] << 8);
        uint32_t header_addr = rom.bank_to_flat(attr_bank, attr_ptr);
        
        if (header_addr + fmt.header_size > rom.size()) {
            throw std::runtime_error(
                std::format("discover_reachable_maps: map ({},{}) header address "
                            "0x{:x} out of ROM bounds", ref.group, ref.map, header_addr));
        }
        
        auto header = rom.read_bytes(header_addr, fmt.header_size);
        uint8_t conn_byte = header[fmt.connections_offset];
        uint8_t script_bank = header[fmt.script_bank_offset];
        
        // Read connections (immediately after header)
        uint32_t conn_ptr = header_addr + fmt.header_size;
        
        auto read_conn = [&]() {
            if (conn_ptr + fmt.connection_size > rom.size()) {
                throw std::runtime_error(
                    std::format("discover_reachable_maps: map ({},{}) connection data "
                                "at 0x{:x} truncated (declared in conn_byte=0x{:02x})",
                                ref.group, ref.map, conn_ptr, conn_byte));
            }
            auto data = rom.read_bytes(conn_ptr, fmt.connection_size);
            uint8_t tgt_group = data[0];
            uint8_t tgt_map = data[1];
            // Validate: Crystal has 26 groups
            if (tgt_group > 0 && tgt_group <= 26 && tgt_map > 0 && tgt_map < 100) {
                enqueue(tgt_group, tgt_map);
            }
            conn_ptr += fmt.connection_size;
        };
        
        if (conn_byte & 0x08) read_conn();  // NORTH
        if (conn_byte & 0x04) read_conn();  // SOUTH
        if (conn_byte & 0x02) read_conn();  // WEST
        if (conn_byte & 0x01) read_conn();  // EAST
        
        // Events pointer is in the header at offset 9-10
        uint16_t events_addr = header[fmt.events_ptr_offset] | (header[fmt.events_ptr_offset + 1] << 8);
        uint32_t events_flat = rom.bank_to_flat(script_bank, events_addr);
        
        if (events_flat + 2 > rom.size()) {
            throw std::runtime_error(
                std::format("discover_reachable_maps: map ({},{}) events address "
                            "0x{:x} out of ROM bounds", ref.group, ref.map, events_flat));
        }
        
        // Parse events: 2 filler bytes, warp_count, warps...
        uint32_t ptr = events_flat;
        ptr += 2;  // Skip 2 filler bytes
        
        if (ptr + 1 > rom.size()) {
            throw std::runtime_error(
                std::format("discover_reachable_maps: map ({},{}) events header truncated "
                            "at 0x{:x}", ref.group, ref.map, ptr));
        }
        uint8_t warp_count = rom.read_byte(ptr++);
        
        // Sanity check warp count — Crystal has at most a handful of warps per map;
        // more than 50 indicates structural corruption rather than a legitimate map.
        if (warp_count > 50) {
            throw std::runtime_error(
                std::format("discover_reachable_maps: map ({},{}) has implausible warp count {} "
                            "— likely ROM structure corruption", ref.group, ref.map, warp_count));
        }
        
        // Read warps
        for (uint8_t i = 0; i < warp_count; ++i) {
            if (ptr + fmt.warp_size > rom.size()) {
                throw std::runtime_error(
                    std::format("discover_reachable_maps: map ({},{}) warp {} of {} truncated "
                                "at ROM address 0x{:x}",
                                ref.group, ref.map, i + 1, warp_count, ptr));
            }
            auto warp = rom.read_bytes(ptr, fmt.warp_size);
            // Warp: y, x, warp_index, target_group, target_map
            uint8_t tgt_group = warp[3];
            uint8_t tgt_map = warp[4];
            // Validate target (Crystal has 26 groups max)
            if (tgt_group > 0 && tgt_group <= 26 && tgt_map > 0 && tgt_map < 100) {
                enqueue(tgt_group, tgt_map);
            }
            ptr += fmt.warp_size;
        }
        
        //=====================================================================
        // FOLLOW SCRIPT MAP REFERENCES
        // This finds maps only reachable via script warp commands 
        // (e.g., NATIONAL_PARK_BUG_CONTEST from bug contest scripts)
        //=====================================================================
        
        // --- Object event scripts ---
        for (const auto& obj : map_result.map.objects) {
            if (obj.script_rom_address != 0) {
                extract_script_map_refs(obj.script_rom_address);
            }
        }
        
        // --- BG event scripts ---
        for (const auto& bg : map_result.map.bg_events) {
            if (bg.script_rom_address != 0) {
                extract_script_map_refs(bg.script_rom_address);
            }
        }
        
        // --- Scene scripts and callbacks from MapScripts header ---
        // Crystal MapScripts structure sizes (from pokecrystal/constants/script_constants.asm)
        constexpr uint8_t SCENE_SCRIPT_SIZE = 4;  // dw script_ptr, dw 0 (filler)
        constexpr uint8_t CALLBACK_SIZE = 3;      // db type, dw script_ptr
        
        // MapScripts header is at script_ptr in script_bank
        uint16_t scripts_ptr = header[fmt.script_ptr_offset] | (header[fmt.script_ptr_offset + 1] << 8);
        uint32_t map_scripts_addr = rom.bank_to_flat(script_bank, scripts_ptr);
        if (map_scripts_addr + 1 <= rom.size()) {
            uint32_t ms_ptr = map_scripts_addr;
            
            // Scene scripts: db count, then count * (dw script_ptr, dw 0)
            uint8_t scene_count = rom.read_byte(ms_ptr++);
            if (scene_count <= 20) {  // Sanity check
                for (uint8_t i = 0; i < scene_count; ++i) {
                    if (ms_ptr + SCENE_SCRIPT_SIZE > rom.size()) break;
                    
                    uint16_t scene_script_ptr = rom.read_word(ms_ptr);
                    ms_ptr += 2;  // script_ptr
                    ms_ptr += 2;  // filler (always 0)
                    
                    if (scene_script_ptr != 0) {
                        uint32_t scene_script_addr = rom.bank_to_flat(script_bank, scene_script_ptr);
                        if (scene_script_addr > 0 && scene_script_addr < rom.size()) {
                            extract_script_map_refs(scene_script_addr);
                        }
                    }
                }
                
                // Callbacks: db count, then count * (db type, dw script_ptr)
                if (ms_ptr + 1 <= rom.size()) {
                    uint8_t callback_count = rom.read_byte(ms_ptr++);
                    if (callback_count <= 20) {  // Sanity check
                        for (uint8_t i = 0; i < callback_count; ++i) {
                            if (ms_ptr + CALLBACK_SIZE > rom.size()) break;
                            
                            ms_ptr++;  // Skip callback type
                            uint16_t callback_ptr = rom.read_word(ms_ptr);
                            ms_ptr += 2;
                            
                            if (callback_ptr != 0) {
                                uint32_t callback_addr = rom.bank_to_flat(script_bank, callback_ptr);
                                if (callback_addr > 0 && callback_addr < rom.size()) {
                                    extract_script_map_refs(callback_addr);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    // Sort by (group, map) for deterministic output
    std::sort(result.begin(), result.end());
    
    return result;
}

} // namespace crystal
