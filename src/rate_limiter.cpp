#include "rate_limiter.hpp"

RateLimiter::RateLimiter(double capacity, double refill_rate)
    : default_capacity_(capacity), default_refill_rate_(refill_rate) {}

std::shared_ptr<TokenBucket> RateLimiter::getOrCreateBucket(const std::string& client_id) {
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = buckets_.find(client_id);
        if (it != buckets_.end()) {
            return it->second;
        }
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
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