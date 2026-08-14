// crystal/rom/loader.cpp
// ROM loading and validation

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"
#include <fstream>
#include <algorithm>
#include <array>

// For hashing
#include <sstream>
#include <iomanip>
#include <cstring>

namespace crystal {

namespace {
    // Crystal ROM sizes
    constexpr size_t CRYSTAL_ROM_SIZE = 2 * 1024 * 1024; // 2MB
    
    // Bank size
    constexpr size_t BANK_SIZE = 0x4000; // 16KB
    
    // Simple SHA-1 implementation for ROM identification
    // This stays in the Crystal frontend - only used for profile matching
    class SHA1 {
    public:
        SHA1() { reset(); }
        
        void update(const uint8_t* data, size_t len) {
            size_t i = 0;
            size_t buffer_used = count_ % 64;
            count_ += len;
            
            if (buffer_used > 0) {
                size_t available = 64 - buffer_used;
                if (len < available) {
                    std::memcpy(buffer_ + buffer_used, data, len);
                    return;
                }
                std::memcpy(buffer_ + buffer_used, data, available);
                process_block(buffer_);
                i = available;
            }
            
            while (i + 64 <= len) {
                process_block(data + i);
                i += 64;
            }
            
            if (i < len) {
                std::memcpy(buffer_, data + i, len - i);
            }
        }
        
        std::string finalize() {
            uint64_t bits = count_ * 8;
            size_t pad_len = (count_ % 64 < 56) ? (56 - count_ % 64) : (120 - count_ % 64);
            
            uint8_t padding[64] = {0x80};
            update(padding, pad_len);
            
            uint8_t length_bytes[8];
            for (int i = 7; i >= 0; --i) {
                length_bytes[7 - i] = static_cast<uint8_t>(bits >> (i * 8));
            }
            update(length_bytes, 8);
            
            std::ostringstream ss;
            for (int i = 0; i < 5; ++i) {
                ss << std::hex << std::setfill('0') << std::setw(8) << state_[i];
            }
            return ss.str();
        }
        
        void reset() {
            count_ = 0;
            state_[0] = 0x67452301;
            state_[1] = 0xEFCDAB89;
            state_[2] = 0x98BADCFE;
            state_[3] = 0x10325476;
            state_[4] = 0xC3D2E1F0;
        }
        
    private:
        uint32_t state_[5];
        uint8_t buffer_[64];
        uint64_t count_;
        
        static uint32_t rotl(uint32_t x, int n) {
            return (x << n) | (x >> (32 - n));
        }
        
        void process_block(const uint8_t* block) {
            uint32_t w[80];
            
            for (int i = 0; i < 16; ++i) {
                w[i] = (block[i*4] << 24) | (block[i*4+1] << 16) | 
                       (block[i*4+2] << 8) | block[i*4+3];
            }
            
            for (int i = 16; i < 80; ++i) {
                w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
            }
            
            uint32_t a = state_[0], b = state_[1], c = state_[2];
            uint32_t d = state_[3], e = state_[4];
            
            for (int i = 0; i < 80; ++i) {
                uint32_t f, k;
                if (i < 20) {
                    f = (b & c) | ((~b) & d);
                    k = 0x5A827999;
                } else if (i < 40) {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1;
                } else if (i < 60) {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDC;
                } else {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6;
                }
                
                uint32_t temp = rotl(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = rotl(b, 30);
                b = a;
                a = temp;
            }
            
            state_[0] += a;
            state_[1] += b;
            state_[2] += c;
            state_[3] += d;
            state_[4] += e;
        }
    };
}

void RomData::parse_header() {
    if (data_.size() < 0x0150) {
        header_.is_valid = false;
        return;
    }
    
    // Title (0x0134-0x0143)
    header_.title = std::string(
        reinterpret_cast<const char*>(&data_[0x0134]), 16);
    // Trim null bytes
    header_.title.erase(
        std::find(header_.title.begin(), header_.title.end(), '\0'),
        header_.title.end());
    
    header_.cgb_flag = data_[0x0143];
    header_.new_licensee = std::string(
        reinterpret_cast<const char*>(&data_[0x0144]), 2);
    header_.sgb_flag = data_[0x0146];
    header_.cartridge_type = data_[0x0147];
    header_.rom_size = data_[0x0148];
    header_.ram_size = data_[0x0149];
    header_.destination = data_[0x014A];
    header_.old_licensee = data_[0x014B];
    header_.version = data_[0x014C];
    header_.header_checksum = data_[0x014D];
    header_.global_checksum = data_[0x014E] | (data_[0x014F] << 8);
    
    // Calculate sizes
    header_.rom_size_bytes = 32768 << header_.rom_size;
    
    switch (header_.ram_size) {
        case 0: header_.ram_size_bytes = 0; break;
        case 1: header_.ram_size_bytes = 2048; break;
        case 2: header_.ram_size_bytes = 8192; break;
        case 3: header_.ram_size_bytes = 32768; break;
        case 4: header_.ram_size_bytes = 131072; break;
        case 5: header_.ram_size_bytes = 65536; break;
        default: header_.ram_size_bytes = 0; break;
    }
    
    // Validate header checksum
    uint8_t checksum = 0;
    for (size_t i = 0x0134; i <= 0x014C; ++i) {
        checksum = checksum - data_[i] - 1;
    }
    header_.is_valid = (checksum == header_.header_checksum);
}

void RomData::compute_hash() {
    // Compute SHA-1 hash for profile identification
    SHA1 sha;
    sha.update(data_.data(), data_.size());
    hash_ = sha.finalize();
}

std::unique_ptr<RomData> RomData::load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return nullptr;
    }
    
    auto size = file.tellg();
    file.seekg(0);
    
    auto rom = std::make_unique<RomData>();
    rom->data_.resize(static_cast<size_t>(size));
    
    if (!file.read(reinterpret_cast<char*>(rom->data_.data()), size)) {
        return nullptr;
    }
    
    rom->parse_header();
    rom->compute_hash();
    
    return rom;
}

bool RomData::validate() const {
    return header_.is_valid && data_.size() >= CRYSTAL_ROM_SIZE;
}

CrystalVersion RomData::detect_version() const {
    // Strict profile-based detection via exact SHA-1 hash
    auto& registry = ProfileRegistry::instance();
    auto version = registry.identify(hash_);
    if (version) {
        switch (*version) {
            case RomVersion::Crystal_USA_v1_1:
                return CrystalVersion::USA_Rev1;
            case RomVersion::Crystal_USA_v1_0:
                return CrystalVersion::USA_Rev0;
            case RomVersion::Crystal_EUR:
                return CrystalVersion::Europe;
            case RomVersion::Crystal_JPN:
                return CrystalVersion::Japan;
            case RomVersion::Crystal_AUS:
                return CrystalVersion::Australia;
            default:
                break;
        }
    }
    
    // No profile match - return Unknown
    // We don't fall back to header-based guessing for extraction
    // Header checks are only for quick "is this even Crystal?" validation
    if (!header_.is_valid) return CrystalVersion::Unknown;
    if (header_.title.find("CRYSTAL") == std::string::npos) {
        return CrystalVersion::Unknown;
    }
    
    // ROM looks like Crystal but we don't have a verified profile for it
    // Return Unknown to force user to use a supported ROM
    return CrystalVersion::Unknown;
}

uint8_t RomData::read_byte(uint32_t address) const {
    if (address >= data_.size()) return 0xFF;
    return data_[address];
}

uint16_t RomData::read_word(uint32_t address) const {
    if (address + 1 >= data_.size()) return 0xFFFF;
    return data_[address] | (data_[address + 1] << 8);
}

std::span<const uint8_t> RomData::read_bytes(uint32_t address, size_t count) const {
    if (address + count > data_.size()) {
        count = data_.size() - address;
    }
    return std::span<const uint8_t>(data_.data() + address, count);
}

uint32_t RomData::bank_to_flat(uint8_t bank, uint16_t offset) const {
    // Bank 0 is always at 0x0000-0x3FFF
    // Bank N is at N * 0x4000 (for offsets 0x4000-0x7FFF)
    if (offset < 0x4000) {
        return offset; // ROM0
    }
    return (bank * BANK_SIZE) + (offset - 0x4000);
}

uint8_t RomData::read_banked_byte(uint8_t bank, uint16_t offset) const {
    return read_byte(bank_to_flat(bank, offset));
}

uint16_t RomData::read_banked_word(uint8_t bank, uint16_t offset) const {
    return read_word(bank_to_flat(bank, offset));
}

uint32_t RomData::read_pointer(uint32_t address) const {
    // Crystal pointers are 3 bytes: bank, low addr, high addr
    uint8_t bank = read_byte(address);
    uint16_t addr = read_word(address + 1);
    return bank_to_flat(bank, addr);
}

uint8_t RomData::flat_to_bank(uint32_t flat_address) const {
    // Convert flat address to bank number
    // Bank 0 is 0x0000-0x3FFF
    // Bank N is at flat offset N * 0x4000
    if (flat_address < BANK_SIZE) {
        return 0;
    }
    return static_cast<uint8_t>(flat_address / BANK_SIZE);
}

ValidationResult validate_crystal_rom(const RomData& rom) {
    ValidationResult result;
    
    if (!rom.validate()) {
        result.errors.push_back("ROM validation failed");
        return result;
    }
    
    result.version = rom.detect_version();
    
    if (result.version == CrystalVersion::Unknown) {
        result.errors.push_back("Not a recognized Pokemon Crystal ROM");
        return result;
    }
    
    // Check ROM size
    if (rom.size() != CRYSTAL_ROM_SIZE) {
        result.warnings.push_back("Unexpected ROM size");
    }
    
    result.valid = true;
    return result;
}

} // namespace crystal
