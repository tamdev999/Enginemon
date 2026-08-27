#pragma once
// engine/build/asset_cache.hpp
// Generic compute-once shared asset cache for parallel compilation
//
// Key features:
// - First requester computes, concurrent requesters await
// - Cycle detection in dependency chains
// - Type-erased storage for any asset type
// - Thread-safe with atomic operations
//
// Not Crystal-specific - usable by any frontend compiler.

#include <any>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace enginemon::build {

// Error result for failed computations
struct AssetError {
    std::string message;
    std::string key;
};

// Asset computation result - either success or failure
template<typename T>
using AssetResult = std::variant<T, AssetError>;

// Extract value or throw
template<typename T>
const T& get_asset(const AssetResult<T>& result) {
    if (auto* value = std::get_if<T>(&result)) {
        return *value;
    }
    throw std::runtime_error(std::get<AssetError>(result).message);
}

// Check if result is success
template<typename T>
bool is_success(const AssetResult<T>& result) {
    return std::holds_alternative<T>(result);
}

// Dependency cycle error
class DependencyCycleError : public std::runtime_error {
public:
    explicit DependencyCycleError(const std::string& key)
        : std::runtime_error("Dependency cycle detected for key: " + key)
        , key_(key) {}
    
    const std::string& key() const { return key_; }

private:
    std::string key_;
};

// Per-thread dependency tracking for cycle detection
class DependencyTracker {
public:
    // RAII guard for tracking dependency chain
    class Guard {
    public:
        Guard(DependencyTracker& tracker, const std::string& key)
            : tracker_(tracker), key_(key), pushed_(false) {
            if (tracker_.contains(key)) {
                throw DependencyCycleError(key);
            }
            tracker_.push(key);
            pushed_ = true;
        }
        
        ~Guard() {
            if (pushed_) {
                tracker_.pop(key_);
            }
        }
        
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        
    private:
        DependencyTracker& tracker_;
        std::string key_;
        bool pushed_;
    };
    
    static DependencyTracker& current() {
        thread_local DependencyTracker instance;
        return instance;
    }
    
    bool contains(const std::string& key) const {
        return chain_.contains(key);
    }

private:
    void push(const std::string& key) {
        chain_.insert(key);
    }
    
    void pop(const std::string& key) {
        chain_.erase(key);
    }
    
    std::unordered_set<std::string> chain_;
};

// Generic shared asset cache
// Assets are keyed by string (kind + canonical symbol + params)
// Values are immutable after publication
class AssetCache {
public:
    AssetCache() = default;
    ~AssetCache() = default;
    
    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;
    
    // Get or compute an asset
    // - compute is called if asset not cached
    // - concurrent callers for same key block until computation completes
    // - cycle detection: throws DependencyCycleError if key is already in dependency chain
    //
    // Template params:
    //   T = asset type
    //   F = compute function returning AssetResult<T>
    template<typename T, typename F>
    AssetResult<T> get_or_compute(const std::string& key, F&& compute) {
        // Check dependency chain for cycles
        DependencyTracker::Guard dep_guard(DependencyTracker::current(), key);
        
        // Fast path: already computed
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = cache_.find(key);
            if (it != cache_.end()) {
                if (auto* entry = std::any_cast<Entry<T>>(&it->second)) {
                    ++stats_.cache_hits;
                    return entry->result;
                }
            }
        }
        
        // Slow path: need to compute or wait
        std::unique_lock<std::mutex> compute_lock(compute_mutex_);
        
        // Check again under exclusive lock
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = cache_.find(key);
            if (it != cache_.end()) {
                if (auto* entry = std::any_cast<Entry<T>>(&it->second)) {
                    ++stats_.cache_hits;
                    return entry->result;
                }
            }
        }
        
        // Check if another thread is computing this key
        {
            auto pending_it = pending_.find(key);
            if (pending_it != pending_.end()) {
                // Wait for completion
                compute_lock.unlock();
                
                std::unique_lock<std::mutex> wait_lock(wait_mutex_);
                wait_cv_.wait(wait_lock, [this, &key] {
                    std::shared_lock<std::shared_mutex> lock(mutex_);
                    return cache_.contains(key);
                });
                
                // Get result
                std::shared_lock<std::shared_mutex> lock(mutex_);
                auto it = cache_.find(key);
                if (it != cache_.end()) {
                    if (auto* entry = std::any_cast<Entry<T>>(&it->second)) {
                        ++stats_.cache_hits;
                        return entry->result;
                    }
                }
                
                // Type mismatch - shouldn't happen
                return AssetResult<T>{AssetError{"Type mismatch in cache for: " + key, key}};
            }
        }
        
        // Mark as pending computation
        pending_.insert(key);
        ++stats_.cache_misses;
        compute_lock.unlock();
        
        // Compute the asset
        AssetResult<T> result = std::invoke(std::forward<F>(compute));
        
        // Store result
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            cache_[key] = Entry<T>{result};
        }
        
        // Remove from pending and notify waiters
        {
            std::lock_guard<std::mutex> lock(compute_mutex_);
            pending_.erase(key);
        }
        wait_cv_.notify_all();
        
        return result;
    }
    
    // Check if key exists
    bool contains(const std::string& key) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return cache_.contains(key);
    }
    
    // Clear all cached assets
    void clear() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        cache_.clear();
        pending_.clear();
    }
    
    // Statistics
    struct Stats {
        std::atomic<uint64_t> cache_hits{0};
        std::atomic<uint64_t> cache_misses{0};
    };
    const Stats& stats() const { return stats_; }

private:
    template<typename T>
    struct Entry {
        AssetResult<T> result;
    };
    
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::any> cache_;
    
    std::mutex compute_mutex_;
    std::unordered_set<std::string> pending_;
    
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    
    mutable Stats stats_;
};

// Helper to make cache keys
// Format: "kind:symbol:param1=value1,param2=value2"
inline std::string make_cache_key(
    const std::string& kind,
    const std::string& symbol,
    const std::string& params = "") {
    if (params.empty()) {
        return kind + ":" + symbol;
    }
    return kind + ":" + symbol + ":" + params;
}

} // namespace enginemon::build
