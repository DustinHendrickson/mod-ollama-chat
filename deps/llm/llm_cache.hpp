#pragma once

// llm-cache: single-header LRU cache for LLM responses
// #define LLM_CACHE_IMPLEMENTATION in ONE .cpp file before including.

#include <chrono>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace llm {

struct CacheConfig {
    size_t max_entries    = 1000;
    double ttl_seconds    = 3600.0;  // 0 = no expiry
    bool   case_sensitive = false;   // normalize keys to lowercase if false
};

struct CacheStats {
    size_t hits;
    size_t misses;
    size_t evictions;
    size_t current_size;
    double hit_rate() const {
        size_t total = hits + misses;
        return total == 0 ? 0.0 : static_cast<double>(hits) / total;
    }
};

struct CacheEntry {
    std::string value;
    std::chrono::steady_clock::time_point inserted_at;
};

class ResponseCache {
public:
    explicit ResponseCache(CacheConfig config = {});

    // Look up a cached response for the given prompt key.
    // Returns std::nullopt on miss or expiry (expired entries are evicted).
    std::optional<std::string> get(const std::string& key);

    // Store a response. Evicts LRU entry if at capacity.
    void put(const std::string& key, const std::string& value);

    // Remove a specific key.
    bool invalidate(const std::string& key);

    // Clear all entries.
    void clear();

    CacheStats stats() const;
    size_t size() const;

    // Convenience: get cached or compute + store.
    // fn is only called on a cache miss.
    std::string get_or_compute(
        const std::string& key,
        std::function<std::string()> fn
    );

private:
    std::string normalize_key(const std::string& key) const;
    bool is_expired(const CacheEntry& entry) const;
    void evict_lru();

    CacheConfig config_;
    mutable std::mutex mutex_;

    // LRU: list front = most recently used
    std::list<std::string> lru_order_;
    std::unordered_map<std::string,
        std::pair<CacheEntry, std::list<std::string>::iterator>> map_;

    mutable CacheStats stats_ = {0, 0, 0, 0};
};

// Simple hash of a prompt string — useful as a cache key
std::string prompt_hash(const std::string& prompt);

} // namespace llm

#ifdef LLM_CACHE_IMPLEMENTATION

#include <algorithm>
#include <cctype>
#include <sstream>
#include <iomanip>

namespace llm {

ResponseCache::ResponseCache(CacheConfig config)
    : config_(std::move(config)) {}

std::string ResponseCache::normalize_key(const std::string& key) const {
    if (config_.case_sensitive) return key;
    std::string out = key;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool ResponseCache::is_expired(const CacheEntry& entry) const {
    if (config_.ttl_seconds <= 0.0) return false;
    auto now = std::chrono::steady_clock::now();
    double age = std::chrono::duration<double>(now - entry.inserted_at).count();
    return age > config_.ttl_seconds;
}

void ResponseCache::evict_lru() {
    if (lru_order_.empty()) return;
    const std::string& lru_key = lru_order_.back();
    map_.erase(lru_key);
    lru_order_.pop_back();
    ++stats_.evictions;
    if (stats_.current_size > 0) --stats_.current_size;
}

std::optional<std::string> ResponseCache::get(const std::string& key) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto nkey = normalize_key(key);
    auto it = map_.find(nkey);
    if (it == map_.end()) { ++stats_.misses; return std::nullopt; }
    if (is_expired(it->second.first)) {
        lru_order_.erase(it->second.second);
        map_.erase(it);
        ++stats_.misses;
        if (stats_.current_size > 0) --stats_.current_size;
        return std::nullopt;
    }
    // Move to front (most recently used)
    lru_order_.erase(it->second.second);
    lru_order_.push_front(nkey);
    it->second.second = lru_order_.begin();
    ++stats_.hits;
    return it->second.first.value;
}

void ResponseCache::put(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto nkey = normalize_key(key);
    auto it = map_.find(nkey);
    if (it != map_.end()) {
        lru_order_.erase(it->second.second);
        map_.erase(it);
        if (stats_.current_size > 0) --stats_.current_size;
    }
    while (stats_.current_size >= config_.max_entries) evict_lru();
    lru_order_.push_front(nkey);
    CacheEntry entry{value, std::chrono::steady_clock::now()};
    map_.emplace(nkey, std::make_pair(std::move(entry), lru_order_.begin()));
    ++stats_.current_size;
}

bool ResponseCache::invalidate(const std::string& key) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto nkey = normalize_key(key);
    auto it = map_.find(nkey);
    if (it == map_.end()) return false;
    lru_order_.erase(it->second.second);
    map_.erase(it);
    if (stats_.current_size > 0) --stats_.current_size;
    return true;
}

void ResponseCache::clear() {
    std::lock_guard<std::mutex> lk(mutex_);
    map_.clear();
    lru_order_.clear();
    stats_.current_size = 0;
}

CacheStats ResponseCache::stats() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return stats_;
}

size_t ResponseCache::size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return stats_.current_size;
}

std::string ResponseCache::get_or_compute(
    const std::string& key,
    std::function<std::string()> fn
) {
    auto cached = get(key);
    if (cached) return *cached;
    std::string result = fn();
    put(key, result);
    return result;
}

// FNV-1a 64-bit hash, hex-encoded
std::string prompt_hash(const std::string& prompt) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : prompt) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return oss.str();
}

} // namespace llm

#endif // LLM_CACHE_IMPLEMENTATION
