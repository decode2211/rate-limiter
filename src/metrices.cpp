#include "metrices.hpp"
#include <sstream>

// Boundaries chosen to bracket real end-to-end POST /check latency: the
// rate-limiting logic itself is sub-microsecond (see benchmarks/
// benchmark.cpp, ~0.6-2us for RateLimiter::check() alone), but a real HTTP
// request adds JSON parse/serialize and loopback socket I/O on top of
// that. Verified empirically in this session against the built binary
// (see the Stage 4 commit message for the actual /metrics output) rather
// than picked blind: observed request latency landed mostly in the
// low-hundreds-of-microseconds range, comfortably inside this span.
const double Metrics::kLatencyBucketBoundsSeconds[Metrics::kNumLatencyBuckets] = {
    0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005, 0.01, 0.05
};

Metrics& Metrics::instance() {
    static Metrics instance;
    return instance;
}

void Metrics::incrementRequestsReceived() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++requests_received_;
}

void Metrics::incrementAllowed() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++allowed_requests_;
}

void Metrics::incrementRejected() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++rejected_requests_;
}

void Metrics::incrementActiveBuckets() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++active_buckets_;
}

void Metrics::decrementActiveBuckets(uint64_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Caller (RateLimiter::sweepExpiredLocked) only ever passes the exact
    // number of buckets it just erased, and every erased bucket was
    // counted by a prior incrementActiveBuckets() call, so this can never
    // underflow in practice.
    active_buckets_ -= count;
}

void Metrics::incrementEvictedBuckets(uint64_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    evicted_buckets_total_ += count;
}

void Metrics::recordRequestLatency(double seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Cumulative ("le" = less-than-or-equal) histogram semantics: an
    // observation counts toward every bucket whose bound is >= it, not
    // just the tightest one.
    for (int i = 0; i < kNumLatencyBuckets; ++i) {
        if (seconds <= kLatencyBucketBoundsSeconds[i]) {
            ++latency_bucket_counts_[i];
        }
    }
    latency_sum_seconds_ += seconds;
    ++latency_count_;
}

std::string Metrics::serialize() const {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t total = allowed_requests_ + rejected_requests_;

    std::ostringstream ss;

    ss << "# HELP rate_limiter_requests_received_total Total number of POST /check requests received.\n"
       << "# TYPE rate_limiter_requests_received_total counter\n"
       << "rate_limiter_requests_received_total " << requests_received_ << "\n\n";

    ss << "# HELP rate_limiter_requests_total Total number of processed rate limit checks, by outcome.\n"
       << "# TYPE rate_limiter_requests_total counter\n"
       << "rate_limiter_requests_total{status=\"allowed\"} " << allowed_requests_ << "\n"
       << "rate_limiter_requests_total{status=\"rejected\"} " << rejected_requests_ << "\n\n";

    ss << "# HELP rate_limiter_requests_aggregate_total Total cumulative checks (allowed + rejected).\n"
       << "# TYPE rate_limiter_requests_aggregate_total counter\n"
       << "rate_limiter_requests_aggregate_total " << total << "\n\n";

    ss << "# HELP rate_limiter_active_buckets Number of client buckets currently tracked in memory.\n"
       << "# TYPE rate_limiter_active_buckets gauge\n"
       << "rate_limiter_active_buckets " << active_buckets_ << "\n\n";

    ss << "# HELP rate_limiter_buckets_evicted_total Total number of idle client buckets evicted.\n"
       << "# TYPE rate_limiter_buckets_evicted_total counter\n"
       << "rate_limiter_buckets_evicted_total " << evicted_buckets_total_ << "\n\n";

    ss << "# HELP rate_limiter_check_duration_seconds Latency of POST /check requests.\n"
       << "# TYPE rate_limiter_check_duration_seconds histogram\n";
    for (int i = 0; i < kNumLatencyBuckets; ++i) {
        ss << "rate_limiter_check_duration_seconds_bucket{le=\"" << kLatencyBucketBoundsSeconds[i] << "\"} "
           << latency_bucket_counts_[i] << "\n";
    }
    ss << "rate_limiter_check_duration_seconds_bucket{le=\"+Inf\"} " << latency_count_ << "\n"
       << "rate_limiter_check_duration_seconds_sum " << latency_sum_seconds_ << "\n"
       << "rate_limiter_check_duration_seconds_count " << latency_count_ << "\n";

    return ss.str();
}
