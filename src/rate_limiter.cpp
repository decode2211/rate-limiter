#include "rate_limiter.hpp"

namespace {

// How long a bucket must sit untouched before it's eligible for eviction.
// A non-positive refill_rate has no well-defined "time to full" (see
// TokenBucket's own zero-refill-rate handling), so such a bucket is never
// considered idle-eligible rather than being evicted out from under
// whatever is deliberately using that degenerate configuration (tests,
// benchmarks -- Config::validate rejects this for the live service).
std::chrono::duration<double> idleThreshold(double capacity, double refill_rate, double multiplier) {
    if (refill_rate <= 0.0) {
        return std::chrono::duration<double>::max();
    }
    return std::chrono::duration<double>(multiplier * (capacity / refill_rate));
}

} // namespace

RateLimiter::RateLimiter(double capacity, double refill_rate, EvictionConfig eviction)
    : default_capacity_(capacity),
      default_refill_rate_(refill_rate),
      eviction_(eviction),
      last_sweep_(std::chrono::steady_clock::now()) {}

// Caller holds mutex_ exclusively (see getOrCreateBucket). Each bucket's
// lastActivity() takes only that TokenBucket's own internal mutex, never
// RateLimiter::mutex_, so lock order is always
// RateLimiter::mutex_ -> TokenBucket::mutex_ and never the reverse --
// no cycle, so this cannot deadlock. A concurrent check() for a bucket
// that gets erased here already holds its own shared_ptr copy (returned
// from getOrCreateBucket before this runs on a later call), so an
// in-flight request completes safely against the bucket it resolved; a
// later request for that same client_id simply gets a fresh, full bucket,
// which is the same outcome as that client_id never having been seen.
void RateLimiter::sweepExpiredLocked(std::chrono::steady_clock::time_point now) {
    auto threshold = idleThreshold(default_capacity_, default_refill_rate_, eviction_.idle_ttl_multiplier);
    for (auto it = buckets_.begin(); it != buckets_.end(); ) {
        if (now - it->second->lastActivity() >= threshold) {
            it = buckets_.erase(it);
        } else {
            ++it;
        }
    }
    last_sweep_ = now;
}

std::shared_ptr<TokenBucket> RateLimiter::getOrCreateBucket(const std::string& client_id) {
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = buckets_.find(client_id);
        if (it != buckets_.end()) {
            return it->second;
        }
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> since_last_sweep = now - last_sweep_;
    if (since_last_sweep.count() >= eviction_.sweep_interval_seconds) {
        sweepExpiredLocked(now);
    }

    auto [it, inserted] = buckets_.try_emplace(
        client_id,
        std::make_shared<TokenBucket>(default_capacity_, default_refill_rate_)
    );
    return it->second;
}

RateLimitResult RateLimiter::check(const std::string& client_id) {
    auto bucket = getOrCreateBucket(client_id);
    return bucket->consume(1.0);
}

size_t RateLimiter::bucketCount() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return buckets_.size();
}