// corpus_lowering_audit.cpp - Enumerate ALL lowering failures in expanded corpus
// Reports every body that fails to compile with exact failure details
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/compile/corpus_discovery.hpp"
#include "crystal/script/typed_decoder.hpp"
#include "crystal/script/crystal_cfg.hpp"
#include "crystal/script/semantic_legalizer.hpp"
#include "crystal/script/behavior_table.hpp"
#include "crystal/script/legality_gate.hpp"
#include "crystal/script/native_registry.hpp"
#include "crystal/script/elevator_registry.hpp"
#include "crystal/script/semantic_linker.hpp"
#include "crystal/script/text_registry.hpp"
#include "crystal/script/decoder.hpp"
#include "crystal/extract/map_extractor.hpp"
#include "engine/scripting/semantic_ir.hpp"
#include <iostream>
#include <iomanip>
#include <map>
#include <set>
#include <vector>
#include <sstream>

using namespace crystal;
using namespace enginemon;

// Opcode to mnemonic name mapping (from pokecrystal)
static const char* opcode_name(uint8_t opcode) {
    static const char* names[] = {
        "scall",           // 0x00
        "farscall",        // 0x01
        "memcall",         // 0x02
        "sjump",           // 0x03
        "farsjump",        // 0x04
        "memjump",         // 0x05
        "ifequal",         // 0x06
        "ifnotequal",      // 0x07
        "iffalse",         // 0x08
        "iftrue",          // 0x09
        "ifgreater",       // 0x0A
        "ifless",          // 0x0B
        "jumpstd",         // 0x0C
        "callstd",         // 0x0D
        "callasm",         // 0x0E
        "special",         // 0x0F
        "memcallasm",      // 0x10
        "checkmapscene",   // 0x11
        "setmapscene",     // 0x12
        "checkscene",      // 0x13
        "setscene",        // 0x14
        "setval",          // 0x15
        "addval",          // 0x16
        "random",          // 0x17
        "checkver",        // 0x18
        "readmem",         // 0x19
        "writemem",        // 0x1A
        "loadmem",         // 0x1B
        "readvar",         // 0x1C
        "writevar",        // 0x1D
        "loadvar",         // 0x1E
        "giveitem",        // 0x1F
        "takeitem",        // 0x20
        "checkitem",       // 0x21
        "givemoney",       // 0x22
        "takemoney",       // 0x23
        "checkmoney",      // 0x24
        "givecoins",       // 0x25
        "takecoins",       // 0x26
        "checkcoins",      // 0x27
        "addcellnum",      // 0x28
        "delcellnum",      // 0x29
        "checkcellnum",    // 0x2A
        "checktime",       // 0x2B
        "checkpoke",       // 0x2C
        "givepoke",        // 0x2D
        "giveegg",         // 0x2E
        "givepokemail",    // 0x2F
        "checkpokemail",   // 0x30
        "checkevent",      // 0x31
        "clearevent",      // 0x32
        "setevent",        // 0x33
        "checkflag",       // 0x34
        "clearflag",       // 0x35
        "setflag",         // 0x36
        "wildon",          // 0x37
        "wildoff",         // 0x38
        "xycompare",       // 0x39
        "warpmod",         // 0x3A
        "blackoutmod",     // 0x3B
        "warp",            // 0x3C
        "getmoney",        // 0x3D
        "getcoins",        // 0x3E
        "getnum",          // 0x3F
        "getmonname",      // 0x40
        "getitemname",     // 0x41
        "getcurlandmarkname", // 0x42
        "gettrainername",  // 0x43
        "getstring",       // 0x44
        "itemnotify",      // 0x45
        "pocketisfull",    // 0x46
        "opentext",        // 0x47
        "reanchormap",     // 0x48
        "closetext",       // 0x49
        "writeunusedbyte", // 0x4A
        "farwritetext",    // 0x4B
        "writetext",       // 0x4C
        "repeattext",      // 0x4D
        "yesorno",         // 0x4E
        "loadmenu",        // 0x4F
        "closewindow",     // 0x50
        "jumptextfaceplayer", // 0x51
        "farjumptext",     // 0x52
        "jumptext",        // 0x53
        "waitbutton",      // 0x54
        "promptbutton",    // 0x55
        "pokepic",         // 0x56
        "closepokepic",    // 0x57
        "2dmenu",          // 0x58
        "verticalmenu",    // 0x59
        "loadpikachudata", // 0x5A
        "randomwildmon",   // 0x5B
        "loadtemptrainer", // 0x5C
        "loadwildmon",     // 0x5D
        "loadtrainer",     // 0x5E
        "startbattle",     // 0x5F
        "reloadmapafterbattle", // 0x60
        "catchtutorial",   // 0x61
        "trainertext",     // 0x62
        "trainerflagaction", // 0x63
        "winlosstext",     // 0x64
        "scripttalkafter", // 0x65
        "endifjustbattled", // 0x66
        "checkjustbattled", // 0x67
        "setlasttalked",   // 0x68
        "applymovement",   // 0x69
        "applymovementlasttalked", // 0x6A
        "faceplayer",      // 0x6B
        "faceobject",      // 0x6C
        "variablesprite",  // 0x6D
        "disappear",       // 0x6E
        "appear",          // 0x6F
        "follow",          // 0x70
        "stopfollow",      // 0x71
        "moveobject",      // 0x72
        "writeobjectxy",   // 0x73
        "loademote",       // 0x74
        "showemote",       // 0x75
        "turnobject",      // 0x76
        "follownotexact",  // 0x77
        "earthquake",      // 0x78
        "changemapblocks", // 0x79
        "changeblock",     // 0x7A
        "reloadmap",       // 0x7B
        "refreshmap",      // 0x7C
        "writecmdqueue",   // 0x7D
        "delcmdqueue",     // 0x7E
        "playmusic",       // 0x7F
        "encountermusic",  // 0x80
        "musicfadeout",    // 0x81
        "playmapmusic",    // 0x82
        "dontrestartmapmusic", // 0x83
        "cry",             // 0x84
        "playsound",       // 0x85
        "waitsfx",         // 0x86
        "warpsound",       // 0x87
        "specialsound",    // 0x88
        "autoinput",       // 0x89
        "newloadmap",      // 0x8A
        "pause",           // 0x8B
        "deactivatefacing", // 0x8C
        "sdefer",          // 0x8D
        "warpcheck",       // 0x8E
        "stopandsjump",    // 0x8F
        "endcallback",     // 0x90
        "end",             // 0x91
        "reloadend",       // 0x92
        "endall",          // 0x93
        "pokemart",        // 0x94
        "elevator",        // 0x95
        "trade",           // 0x96
        "askforphonenumber", // 0x97
        "phonecall",       // 0x98
        "hangup",          // 0x99
        "describedecoration", // 0x9A
        "fruittree",       // 0x9B
        "specialphonecall", // 0x9C
        "checkphonecall",  // 0x9D
        "verbosegiveitem", // 0x9E
        "verbosegiveitemvar", // 0x9F
        "swarm",           // 0xA0
        "halloffame",      // 0xA1
        "credits",         // 0xA2
        "warpfacing",      // 0xA3
        "battletowertext", // 0xA4
        "getlandmarkname", // 0xA5
        "gettrainerclassname", // 0xA6
        "getname",         // 0xA7
        "wait",            // 0xA8
        "checksave",       // 0xA9
    };
    if (opcode <= 0xA9) return names[opcode];
    return "unknown";
}

struct LoweringFailure {
    uint32_t body_root;
    ScriptRootType root_type;
    std::string provenance;  // string for flexibility (could be address or diagnostic)
    uint8_t opcode;
    std::string failure_stage;
    std::string reason;
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <rom_path>\n";
        return 1;
    }
    
    auto rom = RomData::load(argv[1]);
    if (!rom) {
        std::cerr << "Failed to load ROM\n";
        return 1;
    }
    
    auto& registry = ProfileRegistry::instance();
    auto profile = registry.get_profile_by_hash(rom->hash());
    if (!profile) {
        std::cerr << "ROM not supported\n";
        return 1;
    }
    
    std::cout << "=== Corpus Lowering Audit ===\n\n";
    
    // Load StdScripts table
    StdScriptsTable std_scripts;
    std_scripts.load(*rom, profile->offsets.std_scripts, profile->offsets.std_scripts_count);
    
    // Setup decoder and pipeline
    SymbolMap symbols;
    TypedScriptDecoder decoder(*rom, symbols);
    
    NativeCallRegistry native_registry;
    native_registry.initialize();
    RamAddressRegistry ram_registry;
    ram_registry.initialize();
    ElevatorRegistry elevator_registry(*rom);
    
    CFGBuilder cfg_builder;
    cfg_builder.set_std_scripts(&std_scripts);
    cfg_builder.set_native_registry(&native_registry);
    
    SemanticLegalizer legalizer;
    legalizer.set_native_registry(&native_registry);
    legalizer.set_ram_registry(&ram_registry);
    legalizer.set_elevator_registry(&elevator_registry);
    // Note: num_pokemon defaults to 251 (vanilla Crystal) - appropriate for this tool
    // which only operates on supported vanilla Crystal ROM profiles.
    // The profile system guarantees this matches profile.counts.num_pokemon.
    legalizer.set_num_pokemon(profile->counts.num_pokemon);
    
    // Initialize TextRegistry so text-sequence lowering produces populated sequences.
    // Without this, writetext/jumptext bodies would produce empty Sem_ShowText sequences
    // which the legality gate rejects — giving a false impression of legality failures.
    // Uses ScriptDecoder (not TypedScriptDecoder) for text decoding, matching FullGameCompiler.
    crystal::SymbolMap text_symbols;
    ScriptDecoder script_decoder(*rom, text_symbols);
    TextRegistry text_registry(
        [&script_decoder](uint32_t addr) { return script_decoder.decode_text_sequence(addr); });
    legalizer.set_text_registry(&text_registry);
    
    // Build CompiledGameData with behavior_names for Sem_GameSpecificEvent validation
    CompiledGameData audit_game_data;
    for (std::size_t i = 0; i < BEHAVIOR_TABLE_SIZE; ++i) {
        audit_game_data.behavior_names.insert(BEHAVIOR_TABLE[i].behavior_name);
    }
    
    LegalityGate legality_gate;
    
    // Discover corpus
    MapExtractor extractor(*rom, *profile);
    auto corpus = discover_corpus(*rom, *profile, extractor, decoder, std_scripts);
    
    auto all_addresses = corpus.all_addresses();
    
    std::cout << "Total bodies to audit: " << all_addresses.size() << "\n\n";
    
    // Track failures
    std::vector<LoweringFailure> failures;
    std::map<uint8_t, std::set<uint32_t>> opcode_to_bodies;  // opcode -> bodies containing it
    
    size_t decode_failures = 0;
    size_t cfg_failures = 0;
    size_t lowering_failures_count = 0;
    size_t legality_failures = 0;
    size_t successes = 0;
    
    for (uint32_t addr : all_addresses) {
        ScriptRootType root_type = corpus.get_root_type(addr);
        
        // Stage 1: Decode
        CrystalScriptIR ir;
        try {
            ir = decoder.decode_script(addr);
            if (ir.commands.empty()) {
                std::ostringstream prov;
                prov << "0x" << std::hex << addr;
                failures.push_back({addr, root_type, prov.str(), 0, "decode", "empty command list"});
                ++decode_failures;
                continue;
            }
        } catch (const std::exception& e) {
            std::ostringstream prov;
            prov << "0x" << std::hex << addr;
            failures.push_back({addr, root_type, prov.str(), 0, "decode", e.what()});
            ++decode_failures;
            continue;
        }
        
        // Stage 2: CFG
        CrystalCFG cfg = cfg_builder.build(ir);
        if (!cfg.validation.valid) {
            std::ostringstream oss;
            for (const auto& err : cfg.validation.errors) {
                oss << err << "; ";
            }
            failures.push_back({addr, root_type, "0x" + std::to_string(addr), 0, "cfg", oss.str()});
            ++cfg_failures;
            continue;
        }
        
        // Stage 4: Lower
        LoweringResult lowering = legalizer.lower(ir, cfg);
        if (lowering.commands_unlowered > 0) {
            for (const auto& diag : lowering.unlowered) {
                // Track which opcode failed
                opcode_to_bodies[diag.opcode].insert(addr);
                
                failures.push_back({addr, root_type, diag.provenance, diag.opcode, "lowering", diag.reason});
            }
            ++lowering_failures_count;
            continue;
        }
        
        // Stage 5: Legality
        // Perform actual round-trip validation (structural integrity)
        std::vector<std::string> round_trip_errors;
        decoder.validate_script_round_trip(ir, &round_trip_errors);
        
        LegalityInput input;
        input.ir = &ir;
        input.decode_complete = true;
        input.round_trip_failures = static_cast<uint32_t>(round_trip_errors.size());
        input.unknown_opcodes = 0;
        for (const auto& cmd : ir.commands) {
            if (std::holds_alternative<Cmd_Unknown>(cmd.data)) {
                input.unknown_opcodes++;
            }
        }
        input.cfg = &cfg;
        input.native_registry = &native_registry;
        input.ram_registry = &ram_registry;
        input.lowering = &lowering;
        input.game_data = &audit_game_data;
        
        LegalityResult legality = legality_gate.validate(input);
        if (!legality.is_legal) {
            std::ostringstream oss;
            for (const auto& diag : legality.diagnostics()) {
                oss << diag.reason << "; ";
            }
            std::ostringstream prov;
            prov << "0x" << std::hex << addr;
            failures.push_back({addr, root_type, prov.str(), 0, "legality", oss.str()});
            ++legality_failures;
            continue;
        }
        
        ++successes;
    }
    
    std::cout << "=== Pipeline Results ===\n";
    std::cout << "Decode failures:   " << decode_failures << "\n";
    std::cout << "CFG failures:      " << cfg_failures << "\n";
    std::cout << "Lowering failures: " << lowering_failures_count << "\n";
    std::cout << "Legality failures: " << legality_failures << "\n";
    std::cout << "Successes:         " << successes << "\n";
    std::cout << "Total:             " << all_addresses.size() << "\n\n";
    
    if (!opcode_to_bodies.empty()) {
        std::cout << "=== Unlowered Opcodes Summary ===\n";
        std::cout << std::setw(6) << "Opcode" << " | " 
                  << std::setw(22) << "Mnemonic" << " | "
                  << std::setw(8) << "Bodies" << " | Sample Body Roots\n";
        std::cout << std::string(70, '-') << "\n";
        
        for (const auto& [opcode, bodies] : opcode_to_bodies) {
            std::cout << "  0x" << std::hex << std::setw(2) << std::setfill('0') << (int)opcode 
                      << std::dec << std::setfill(' ')
                      << " | " << std::setw(22) << opcode_name(opcode)
                      << " | " << std::setw(8) << bodies.size() << " | ";
            
            size_t count = 0;
            for (uint32_t body : bodies) {
                if (count++ >= 3) {
                    std::cout << "...";
                    break;
                }
                std::cout << "0x" << std::hex << body << std::dec << " ";
            }
            std::cout << "\n";
        }
    }
    
    if (!failures.empty()) {
        std::cout << "\n=== Complete Failure List ===\n";
        for (const auto& f : failures) {
            std::cout << "Body 0x" << std::hex << f.body_root << std::dec
                      << " (" << script_root_type_name(f.root_type) << ")"
                      << " @ " << f.provenance
                      << " opcode=0x" << std::hex << std::setw(2) << std::setfill('0') << (int)f.opcode << std::dec << std::setfill(' ')
                      << " [" << opcode_name(f.opcode) << "]"
                      << " stage=" << f.failure_stage
                      << " reason: " << f.reason << "\n";
        }
    }
    
    if (successes == all_addresses.size()) {
        std::cout << "\n✓ ALL " << all_addresses.size() << " BODIES COMPILE SUCCESSFULLY\n";
        return 0;
    } else {
        std::cout << "\n✗ " << (all_addresses.size() - successes) << " BODIES FAILED\n";
        return 1;
    }
}
