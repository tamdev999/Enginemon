#pragma once
// crystal/rom/loader.hpp
// Loads and validates Crystal ROM
// All Game Boy banking/addressing knowledge stays in this frontend

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace crystal {

// ROM header info
struct RomHeader {
    std::string title;              // 0x0134-0x0143
    uint8_t cgb_flag;               // 0x0143
    std::string new_licensee;       // 0x0144-0x0145
    uint8_t sgb_flag;               // 0x0146
    uint8_t cartridge_type;         // 0x0147
    uint8_t rom_size;               // 0x0148
    uint8_t ram_size;               // 0x0149
    uint8_t destination;            // 0x014A
    uint8_t old_licensee;           // 0x014B
    uint8_t version;                // 0x014C
    uint8_t header_checksum;        // 0x014D
    uint16_t global_checksum;       // 0x014E-0x014F
    
    // Computed
    size_t rom_size_bytes;
    size_t ram_size_bytes;
    bool is_valid;
};

// Crystal ROM variants
enum class CrystalVersion {
    Unknown,
    USA_Rev0,
    USA_Rev1,
    Europe,
    Japan,
    Australia
};

// Loaded ROM data
class RomData {
public:
    // Load from file
    static std::unique_ptr<RomData> load(const std::filesystem::path& path);
    
    // Validation
    bool validate() const;
    CrystalVersion detect_version() const;
    
    // Header access
    const RomHeader& header() const { return header_; }
    
    // Raw byte access (flat address)
    uint8_t read_byte(uint32_t address) const;
    uint16_t read_word(uint32_t address) const;  // Little endian
    std::span<const uint8_t> read_bytes(uint32_t address, size_t count) const;
    
    // Bank-aware access (Game Boy banking)
    // Converts bank:offset to flat address
    uint8_t read_banked_byte(uint8_t bank, uint16_t offset) const;
    uint16_t read_banked_word(uint8_t bank, uint16_t offset) const;
    uint32_t bank_to_flat(uint8_t bank, uint16_t offset) const;
    
    // Pointer reading (common in Crystal - 3 byte bank:addr)
    uint32_t read_pointer(uint32_t address) const;
    
    // Banking helpers for script decoder
    uint32_t bank_addr_to_flat(uint8_t bank, uint16_t addr) const { return bank_to_flat(bank, addr); }
    uint8_t flat_to_bank(uint32_t flat_address) const;
    
    // ROM hash (for identification/caching)
    const std::string& hash() const { return hash_; }
    
    // Size
    size_t size() const { return data_.size(); }
    
    // Raw access (for bulk operations)
    const std::vector<uint8_t>& raw() const { return data_; }

private:
    std::vector<uint8_t> data_;
    RomHeader header_;
    std::string hash_;
    
    void parse_header();
    void compute_hash();
};

// ROM validation results
struct ValidationResult {
    bool valid = false;
    CrystalVersion version = CrystalVersion::Unknown;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Validate ROM for Crystal compatibility
ValidationResult validate_crystal_rom(const RomData& rom);

} // namespace crystal
