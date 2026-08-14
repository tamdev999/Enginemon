// trainer_verify.cpp - Diagnostic tool for TrainerRegistry verification
#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include "crystal/script/trainer_registry.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstdint>

using namespace crystal;

// Known trainer counts from pokecrystal source (parties.asm)
// Counted by: ; TRAINERNAME (N) comment lines per group
// Format: {group_id, count, "GroupName"}
struct AuthoritativeGroup {
    uint8_t group_id;
    size_t count;
    const char* name;
};

std::vector<AuthoritativeGroup> AUTHORITATIVE_COUNTS = {
    {1, 1, "FalknerGroup"},
    {2, 1, "WhitneyGroup"},
    {3, 1, "BugsyGroup"},
    {4, 1, "MortyGroup"},
    {5, 1, "PryceGroup"},
    {6, 1, "JasmineGroup"},
    {7, 1, "ChuckGroup"},
    {8, 1, "ClairGroup"},
    {9, 15, "Rival1Group"},
    {10, 0, "PokemonProfGroup"},  // Empty
    {11, 1, "WillGroup"},
    {12, 3, "PKMNTrainerGroup"},
    {13, 1, "BrunoGroup"},
    {14, 1, "KarenGroup"},
    {15, 1, "KogaGroup"},
    {16, 1, "ChampionGroup"},
    {17, 1, "BrockGroup"},
    {18, 1, "MistyGroup"},
    {19, 1, "LtSurgeGroup"},
    {20, 5, "ScientistGroup"},
    {21, 1, "ErikaGroup"},
    {22, 14, "YoungsterGroup"},
    {23, 24, "SchoolboyGroup"},
    {24, 19, "BirdKeeperGroup"},
    {25, 17, "LassGroup"},
    {26, 1, "JanineGroup"},
    {27, 20, "CooltrainerMGroup"},
    {28, 21, "CooltrainerFGroup"},
    {29, 17, "BeautyGroup"},
    {30, 15, "PokemaniacGroup"},
    {31, 31, "GruntMGroup"},
    {32, 5, "GentlemanGroup"},
    {33, 2, "SkierGroup"},
    {34, 3, "TeacherGroup"},
    {35, 1, "SabrinaGroup"},
    {36, 19, "BugCatcherGroup"},
    {37, 25, "FisherGroup"},
    {38, 21, "SwimmerMGroup"},
    {39, 19, "SwimmerFGroup"},
    {40, 13, "SailorGroup"},
    {41, 14, "SuperNerdGroup"},
    {42, 6, "Rival2Group"},
    {43, 2, "GuitaristGroup"},
    {44, 22, "HikerGroup"},
    {45, 9, "BikerGroup"},
    {46, 1, "BlaineGroup"},
    {47, 3, "BurglarGroup"},
    {48, 8, "FirebreatherGroup"},
    {49, 6, "JugglerGroup"},
    {50, 9, "BlackbeltGroup"},
    {51, 4, "ExecutiveMGroup"},
    {52, 12, "PsychicGroup"},
    {53, 26, "PicnickerGroup"},
    {54, 22, "CamperGroup"},
    {55, 2, "ExecutiveFGroup"},
    {56, 12, "SageGroup"},
    {57, 7, "MediumGroup"},
    {58, 3, "BoarderGroup"},
    {59, 14, "PokefanMGroup"},
    {60, 6, "KimonoGirlGroup"},
    {61, 10, "TwinsGroup"},
    {62, 6, "PokefanFGroup"},
    {63, 1, "RedGroup"},
    {64, 1, "BlueGroup"},
    {65, 2, "OfficerGroup"},
    {66, 5, "GruntFGroup"},
    {67, 1, "MysticalmanGroup"},
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
    
    std::cout << "=== TrainerRegistry Verification ===\n\n";
    std::cout << "TrainerGroups table: 0x" << std::hex << profile->offsets.trainer_groups 
              << std::dec << "\n";
    std::cout << "NUM_TRAINER_CLASSES: " << profile->counts.num_trainer_classes << "\n\n";
    
    TrainerRegistry trainer_registry(*rom, profile->offsets.trainer_groups,
        static_cast<uint8_t>(profile->counts.num_trainer_classes));
    
    std::cout << "Registry total: " << trainer_registry.total_count() << "\n";
    std::cout << "Groups with trainers: " << trainer_registry.group_count_total() << "\n\n";
    
    // Read group pointers from ROM for boundary analysis
    std::vector<uint32_t> group_starts;
    uint8_t table_bank = static_cast<uint8_t>(profile->offsets.trainer_groups / 0x4000);
    
    for (uint8_t i = 1; i <= 67; ++i) {
        uint32_t ptr_offset = profile->offsets.trainer_groups + (static_cast<size_t>(i - 1) * 2);
        uint16_t group_ptr = rom->read_word(ptr_offset);
        uint32_t flat_addr = (static_cast<uint32_t>(table_bank) * 0x4000) + (group_ptr - 0x4000);
        group_starts.push_back(flat_addr);
    }
    
    std::cout << "=== Per-Group Analysis ===\n\n";
    std::cout << std::setw(6) << "Group" << " | "
              << std::setw(20) << "Name" << " | "
              << std::setw(8) << "Expected" << " | "
              << std::setw(8) << "Extracted" << " | "
              << std::setw(10) << "ROM Start" << " | "
              << std::setw(10) << "ROM End" << " | "
              << "Status\n";
    std::cout << std::string(90, '-') << "\n";
    
    size_t expected_total = 0;
    size_t extracted_total = 0;
    bool any_mismatch = false;
    
    for (const auto& auth : AUTHORITATIVE_COUNTS) {
        uint8_t group = auth.group_id;
        size_t expected = auth.count;
        size_t extracted = trainer_registry.group_count(group);
        
        expected_total += expected;
        extracted_total += extracted;
        
        uint32_t group_start = group_starts[group - 1];
        uint32_t group_end = (group < 67) ? group_starts[group] : 0x3FFFF;
        
        std::string status = (extracted == expected) ? "OK" : "MISMATCH";
        if (extracted != expected) any_mismatch = true;
        
        std::cout << std::setw(6) << (int)group << " | "
                  << std::setw(20) << auth.name << " | "
                  << std::setw(8) << expected << " | "
                  << std::setw(8) << extracted << " | "
                  << "0x" << std::hex << std::setw(5) << group_start << std::dec << "  | "
                  << "0x" << std::hex << std::setw(5) << group_end << std::dec << "  | "
                  << status << "\n";
    }
    
    std::cout << std::string(90, '-') << "\n";
    std::cout << std::setw(6) << "TOTAL" << " | "
              << std::setw(20) << "" << " | "
              << std::setw(8) << expected_total << " | "
              << std::setw(8) << extracted_total << " | "
              << std::setw(10) << "" << " | "
              << std::setw(10) << "" << " | "
              << (extracted_total == expected_total ? "OK" : "MISMATCH") << "\n\n";
    
    if (any_mismatch) {
        std::cout << "*** BUG FOUND: Parser extracts incorrect trainer counts ***\n";
        return 1;
    } else {
        std::cout << "*** VERIFIED: All counts match authoritative source ***\n";
        return 0;
    }
}
