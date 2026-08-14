// Quick test to see generated Lua output
#include <iostream>
#include "crystal/rom/loader.hpp"
#include "crystal/rom/symbol_map.hpp"
#include "crystal/script/decoder.hpp"
#include "crystal/script/lua_emitter.hpp"
using namespace crystal;

int main(int argc, char* argv[]) {
    if (argc < 2) { std::cerr << "Usage: lua_demo <rom>\n"; return 1; }
    auto rom = RomData::load(argv[1]);
    if (!rom) { std::cerr << "Failed to load ROM\n"; return 1; }
    
    SymbolMap symbols;
    ScriptDecoder decoder(*rom, symbols);
    
    // NewBarkTownSign - bank 0x6A, addr 0x40C8
    uint32_t addr = rom->bank_to_flat(0x6A, 0x40C8);
    auto script = decoder.decode_script(addr, "NewBarkTownSign");
    
    std::cout << "=== NewBarkTownSign Script ===\n";
    std::cout << "Instructions: " << script.instructions.size() << "\n";
    std::cout << "ROM range: 0x" << std::hex << script.rom_start 
              << " - 0x" << script.rom_end << std::dec << "\n\n";
    
    LuaEmitter emitter;
    std::cout << "=== Generated Lua ===\n";
    std::cout << emitter.emit(script) << "\n";
    
    return 0;
}
