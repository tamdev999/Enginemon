// engine/build/package_cache.cpp
// Persistent package cache implementation

#include "engine/build/package_cache.hpp"
#include "engine/package/package_reader.hpp"
#include "engine/package/package_format.hpp"
#include <fstream>
#include <sstream>
#include <array>
#include <cstdlib>

// Simple SHA1 for identity hash (reuse existing implementation if available)
// For now, use a simple hash combining function
namespace {

uint64_t simple_hash(const std::string& s) {
    uint64_t hash = 0xcbf29ce484222325ULL;  // FNV-1a offset basis
    for (char c : s) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 0x100000001b3ULL;  // FNV-1a prime
    }
    return hash;
}

std::string to_hex(uint64_t value) {
    static const char* hex = "0123456789abcdef";
    std::string result(16, '0');
    for (int i = 15; i >= 0; --i) {
        result[i] = hex[value & 0xF];
        value >>= 4;
    }
    return result;
}

} // anonymous namespace

namespace enginemon::build {

std::string BuildIdentity::compute_hash() const {
    // Combine all identity components
    std::string combined = rom_sha1 + "|" + compiler_version + "|" + 
                           std::to_string(format_version) + "|" + options_hash;
    return to_hex(simple_hash(combined));
}

std::string BuildIdentity::serialize() const {
    std::ostringstream ss;
    ss << "rom_sha1=" << rom_sha1 << "\n";
    ss << "compiler_version=" << compiler_version << "\n";
    ss << "format_version=" << format_version << "\n";
    ss << "options_hash=" << options_hash << "\n";
    return ss.str();
}

std::optional<BuildIdentity> BuildIdentity::deserialize(const std::string& data) {
    // Reject empty input immediately — no point parsing.
    if (data.empty()) return std::nullopt;

    BuildIdentity id;
    // Track which mandatory fields were present.
    bool has_rom_sha1          = false;
    bool has_compiler_version  = false;
    bool has_format_version    = false;
    bool has_options_hash      = false;

    std::istringstream ss(data);
    std::string line;
    
    while (std::getline(ss, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;   // skip malformed lines silently
        
        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        
        if (key == "rom_sha1") {
            if (value.empty()) return std::nullopt;  // present but blank = malformed
            id.rom_sha1 = value;
            has_rom_sha1 = true;
        } else if (key == "compiler_version") {
            if (value.empty()) return std::nullopt;
            id.compiler_version = value;
            has_compiler_version = true;
        } else if (key == "format_version") {
            // Guard numeric parsing: garbage or out-of-range → cache miss, not throw.
            if (value.empty()) return std::nullopt;
            try {
                unsigned long parsed = std::stoul(value);
                // format_version must be a plausible non-zero version number.
                if (parsed == 0 || parsed > 0xFFFFFFFFUL) return std::nullopt;
                id.format_version = static_cast<uint32_t>(parsed);
                has_format_version = true;
            } catch (const std::invalid_argument&) {
                return std::nullopt;  // not a number
            } catch (const std::out_of_range&) {
                return std::nullopt;  // too large
            }
        } else if (key == "options_hash") {
            // options_hash may legitimately be empty for configs that hash to "".
            id.options_hash = value;
            has_options_hash = true;
        }
    }
    
    // All four fields are mandatory.  Missing any one → malformed manifest → cache miss.
    if (!has_rom_sha1 || !has_compiler_version || !has_format_version || !has_options_hash) {
        return std::nullopt;
    }
    
    return id;
}

std::filesystem::path PackageCache::default_cache_dir() {
    // Use platform-specific cache directory
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOMEDRIVE");
#else
    const char* home = std::getenv("HOME");
#endif
    
    if (home) {
        return std::filesystem::path(home) / ".enginemon" / "cache" / "packages";
    }
    
    // Fallback to current directory
    return std::filesystem::current_path() / ".enginemon_cache";
}

PackageCache::PackageCache(const std::filesystem::path& cache_dir)
    : cache_dir_(cache_dir) {
    // Create cache directory if it doesn't exist
    std::error_code ec;
    std::filesystem::create_directories(cache_dir_, ec);
}

std::filesystem::path PackageCache::get_package_path(const BuildIdentity& id) const {
    return cache_dir_ / (id.compute_hash() + ".emon");
}

std::filesystem::path PackageCache::get_manifest_path(const BuildIdentity& id) const {
    return cache_dir_ / (id.compute_hash() + ".manifest");
}

bool PackageCache::validate_cached_package(const BuildIdentity& id) const {
    auto pkg_path = get_package_path(id);
    auto manifest_path = get_manifest_path(id);
    
    // Both files must exist
    if (!std::filesystem::exists(pkg_path) || !std::filesystem::exists(manifest_path)) {
        return false;
    }
    
    // Read and validate manifest
    std::ifstream manifest_file(manifest_path);
    if (!manifest_file) return false;
    
    std::string manifest_content((std::istreambuf_iterator<char>(manifest_file)),
                                  std::istreambuf_iterator<char>());
    
    auto stored_id = BuildIdentity::deserialize(manifest_content);
    if (!stored_id) return false;
    
    // Verify identity matches
    if (stored_id->rom_sha1 != id.rom_sha1 ||
        stored_id->compiler_version != id.compiler_version ||
        stored_id->format_version != id.format_version ||
        stored_id->options_hash != id.options_hash) {
        return false;
    }
    
    // Check package is non-empty and structurally valid (MAGIC, version, CRC)
    auto pkg_size = std::filesystem::file_size(pkg_path);
    if (pkg_size < sizeof(PackageHeader)) return false;
    
    // Open and validate the cached package using the production reader.
    // This proves magic, format version, chunk bounds, and per-chunk CRC are intact.
    // A damaged cache entry is treated as a cache miss so normal compilation recovers.
    auto reader = enginemon::PackageReader::open(pkg_path);
    if (!reader) return false;           // open() failed (magic, version, or structure)
    if (!reader->validate()) return false;  // per-chunk CRC check failed
    
    return true;
}

std::optional<std::filesystem::path> PackageCache::find(const BuildIdentity& id) const {
    try {
        if (validate_cached_package(id)) {
            return get_package_path(id);
        }
    } catch (...) {
        // Any unexpected exception during validation → cache miss, not crash.
    }
    return std::nullopt;
}

bool PackageCache::store(const BuildIdentity& id, const std::filesystem::path& package_path) {
    // Verify source package exists
    if (!std::filesystem::exists(package_path)) {
        return false;
    }
    
    std::error_code ec;
    auto pkg_dest = get_package_path(id);
    auto manifest_dest = get_manifest_path(id);
    
    // Atomic write: copy to temp, then rename
    auto temp_pkg = pkg_dest;
    temp_pkg += ".tmp";
    auto temp_manifest = manifest_dest;
    temp_manifest += ".tmp";
    
    // Write manifest first (smaller, faster)
    {
        std::ofstream manifest_file(temp_manifest);
        if (!manifest_file) return false;
        manifest_file << id.serialize();
        manifest_file.flush();
        if (!manifest_file) {
            std::filesystem::remove(temp_manifest, ec);
            return false;
        }
    }
    
    // Copy package
    std::filesystem::copy_file(package_path, temp_pkg, 
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::filesystem::remove(temp_manifest, ec);
        return false;
    }
    
    // Atomic rename: manifest first, then package
    std::filesystem::rename(temp_manifest, manifest_dest, ec);
    if (ec) {
        std::filesystem::remove(temp_pkg, ec);
        return false;
    }
    
    std::filesystem::rename(temp_pkg, pkg_dest, ec);
    if (ec) {
        std::filesystem::remove(manifest_dest, ec);
        return false;
    }
    
    return true;
}

bool PackageCache::copy_to(const BuildIdentity& id, const std::filesystem::path& dest) const {
    auto cached_path = find(id);
    if (!cached_path) return false;
    
    std::error_code ec;
    std::filesystem::copy_file(*cached_path, dest, 
                               std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}

void PackageCache::clear() {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(cache_dir_, ec)) {
        std::filesystem::remove(entry.path(), ec);
    }
}

PackageCache::Stats PackageCache::get_stats() const {
    Stats stats;
    std::error_code ec;
    
    for (const auto& entry : std::filesystem::directory_iterator(cache_dir_, ec)) {
        if (entry.path().extension() == ".emon") {
            ++stats.total_packages;
            stats.total_bytes += entry.file_size(ec);
        }
    }
    
    return stats;
}

} // namespace enginemon::build
