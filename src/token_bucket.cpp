#include "token_bucket.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

TokenBucket::TokenBucket(double capacity, double refill_rate)
    : capacity_(capacity), refill_rate_(refill_rate), tokens_(capacity), last_refill_(std::chrono::steady_clock::now()) {}

void TokenBucket::refill() {
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - last_refill_;
    double tokens_to_add = elapsed.count() * refill_rate_;

    if (tokens_to_add > 0.0) {
        tokens_ = std::min(capacity_, tokens_ + tokens_to_add);
        last_refill_ = now;
    }
}

RateLimitResult TokenBucket::consume(double tokens) {
    std::lock_guard<std::mutex> lock(mutex_);
    refill();

    RateLimitResult result;

    if (tokens_ >= tokens) {
        tokens_ -= tokens;
        result.allowed = true;
        result.remaining_tokens = tokens_;
        result.retry_after_seconds = 0;
    } else {
        result.allowed = false;
        result.remaining_tokens = tokens_;
        // A non-positive refill rate means the bucket never regains tokens on
        // its own (Config rejects this for the live service at load time —
        // see Config::validate — but TokenBucket is also used directly, e.g.
        // in tests/benchmarks, so it must stay well-defined on its own).
        // There is no finite wait that would ever satisfy this request, so
        // report that with a sentinel rather than dividing by zero.
        if (refill_rate_ <= 0.0) {
            result.retry_after_seconds = std::numeric_limits<uint64_t>::max();
        } else {
            double tokens_needed = tokens - tokens_;
            result.retry_after_seconds = static_cast<uint64_t>(std::ceil(tokens_needed / refill_rate_));
        }
    }

    return result;
}