// tools/emon_compile.cpp
// Crystal ROM → EMON package compiler (full game)
//
// Usage: emon_compile <rom_path> [output_path] [--workers=N] [--no-cache]
//
// Compiles a Crystal ROM to an EMON package file that the runtime can load
// without needing the original ROM.
//
// Features:
// - Discovers ALL maps from ROM tables (no manual list)
// - Parallel compilation with thread pool
// - Shared asset cache with compute-once semantics
// - Deterministic byte-identical EMON output
// - Persistent package cache (skips compilation if cached)

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/compile/full_compiler.hpp"

#include <iostream>
#include <filesystem>
#include <string>
#include <cstring>

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " <rom_path> [output_path] [options]\n";
    std::cerr << "\nCompiles a Crystal ROM to an EMON package file.\n";
    std::cerr << "\nOptions:\n";
    std::cerr << "  --workers=N    Use N worker threads (default: auto)\n";
    std::cerr << "  --no-cache     Disable persistent package cache\n";
    std::cerr << "  --help         Show this help message\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    // Parse arguments
    std::filesystem::path rom_path;
    std::filesystem::path output_path;
    crystal::FullCompilerConfig config;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        else if (arg.starts_with("--workers=")) {
            config.worker_count = std::stoul(arg.substr(10));
        }
        else if (arg == "--no-cache") {
            config.use_package_cache = false;
        }
        else if (rom_path.empty()) {
            rom_path = arg;
        }
        else if (output_path.empty()) {
            output_path = arg;
        }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }
    
    if (rom_path.empty()) {
        std::cerr << "Error: ROM path required\n";
        print_usage(argv[0]);
        return 1;
    }
    
    if (output_path.empty()) {
        // Default output: same name as ROM but with .emon extension
        output_path = rom_path;
        output_path.replace_extension(".emon");
    }
    
    std::cout << "=== EMON Full Game Compiler ===\n";
    std::cout << "ROM: " << rom_path << "\n";
    std::cout << "Output: " << output_path << "\n\n";
    
    // Load ROM
    auto rom = crystal::RomData::load(rom_path.string());
    if (!rom) {
        std::cerr << "Failed to load ROM: " << rom_path << "\n";
        return 1;
    }
    
    // Identify ROM version — try exact hash first, then Crystal layout validation
    auto& registry = crystal::ProfileRegistry::instance();

    // Pass the stock Crystal v1.1 profile as the explicit compatibility fallback.
    // This allows Crystal ROM hacks (different SHA-1 but same table layout) to
    // compile without requiring a separate registered profile.
    const crystal::ExtractionProfile* crystal_v11 =
        registry.get_profile(crystal::RomVersion::Crystal_USA_v1_1);

    auto compat = registry.get_profile_for_rom(
        rom->hash(),
        rom->raw().data(),
        rom->size(),
        crystal_v11);

    if (!compat.profile) {
        std::cerr << "ROM not recognized and layout validation failed.\n";
        std::cerr << "SHA-1: " << rom->hash() << "\n";
        std::cerr << "Reason: " << compat.reason << "\n";
        return 1;
    }

    const crystal::ExtractionProfile* profile = compat.profile;

    if (compat.match_type == crystal::ProfileRegistry::CompatMatchType::ExactHash) {
        std::cout << "ROM identified: " << profile->version_string << "\n\n";
    } else {
        std::cout << "ROM SHA-1 not in registry — accepted via layout validation "
                  << "against " << profile->version_string << " profile.\n";
        std::cout << "SHA-1: " << rom->hash() << "\n\n";
    }
    
    // Create full compiler and compile
    crystal::FullGameCompiler compiler(*rom, *profile);
    
    if (!compiler.compile(output_path, config)) {
        std::cerr << "\nCompilation failed.\n";
        
        // Print validation errors if any
        const auto& validation = compiler.validation();
        if (!validation.success) {
            std::cerr << "\nValidation errors:\n";
            for (const auto& err : validation.errors) {
                std::cerr << "  - " << err.message << "\n";
            }
        }
        
        return 1;
    }
    
    std::cout << "\nPackage written to: " << output_path << "\n";
    return 0;
}
