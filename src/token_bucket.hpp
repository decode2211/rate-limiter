#pragma once

#include <chrono>
#include <mutex>

struct RateLimitResult {
    bool allowed;
    double remaining_tokens;
    uint64_t retry_after_seconds;
};

class TokenBucket {
public:
    TokenBucket(double capacity, double refill_rate);

    RateLimitResult consume(double tokens = 1.0);

private:
    void refill();

    double capacity_;
    double refill_rate_;
    double tokens_;
    std::chrono::steady_clock::time_point last_refill_;
    mutable std::mutex mutex_;
};