#pragma once
// engine/build/package_cache.hpp
// Persistent compiled package cache
//
// Caches deterministic EMON packages by build identity:
// - ROM content hash
// - Compiler semantic version
// - Package format version
// - Compilation options
//
// On cache hit, skips entire compilation.
// Atomic writes prevent partial packages from appearing valid.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace enginemon::build {

// Build identity for cache lookup
struct BuildIdentity {
    std::string rom_sha1;           // 40-char hex hash of ROM content
    std::string compiler_version;   // e.g., "crystal-1.0.0"
    uint32_t format_version;        // EMON format version
    std::string options_hash;       // Hash of compilation options
    
    // Compute combined identity hash for filename
    std::string compute_hash() const;
    
    // Serialize to/from string
    std::string serialize() const;
    static std::optional<BuildIdentity> deserialize(const std::string& data);
};

// Persistent package cache
class PackageCache {
public:
    // Default cache directory: ~/.enginemon/cache/packages/
    explicit PackageCache(const std::filesystem::path& cache_dir = default_cache_dir());
    
    // Check if a valid cached package exists
    // Returns path to cached package if valid, nullopt otherwise
    std::optional<std::filesystem::path> find(const BuildIdentity& id) const;
    
    // Store a compiled package in the cache
    // Performs atomic write (temp file + rename)
    // Returns true on success
    bool store(const BuildIdentity& id, const std::filesystem::path& package_path);
    
    // Copy cached package to destination
    // Returns true on success
    bool copy_to(const BuildIdentity& id, const std::filesystem::path& dest) const;
    
    // Clear all cached packages
    void clear();
    
    // Get cache statistics
    struct Stats {
        uint64_t total_packages{0};
        uint64_t total_bytes{0};
    };
    Stats get_stats() const;
    
    // Get default cache directory
    static std::filesystem::path default_cache_dir();

private:
    std::filesystem::path cache_dir_;
    
    // Get path for a cached package
    std::filesystem::path get_package_path(const BuildIdentity& id) const;
    std::filesystem::path get_manifest_path(const BuildIdentity& id) const;
    
    // Validate cached package integrity
    bool validate_cached_package(const BuildIdentity& id) const;
};

} // namespace enginemon::build
