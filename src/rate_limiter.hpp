#pragma once

#include "token_bucket.hpp"
#include <string>
#include <unordered_map>
#include <memory>
#include <shared_mutex>

class RateLimiter {
public:
    RateLimiter(double capacity, double refill_rate);

    RateLimitResult check(const std::string& client_id);

private:
    std::shared_ptr<TokenBucket> getOrCreateBucket(const std::string& client_id);

    double default_capacity_;
    double default_refill_rate_;
    std::unordered_map<std::string, std::shared_ptr<TokenBucket>> buckets_;
    mutable std::shared_mutex mutex_;
};