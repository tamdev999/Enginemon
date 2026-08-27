#pragma once
// crystal/compile/full_compiler.hpp
// Full-game Crystal ROM → EMON package compiler
//
// Architecture:
//   [serial] load + validate ROM
//            walk canonical Crystal tables
//            discover complete content set
//            assign stable semantic IDs
//            resolve source symbols/refs once
//
//   [serial typed script pipeline] process all scripts through:
//            TypedScriptDecoder → CrystalCFG → SemanticLegalizer → legality gate
//            Build production CompiledGameData from actual discovered content
//            Link all scripts through SemanticLinker
//            Fail hard on any decode/CFG/legality/link error
//
//   [parallel worker pool] compile independent maps/assets
//                          shared assets acquired through get-or-compute cache
//
//   [serial linker] collect results
//                   resolve refs
//                   validate completeness
//                   dedup
//                   sort by stable ID
//
//   [serial writer] emit EMON TOC/CRC
//
// Guarantees:
// - No manual reduced map list
// - No runtime ROM access
// - No omitted content
// - Stable IDs assigned before parallel work
// - Deterministic byte-identical EMON regardless of worker order
// - All scripts validated through typed pipeline (no Op_Raw degradation)

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "crystal/extract/tileset_extractor.hpp"
#include "crystal/extract/sprite_extractor.hpp"
#include "crystal/extract/font_extractor.hpp"
#include "crystal/compile/corpus_discovery.hpp"
#include "crystal/script/semantic_linker.hpp"
#include "crystal/output/native_package.hpp"
#include "engine/build/thread_pool.hpp"
#include "engine/build/asset_cache.hpp"
#include "engine/build/build_stats.hpp"
#include "engine/build/package_cache.hpp"
#include "engine/scripting/semantic_ir.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward declarations for typed script pipeline
namespace crystal {
class TypedScriptDecoder;
class ScriptDecoder;
class CFGBuilder;
class SemanticLegalizer;
class LegalityGate;
class ElevatorRegistry;
class TrainerRegistry;
class PokeMailRegistry;
class TextRegistry;
class StdScriptsTable;
class NativeCallRegistry;
class RamAddressRegistry;
class SymbolMap;
}

namespace crystal {

// Compiler version for cache compatibility
// Bump this version when compiler semantics change (extraction, lowering, linking)
// to invalidate cached packages and force recompilation.
// 2.1.0: sprite ID mapping 1-102, directional ledge semantics, connection offset fix
// 2.2.0: BG event type exhaustive mapping, condition_flag propagation fix
// 2.3.0: Semantic stabilization pass - operand order fix, flag namespaces, text commands, movement commands, round-trip wiring
// 2.4.0: Semantic fidelity fixes - string formatting operands, encountermusic/playmapmusic distinction,
//        newloadmap method preservation, reanchormap/refreshmap distinction, sdefer bank resolution,
//        TextDefinition identity with explicit control markers
// 2.5.0: Full semantic distinction preservation pass - Finding 1-11:
//        writetext/jumptext text pointer resolution via TextRegistry,
//        givepoke nickname/OT string resolution,
//        Sem_CatchTutorial distinct from Sem_StartBattle,
//        Sem_EndAll distinct from Sem_End,
//        Sem_LoadMenu/Sem_VerticalMenu/Sem_2DMenu distinct (not Sem_Choice),
//        Sem_DeactivateFacing distinct from Sem_Pause,
//        Sem_GiveItemVerboseVar with variable semantics (not literal quantity),
//        Sem_AskForPhoneNumber distinct from Sem_AddPhoneNumber,
//        Sem_PromptButton distinct from Sem_WaitButton,
//        getname type mapping corrected to pokecrystal constants,
//        InvalidDomain is now a hard linker gate
// 2.6.0: Crystal text frontend fidelity fixes:
//        Finding 1: explicit outer-command vs literal-body parser mode (TX_START/PlaceString)
//        Finding 2: TX_BOX param1=height/param2=width corrected (was transposed)
//        Finding 3: TX_FAR identity includes bank (param2, was using wrong field param1=0)
//        Finding 4: TextRaw identity uses hex byte content (not just length)
// 2.7.0: Script state and dynamic resource semantics fixes:
//        Finding 1: wScriptVar block_ctx invalidated by yesorno, giveitem, takeitem, checkitem,
//                   verbosegiveitem, verticalmenu, _2dmenu, special-fallback; GAMEBOY_CHECK and
//                   CHECK_MOBILE now call on_setval to keep fact in sync with emitted Sem_SetVar
//        Finding 2: cry opcode 0 → Sem_PlayCry{ScriptVar} (was SpeciesId{0} sentinel — wrong)
//        Finding 3: decode_movement_data no longer silently truncates; throws on missing terminator
//        Finding 4: writecmdqueue resolves bank-local pointer to flat ROM address (no raw bank leak)
//        Finding 5: pokepic operand 0 → Sem_Pokepic{ScriptVar} (was SpeciesId{0} sentinel — wrong)
// 2.8.0: Pre-Oracle semantic cleanup:
//        checksave, startbattle, checkpoke, givepoke, giveegg, CheckPokerus now
//        invalidate block-local wScriptVar context (all source-proven writers)
//        pocketisfull now emits Sem_PocketFullNotify (text display) instead of absorbing
//        legality gate rejects Sem_ShowText/Sem_ShowTextAndEnd/Sem_FacePlayerAndShowText with empty sequence
//        Sem_CheckWarp/Sem_CheckSave IR contracts corrected (CheckWarp does NOT write wScriptVar)
//        Sem_PrepareTextArg: account=2/account=3 magic sentinels replaced with typed NumberSource enum
// 2.9.0: Compiler failure-policy hardening — fail-closed gates for asset extraction
//        and reachable-map discovery:
//        Finding 1: link_results() now returns false (FATAL) on any tileset/sprite/
//          font/OBJ-palette extraction failure; previously silently skipped the asset.
//          validate_references() checks emitted-resource inventories (not discovery sets);
//          sprite missing from emitted package is now a hard error, not a warning.
//          Package completeness invariant: discovered == emitted for every asset class.
//        Finding 2: discover_reachable_maps() BFS now throws std::runtime_error when a
//          reachable map fails extraction; previously continued the BFS and silently
//          dropped the failing map, producing an incomplete (potentially unsafe) graph.
//          Script-assisted discovery: decode failures propagate (structural), CFG/lowering
//          failures remain suppressed (non-structural).
//          discover_all_maps/discover_tilesets/discover_sprites: re-extraction failures
//          also throw.  discover_content() catches and returns false with FATAL message.
// 2.10.0: Pre-Oracle pipeline integrity hardening:
//        Finding 1 (corpus shrink): corpus_test now gates on CORPUS_EXPECTED_UNIQUE_BODIES=1788
//          using authoritative discover_corpus() independently of test's own discovery path.
//          collect_initial_roots() silent-continue on map extraction failure → throw.
//        Finding 2 (map identity): discover_all_maps() silent duplicate-MapId drop → throw
//          with identity-collision diagnostic.
//        Finding 3 (package version): both PackageReaders now explicitly validate
//          header_.version before decoding; previously only MAGIC was checked.
//        Finding 4 (serialization narrowing): write_length_string() helper enforces
//          size ≤ 0xFFFF before every string-length write; was silent uint16_t truncation.
//        Finding 5 (traversal silent exits): discover_reachable_maps() BFS — all
//          ROM-bounds continues after result.push_back() converted to throws; width/height
//          degenerate-dimension continue converted to throw; warp_count>50 continue
//          converted to throw; events_flat and ptr bounds continues converted to throws.
// 2.12.0: Runtime package/cache integrity hardening (F1–F4):
//        F1 (RuntimeTileset partial success): from_package_data() now returns
//          std::optional<RuntimeTileset>; every truncation/structural failure returns
//          nullopt rather than a partially-populated tileset.  load_world_state()
//          checks the result and propagates failure — partial tilesets can no longer
//          enter the tileset cache or set state.valid = true.
//        F2 (duplicate package IDs): PackageWriter::add_map/add_tileset/add_tileset_atlas/
//          add_font_atlas/add_script/add_sprite all throw on duplicate ID.
//          PackageReader::open() rejects duplicate IDs in any chunk index (returns nullptr).
//        F3 (cache validation): validate_cached_package() now opens the cached package
//          via PackageReader::open() and calls validate() for per-chunk CRC check.
//          A damaged cache entry is treated as a cache miss; compilation proceeds
//          from source and produces a fresh valid package.
//        F4 (PackageHeader ABI guards): static_assert suite added in package_format.hpp
//          pinning sizeof(PackageHeader)==100 and all field offsets.  Accidental
//          layout changes (padding, reordering) now produce compile-time failures.
//        Adjacent: load_sprite() in package_reader.cpp hardened — stream failures
//          during sprite deserialization now return nullopt instead of partial sprite.
// 3.3.0: Map event ↔ script ID namespace fix:
//        Every packaged map event script reference now matches its package Script chunk key.
//        process_map_root_scripts() persists rom_addr_to_script_id_ (ROM address → canonical ID).
//        link_results() rewrites ObjectEvent/BgEvent/CoordEvent script_id fields before add_map()
//        using that canonical map — local positional IDs like "object_script_0" never survive
//        into the final package.  handle_interact() now hard-fails (script_start_failed=true)
//        when a recognised interaction's script is missing from the package.
// 3.4.0: Script VM P0 fixes:
//   Fix 1: VM result is integer; all bool-returning bindings now return 0/1 int.
//          JumpIf branches use result~=0/result==0. SetVar from ScriptVar uses
//          result~=0 and 1 or 0. CheckTime compound uses explicit integer or.
//   Fix 2: scall/farscall call/return stack: emit() pushes continuation ID onto
//          __call_stack before goto callee; Sem_End pops and dispatches via
//          __dispatch_return table; nested calls unwind correctly.
//   Fix 3: Sem_EndAll emits `__call_stack={}; return` (core VM, not BehaviorTable).
//   Fix 4: HeadlessGameLoop destructor and set_lua_runtime() clear deferred_script_fn
//          from old LuaRuntime preventing stale callback UAF.
//   Fix 5: Deferred script drain propagates start_script failure as script_error.
constexpr const char* CRYSTAL_COMPILER_VERSION = "crystal-3.4.0";
// EMON_FORMAT_VERSION must match PackageHeader::VERSION in engine/package/package_format.hpp.
// Used as part of BuildIdentity cache key — a stale cached package built against an
// older format is rejected and recompiled from source.
constexpr uint32_t EMON_FORMAT_VERSION = 3;

//=============================================================================
// DISCOVERED CONTENT
// Complete set of all content found in ROM, with stable IDs assigned
//=============================================================================

// MapIdRef for fixed-point discovery (group, map index pair)
struct MapIdRef {
    uint8_t group;
    uint8_t map;
    
    bool operator<(const MapIdRef& other) const {
        if (group != other.group) return group < other.group;
        return map < other.map;
    }
    
    bool operator==(const MapIdRef& other) const {
        return group == other.group && map == other.map;
    }
    
    // Pack to uint16_t for quick lookup
    uint16_t packed() const { return (group << 8) | map; }
    static MapIdRef from_packed(uint16_t v) { return {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v & 0xFF)}; }
};

struct DiscoveredMap {
    std::string map_id;
    uint8_t group;
    uint8_t index;
};

struct DiscoveredTileset {
    std::string tileset_id;
    uint8_t tileset_index;
};

struct DiscoveredSprite {
    std::string sprite_id;
    uint8_t sprite_index;
    bool is_player;
};

struct DiscoveredContent {
    std::vector<DiscoveredMap> maps;
    std::vector<DiscoveredTileset> tilesets;
    std::vector<DiscoveredSprite> sprites;
    
    // Lookup maps (ID → index in vectors)
    std::unordered_map<std::string, size_t> map_index;
    std::unordered_map<std::string, size_t> tileset_index;
    std::unordered_map<std::string, size_t> sprite_index;
    
    // Scripts discovered during map compilation (address → ID)
    std::unordered_map<uint32_t, std::string> script_address_to_id;
    std::mutex script_mutex;  // Protects script_address_to_id
};

//=============================================================================
// COMPILED RESULTS
// Results collected from parallel jobs, ready for linking
//=============================================================================

struct CompiledMapResult {
    std::string map_id;
    ExtractedMap map;
    // NOTE: Scripts are no longer stored here - they go through the typed pipeline
    bool success;
    std::string error;
};

//=============================================================================
// SCRIPT ADDRESS TRACKING
// For typed script pipeline
//=============================================================================

struct ScriptAddressInfo {
    uint32_t rom_address;
    std::string script_id;
    enginemon::MapId owning_map;  // MAP_NONE for StdScripts
};

struct LinkerInput {
    std::vector<CompiledMapResult> maps;
    std::mutex maps_mutex;
    
    std::vector<std::string> errors;
    std::mutex errors_mutex;
    
    void add_map(CompiledMapResult&& result) {
        std::lock_guard<std::mutex> lock(maps_mutex);
        maps.push_back(std::move(result));
    }
    
    void add_error(const std::string& error) {
        std::lock_guard<std::mutex> lock(errors_mutex);
        errors.push_back(error);
    }
};

//=============================================================================
// COMPILER CONFIG
//=============================================================================

struct FullCompilerConfig {
    // Worker count (0 = auto-detect)
    size_t worker_count = 0;
    
    // Time of day for palette rendering
    TimeOfDay tileset_time_of_day = TimeOfDay::Day;
    
    // Enable persistent package cache
    bool use_package_cache = true;
    
    // Package cache directory (empty = default)
    std::filesystem::path cache_dir;
    
    // Emit address comments in generated Lua (for debugging)
    bool emit_address_comments = false;
    
    // Compute options hash for cache identity
    std::string compute_options_hash() const;
};

//=============================================================================
// VALIDATION RESULTS
//=============================================================================

struct CompilerValidationError {
    enum class Type {
        MissingWarpDestination,
        MissingConnectionDestination,
        MissingScript,
        MissingSprite,
        MissingTileset,
        MissingFont,
        DuplicateId,
        Other
    };
    
    Type type;
    std::string source;      // e.g., map_id or script_id
    std::string reference;   // What was missing
    std::string message;
};

struct CompilerValidationResult {
    bool success = true;
    std::vector<CompilerValidationError> errors;
    std::vector<std::string> warnings;
};

//=============================================================================
// FULL GAME COMPILER
//=============================================================================

class FullGameCompiler {
public:
    FullGameCompiler(const RomData& rom, const ExtractionProfile& profile);
    ~FullGameCompiler();
    
    // Compile ROM to package file
    // Returns true on success
    bool compile(const std::filesystem::path& output_path, 
                 const FullCompilerConfig& config = {});
    
    // Get build statistics
    const enginemon::build::BuildStats& stats() const { return stats_; }
    
    // Get validation result
    const CompilerValidationResult& validation() const { return validation_; }
    
    // Get linked corpus result (valid after compile())
    const LinkedCorpus& linked_corpus() const { return linked_corpus_; }
    
    // Get compiled game data (valid after compile())
    const CompiledGameData& compiled_game_data() const { return compiled_game_data_; }

private:
    const RomData& rom_;
    const ExtractionProfile& profile_;
    
    // Extractors (thread-safe for read access to ROM)
    std::unique_ptr<MapExtractor> map_extractor_;
    std::unique_ptr<TilesetExtractor> tileset_extractor_;
    std::unique_ptr<SpriteExtractor> sprite_extractor_;
    std::unique_ptr<FontExtractor> font_extractor_;
    
    // Shared asset cache
    std::unique_ptr<enginemon::build::AssetCache> asset_cache_;
    
    // Thread pool
    std::unique_ptr<enginemon::build::ThreadPool> thread_pool_;
    
    // Persistent package cache
    std::unique_ptr<enginemon::build::PackageCache> package_cache_;
    
    // Build state
    DiscoveredContent content_;
    LinkerInput linker_input_;
    
    // Typed script pipeline components (owned, serial usage only)
    std::unique_ptr<SymbolMap> symbols_;
    std::unique_ptr<TypedScriptDecoder> typed_decoder_;
    std::unique_ptr<ScriptDecoder> script_decoder_;  // For text decoding
    std::unique_ptr<StdScriptsTable> std_scripts_;
    std::unique_ptr<NativeCallRegistry> native_registry_;
    std::unique_ptr<RamAddressRegistry> ram_registry_;
    std::unique_ptr<ElevatorRegistry> elevator_registry_;
    std::unique_ptr<TrainerRegistry> trainer_registry_;
    std::unique_ptr<PokeMailRegistry> pokemail_registry_;
    std::unique_ptr<TextRegistry> text_registry_;
    std::unique_ptr<CFGBuilder> cfg_builder_;
    std::unique_ptr<SemanticLegalizer> legalizer_;
    std::unique_ptr<LegalityGate> legality_gate_;
    std::unique_ptr<SemanticLinker> semantic_linker_;
    
    // Production compiled game data (built from actual discovered content)
    CompiledGameData compiled_game_data_;
    
    // Unified corpus discovery result (stored for diagnostics)
    CorpusDiscoveryResult corpus_discovery_;
    
    // Linked corpus result
    LinkedCorpus linked_corpus_;
    
    // Collected script IRs for linking
    std::vector<enginemon::SemanticScriptIR> map_root_irs_;
    std::vector<enginemon::SemanticScriptIR> std_script_irs_;

    // Canonical script ID map: ROM flat address → compiler-assigned script_id.
    // Built in process_map_root_scripts() from ir.source_rom_address → ir.script_id.
    // Used in link_results() to rewrite map event script_id fields before package
    // serialization so every event's script reference matches its package script key.
    //
    // This is the single canonicalization point.  Every event that carries a
    // script_rom_address gets its script_id replaced with the value from this map.
    // Local positional IDs like "object_script_0" or "bg_event_2" never survive
    // into the final package; they are rewritten here before add_map() is called.
    std::unordered_map<uint32_t, std::string> rom_addr_to_script_id_;
    
    // Emitted-resource inventories — populated in link_results() from
    // successfully emitted assets.  validate_references() checks these, not
    // the discovery-intent sets, so a discovery hit whose extraction failed
    // cannot pass reference validation.
    std::unordered_set<std::string> emitted_tileset_ids_;
    std::unordered_set<std::string> emitted_sprite_ids_;
    bool emitted_obj_palettes_ = false;
    bool emitted_font_ = false;
    // Icon type package keys referenced by the species→icon map.
    // Populated in link_results() from build_species_icon_map() output.
    // validate_references() gates: every entry here must be in emitted_sprite_ids_.
    std::unordered_set<std::string> emitted_icon_type_ids_;

    // Single-use contract: compile() must not be called more than once on the
    // same instance.  Accumulated build state (content_, linker_input_, etc.)
    // is not reset between calls and would produce colliding IDs or stale data.
    bool compile_called_ = false;
    
    //=========================================================================
    // TEST SEAMS
    // Null in production; set via for_test_* methods to inject extraction
    // failures in adversarial tests.  std::function<> checked before real
    // extractor — zero overhead when null.
    //=========================================================================
    std::function<enginemon::build::AssetResult<ExtractedTileset>(const std::string&)> test_tileset_override_;
    std::function<enginemon::build::AssetResult<RuntimeSprite>(const std::string&)>    test_sprite_override_;
    std::function<enginemon::build::AssetResult<FontAtlas>()>                           test_font_override_;
    std::function<enginemon::build::AssetResult<SpriteObjPalettes>()>                  test_palettes_override_;
    
public:
    // Test-only injection methods — call before compile()
    void for_test_fail_tileset(const std::string& tileset_id) {
        test_tileset_override_ = [tileset_id](const std::string& id) ->
            enginemon::build::AssetResult<ExtractedTileset> {
            if (id == tileset_id)
                return enginemon::build::AssetError{"injected tileset failure", id};
            return enginemon::build::AssetError{"no real extractor in test override", id};
        };
    }
    void for_test_fail_sprite(const std::string& sprite_id) {
        test_sprite_override_ = [sprite_id](const std::string& id) ->
            enginemon::build::AssetResult<RuntimeSprite> {
            if (id == sprite_id)
                return enginemon::build::AssetError{"injected sprite failure", id};
            return enginemon::build::AssetError{"no real extractor in test override", id};
        };
    }
    void for_test_fail_font() {
        test_font_override_ = []() -> enginemon::build::AssetResult<FontAtlas> {
            return enginemon::build::AssetError{"injected font failure", "font"};
        };
    }
    void for_test_fail_palettes() {
        test_palettes_override_ = []() -> enginemon::build::AssetResult<SpriteObjPalettes> {
            return enginemon::build::AssetError{"injected OBJ palette failure", "obj_palettes"};
        };
    }
    // Causes extract_map() to return failure for this semantic map ID during
    // the full compile() pipeline — used to test discovery fail-closed behaviour.
    // Takes (group, index) since semantic IDs aren't resolved until extraction.
    void for_test_fail_map(uint8_t group, uint8_t index) {
        map_extractor_->for_test_fail_extraction(group, index);
    }
    
private:
    
    enginemon::build::BuildStats stats_;
    CompilerValidationResult validation_;
    
    //=========================================================================
    // PHASE 1: Discovery (serial)
    //=========================================================================
    
    // Discover all content from ROM tables
    bool discover_content();
    
    // Walk map group tables to find all maps
    void discover_all_maps();
    
    // Discover tilesets referenced by maps
    void discover_tilesets();
    
    // Discover sprites referenced by maps
    void discover_sprites();
    
    //=========================================================================
    // PHASE 2: Typed Script Pipeline (serial)
    // Process all scripts through the full typed pipeline
    //=========================================================================
    
    // Initialize typed script pipeline components
    bool init_typed_pipeline();
    
    // Build production CompiledGameData from actual discovered content
    void build_production_game_data();
    
    // Collect all script addresses from discovered maps
    void collect_script_addresses(std::set<uint32_t>& map_root_addresses,
                                   std::map<uint32_t, enginemon::MapId>& address_to_map);
    
    // Process a single script through Stages 1-5 (decode → CFG → lower → legality)
    // Returns nullopt on hard failure (compile fails)
    std::optional<enginemon::SemanticScriptIR> process_script_typed(
        uint32_t rom_address,
        const std::string& script_id);
    
    // Process all map-root scripts through typed pipeline
    bool process_map_root_scripts(const std::set<uint32_t>& addresses,
                                   const std::map<uint32_t, enginemon::MapId>& address_to_map);
    
    // Process all StdScript bodies through typed pipeline  
    bool process_std_scripts();
    
    // Finalize registries after script processing (elevator, etc.)
    void finalize_registries();
    
    // Link all scripts through SemanticLinker
    bool link_scripts(const std::map<uint32_t, enginemon::MapId>& address_to_map);
    
    //=========================================================================
    // PHASE 3: Parallel Asset Compilation
    //=========================================================================
    
    // Submit all compilation jobs to thread pool
    void submit_compilation_jobs(const FullCompilerConfig& config);
    
    // Individual job functions (run on worker threads)
    CompiledMapResult compile_map_job(const DiscoveredMap& map_info,
                                       const FullCompilerConfig& config);
    
    // Get or compute cached tileset
    enginemon::build::AssetResult<ExtractedTileset> get_tileset(const std::string& tileset_id);
    
    // Get or compute cached sprite
    enginemon::build::AssetResult<RuntimeSprite> get_sprite(const std::string& sprite_id);
    
    // Get or compute cached font
    enginemon::build::AssetResult<FontAtlas> get_font();
    
    // Get or compute cached OBJ palettes
    enginemon::build::AssetResult<SpriteObjPalettes> get_obj_palettes();
    
    //=========================================================================
    // PHASE 4: Linking (serial)
    //=========================================================================
    
    // Collect and link all results
    bool link_results(PackageWriter& writer);
    
    // Validate all cross-references
    CompilerValidationResult validate_references();
    
    // Sort results by stable ID for deterministic output
    void sort_by_stable_id();
    
    //=========================================================================
    // PHASE 5: Serialization (serial)
    //=========================================================================
    
    // Write deterministic package
    bool write_package(const std::filesystem::path& output_path, PackageWriter& writer);
    
    //=========================================================================
    // Helpers
    //=========================================================================
    
    // Make globally unique script ID
    static std::string make_global_script_id(const std::string& map_id, 
                                              const std::string& local_id);
    
    // Build identity for cache
    enginemon::build::BuildIdentity make_build_identity(const FullCompilerConfig& config) const;
};

//=============================================================================
// PUBLIC MAP DISCOVERY API
// Fixed-point reachable map discovery for use by compiler and tests
//=============================================================================

// Discover all reachable maps via fixed-point algorithm:
//   seed maps → extract → follow warps/connections → follow script map refs → repeat
// Returns sorted vector of (group, map) references for all discoverable maps.
std::vector<MapIdRef> discover_reachable_maps(
    const RomData& rom,
    const ExtractionProfile& profile,
    MapExtractor& extractor);

} // namespace crystal
