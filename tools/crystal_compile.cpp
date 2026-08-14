// tools/crystal_compile.cpp
// Crystal ROM compiler - converts ROM to native game format
// Status: Skeleton - ROM loading and profile identification only

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/rom/symbol_map.hpp"

#include <iostream>
#include <filesystem>

using namespace crystal;

namespace {

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " <rom_file> [options]\n"
              << "\n"
              << "Compiles a Pokemon Crystal ROM into native Enginemon format.\n"
              << "\n"
              << "Options:\n"
              << "  -o, --output DIR    Output directory (default: ./output)\n"
              << "  -s, --symbols FILE  RGBDS symbol file (from pokecrystal build)\n"
              << "  --verbose           Verbose output\n"
              << "  --help              Show this help\n";
}

struct CompileOptions {
    std::filesystem::path rom_path;
    std::filesystem::path output_path = "./output";
    std::filesystem::path symbols_path;
    bool verbose = false;
};

bool parse_args(int argc, char* argv[], CompileOptions& opts) {
    if (argc < 2) return false;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            return false;
        } else if (arg == "--verbose" || arg == "-v") {
            opts.verbose = true;
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            opts.output_path = argv[++i];
        } else if ((arg == "-s" || arg == "--symbols") && i + 1 < argc) {
            opts.symbols_path = argv[++i];
        } else if (arg[0] != '-') {
            opts.rom_path = arg;
        }
    }
    
    return !opts.rom_path.empty();
}

void log(const CompileOptions& opts, const std::string& msg) {
    if (opts.verbose) {
        std::cout << "[verbose] " << msg << "\n";
    }
}

} // namespace

int main(int argc, char* argv[]) {
    CompileOptions opts;
    if (!parse_args(argc, argv, opts)) {
        print_usage(argv[0]);
        return 1;
    }
    
    // Load ROM
    std::cout << "Loading ROM: " << opts.rom_path << "\n";
    auto rom = RomData::load(opts.rom_path);
    if (!rom) {
        std::cerr << "Failed to load ROM\n";
        return 1;
    }
    
    // Validate
    auto validation = validate_crystal_rom(*rom);
    if (!validation.valid) {
        std::cerr << "ROM validation failed:\n";
        for (const auto& err : validation.errors) {
            std::cerr << "  - " << err << "\n";
        }
        return 1;
    }
    
    std::cout << "ROM validated: " << rom->header().title << "\n";
    std::cout << "SHA-1: " << rom->hash() << "\n";
    std::cout << "Version: ";
    switch (validation.version) {
        case CrystalVersion::USA_Rev0: std::cout << "USA Rev 0"; break;
        case CrystalVersion::USA_Rev1: std::cout << "USA Rev 1"; break;
        case CrystalVersion::Europe: std::cout << "Europe"; break;
        case CrystalVersion::Japan: std::cout << "Japan"; break;
        default: std::cout << "Unknown"; break;
    }
    std::cout << "\n";
    
    // Get extraction profile (strict - must match exactly)
    auto& registry = ProfileRegistry::instance();
    const auto* profile = registry.get_profile_by_hash(rom->hash());
    if (!profile) {
        std::cerr << "Error: ROM not supported\n";
        std::cerr << "This ROM's SHA-1: " << rom->hash() << "\n";
        std::cerr << "\nSupported ROMs:\n";
        for (const auto& [sha1, name] : registry.supported_roms()) {
            std::cerr << "  " << name << "\n";
            std::cerr << "    " << sha1 << "\n";
        }
        std::cerr << "\nExtraction requires a ROM with a known profile.\n";
        std::cerr << "No fallback guessing is performed for data integrity.\n";
        return 1;
    }
    std::cout << "Profile: " << profile->version_string << "\n";
    std::cout << "Profile generator: " << profile->provenance.generator_version << "\n";
    
    // Load symbols (optional, for development)
    std::unique_ptr<SymbolMap> symbols;
    if (!opts.symbols_path.empty()) {
        log(opts, "Loading symbols from " + opts.symbols_path.string());
        symbols = SymbolMap::load(opts.symbols_path);
        if (symbols) {
            std::cout << "Loaded " << symbols->count() << " development symbols\n";
        } else {
            std::cerr << "Warning: Failed to load symbol file\n";
        }
    }
    
    // Create output directory
    std::filesystem::create_directories(opts.output_path);
    log(opts, "Output directory: " + opts.output_path.string());
    
    // TODO: Implement extraction stages
    // Each stage uses profile->offsets for ROM addresses
    
    std::cout << "\n=== Profile Offsets ===\n";
    std::cout << std::hex;
    std::cout << "  MapGroupPointers: 0x" << profile->offsets.map_group_pointers << "\n";
    std::cout << "  BaseData:         0x" << profile->offsets.base_data << "\n";
    std::cout << "  Moves:            0x" << profile->offsets.moves << "\n";
    std::cout << "  Tilesets:         0x" << profile->offsets.tilesets << "\n";
    std::cout << std::dec;
    
    std::cout << "\n=== TODO ===\n";
    std::cout << "  [ ] Extract Pokemon data\n";
    std::cout << "  [ ] Extract moves\n";
    std::cout << "  [ ] Extract items\n";
    std::cout << "  [ ] Extract types\n";
    std::cout << "  [ ] Extract maps\n";
    std::cout << "  [ ] Resolve map connections\n";
    std::cout << "  [ ] Decode scripts → Lua\n";
    std::cout << "  [ ] Extract sprites/tiles\n";
    std::cout << "  [ ] Extract audio\n";
    std::cout << "  [ ] Write native game package\n";
    
    std::cout << "\nProfile system ready. Extraction stages to be implemented.\n";
    return 0;
}
