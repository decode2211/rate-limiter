#pragma once

#include <cstdint>
#include <mutex>
#include <string>

class Metrics {
public:
    static Metrics& instance();

    void incrementRequestsReceived();
    void incrementAllowed();
    void incrementRejected();

    void incrementActiveBuckets();
    void decrementActiveBuckets(uint64_t count = 1);
    void incrementEvictedBuckets(uint64_t count);

    void recordRequestLatency(double seconds);

    std::string serialize() const;

private:
    Metrics() = default;

    // Every field below is guarded by mutex_ -- a single mutex, not a mix
    // of atomics-for-counters plus a separate mutex-for-latency-sum. Two
    // reasons: (1) std::atomic<double>::fetch_add needs C++20, which this
    // project doesn't use, so the latency sum would need a mutex or a CAS
    // loop regardless; (2) one mutex makes serialize() a single consistent
    // snapshot of every counter together, rather than several independent
    // atomic reads that could each reflect a different instant. Request
    // volume here is HTTP-request-bound (JSON parse, socket I/O), so a
    // brief uncontended lock per request is not a meaningful cost.
    //
    // Lock ordering: this mutex is never held while calling into
    // RateLimiter or TokenBucket, and neither of those ever calls back
    // into Metrics while holding their own lock (RateLimiter::mutex_ is
    // released before Server calls incrementAllowed/incrementRejected;
    // RateLimiter::sweepExpiredLocked calls into Metrics *while* holding
    // RateLimiter::mutex_, but Metrics methods never in turn touch
    // RateLimiter::mutex_ or TokenBucket::mutex_). So the only nesting
    // order that ever occurs is RateLimiter::mutex_ -> Metrics::mutex_,
    // never the reverse -- no cycle, so no deadlock risk.
    mutable std::mutex mutex_;

    uint64_t requests_received_ = 0;
    uint64_t allowed_requests_ = 0;
    uint64_t rejected_requests_ = 0;
    uint64_t active_buckets_ = 0;
    uint64_t evicted_buckets_total_ = 0;

    // Histogram buckets for POST /check latency. See metrices.cpp for why
    // these particular boundaries were chosen.
    static constexpr int kNumLatencyBuckets = 8;
    static const double kLatencyBucketBoundsSeconds[kNumLatencyBuckets];
    uint64_t latency_bucket_counts_[kNumLatencyBuckets] = {};
    double latency_sum_seconds_ = 0.0;
    uint64_t latency_count_ = 0;
};
