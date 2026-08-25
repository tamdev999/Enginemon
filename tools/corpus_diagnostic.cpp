// Temporary diagnostic: find corpus scripts with real endall + scall-that-starts
// Build: cmake --build build --target corpus_diagnostic --config Release

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/compile/full_compiler.hpp"
#include "engine/package/package_reader.hpp"
#include "engine/core/game_state.hpp"
#include "engine/core/game_loop.hpp"
#include "engine/scripting/lua_runtime.hpp"
#include <iostream>
#include <filesystem>
#include <string>
#include <algorithm>

static bool try_start(enginemon::PackageReader& reader, const std::string& sid) {
    enginemon::GameState gs;
    gs.rng.seed(0xDEADBEEFULL);
    enginemon::LuaRuntime rt;
    rt.set_game_state(&gs);
    enginemon::HeadlessGameLoop loop;
    loop.set_game_state(&gs);
    loop.set_lua_runtime(&rt);
    loop.set_script_loader([&reader](const std::string& id) -> std::string {
        auto l = reader.load_script(id);
        return l.value_or("");
    });
    loop.set_collision_data([](int32_t, int32_t) {
        return enginemon::CollisionClass::Floor;
    });
    return loop.start_script(sid);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "Usage: corpus_diagnostic <rom>\n"; return 1; }
    auto rom = crystal::RomData::load(argv[1]);
    if (!rom) { std::cerr << "no rom\n"; return 1; }
    auto& reg = crystal::ProfileRegistry::instance();
    auto* prof = reg.get_profile_by_hash(rom->hash());
    if (!prof) { std::cerr << "no profile\n"; return 1; }

    namespace fs = std::filesystem;
    auto pkg = fs::temp_directory_path() / "corpus_diag.emon";
    crystal::FullGameCompiler compiler(*rom, *prof);
    crystal::FullCompilerConfig cfg;
    cfg.use_package_cache = false;
    cfg.worker_count = 1;
    std::cerr << "Compiling...\n";
    if (!compiler.compile(pkg, cfg)) { std::cerr << "compile failed\n"; return 1; }

    auto reader = enginemon::PackageReader::open(pkg);
    if (!reader) { std::cerr << "no reader\n"; return 1; }

    auto scripts = reader->list_scripts();
    std::cerr << "Total scripts: " << scripts.size() << "\n";

    // 1. Real endall scripts: "__call_stack = {}; return" on ONE line
    std::cout << "=== REAL ENDALL ===\n";
    for (const auto& sid : scripts) {
        auto lua = reader->load_script(sid);
        if (!lua) continue;
        bool has_endall = (lua->find("__call_stack = {}; return") != std::string::npos);
        if (!has_endall) continue;
        std::cout << "ENDALL_SCRIPT: " << sid << "\n";
        // Show 200 chars before the endall to see what precedes it
        size_t pos = lua->find("__call_stack = {}; return");
        size_t start = (pos > 300) ? pos - 300 : 0;
        std::string snippet = lua->substr(start, std::min((size_t)400, lua->size() - start));
        // Replace newlines for readable output
        for (auto& c : snippet) if (c == '\n') c = '|';
        std::cout << "  CONTEXT: " << snippet << "\n";
    }

    // 2. Scall scripts that actually start
    std::cout << "\n=== SCALL SCRIPTS THAT START ===\n";
    for (const auto& sid : scripts) {
        auto lua = reader->load_script(sid);
        if (!lua) continue;
        if (lua->find("table.insert(__call_stack") == std::string::npos) continue;
        bool started = try_start(*reader, sid);
        if (started) {
            std::cout << "STARTS: " << sid << "\n";
            // Show first 400 chars
            std::string head = lua->substr(0, std::min((size_t)400, lua->size()));
            for (auto& c : head) if (c == '\n') c = '|';
            std::cout << "  HEAD: " << head << "\n";
        }
    }

    // 3. StdScript list with farscall indicator
    std::cout << "\n=== STD SCRIPTS ===\n";
    for (const auto& sid : scripts) {
        if (sid.find("std_") != 0) continue;
        auto lua = reader->load_script(sid);
        if (!lua) continue;
        bool has_call = lua->find("table.insert(__call_stack") != std::string::npos;
        std::cout << "STD: " << sid
                  << " call_stack=" << (has_call ? "yes" : "no")
                  << " len=" << lua->size() << "\n";
    }

    // Dump full content of specific scripts
    std::cout << "\n=== FULL WHITNEY SCRIPT ===\n";
    {
        auto lua = reader->load_script("map_11_3_0x344076");
        if (lua) std::cout << *lua << "\n";
    }

    // Check specific addresses
    std::cout << "\n=== SPECIFIC FLAT ADDRESSES ===\n";
    std::vector<std::pair<std::string, uint32_t>> check_addrs = {
        {"PhoneScript_AnswerPhone_Male", 777806},
        {"PhoneScript_GreetPhone_Male",  778678},
        {"AlanPhoneCalleeScript",        47 * 16384 + (0x58a6 - 0x4000)},
        {"Script_AbortBugContest",       4 * 16384 + (0x62c1 - 0x4000)},
        {"GoldenrodGymWhitneyScript",    344076},
    };
    for (const auto& [name, flat] : check_addrs) {
        // Try several ID formats
        std::vector<std::string> try_ids = {
            "map_11_3_0x" + std::to_string(flat),
            "script_0x" + std::to_string(flat),
            "std_0x" + std::to_string(flat),
        };
        // Also scan all scripts for flat address in name
        bool found = false;
        for (const auto& sid : scripts) {
            if (sid.find("0x" + std::to_string(flat)) != std::string::npos) {
                auto lua = reader->load_script(sid);
                std::cout << "  FOUND " << name << " flat=" << flat
                          << " as '" << sid << "' len=" << (lua ? lua->size() : 0) << "\n";
                if (lua) {
                    std::string head = lua->substr(0, std::min((size_t)200, lua->size()));
                    for (auto& c : head) if (c == '\n') c = '|';
                    std::cout << "    head: " << head << "\n";
                    bool started = try_start(*reader, sid);
                    std::cout << "    starts=" << started << "\n";
                }
                found = true;
                break;
            }
        }
        if (!found) std::cout << "  NOT FOUND: " << name << " flat=" << flat << "\n";
    }

    return 0;
}
