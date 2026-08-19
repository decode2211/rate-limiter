#include "metrics.hpp"
#include <sstream>

Metrics& Metrics::instance() {
    static Metrics instance;
    return instance;
}

void Metrics::incrementAllowed() {
    allowed_requests_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::incrementRejected() {
    rejected_requests_.fetch_add(1, std::memory_order_relaxed);
}

std::string Metrics::serialize() const {
    std::ostringstream ss;

    uint64_t allowed = allowed_requests_.load(std::memory_order_relaxed);
    uint64_t rejected = rejected_requests_.load(std::memory_order_relaxed);
    uint64_t total = allowed + rejected;

    ss << "# HELP rate_limiter_requests_total Total number of processed rate limit checks.\n"
       << "# TYPE rate_limiter_requests_total counter\n"
       << "rate_limiter_requests_total{status=\"allowed\"} " << allowed << "\n"
       << "rate_limiter_requests_total{status=\"rejected\"} " << rejected << "\n\n"
       << "# HELP rate_limiter_requests_aggregate Total cumulative checks.\n"
       << "# TYPE rate_limiter_requests_aggregate counter\n"
       << "rate_limiter_requests_aggregate " << total << "\n";

    return ss.str();
}