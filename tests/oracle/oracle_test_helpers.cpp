// tests/oracle/oracle_test_helpers.cpp
// Global variable definitions and shared helper function implementations.
// Shared across all oracle_test_*.cpp translation units via oracle_shared.hpp.

#include "oracle_shared.hpp"

// =============================================================================
// GLOBAL DEFINITIONS
// =============================================================================

int  g_tests_passed      = 0;
int  g_tests_failed      = 0;
bool g_current_test_failed = false;

const crystal::RomData*           g_rom     = nullptr;
const crystal::ExtractionProfile* g_profile = nullptr;

std::filesystem::path                     g_oracle_package_path;
std::unique_ptr<enginemon::PackageReader> g_oracle_reader;

// =============================================================================
// TEST FRAMEWORK
// =============================================================================

void run_test(const char* name, void (*test)()) {
    std::cout << "Running " << name << "... ";
    std::cout.flush();
    g_current_test_failed = false;
    try {
        test();
        if (g_current_test_failed) {
            std::cout << "FAIL\n";
            g_tests_failed++;
        } else {
            std::cout << "PASS\n";
            g_tests_passed++;
        }
    } catch (const std::exception& e) {
        std::cout << "EXCEPTION: " << e.what() << "\n";
        g_tests_failed++;
    }
}

// =============================================================================
// ORACLE HELPERS
// =============================================================================

std::filesystem::path oracle_dir() {
    std::filesystem::path src_file = __FILE__;
    if (src_file.is_absolute() && std::filesystem::exists(src_file.parent_path())) {
        return src_file.parent_path();
    }
    return std::filesystem::current_path() / "tests" / "oracle";
}

std::vector<uint8_t> load_fixture(const std::string& relative_path) {
    auto path = oracle_dir() / relative_path;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("Cannot open fixture: " + path.string());
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

std::unique_ptr<crystal::RomData> make_rom_from_bytes(const std::vector<uint8_t>& bytes) {
    auto tmp = std::filesystem::temp_directory_path() /
               ("oracle_fixture_" + std::to_string(reinterpret_cast<uintptr_t>(bytes.data())) + ".bin");
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    auto rom = crystal::RomData::load(tmp);
    std::filesystem::remove(tmp);
    if (!rom) {
        throw std::runtime_error("Failed to load ROM from fixture bytes");
    }
    return rom;
}

crystal::CrystalScriptIR make_single_cmd_ir_with_entry(
    crystal::CrystalCommand cmd,
    uint32_t entry_address)
{
    using namespace crystal;
    CrystalScriptIR ir;
    ir.name           = "oracle_test";
    ir.entry_address  = entry_address;
    ir.rom_start      = entry_address;
    ir.rom_end        = entry_address + 4;
    cmd.span.rom_address = entry_address;
    ir.commands.push_back(std::move(cmd));

    CrystalCommand end_cmd;
    end_cmd.data              = Cmd_End{};
    end_cmd.span.rom_address  = entry_address + 4;
    end_cmd.span.raw_bytes    = {0x91};
    end_cmd.status            = DecodeStatus::Success;
    ir.commands.push_back(std::move(end_cmd));
    return ir;
}

enginemon::LoweringResult lower_ir(const crystal::CrystalScriptIR& ir) {
    using namespace crystal;
    CrystalCFG cfg;
    cfg.entry_address = ir.entry_address;
    cfg.script_name   = ir.name;
    cfg.source_ir     = &ir;

    BasicBlock block;
    block.id            = 0;
    block.start_address = ir.entry_address;
    block.end_address   = ir.entry_address + 10;
    block.command_start = 0;
    block.command_count = static_cast<uint32_t>(ir.commands.size());
    block.is_entry      = true;
    block.is_reachable  = true;
    block.exit.kind     = ExitKind::Terminal;
    cfg.blocks.push_back(block);
    cfg.address_to_block[ir.entry_address] = 0;

    cfg.validation.valid            = true;
    cfg.validation.terminal_exits   = 1;
    cfg.validation.commands_covered = ir.commands.size();
    cfg.validation.commands_total   = ir.commands.size();
    for (const auto& c : ir.commands) {
        cfg.command_boundaries.insert(c.span.rom_address);
    }

    SemanticLegalizer legalizer;
    return legalizer.lower(ir, cfg);
}
