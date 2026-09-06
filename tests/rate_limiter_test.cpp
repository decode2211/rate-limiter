#include <gtest/gtest.h>
#include "token_bucket.hpp"
#include "rate_limiter.hpp"
#include "config.hpp"
#include <thread>
#include <vector>
#include <fstream>
#include <cstdio>
#include <limits>

TEST(TokenBucketTest, AllowsRequestsUpToCapacity) {
    TokenBucket bucket(3.0, 1.0);

    EXPECT_TRUE(bucket.consume().allowed);
    EXPECT_TRUE(bucket.consume().allowed);
    EXPECT_TRUE(bucket.consume().allowed);
    EXPECT_FALSE(bucket.consume().allowed);
}

TEST(TokenBucketTest, RefillsTokensOverTime) {
    TokenBucket bucket(2.0, 2.0); // 2 tokens/sec

    EXPECT_TRUE(bucket.consume().allowed);
    EXPECT_TRUE(bucket.consume().allowed);
    EXPECT_FALSE(bucket.consume().allowed);

    std::this_thread::sleep_for(std::chrono::milliseconds(1050)); // Wait ~1 sec

    EXPECT_TRUE(bucket.consume().allowed);
    EXPECT_TRUE(bucket.consume().allowed);
    EXPECT_FALSE(bucket.consume().allowed);
}

TEST(RateLimiterTest, HandlesMultipleClientsIndependently) {
    RateLimiter limiter(1.0, 1.0);

    EXPECT_TRUE(limiter.check("client_A").allowed);
    EXPECT_FALSE(limiter.check("client_A").allowed);

    EXPECT_TRUE(limiter.check("client_B").allowed);
    EXPECT_FALSE(limiter.check("client_B").allowed);
}

TEST(RateLimiterTest, ThreadSafetyUnderConcurrency) {
    RateLimiter limiter(100.0, 10.0);
    constexpr int threads = 10;
    constexpr int requests_per_thread = 10;

    std::vector<std::thread> workers;
    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([&limiter]() {
            for (int j = 0; j < requests_per_thread; ++j) {
                limiter.check("concurrent_client");
            }
        });
    }

    for (auto& t : workers) {
        t.join();
    }

    RateLimitResult result = limiter.check("concurrent_client");
    EXPECT_FALSE(result.allowed); // 100 capacity exhausted by 100 calls
}

// --- RateLimiter eviction ---

TEST(RateLimiterTest, EvictsIdleBucketsAndShrinksMap) {
    EvictionConfig eviction;
    eviction.idle_ttl_multiplier = 1.0;
    eviction.sweep_interval_seconds = 0.0; // always eligible to sweep

    // capacity/refill_rate chosen so the idle threshold
    // (multiplier * capacity / refill_rate) is a few milliseconds.
    RateLimiter limiter(1.0, 1000.0, eviction);

    constexpr int kNumClients = 50;
    for (int i = 0; i < kNumClients; ++i) {
        limiter.check("client_" + std::to_string(i));
    }
    EXPECT_EQ(limiter.bucketCount(), static_cast<size_t>(kNumClients));

    // Let all of the above go idle past the eviction threshold.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Sweeping happens on the new-bucket insertion path, so trigger one.
    limiter.check("trigger_sweep");

    // All 50 idle buckets should have been evicted; only the new one remains.
    EXPECT_EQ(limiter.bucketCount(), 1u);
}

// --- TokenBucket edge cases: zero / negative refill_rate ---
//
// Config::loadFromFile rejects a non-positive refill_rate for the live
// service (see ConfigTest below), but TokenBucket is a lower-level class
// used directly here and in benchmarks/benchmark.cpp, so it must stay
// well-defined even when constructed with a degenerate rate directly.

TEST(TokenBucketTest, ZeroRefillRateNeverDividesByZero) {
    TokenBucket bucket(1.0, 0.0);

    EXPECT_TRUE(bucket.consume().allowed);

    RateLimitResult result = bucket.consume();
    EXPECT_FALSE(result.allowed);
    EXPECT_EQ(result.retry_after_seconds, std::numeric_limits<uint64_t>::max());
}

TEST(TokenBucketTest, NegativeRefillRateNeverDividesByZero) {
    TokenBucket bucket(1.0, -5.0);

    EXPECT_TRUE(bucket.consume().allowed);

    RateLimitResult result = bucket.consume();
    EXPECT_FALSE(result.allowed);
    EXPECT_EQ(result.retry_after_seconds, std::numeric_limits<uint64_t>::max());
}

// --- Config validation ---

namespace {

// Writes `contents` to a temp file and returns its path; the file is
// removed when the returned guard goes out of scope.
class TempConfigFile {
public:
    explicit TempConfigFile(const std::string& contents)
        : path_("test_config_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".yaml") {
        std::ofstream out(path_);
        out << contents;
    }
    ~TempConfigFile() { std::remove(path_.c_str()); }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

} // namespace

TEST(ConfigTest, LoadsValidFileAndDefaultsOmittedFields) {
    TempConfigFile file(
        "rate_limit:\n"
        "  capacity: 5\n"
        "  refill_rate: 1.5\n");

    Config config = Config::loadFromFile(file.path());

    EXPECT_DOUBLE_EQ(config.rate_limit.capacity, 5.0);
    EXPECT_DOUBLE_EQ(config.rate_limit.refill_rate, 1.5);
    // server/redis sections were omitted entirely -> struct defaults apply.
    EXPECT_EQ(config.server.host, "0.0.0.0");
    EXPECT_EQ(config.server.port, 8080);
    EXPECT_FALSE(config.redis.enabled);
}

TEST(ConfigTest, RejectsNonPositiveCapacity) {
    TempConfigFile file(
        "rate_limit:\n"
        "  capacity: 0\n"
        "  refill_rate: 1.0\n");

    EXPECT_THROW(Config::loadFromFile(file.path()), std::runtime_error);
}

TEST(ConfigTest, RejectsNegativeCapacity) {
    TempConfigFile file(
        "rate_limit:\n"
        "  capacity: -10\n"
        "  refill_rate: 1.0\n");

    EXPECT_THROW(Config::loadFromFile(file.path()), std::runtime_error);
}

TEST(ConfigTest, RejectsZeroRefillRate) {
    TempConfigFile file(
        "rate_limit:\n"
        "  capacity: 10\n"
        "  refill_rate: 0\n");

    EXPECT_THROW(Config::loadFromFile(file.path()), std::runtime_error);
}

TEST(ConfigTest, RejectsNegativeRefillRate) {
    TempConfigFile file(
        "rate_limit:\n"
        "  capacity: 10\n"
        "  refill_rate: -2.0\n");

    EXPECT_THROW(Config::loadFromFile(file.path()), std::runtime_error);
}

TEST(ConfigTest, RejectsOutOfRangePort) {
    TempConfigFile file(
        "server:\n"
        "  port: 70000\n");

    EXPECT_THROW(Config::loadFromFile(file.path()), std::runtime_error);
}

TEST(ConfigTest, RejectsMalformedYaml) {
    TempConfigFile file(
        "server: [this is not\n"
        "  a valid: yaml structure\n");

    EXPECT_THROW(Config::loadFromFile(file.path()), std::runtime_error);
}

TEST(ConfigTest, RejectsMissingFile) {
    EXPECT_THROW(Config::loadFromFile("this_file_does_not_exist.yaml"), std::runtime_error);
}