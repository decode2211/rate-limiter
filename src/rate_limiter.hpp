#pragma once

#include "token_bucket.hpp"
#include <string>
#include <unordered_map>
#include <memory>
#include <shared_mutex>
#include <chrono>

// Tunables for reclaiming per-client buckets that are no longer in active
// use. Without this, RateLimiter::buckets_ would grow by one entry per
// unique client_id ever seen and never shrink for the life of the process.
struct EvictionConfig {
    // A bucket becomes eligible for eviction once it has sat idle for at
    // least idle_ttl_multiplier * (capacity / refill_rate) -- i.e. long
    // enough that it would have fully refilled several times over, so
    // evicting it is behaviorally indistinguishable from that client_id
    // never having been seen before.
    double idle_ttl_multiplier = 10.0;
    // Minimum time between eviction sweeps, so a sustained flood of unique
    // client_ids doesn't pay for an O(bucket count) scan on every insert.
    double sweep_interval_seconds = 60.0;
};

class RateLimiter {
public:
    RateLimiter(double capacity, double refill_rate, EvictionConfig eviction = EvictionConfig());

    RateLimitResult check(const std::string& client_id);

    // Number of client buckets currently tracked. Exposed for observability
    // (Metrics) and tests.
    size_t bucketCount() const;

private:
    std::shared_ptr<TokenBucket> getOrCreateBucket(const std::string& client_id);

    // Removes buckets idle past the configured threshold. Caller must
    // already hold mutex_ exclusively -- this never acquires mutex_ itself,
    // so it cannot deadlock or race with a concurrent check()/find().
    void sweepExpiredLocked(std::chrono::steady_clock::time_point now);

    double default_capacity_;
    double default_refill_rate_;
    EvictionConfig eviction_;
    std::unordered_map<std::string, std::shared_ptr<TokenBucket>> buckets_;
    std::chrono::steady_clock::time_point last_sweep_;
    mutable std::shared_mutex mutex_;
};